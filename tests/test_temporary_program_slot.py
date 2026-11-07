"""Filesystem-level slot transactions with injected clients; no device access."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.temporary_program_slot import (
    SlotRestoreError, TemporaryProgramSlot, TemporaryProgramSlotError,
)


TARGET = "A:\\系统\\程序\\D88MEM.exe"
ORIGINAL = bytes(range(251)) * 19 + b"original program"
TEMPORARY = bytes(range(255, -1, -1)) * 23 + b"readonly probe"
FOREIGN = b"another program, not owned by this slot session"


def sha(data):
    return hashlib.sha256(data).hexdigest()


class FakeFS:
    """Full remote file contents survive client failure/reconnection."""

    def __init__(self, output):
        self.files = {TARGET: ORIGINAL}
        self.clients = []
        self.calls = []
        self.put_actions = []
        self.fail_get_numbers = set()
        self.get_number = 0
        self.output = output

    def factory(self):
        client = FakeClient(self)
        self.clients.append(client)
        return client


class FakeClient:
    def __init__(self, fs):
        self.fs = fs
        self.broken = self.closed = False

    def get_file(self, remote, local):
        assert not self.broken and not self.closed
        self.fs.get_number += 1
        self.fs.calls.append(("get", remote))
        if self.fs.get_number in self.fs.fail_get_numbers:
            self.broken = True
            raise TimeoutError("GET connection lost")
        data = self.fs.files[remote]
        Path(local).write_bytes(data)
        return {"size": len(data)}

    def put_file(self, local, remote, *, replace=False):
        assert not self.broken and not self.closed
        assert replace is True
        data = Path(local).read_bytes()
        journal = json.loads((self.fs.output / "slot-session.json").read_text(encoding="utf8"))
        assert journal["replacement_attempted"] is True
        assert sha(Path(journal["backup_path"]).read_bytes()) == sha(ORIGINAL)
        if data == ORIGINAL:
            assert journal["restore_put_attempted"] is True
        self.fs.calls.append(("put", remote, data))
        action = self.fs.put_actions.pop(0) if self.fs.put_actions else "ok"
        if action == "timeout_before":
            self.broken = True
            raise TimeoutError("PUT did not commit")
        if action == "no_commit":
            return {"committed": True}
        self.fs.files[remote] = b"corrupted upload" if action == "corrupt" else data
        if action in ("timeout_after", "timeout_after_unbroken"):
            self.broken = action != "timeout_after_unbroken"
            raise TimeoutError("PUT committed but reply lost")
        if action == "interrupt_after":
            self.broken = True
            raise KeyboardInterrupt("interrupted after commit")
        return {"committed": True, "size": len(data)}

    def close(self):
        self.closed = self.broken = True
        self.fs.calls.append(("close",))


class TemporaryProgramSlotTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.output = self.root / "session"
        self.source = self.root / "probe.exe"
        self.source.write_bytes(TEMPORARY)
        self.fs = FakeFS(self.output)

    def slot(self, **kwargs):
        args = dict(client_factory=self.fs.factory, target=TARGET,
                    original_sha=sha(ORIGINAL), temp_source=self.source,
                    temp_sha=sha(TEMPORARY), output=self.output)
        args.update(kwargs)
        return TemporaryProgramSlot(**args)

    def journal(self):
        return json.loads((self.output / "slot-session.json").read_text(encoding="utf8"))

    def puts(self):
        return [c[2] for c in self.fs.calls if c[0] == "put"]

    def assert_closed(self):
        self.assertTrue(self.fs.clients)
        self.assertTrue(all(c.closed for c in self.fs.clients))

    def assert_restored(self, slot):
        self.assertEqual(self.fs.files[TARGET], ORIGINAL)
        self.assertTrue(slot.restored)
        self.assertEqual(slot.restore_sha, sha(ORIGINAL))
        self.assertIsNone(slot.restore_error)
        report = self.journal()
        self.assertTrue(report["restored"])
        self.assertEqual(report["restore_sha256"], sha(ORIGINAL))
        self.assertEqual(report["status"], "restored")
        self.assertIsNone(report["restore_error"])
        self.assertEqual((self.output / "original-backup.bin").read_bytes(), ORIGINAL)
        self.assert_closed()

    def test_success_restores_full_original_and_persists_sha(self):
        slot = self.slot()
        with slot as entered:
            self.assertIs(entered, slot)
            self.assertIs(slot.client, self.fs.clients[0])
            self.assertEqual(self.fs.files[TARGET], TEMPORARY)
            self.assertEqual(self.journal()["status"], "active")
        self.assertEqual(self.puts(), [TEMPORARY, ORIGINAL])
        self.assert_restored(slot)
        with self.assertRaises(TemporaryProgramSlotError):
            _ = slot.client

    def test_probe_exception_restores_then_preserves_probe_error(self):
        slot = self.slot()
        with self.assertRaisesRegex(ValueError, "probe failed"):
            with slot:
                raise ValueError("probe failed")
        self.assert_restored(slot)
        self.assertEqual(self.journal()["probe_error"]["type"], "ValueError")

    def test_probe_keyboard_interrupt_also_restores(self):
        slot = self.slot()
        with self.assertRaises(KeyboardInterrupt):
            with slot:
                raise KeyboardInterrupt("cancelled probe")
        self.assert_restored(slot)

    def test_enter_upload_commits_then_timeout_still_restores(self):
        self.fs.put_actions = ["timeout_after"]
        slot = self.slot()
        with self.assertRaisesRegex(TimeoutError, "reply lost"):
            with slot:
                self.fail("enter must not yield after a timeout")
        self.assertEqual(self.puts(), [TEMPORARY, ORIGINAL])
        self.assertEqual(len(self.fs.clients), 2)
        self.assert_restored(slot)
        self.assertEqual(self.journal()["enter_error"]["type"], "TimeoutError")

    def test_uncertain_put_forces_reconnect_even_if_client_did_not_mark_broken(self):
        self.fs.put_actions = ["timeout_after_unbroken"]
        slot = self.slot()
        with self.assertRaises(TimeoutError):
            with slot:
                pass
        self.assertEqual(len(self.fs.clients), 2)
        self.assert_restored(slot)

    def test_enter_timeout_before_commit_confirms_original_and_skips_put(self):
        self.fs.put_actions = ["timeout_before"]
        slot = self.slot()
        with self.assertRaises(TimeoutError):
            with slot:
                pass
        self.assertEqual(self.puts(), [TEMPORARY])
        self.assertEqual(self.journal()["restore_action"], "already_original")
        self.assert_restored(slot)

    def test_enter_interrupt_after_commit_is_cleaned(self):
        self.fs.put_actions = ["interrupt_after"]
        slot = self.slot()
        with self.assertRaises(KeyboardInterrupt):
            with slot:
                pass
        self.assert_restored(slot)

    def test_unknown_target_is_not_overwritten_and_backup_is_kept(self):
        slot = self.slot()
        with self.assertRaisesRegex(SlotRestoreError, "Unknown target SHA"):
            with slot:
                self.fs.files[TARGET] = FOREIGN
        self.assertEqual(self.fs.files[TARGET], FOREIGN)
        self.assertEqual(self.puts(), [TEMPORARY])
        self.assertFalse(slot.restored)
        report = self.journal()
        self.assertEqual(report["status"], "restore_failed")
        self.assertEqual(report["before_restore"]["sha256"], sha(FOREIGN))
        self.assertEqual((self.output / "original-backup.bin").read_bytes(), ORIGINAL)
        self.assert_closed()

    def test_unknown_target_cleanup_error_chains_original_probe_failure(self):
        with self.assertRaises(SlotRestoreError) as raised:
            with self.slot():
                self.fs.files[TARGET] = FOREIGN
                raise ValueError("probe failure too")
        self.assertIsInstance(raised.exception.__cause__, ValueError)
        self.assertEqual(self.journal()["probe_error"]["message"], "probe failure too")

    def test_restore_corruption_is_not_reported_success(self):
        self.fs.put_actions = ["ok", "corrupt"]
        slot = self.slot()
        with self.assertRaisesRegex(SlotRestoreError, "SHA-256 not confirmed"):
            with slot:
                pass
        self.assertEqual(len(self.puts()), 2)
        self.assertEqual(self.fs.files[TARGET], b"corrupted upload")
        report = self.journal()
        self.assertFalse(report["restored"])
        self.assertIsNone(report["restore_sha256"])
        self.assertNotEqual(report["restored_readback"]["sha256"], sha(ORIGINAL))
        self.assert_closed()

    def test_restore_timeout_after_commit_can_be_confirmed_by_fresh_get(self):
        self.fs.put_actions = ["ok", "timeout_after"]
        slot = self.slot()
        with slot:
            pass
        self.assertEqual(self.puts(), [TEMPORARY, ORIGINAL])
        self.assertEqual(self.journal()["restore_put_error"]["type"], "TimeoutError")
        self.assertEqual(len(self.fs.clients), 2)
        self.assert_restored(slot)

    def test_restore_timeout_before_commit_does_not_blindly_retry(self):
        self.fs.put_actions = ["ok", "timeout_before"]
        slot = self.slot()
        with self.assertRaises(SlotRestoreError):
            with slot:
                pass
        self.assertEqual(self.puts(), [TEMPORARY, ORIGINAL])
        self.assertEqual(self.fs.files[TARGET], TEMPORARY)
        self.assertFalse(slot.restored)
        self.assert_closed()

    def test_broken_probe_client_is_closed_and_reconnected_for_restore(self):
        slot = self.slot()
        with slot:
            slot.client.broken = True
        self.assertEqual(len(self.fs.clients), 2)
        self.assert_restored(slot)

    def test_read_only_restore_confirmation_can_retry_on_new_connection(self):
        self.fs.fail_get_numbers = {3}
        slot = self.slot()
        with slot:
            pass
        self.assertEqual(len(self.fs.clients), 2)
        self.assertEqual(self.puts(), [TEMPORARY, ORIGINAL])
        self.assert_restored(slot)

    def test_unreadable_target_refuses_to_restore_blindly(self):
        self.fs.fail_get_numbers = {3, 4}
        slot = self.slot()
        with self.assertRaises(SlotRestoreError):
            with slot:
                pass
        self.assertEqual(self.puts(), [TEMPORARY])
        self.assertEqual(self.fs.files[TARGET], TEMPORARY)
        self.assertFalse(slot.restored)
        self.assert_closed()

    def test_already_original_is_verified_without_uploading_again(self):
        slot = self.slot()
        with slot:
            self.fs.files[TARGET] = ORIGINAL
        self.assertEqual(self.puts(), [TEMPORARY])
        self.assertEqual(self.journal()["restore_action"], "already_original")
        self.assert_restored(slot)

    def test_changed_local_backup_is_not_uploaded(self):
        slot = self.slot()
        with self.assertRaisesRegex(SlotRestoreError, "backup changed"):
            with slot:
                (self.output / "original-backup.bin").write_bytes(FOREIGN)
        self.assertEqual(self.puts(), [TEMPORARY])
        self.assertEqual(self.fs.files[TARGET], TEMPORARY)
        self.assert_closed()

    def test_source_is_frozen_before_remote_replace(self):
        slot = self.slot()
        with slot:
            self.source.write_bytes(FOREIGN)
            self.assertEqual((self.output / "temporary-image.bin").read_bytes(), TEMPORARY)
        self.assert_restored(slot)

    def test_wrong_temporary_source_fails_before_connecting(self):
        self.source.write_bytes(FOREIGN)
        with self.assertRaisesRegex(TemporaryProgramSlotError, "Temporary source SHA"):
            with self.slot():
                pass
        self.assertEqual(self.fs.calls, [])

    def test_wrong_original_target_is_backed_up_but_not_replaced(self):
        self.fs.files[TARGET] = FOREIGN
        slot = self.slot()
        with self.assertRaisesRegex(TemporaryProgramSlotError, "Original target SHA"):
            with slot:
                pass
        self.assertEqual(self.puts(), [])
        self.assertEqual(self.fs.files[TARGET], FOREIGN)
        self.assertEqual(self.journal()["status"], "not_replaced")
        self.assertFalse(self.journal()["replacement_attempted"])
        self.assertEqual((self.output / "original-backup.bin").read_bytes(), FOREIGN)
        self.assert_closed()

    def test_failed_original_download_never_attempts_replace(self):
        self.fs.fail_get_numbers = {1}
        with self.assertRaises(TimeoutError):
            with self.slot():
                pass
        self.assertEqual(self.puts(), [])
        self.assertEqual(self.fs.files[TARGET], ORIGINAL)
        self.assert_closed()

    def test_false_upload_success_does_not_bypass_readback(self):
        self.fs.put_actions = ["no_commit"]
        slot = self.slot()
        with self.assertRaisesRegex(TemporaryProgramSlotError, "Temporary target readback"):
            with slot:
                pass
        self.assertEqual(self.puts(), [TEMPORARY])
        self.assert_restored(slot)

    def test_corrupt_temporary_upload_is_preserved_as_unknown_not_overwritten(self):
        self.fs.put_actions = ["corrupt"]
        with self.assertRaises(SlotRestoreError):
            with self.slot():
                pass
        self.assertEqual(self.puts(), [TEMPORARY])
        self.assertEqual(self.fs.files[TARGET], b"corrupted upload")
        self.assertEqual(self.journal()["enter_error"]["type"], "TemporaryProgramSlotError")
        self.assert_closed()

    def test_existing_journal_is_not_overwritten_or_connected(self):
        self.output.mkdir()
        journal = self.output / "slot-session.json"
        journal.write_text("prior evidence", encoding="utf8")
        with self.assertRaisesRegex(TemporaryProgramSlotError, "fresh output"):
            with self.slot():
                pass
        self.assertEqual(journal.read_text(encoding="utf8"), "prior evidence")
        self.assertEqual(self.fs.calls, [])

    def test_session_is_single_use(self):
        slot = self.slot()
        with slot:
            pass
        with self.assertRaises(TemporaryProgramSlotError):
            with slot:
                pass
        self.assertEqual(self.puts(), [TEMPORARY, ORIGINAL])

    def test_invalid_sha_rejected_and_uppercase_sha_accepted(self):
        for digest in ("", "a" * 63, "z" * 64, " " * 64):
            with self.subTest(digest=digest), self.assertRaises(ValueError):
                self.slot(original_sha=digest)
        slot = self.slot(original_sha=sha(ORIGINAL).upper(), temp_sha=sha(TEMPORARY).upper())
        with slot:
            pass
        self.assert_restored(slot)

    def test_journal_failure_before_put_blocks_replace_but_confirms_original(self):
        slot = self.slot()
        save = slot._save

        def fail_before_put():
            if slot._report["status"] == "replacing":
                raise OSError("disk unavailable")
            save()

        with patch.object(slot, "_save", fail_before_put):
            with self.assertRaises(TemporaryProgramSlotError):
                with slot:
                    pass
        self.assertEqual(self.puts(), [])
        self.assert_restored(slot)
        self.assertTrue(self.journal()["journal_errors"])

    def test_journal_failure_after_replace_does_not_prevent_finally_restore(self):
        slot = self.slot()
        save = slot._save

        def fail_active():
            if slot._report["status"] == "active":
                raise OSError("disk unavailable after replace")
            save()

        with patch.object(slot, "_save", fail_active):
            with self.assertRaises(TemporaryProgramSlotError):
                with slot:
                    self.fail("active journal must be durable before yielding")
        self.assertEqual(self.puts(), [TEMPORARY, ORIGINAL])
        self.assert_restored(slot)


if __name__ == "__main__":
    unittest.main()
