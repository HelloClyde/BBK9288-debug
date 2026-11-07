"""Experimental BBK 9288 captured-firmware USB debugger client.

Run ``python host/client.py --help`` without installing optional dependencies.
Serial connections require pyserial; PNG output additionally requires Pillow.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import time
import zlib

MAGIC = b"88DB"
VERSION = 1
IDENTITY = b"BBK9288DEBUG/1 CAPTURED-0215EBC4 EXPERIMENTAL"
VID, PID = 0x1209, 0x9288
MAX_PAYLOAD = 512
CAP_READ, CAP_WRITE, CAP_KEY, CAP_SCREEN, CAP_STOP, CAP_SCRATCH = (1 << bit for bit in range(6))
CAP_FILES = 1 << 6
CAP_APP_REFRESH = 1 << 7
APP_REFRESH = 21
REFRESH_PARTIAL_WARNING = ("Application records may already be partially refreshed; inspect the device desktop. "
                           "Do not automatically replay refresh or re-upload the program.")
APP_REFRESH_FIELDS = ("version", "before_total", "after_total", "before_games", "after_games", "post_result")
FILE_PATH_MAX, FILE_CHUNK, FILE_MAX_BYTES = 240, 480, 16 * 1024 * 1024
FILE_LIST_OPEN, FILE_LIST_NEXT, FILE_GET_OPEN, FILE_GET_READ = 11, 12, 13, 14
FILE_PUT_OPEN, FILE_PUT_DATA, FILE_FINISH, FILE_ABORT, FILE_POLL, FILE_CANCEL = 15, 16, 17, 18, 19, 20
FILE_CANCELLED = -710
FLAG_FOREGROUND, FLAG_HARDWARE_UNVERIFIED = 1, 2
FLAG_UPTIME_UNAVAILABLE = 4
HEADER = struct.Struct("<4sBBHHH")
U32 = struct.Struct("<I")
STATUS_FIELDS = ("image_id", "caps", "requests", "errors", "uptime_ms",
                 "snapshot_token", "snapshot_bytes", "flags")
SCREEN_FIELDS = ("token", "width", "height", "bpp", "stride", "size", "format", "flags")


class ProtocolError(RuntimeError):
    """The peer or transport violated the protocol; reconnect before proceeding."""


class DeviceError(RuntimeError):
    """A valid reply explicitly rejected the request."""

    def __init__(self, opcode: int, status: int, payload: bytes):
        self.opcode, self.status, self.payload = opcode, status, bytes(payload)
        super().__init__(f"Device rejected opcode {opcode}: status={status}, payload={payload.hex()}")


class FileError(RuntimeError):
    """A completed, synchronized file job failed; the connection remains usable."""

    def __init__(self, opcode: int, result: int, job_id: int = 0, payload: bytes = b""):
        self.opcode, self.result, self.job_id, self.payload = opcode, result, job_id, bytes(payload)
        names = {-701: "invalid request", -702: "busy", -703: "file I/O failed",
                 -704: "not found", -705: "destination exists", -706: "CRC32 mismatch",
                 -707: "invalid transaction state", -708: "invalid GBK path",
                 -709: "firmware profile rejected", -710: "cancelled", -711: "size mismatch",
                 -712: "limit exceeded", -713: "commit failed", -714: "recovery required",
                 -715: "unsupported"}
        detail = f"; recovery data={self.payload.hex()}" if self.payload else ""
        if opcode == APP_REFRESH and result in (-703, -709):
            detail += "; " + REFRESH_PARTIAL_WARNING
        super().__init__(f"File job {job_id}, opcode {opcode}: {names.get(result, 'failed')} ({result}){detail}")


def remote_path(value: str, *, directory: bool = False) -> tuple[str, bytes]:
    """Normalize Unicode before encoding: a GBK trail byte 0x5C is not a separator."""
    if not isinstance(value, str):
        raise ValueError("Remote path must be text")
    value = value.replace("/", "\\")
    if value[:3].upper() != "A:\\":
        raise ValueError("Remote path must be absolute on A:\\")
    value = "A:\\" + value[3:]
    if directory and value.endswith("\\") and len(value) > 3:
        value = value[:-1]
    parts = value[3:].split("\\") if len(value) > 3 else []
    if (not parts and not directory) or any(
        not part or part in (".", "..") or part.endswith((" ", ".")) or
        any(ord(ch) < 32 or ord(ch) == 127 or ch in '<>:"|?*' for ch in part) for part in parts
    ):
        raise ValueError("Remote path contains an invalid component")
    try:
        encoded = value.encode("gbk", errors="strict")
    except UnicodeError as exc:
        raise ValueError("Remote path cannot be represented in GBK") from exc
    if not 3 <= len(encoded) <= FILE_PATH_MAX:
        raise ValueError("Remote GBK path exceeds 240 bytes")
    return value, encoded


def bounded(value: int, minimum: int, maximum: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise ValueError(f"{name} must be {minimum}..{maximum}")
    return value


def integer(value: str) -> int:
    try:
        return int(value, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"Invalid integer: {value}") from exc


def frame(opcode: int, sequence: int, payload: bytes = b"", status: int = 0) -> bytes:
    bounded(opcode, 1, 255, "opcode")
    bounded(sequence, 0, 65535, "sequence")
    bounded(status, 0, 65535, "status")
    payload = bytes(payload)
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("Payload exceeds 512 bytes")
    packet = HEADER.pack(MAGIC, VERSION, opcode, sequence, len(payload), status) + payload
    return packet + U32.pack(zlib.crc32(packet) & 0xFFFFFFFF)


def serial_dependencies():
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError("Serial support requires pyserial: python -m pip install pyserial") from exc
    return serial, list_ports


def ports() -> list[dict]:
    """List only the experimental 1209:9288 USB CDC identity, without opening it."""
    _, list_ports = serial_dependencies()
    return [{"port": p.device, "description": p.description, "serial_number": p.serial_number,
             "vid": p.vid, "pid": p.pid, "hwid": p.hwid}
            for p in list_ports.comports() if p.vid == VID and p.pid == PID]


def validate_screen(meta: dict) -> None:
    """Validate dimensions and storage before allocating or decoding remote data."""
    try:
        bounded(meta["token"], 1, 0xFFFFFFFF, "snapshot token")
        width = bounded(meta["width"], 1, 1024, "screen width")
        height = bounded(meta["height"], 1, 1024, "screen height")
        expected_bpp = {1: 8, 2: 2}.get(meta["format"])
        if expected_bpp is None or meta["bpp"] != expected_bpp:
            raise ValueError("Unsupported screen format/bpp")
        minimum_stride = (width * expected_bpp + 7) // 8
        bounded(meta["stride"], minimum_stride, minimum_stride + 64, "screen stride")
        if meta["size"] != meta["stride"] * height or meta["size"] > 1024 * 1024:
            raise ValueError("Screen size/stride/height mismatch")
    except (KeyError, TypeError, ValueError) as exc:
        raise ProtocolError(f"Invalid screenshot metadata: {exc}") from exc


def screen_pixels(meta: dict, data: bytes) -> bytes:
    """Return row-major GRAY8 at the original dimensions, discarding row padding."""
    validate_screen(meta)
    if len(data) != meta["size"]:
        raise ProtocolError("Screenshot byte length mismatch")
    width, height, stride = meta["width"], meta["height"], meta["stride"]
    pixels = bytearray(width * height)
    for y in range(height):
        row = data[y * stride:(y + 1) * stride]
        if meta["format"] == 1:
            pixels[y * width:(y + 1) * width] = row[:width]
        else:
            for x in range(width):
                pixels[y * width + x] = ((row[x // 4] >> (6 - 2 * (x % 4))) & 3) * 85
    return bytes(pixels)


def save_png(path: str | Path, meta: dict, data: bytes) -> None:
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError("PNG output requires Pillow: python -m pip install Pillow") from exc
    pixels = screen_pixels(meta, data)
    target = Path(path).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=target.name + ".part-", dir=target.parent)
    try:
        with os.fdopen(fd, "wb") as output:
            Image.frombytes("L", (meta["width"], meta["height"]), pixels).save(output, format="PNG")
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class Client:
    """One serial connection; requests are serialized and never automatically replayed.

    An explicit port can be a UART adapter. Automatic discovery selects only the
    experimental USB VID/PID. Both still require the exact 9288 identity reply.
    ``transport`` is a serial-like object for offline tests and custom adapters.
    """

    def __init__(self, port: str | None = None, *, baud: int = 38400,
                 timeout: float = 5.0, transport=None):
        if not math.isfinite(timeout) or not 0.01 <= timeout <= 60:
            raise ValueError("timeout must be 0.01..60 seconds")
        bounded(baud, 1, 4000000, "baud")
        self.timeout, self.sequence = timeout, 0
        self._broken = False
        self._lock = threading.RLock()
        self._scratch = None
        self._file_transaction_owned = False
        self._file_recovery = False
        if transport is None:
            serial, _ = serial_dependencies()
            if port is None:
                candidates = ports()
                if len(candidates) != 1:
                    raise RuntimeError("Expected exactly one experimental 9288 USB port; found " +
                                       str([p["port"] for p in candidates]) + "; specify --port for UART")
                port = candidates[0]["port"]
            self.serial = serial.Serial(port, baud, timeout=min(0.1, timeout), write_timeout=timeout)
        else:
            self.serial = transport
        self.port = port or "injected-transport"
        try:
            self.serial.reset_input_buffer()
            identity = self.rpc(1)
            if identity != IDENTITY:
                self._fail(f"9288 identity mismatch: {identity!r}")
            self.identity = identity.decode("ascii")
        except BaseException:
            self.close()
            raise

    @property
    def broken(self) -> bool:
        return self._broken

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def close(self) -> None:
        with self._lock:
            self._broken = True
            self.serial.close()

    def _fail(self, message: str):
        self._broken = True
        raise ProtocolError(message)

    def _exact(self, count: int, deadline: float) -> bytes:
        data = bytearray()
        while len(data) < count and time.monotonic() < deadline:
            part = self.serial.read(count - len(data))
            if part:
                if len(part) > count - len(data):
                    self._fail("Transport returned more bytes than requested")
                data.extend(part)
        if len(data) != count:
            raise TimeoutError(f"Reply incomplete ({len(data)}/{count}); command outcome may be unknown")
        return bytes(data)

    def rpc(self, opcode: int, payload: bytes = b"") -> bytes:
        bounded(opcode, 1, 127, "request opcode")
        payload = bytes(payload)
        if len(payload) > MAX_PAYLOAD:
            raise ValueError("Request exceeds 512 bytes")
        with self._lock:
            if self._broken:
                raise ProtocolError("Session is closed or desynchronized; reconnect explicitly")
            self.sequence = (self.sequence + 1) & 65535
            packet = frame(opcode, self.sequence, payload)
            deadline = time.monotonic() + self.timeout
            try:
                if self.serial.write(packet) != len(packet):
                    self._fail("Incomplete write; command outcome may be unknown")
                head = self._exact(HEADER.size, deadline)
                magic, version, reply, sequence, size, status = HEADER.unpack(head)
                if ((magic, version, reply, sequence) != (MAGIC, VERSION, opcode | 0x80, self.sequence)
                        or size > MAX_PAYLOAD):
                    self._fail("Reply magic/version/opcode/sequence/length mismatch")
                tail = self._exact(size + 4, deadline)
                if (zlib.crc32(head + tail[:-4]) & 0xFFFFFFFF) != U32.unpack(tail[-4:])[0]:
                    self._fail("Reply CRC32 mismatch")
            except BaseException:
                self._broken = True
                raise
            if status:
                raise DeviceError(opcode, status, tail[:-4])
            return tail[:-4]

    def _words(self, opcode: int, fields: tuple[str, ...], payload: bytes = b"") -> dict:
        data = self.rpc(opcode, payload)
        if len(data) != 4 * len(fields):
            self._fail(f"Opcode {opcode} reply size mismatch")
        return dict(zip(fields, struct.unpack("<" + "I" * len(fields), data)))

    def _empty(self, opcode: int, payload: bytes = b"") -> None:
        if self.rpc(opcode, payload):
            self._fail(f"Opcode {opcode} expected an empty reply")

    def ping(self) -> str:
        identity = self.rpc(1)
        if identity != IDENTITY:
            self._fail("9288 identity changed during this session")
        return identity.decode("ascii")

    def status(self) -> dict:
        return self._words(2, STATUS_FIELDS)

    @staticmethod
    def _range(address: int, count: int) -> None:
        bounded(address, 0, 0xFFFFFFFF, "address")
        bounded(count, 1, 8 * 1024 * 1024, "byte count")
        if address + count > 0x100000000:
            raise ValueError("Address range overflows u32")

    def read_memory(self, address: int, count: int) -> bytes:
        self._range(address, count)
        with self._lock:
            output = bytearray()
            while len(output) < count:
                wanted = min(MAX_PAYLOAD, count - len(output))
                data = self.rpc(3, struct.pack("<II", address + len(output), wanted))
                if len(data) != wanted:
                    self._fail("READ reply size mismatch")
                output.extend(data)
            return bytes(output)

    def scratch(self) -> dict:
        result = self._words(10, ("address", "size"))
        try:
            self._range(result["address"], result["size"])
        except ValueError as exc:
            self._fail(f"Invalid scratch region: {exc}")
        self._scratch = result
        return result

    def write_memory(self, address: int, data: bytes) -> None:
        data = bytes(data)
        self._range(address, len(data))
        with self._lock:
            region = self._scratch or self.scratch()
            if address < region["address"] or address + len(data) > region["address"] + region["size"]:
                raise ValueError("WRITE is restricted to the device-owned scratch region")
            for offset in range(0, len(data), MAX_PAYLOAD - 4):
                self._empty(9, U32.pack(address + offset) + data[offset:offset + MAX_PAYLOAD - 4])

    def press(self, scan: int) -> None:
        bounded(scan, 1, 127, "scan code")
        self._empty(4, U32.pack(scan))

    def refresh_apps(self, *, cancel_event=None) -> dict:
        """Ask GUI1 to scan program records once while the native desktop is idle."""
        with self._lock:
            if self._file_recovery:
                raise FileError(APP_REFRESH, -714)
            if self._file_transaction_owned:
                raise FileError(APP_REFRESH, -702)
            if not self.status()["caps"] & CAP_APP_REFRESH:
                raise FileError(APP_REFRESH, -715)
            values = self._file_words(APP_REFRESH, b"", len(APP_REFRESH_FIELDS), cancel_event)
            result = dict(zip(APP_REFRESH_FIELDS, values))
            if (result["version"] != 1 or result["post_result"] != 0 or
                    any(result[name] > 128 for name in APP_REFRESH_FIELDS[1:5]) or
                    result["before_games"] > result["before_total"] or
                    result["after_games"] > result["after_total"]):
                self._fail("Invalid application refresh report")
            return result

    def _require_files(self) -> None:
        if self._file_recovery:
            raise FileError(FILE_LIST_OPEN, -714)
        if not self.status()["caps"] & CAP_FILES:
            raise FileError(FILE_LIST_OPEN, -715)

    def _file_command(self, opcode: int, body: bytes, cancel_event=None) -> bytes:
        """Submit once, then query only that job. Cancellation is not a new ABORT job."""
        if cancel_event is not None and cancel_event.is_set():
            raise FileError(opcode, FILE_CANCELLED)
        submitted = self.rpc(opcode, body)
        if len(submitted) != 8:
            self._fail("File submit reply size mismatch")
        job, result = struct.unpack("<Ii", submitted)
        if result:
            if job or not -715 <= result <= -701:
                self._fail("Invalid rejected file job")
            if result == -714:
                self._file_recovery = True
            raise FileError(opcode, result)
        if not job:
            self._fail("File submit returned a zero job id")
        if opcode in (FILE_LIST_OPEN, FILE_GET_OPEN, FILE_PUT_OPEN):
            self._file_transaction_owned = True
        try:
            return self._wait_file_job(opcode, job, cancel_event)
        except FileError:
            raise
        except BaseException:
            # An interrupt or rejected poll must not submit ABORT over the
            # still-pending job. Closing DTR schedules device-side cleanup.
            self._broken = True
            raise

    def _wait_file_job(self, opcode: int, job: int, cancel_event=None) -> bytes:
        # FINISH includes complete temporary/final-file readback on the device.
        # This job budget does not extend the timeout of an individual RPC.
        deadline = time.monotonic() + max(900.0 if opcode == FILE_FINISH else 60.0,
                                          self.timeout * 60.0)
        cancelled = False
        while True:
            if cancel_event is not None and cancel_event.is_set() and not cancelled:
                self._empty(FILE_CANCEL, U32.pack(job))
                cancelled = True
            raw = self.rpc(FILE_POLL, U32.pack(job))
            if len(raw) < 16:
                self._fail("Truncated file job reply")
            reply_job, state, result, length = struct.unpack_from("<IIiI", raw)
            if reply_job != job or state not in (1, 2, 3) or length > 496 or len(raw) != 16 + length:
                self._fail("Invalid file job id/state/length")
            if state == 3:
                if result:
                    if not -715 <= result <= -701:
                        self._fail("Invalid completed file result")
                    if result == -714:
                        self._file_recovery = True
                    raise FileError(opcode, result, job, raw[16:])
                if opcode in (FILE_FINISH, FILE_ABORT):
                    self._file_transaction_owned = False
                return raw[16:]
            if result or length:
                self._fail("Pending file job contains a result")
            if time.monotonic() >= deadline:
                self._broken = True
                raise TimeoutError("File job deadline exceeded; outcome may be unknown")
            time.sleep(0.02)

    def _file_words(self, opcode: int, body: bytes, count: int, cancel_event=None) -> tuple[int, ...]:
        raw = self._file_command(opcode, body, cancel_event)
        if len(raw) != count * 4:
            self._fail("File command reply size mismatch")
        return struct.unpack("<" + "I" * count, raw)

    def _file_abort_cleanup(self) -> None:
        """Cleanup only a synchronized session. Never replay a damaged exchange."""
        if self._broken or self._file_recovery or not self._file_transaction_owned:
            return
        active_exception = sys.exc_info()[0] is not None
        try:
            self._file_words(FILE_ABORT, U32.pack(0), 0)
        except Exception:
            if not active_exception or self._broken:
                raise

    def abort_file(self) -> None:
        with self._lock:
            self._require_files()
            self._file_words(FILE_ABORT, U32.pack(0), 0)

    def list_files(self, path: str, *, cancel_event=None) -> list[dict]:
        path, encoded = remote_path(path, directory=True)
        with self._lock:
            self._require_files()
            active = True
            try:
                token, = self._file_words(FILE_LIST_OPEN, U32.pack(len(encoded)) + encoded, 1, cancel_event)
                if not token:
                    self._fail("LIST returned a zero token")
                entries = []
                for index in range(65536):
                    raw = self._file_command(FILE_LIST_NEXT, struct.pack("<II", token, index), cancel_event)
                    if len(raw) < 24:
                        self._fail("Truncated directory entry")
                    reply_token, reply_index, eof, attributes, size, name_size = struct.unpack_from("<6I", raw)
                    if (reply_token != token or reply_index != index or eof not in (0, 1) or
                        name_size > FILE_PATH_MAX or len(raw) != 24 + name_size or (eof and name_size)):
                        self._fail("Invalid directory entry token/index/name")
                    if eof:
                        self._file_words(FILE_FINISH, U32.pack(token), 0, cancel_event)
                        active = False
                        return entries
                    try:
                        name = raw[24:].decode("gbk", errors="strict")
                        canonical, _ = remote_path(path.rstrip("\\") + "\\" + name)
                    except (UnicodeError, ValueError):
                        self._fail("Invalid GBK directory entry")
                    if not name or "\\" in name or "/" in name:
                        self._fail("Directory entry is not a basename")
                    entries.append({"name": name, "path": canonical, "attributes": attributes, "size": size})
                self._fail("Directory entry count exceeds client limit")
            finally:
                if active:
                    self._file_abort_cleanup()

    def get_file(self, remote: str, local: str | Path, *, progress=None, cancel_event=None) -> dict:
        remote, encoded = remote_path(remote)
        target = Path(local).resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        with self._lock:
            self._require_files()
            active = True
            try:
                token, size = self._file_words(FILE_GET_OPEN, U32.pack(len(encoded)) + encoded, 2, cancel_event)
                if not token or size > FILE_MAX_BYTES:
                    self._fail("Invalid GET token/size")
                offset = crc = 0
                fd, temporary = tempfile.mkstemp(prefix=target.name + ".part-", dir=target.parent)
                with os.fdopen(fd, "wb") as output:
                    if progress:
                        progress(0, size)
                    while offset < size:
                        wanted = min(FILE_CHUNK, size - offset)
                        raw = self._file_command(FILE_GET_READ, struct.pack("<III", token, offset, wanted), cancel_event)
                        if len(raw) != 12 + wanted:
                            self._fail("GET chunk size mismatch")
                        reply_token, reply_offset, chunk_crc = struct.unpack_from("<III", raw)
                        data = raw[12:]
                        if reply_token != token or reply_offset != offset:
                            self._fail("GET chunk token/offset mismatch")
                        if zlib.crc32(data) & 0xFFFFFFFF != chunk_crc:
                            self._fail("GET chunk CRC32 mismatch")
                        output.write(data)
                        offset += len(data)
                        crc = zlib.crc32(data, crc) & 0xFFFFFFFF
                        if progress:
                            progress(offset, size)
                final_token, final_size, final_crc = self._file_words(FILE_FINISH, U32.pack(token), 3, cancel_event)
                if (final_token, final_size, final_crc) != (token, size, crc):
                    self._fail("GET final size/CRC32 mismatch")
                active = False
                os.replace(temporary, target)
                temporary = None
                return {"remote": remote, "output": str(target), "size": size, "crc32": f"{crc:08x}"}
            finally:
                try:
                    if active:
                        self._file_abort_cleanup()
                finally:
                    if temporary is not None:
                        Path(temporary).unlink(missing_ok=True)

    def put_file(self, local: str | Path, remote: str, *, replace: bool = False,
                 progress=None, cancel_event=None) -> dict:
        remote, encoded = remote_path(remote)
        if not isinstance(replace, bool):
            raise ValueError("replace must be boolean")
        with self._lock, Path(local).open("rb") as source:
            size = os.fstat(source.fileno()).st_size
            bounded(size, 0, FILE_MAX_BYTES, "file size")
            full_crc = measured = 0
            for block in iter(lambda: source.read(65536), b""):
                measured += len(block)
                if measured > FILE_MAX_BYTES:
                    raise ValueError("File exceeds 16MiB")
                full_crc = zlib.crc32(block, full_crc) & 0xFFFFFFFF
            if measured != size:
                raise ValueError("Local file size changed before upload")
            source.seek(0)
            self._require_files()
            active = True
            try:
                token, offset = self._file_words(FILE_PUT_OPEN,
                    struct.pack("<4I", size, full_crc, int(replace), len(encoded)) + encoded, 2, cancel_event)
                if not token or offset:
                    self._fail("Invalid PUT token/initial offset")
                running_crc = 0
                if progress:
                    progress(0, size)
                while offset < size:
                    data = source.read(min(FILE_CHUNK, size - offset))
                    if not data:
                        raise ValueError("Local file shortened during upload")
                    running_crc = zlib.crc32(data, running_crc) & 0xFFFFFFFF
                    ack = self._file_words(FILE_PUT_DATA,
                        struct.pack("<III", token, offset, zlib.crc32(data) & 0xFFFFFFFF) + data, 3, cancel_event)
                    offset += len(data)
                    if ack != (token, offset, running_crc):
                        self._fail("PUT token/offset/running CRC32 mismatch")
                    if progress:
                        progress(offset, size)
                if source.read(1) or running_crc != full_crc:
                    raise ValueError("Local file contents changed during upload")
                ack = self._file_words(FILE_FINISH, U32.pack(token), 3, cancel_event)
                if ack != (token, size, full_crc):
                    self._fail("PUT final size/CRC32 mismatch")
                active = False
                return {"remote": remote, "size": size, "crc32": f"{full_crc:08x}",
                        "replace": replace, "committed": True}
            finally:
                if active:
                    self._file_abort_cleanup()

    def capture_screen(self) -> tuple[dict, bytes]:
        with self._lock:
            meta = self._words(5, SCREEN_FIELDS)
            token = meta["token"]
            try:
                try:
                    validate_screen(meta)
                except ProtocolError:
                    self._broken = True
                    raise
                data = bytearray()
                while len(data) < meta["size"]:
                    wanted = min(MAX_PAYLOAD, meta["size"] - len(data))
                    chunk = self.rpc(6, struct.pack("<III", token, len(data), wanted))
                    if len(chunk) != wanted:
                        self._fail("SCREEN_READ reply size mismatch")
                    data.extend(chunk)
                return meta, bytes(data)
            finally:
                if token and not self._broken:
                    active_exception = sys.exc_info()[0] is not None
                    try:
                        self._empty(7, U32.pack(token))
                    except Exception:
                        if not active_exception:
                            raise

    def screenshot(self, output: str | Path) -> dict:
        meta, data = self.capture_screen()
        save_png(output, meta, data)
        return {**meta, "output": str(Path(output).resolve()),
                "raw_sha256": hashlib.sha256(data).hexdigest()}

    def stop(self) -> None:
        self._empty(8)


def is_program_path(remote: str) -> bool:
    """Only downloaded EXEs directly inside the native program directory qualify."""
    canonical, _ = remote_path(remote)
    parts = canonical[3:].split("\\")
    return (len(parts) == 3 and parts[0] == "系统" and parts[1] == "程序" and
            parts[2].lower().endswith(".exe"))


def upload_with_refresh(client: Client, local: str | Path, remote: str, *,
                        replace: bool = False, refresh_apps: bool = False,
                        progress=None, cancel_event=None) -> dict:
    """Preserve a committed upload result even when the separate refresh fails."""
    result = client.put_file(local, remote, replace=replace, progress=progress,
                             cancel_event=cancel_event)
    if not refresh_apps:
        return result
    result = dict(result)
    if not is_program_path(result["remote"]):
        result["refresh"] = {"requested": True, "attempted": False,
                             "reason": "not_program_path"}
        return result
    try:
        refreshed = client.refresh_apps(cancel_event=cancel_event)
        result["refresh"] = {"requested": True, "attempted": True, "ok": True,
                             "result": refreshed}
    except Exception as error:
        result["refresh"] = {"requested": True, "attempted": True, "ok": False,
                             "error_type": type(error).__name__, "error": str(error),
                             "connection_lost": client.broken,
                             "outcome_unknown": isinstance(error, (ProtocolError, TimeoutError, OSError)) or
                                                isinstance(error, FileError) and error.result in (-703, -709)}
        if result["refresh"]["outcome_unknown"]:
            result["refresh"].update(partial_refresh_possible=True, warning=REFRESH_PARTIAL_WARNING)
        if isinstance(error, FileError):
            result["refresh"].update(file_result=error.result, file_job_id=error.job_id,
                                     recovery_required=error.result == -714,
                                     recovery_data_hex=error.payload.hex())
            if error.result == -715:
                result["refresh"]["attempted"] = False
                result["refresh"]["reason"] = "unsupported"
    return result


KEYS = {"ESC": 1, "ESCAPE": 1, "ENTER": 28, "RETURN": 28, "SPACE": 57,
        "BACKSPACE": 14, "TAB": 15, "UP": 103, "DOWN": 108,
        "LEFT": 105, "RIGHT": 106, "HOME": 102, "END": 107,
        "PAGEUP": 104, "PAGEDOWN": 109, "DELETE": 111, "INSERT": 110}
KEYS.update(zip("QWERTYUIOP", range(16, 26)))
KEYS.update(zip("ASDFGHJKL", range(30, 39)))
KEYS.update(zip("ZXCVBNM", range(44, 51)))
KEYS.update({str(i): i + 1 for i in range(1, 10)})
KEYS["0"] = 11
KEYS.update({f"F{i}": 58 + i for i in range(1, 11)})
KEYS.update({"F11": 87, "F12": 88})


def scan_code(value: str) -> int:
    if value.upper() in KEYS:
        return KEYS[value.upper()]
    return bounded(integer(value), 1, 127, "scan code")


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", help="Explicit UART/CDC port (automatic: experimental USB 1209:9288)")
    p.add_argument("--baud", type=integer, default=38400)
    p.add_argument("--timeout", type=float, default=5.0)
    p.add_argument("--report", type=Path, help="Save JSON result, including failures")
    commands = p.add_subparsers(dest="command", required=True)
    for name in ("list", "ping", "status", "stop", "scratch", "refresh-apps"):
        commands.add_parser(name)
    read = commands.add_parser("read")
    read.add_argument("address", type=integer)
    read.add_argument("count", type=integer)
    read.add_argument("--output", type=Path)
    write = commands.add_parser("write", help="Write only to the device-owned scratch region")
    write.add_argument("address", type=integer)
    source = write.add_mutually_exclusive_group(required=True)
    source.add_argument("--input", type=Path)
    source.add_argument("--hex", dest="hex_data")
    press = commands.add_parser("press", help="Queue one complete key DOWN+UP")
    press.add_argument("scan", type=scan_code, help="Name (ENTER/ESC/A/UP) or numeric scan code, e.g. 0x1c")
    screenshot = commands.add_parser("screenshot")
    screenshot.add_argument("output", type=Path)
    listing = commands.add_parser("file-list", help="List a GBK directory on A:\\")
    listing.add_argument("remote")
    download = commands.add_parser("file-get", help="Download and verify a file before replacing the local target")
    download.add_argument("remote")
    download.add_argument("local", type=Path)
    upload = commands.add_parser("file-put", help="Upload using a verified temporary file and guarded commit")
    upload.add_argument("local", type=Path)
    upload.add_argument("remote")
    upload.add_argument("--replace", action="store_true", help="Explicitly replace the remote target with backup/recovery")
    upload.add_argument("--refresh-apps", action="store_true",
                        help="After a committed program EXE upload, refresh native application records")
    return p


def execute(args: argparse.Namespace) -> dict:
    if args.command == "list":
        return {"ports": ports(), "vid": VID, "pid": PID, "experimental": True}
    with Client(args.port, baud=args.baud, timeout=args.timeout) as client:
        if args.command == "ping":
            return {"identity": client.ping()}
        if args.command == "status":
            return client.status()
        if args.command == "scratch":
            return client.scratch()
        if args.command == "stop":
            client.stop()
            return {"stop_accepted": True}
        if args.command == "press":
            client.press(args.scan)
            return {"scan": args.scan, "queued": True}
        if args.command == "screenshot":
            return client.screenshot(args.output)
        if args.command == "refresh-apps":
            return client.refresh_apps()
        if args.command == "file-list":
            return {"entries": client.list_files(args.remote)}
        if args.command == "file-get":
            return client.get_file(args.remote, args.local)
        if args.command == "file-put":
            return upload_with_refresh(client, args.local, args.remote, replace=args.replace,
                                       refresh_apps=args.refresh_apps)
        if args.command == "read":
            data = client.read_memory(args.address, args.count)
            result = {"address": args.address, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            if args.output:
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_bytes(data)
                result["output"] = str(args.output.resolve())
            else:
                result["hex"] = data.hex()
            return result
        if args.command == "write":
            data = args.input.read_bytes() if args.input else bytes.fromhex(args.hex_data)
            client.write_memory(args.address, data)
            return {"address": args.address, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    raise ValueError("Unknown command")


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        result = {"ok": True, "command": args.command, "result": execute(args)}
        code = 0
    except Exception as exc:
        result = {"ok": False, "command": args.command, "error_type": type(exc).__name__, "error": str(exc),
                  "outcome_unknown": isinstance(exc, (ProtocolError, TimeoutError, OSError)) or
                                     isinstance(exc, FileError) and (exc.result == -714 or
                                         exc.opcode == APP_REFRESH and exc.result in (-703, -709))}
        if args.command == "refresh-apps" and result["outcome_unknown"]:
            result.update(partial_refresh_possible=True, refresh_warning=REFRESH_PARTIAL_WARNING)
        if isinstance(exc, FileError):
            result.update(file_result=exc.result, file_job_id=exc.job_id,
                          recovery_required=exc.result == -714, recovery_data_hex=exc.payload.hex())
        code = 1
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.report:
        try:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(rendered + "\n", encoding="utf-8")
        except OSError as exc:
            print(f"Could not save JSON report: {exc}", file=sys.stderr)
            return 1
    return code


if __name__ == "__main__":
    raise SystemExit(main())
