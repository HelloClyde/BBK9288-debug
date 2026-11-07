"""Temporarily replace an owned program and verify its restoration on exit.

``client_factory`` returns a connected ``host.client.Client`` (or an injected
object with get_file, put_file, close and broken). No serial port is opened by
this module itself. PUT is never automatically replayed: after an uncertain
outcome, a full GET determines whether the original, temporary, or an unknown
file occupies the slot. Backups and the journal remain in ``output``.

The caller must keep the process alive until the context exits. This is a
finally-based transaction, not crash/power-loss recovery or a remote file lock.
"""
from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
from typing import Callable


class TemporaryProgramSlotError(RuntimeError):
    """The slot session could not complete its verified lifecycle."""


class SlotRestoreError(TemporaryProgramSlotError):
    """Restoration was not verified; inspect slot-session.json and backups."""


def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _expected_sha(value: str, name: str) -> str:
    if not isinstance(value, str) or len(value) != 64:
        raise ValueError(f"{name} must be a complete SHA-256 hex digest")
    try:
        bytes.fromhex(value)
    except ValueError as error:
        raise ValueError(f"{name} must be a complete SHA-256 hex digest") from error
    if any(c not in "0123456789abcdefABCDEF" for c in value):
        raise ValueError(f"{name} must be a complete SHA-256 hex digest")
    return value.lower()


def _error(error: BaseException) -> dict:
    return {"type": type(error).__name__, "message": str(error)}


class TemporaryProgramSlot:
    """Use ``with TemporaryProgramSlot(...) as slot: c = slot.client``.

    Only the two explicitly supplied content hashes are accepted at the target.
    A different or unreadable target is preserved and reported as a restore
    failure. A completed full-file SHA readback is required for ``restored``.
    A session object and its output journal cannot be reused.
    """

    def __init__(self, client_factory: Callable, target: str, original_sha: str,
                 temp_source: str | Path, temp_sha: str, output: str | Path):
        if not callable(client_factory):
            raise TypeError("client_factory must be callable")
        if not isinstance(target, str) or not target or "\0" in target:
            raise ValueError("target must be a nonempty remote path")
        self.client_factory = client_factory
        self.target = target
        self.original_sha = _expected_sha(original_sha, "original_sha")
        self.temp_sha = _expected_sha(temp_sha, "temp_sha")
        self.temp_source = Path(temp_source).resolve()
        self.output = Path(output).resolve()
        self.journal_path = self.output / "slot-session.json"
        self.backup_path = self.output / "original-backup.bin"
        self.temp_path = self.output / "temporary-image.bin"
        self._client = None
        self._used = self._active = self._journal_created = False
        self._read_number = 0
        self._journal_errors: list[dict] = []
        self.restored = False
        self.restore_sha: str | None = None
        self.restore_error: str | None = None
        self._report = {
            "version": 1, "target": target,
            "original_sha256": self.original_sha,
            "temporary_sha256": self.temp_sha,
            "temporary_source": str(self.temp_source),
            "backup_path": str(self.backup_path),
            "temporary_copy_path": str(self.temp_path),
            "status": "preflight", "replacement_attempted": False,
            "restore_put_attempted": False, "restored": False,
            "restore_sha256": None, "restore_error": None,
            "connections": 0, "events": [], "close_errors": [],
        }

    @property
    def client(self):
        if not self._active:
            raise TemporaryProgramSlotError("Slot client is available only inside the active context")
        return self._connection()

    def _save(self) -> None:
        """Atomic journal update; the initial PUT depends on this succeeding."""
        self._report["updated_utc"] = datetime.now(timezone.utc).isoformat()
        self._report["journal_errors"] = list(self._journal_errors)
        temporary = self.journal_path.with_suffix(".json.tmp")
        with temporary.open("w", encoding="utf-8", newline="\n") as handle:
            json.dump(self._report, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, self.journal_path)

    def _record(self, event: str, *, required: bool = False, **details) -> None:
        self._report["events"].append({"event": event, **details})
        if not self._journal_created:
            return
        try:
            self._save()
        except OSError as error:
            self._journal_errors.append(_error(error))
            if required:
                raise
            # A local journal failure must not prevent the finally restoration.

    def _close_connection(self) -> BaseException | None:
        client, self._client = self._client, None
        if client is None:
            return None
        try:
            client.close()
        except BaseException as error:
            self._report["close_errors"].append(_error(error))
            self._record("close_failed", error=_error(error))
            return error
        self._record("connection_closed")
        return None

    def _connection(self):
        if self._client is not None and bool(getattr(self._client, "broken", False)):
            self._close_connection()
        if self._client is None:
            self._client = self.client_factory()
            self._report["connections"] += 1
            self._record("connected", number=self._report["connections"])
        return self._client

    def _read_target(self, stage: str, *, backup: bool = False) -> dict:
        # Retrying a complete read on a new identity-checked Client is safe.
        # Never retry a PUT here, nor use a partial/stale local download.
        for attempt in range(2):
            self._read_number += 1
            path = self.backup_path if backup else self.output / f"readback-{self._read_number:03d}.bin"
            if path.exists():
                raise TemporaryProgramSlotError(f"Refusing to reuse existing local readback: {path}")
            try:
                self._connection().get_file(self.target, path)
                data = path.read_bytes()
                result = {"path": str(path), "size": len(data), "sha256": _sha(data)}
                self._record("target_read", stage=stage, **result)
                return result
            except BaseException as error:
                self._record("target_read_failed", stage=stage, attempt=attempt + 1,
                             error=_error(error))
                self._close_connection()
                # Do not overwrite any possibly partial original backup. A
                # failed initial backup means no replacement can be attempted.
                if backup or attempt or not isinstance(error, Exception):
                    raise
        raise AssertionError("unreachable")

    def __enter__(self):
        if self._used:
            raise TemporaryProgramSlotError("A slot session can be entered only once")
        self._used = True
        # Freeze the validated temporary image so a later build cannot change
        # what is uploaded. Preflight failures here have made no remote calls.
        image = self.temp_source.read_bytes()
        if _sha(image) != self.temp_sha:
            raise TemporaryProgramSlotError("Temporary source SHA-256 does not match the expected image")
        self.output.mkdir(parents=True, exist_ok=True)
        if any(path.exists() for path in (self.journal_path, self.backup_path, self.temp_path)):
            raise TemporaryProgramSlotError("Use a fresh output directory; prior slot evidence already exists")
        # Exclusive creation prevents this tool from taking over an old session.
        with self.journal_path.open("x", encoding="utf-8") as handle:
            handle.write("{}\n")
        self._journal_created = True
        try:
            self._record("session_started", required=True)
            with self.temp_path.open("xb") as handle:
                handle.write(image)
                handle.flush()
                os.fsync(handle.fileno())
            before = self._read_target("backup", backup=True)
            self._report["backup"] = before
            if before["sha256"] != self.original_sha:
                raise TemporaryProgramSlotError("Original target SHA-256 mismatch; target was not replaced")
            self._report["status"] = "replacing"
            # Persist this BEFORE PUT: its exception can follow a remote commit.
            self._report["replacement_attempted"] = True
            self._record("replacement_attempted", required=True)
            try:
                self._connection().put_file(self.temp_path, self.target, replace=True)
            except BaseException:
                self._close_connection()
                raise
            check = self._read_target("temporary_verification")
            self._report["temporary_readback"] = check
            if check["sha256"] != self.temp_sha:
                raise TemporaryProgramSlotError("Temporary target readback SHA-256 mismatch")
            self._active = True
            self._report["status"] = "active"
            self._record("temporary_verified", required=True)
            return self
        except BaseException as error:
            self._report["enter_error"] = _error(error)
            failure = self._finish()
            if failure is not None:
                raise failure from error
            raise

    def _restore(self) -> None:
        current = self._read_target("before_restore")
        self._report["before_restore"] = current
        digest = current["sha256"]
        if digest == self.original_sha:
            self._report["restore_action"] = "already_original"
        elif digest == self.temp_sha:
            # Inspect current remote content before restoring. The backup must
            # still be exactly the original that was authorized for this slot.
            if _sha(self.backup_path.read_bytes()) != self.original_sha:
                raise SlotRestoreError("Local original backup changed; refusing to upload it")
            self._report["status"] = "restoring"
            self._report["restore_put_attempted"] = True
            self._report["restore_action"] = "upload_original"
            self._record("restore_attempted")
            try:
                self._connection().put_file(self.backup_path, self.target, replace=True)
            except BaseException as error:
                self._report["restore_put_error"] = _error(error)
                self._record("restore_put_failed", error=_error(error))
                self._close_connection()
                # The restore may already have committed. Read to decide; do
                # not blindly replay either the replacement or the restoration.
            current = self._read_target("restored_verification")
            self._report["restored_readback"] = current
            if current["sha256"] != self.original_sha:
                raise SlotRestoreError("Original restoration SHA-256 not confirmed; target preserved without another PUT")
        else:
            raise SlotRestoreError(
                f"Unknown target SHA-256 {digest}; refusing to overwrite it with the backup")
        self.restored = True
        self.restore_sha = current["sha256"]
        self._report["restored"] = True
        self._report["restore_sha256"] = self.restore_sha
        self._record("original_verified", sha256=self.restore_sha)

    def _finish(self) -> BaseException | None:
        """Always attempt restore/verification, then always close the client."""
        self._active = False
        failure = None
        try:
            if self._report["replacement_attempted"]:
                self._restore()
            else:
                self._report["restore_action"] = "not_needed_no_replacement_attempted"
        except BaseException as error:
            self.restore_error = str(error)
            self._report["restore_error"] = _error(error)
            failure = SlotRestoreError(f"Original slot restoration was NOT verified: {error}; journal: {self.journal_path}")
        finally:
            close_error = self._close_connection()
            if failure is None and close_error is not None:
                failure = TemporaryProgramSlotError(f"Slot was verified but closing the client failed: {close_error}")
            self._report["status"] = "restore_failed" if self.restore_error else (
                "restored" if self.restored else "not_replaced")
            self._record("session_finished")
            if failure is None and self._journal_errors:
                failure = TemporaryProgramSlotError(
                    f"Slot cleanup completed, but journal writes failed: {self._journal_errors}; {self.journal_path}")
        return failure

    def __exit__(self, exc_type, exc, traceback):
        if exc is not None:
            self._report["probe_error"] = _error(exc)
        failure = self._finish()
        if failure is not None:
            raise failure from exc
        return False
