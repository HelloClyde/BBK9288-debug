"""Offline v1 file jobs: exact wire layouts, cleanup, cancellation and no replay."""
import os
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

from host import client
from test_host import Transport


def pack(*values):
    return struct.pack("<" + "I" * len(values), *values)


class Files:
    def __init__(self):
        self.files = {"A:\\old.bin": b"original", "A:\\read.bin": bytes(range(251)) * 5}
        self.transaction = None
        self.next_job = 0
        self.current = None
        self.calls = []
        self.cap = client.CAP_FILES
        self.bad = None
        self.event = None
        self.cancel_at = None
        self.late_commit = False
        self.finish_error = 0
        self.finished = []

    def path(self, body, offset=0):
        size, = struct.unpack_from("<I", body, offset)
        assert len(body) == offset + 4 + size
        return body[offset + 4:].decode("gbk")

    def handler(self, op, body):
        self.calls.append((op, bytes(body)))
        if op == 2:
            return 0, pack(1, self.cap, 0, 0, 0, 0, 0, 0)
        if op == 20:
            job, = struct.unpack("<I", body)
            assert job == self.current[0]
            self.current[4] = True
            return 0, b""
        if op == 19:
            job, = struct.unpack("<I", body)
            assert job == self.current[0]
            self.current[3] += 1
            _, command, request, count, cancelled = self.current
            if count == 1:
                if self.cancel_at == command:
                    self.event.set()
                return 0, pack(job, 1, 0, 0)
            if cancelled and not (command == 17 and self.late_commit):
                self.transaction = None
                result, data = -710, b""
            else:
                result, data = self.command(command, request)
            self.finished.append((job, command, result))
            raw = struct.pack("<IIiI", job, 3, result, len(data)) + data
            if self.bad == "poll-length":
                raw = raw[:12] + pack(497) + raw[16:]
            return 0, raw
        assert 11 <= op <= 18
        self.next_job += 1
        self.current = [self.next_job, op, body, 0, False]
        return 0, struct.pack("<Ii", self.next_job, 0)

    def command(self, op, body):
        token = 73
        if op in (11, 13):
            path = self.path(body)
            if op == 13 and path not in self.files:
                return -704, b""
            self.transaction = {"kind": op, "path": path, "offset": 0, "crc": 0}
            return 0, pack(token) if op == 11 else pack(token, len(self.files[path]))
        if op == 12:
            tok, index = struct.unpack("<II", body)
            assert tok == token
            if index:
                return 0, pack(token, index, 1, 0, 0, 0)
            name = (b"\x81\x5c".decode("gbk") + ".bin").encode("gbk")
            return 0, pack(token, index, 0, 0, 17, len(name)) + name
        if op == 15:
            size, crc, flags = struct.unpack_from("<III", body)
            path = self.path(body, 12)
            if path in self.files and not flags:
                return -705, b""
            self.transaction = {"kind": op, "path": path, "size": size, "crc_expected": crc,
                                "offset": 0, "crc": 0, "data": bytearray()}
            return 0, pack(token, 0)
        if op == 18:
            assert struct.unpack("<I", body)[0] in (0, token)
            self.transaction = None
            return 0, b""
        txn = self.transaction
        assert txn is not None
        if op == 14:
            tok, offset, wanted = struct.unpack("<III", body)
            assert tok == token and offset == txn["offset"] and 0 < wanted <= 480
            data = self.files[txn["path"]][offset:offset + wanted]
            txn["offset"] += len(data)
            txn["crc"] = zlib.crc32(data, txn["crc"]) & 0xffffffff
            if self.bad == "get-short":
                data = data[:-1]
            crc = zlib.crc32(data) & 0xffffffff
            return 0, pack(token, offset + int(self.bad == "get-offset"), crc ^ int(self.bad == "get-crc")) + data
        if op == 16:
            tok, offset, crc = struct.unpack_from("<III", body)
            data = body[12:]
            assert tok == token and offset == txn["offset"] and 0 < len(data) <= 480
            assert crc == zlib.crc32(data) & 0xffffffff
            txn["data"].extend(data)
            txn["offset"] += len(data)
            txn["crc"] = zlib.crc32(data, txn["crc"]) & 0xffffffff
            return 0, pack(token, txn["offset"] + int(self.bad == "put-offset"),
                           txn["crc"] ^ int(self.bad == "put-crc"))
        assert op == 17 and body == pack(token)
        if self.finish_error:
            return self.finish_error, b"A:\\OWNED.BAK\x00" if self.finish_error == -714 else b""
        self.transaction = None
        if txn["kind"] == 11:
            return 0, b""
        if txn["kind"] == 15:
            assert txn["offset"] == txn["size"] and txn["crc"] == txn["crc_expected"]
            self.files[txn["path"]] = bytes(txn["data"])
        return 0, pack(token, txn["offset"], txn["crc"] ^ int(self.bad == "final-crc"))


class HostFilesTests(unittest.TestCase):
    def connect(self):
        peer = Files()
        transport = Transport(peer.handler, fragment=17)
        c = client.Client(transport=transport, timeout=0.1)
        self.addCleanup(c.close)
        sleep = patch.object(client.time, "sleep", return_value=None)
        sleep.start()
        self.addCleanup(sleep.stop)
        return c, peer, transport

    def test_get_stream_verified_then_local_replace(self):
        c, peer, _ = self.connect()
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "out.bin"
            target.write_bytes(b"old local")
            progress = []
            result = c.get_file("A:/read.bin", target, progress=lambda *p: progress.append(p))
            self.assertEqual(target.read_bytes(), peer.files["A:\\read.bin"])
            self.assertEqual(result["size"], 1255)
            self.assertEqual(progress[-1], (1255, 1255))
            self.assertEqual(list(Path(directory).glob("*.part-*")), [])
        self.assertEqual([len(b) for op, b in peer.calls if op == 14], [12, 12, 12])
        self.assertFalse(c.broken)

    def test_put_once_and_explicit_replace(self):
        c, peer, _ = self.connect()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "in.bin"
            source.write_bytes(bytes(range(251)) * 5)
            result = c.put_file(source, "A:\\new.bin")
            self.assertTrue(result["committed"])
            self.assertEqual(peer.files["A:\\new.bin"], source.read_bytes())
            writes = [b for op, b in peer.calls if op == 16]
            self.assertEqual([len(b) - 12 for b in writes], [480, 480, 295])
            self.assertEqual([struct.unpack_from("<I", b, 4)[0] for b in writes], [0, 480, 960])
            with self.assertRaises(client.FileError) as error:
                c.put_file(source, "A:\\old.bin")
            self.assertEqual(error.exception.result, -705)
            self.assertEqual(peer.files["A:\\old.bin"], b"original")
            self.assertFalse(c.broken)
            c.put_file(source, "A:\\old.bin", replace=True)
            self.assertEqual(peer.files["A:\\old.bin"], source.read_bytes())
            self.assertEqual(sum(op == 15 for op, _ in peer.calls), 3)

    def test_empty_file_and_listing_dbcs_trail_backslash(self):
        c, peer, _ = self.connect()
        entries = c.list_files("A:\\")
        self.assertEqual(entries[0]["name"].encode("gbk"), b"\x81\x5c.bin")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "empty.bin"
            source.write_bytes(b"")
            c.put_file(source, "A:\\empty.bin")
            c.get_file("A:\\empty.bin", source)
            self.assertEqual(source.read_bytes(), b"")
        self.assertFalse(any(op in (14, 16) for op, _ in peer.calls))

    def test_path_validation_and_scan_limit(self):
        self.assertEqual(client.remote_path("a:/中文/文件.bin")[0], "A:\\中文\\文件.bin")
        for path in ("B:\\x", "relative", "A:\\..\\x", "A:\\x\\.\\a", "A:\\\\x",
                     "A:\\bad\x00name", "A:\\bad*", "A:\\bad.", "A:\\😀", "A:\\" + "中" * 120):
            with self.subTest(path=path), self.assertRaises(ValueError):
                client.remote_path(path)
        c, _, t = self.connect()
        before = len(t.writes)
        with self.assertRaises(ValueError):
            c.press(128)
        with self.assertRaises(ValueError):
            client.scan_code("255")
        self.assertEqual(client.scan_code("127"), 127)
        self.assertEqual(len(t.writes), before)

    def test_crc_offset_short_and_poll_failures_do_not_abort_or_replay(self):
        for bad in ("get-crc", "get-offset", "get-short", "poll-length", "final-crc"):
            with self.subTest(bad=bad):
                c, peer, _ = self.connect()
                peer.bad = bad
                with tempfile.TemporaryDirectory() as directory:
                    target = Path(directory) / "old.bin"
                    target.write_bytes(b"keep")
                    with self.assertRaises(client.ProtocolError):
                        c.get_file("A:\\read.bin", target)
                    self.assertEqual(target.read_bytes(), b"keep")
                    self.assertEqual(list(Path(directory).glob("*.part-*")), [])
                self.assertTrue(c.broken)
                self.assertFalse(any(op == 18 for op, _ in peer.calls))
                self.assertEqual(sum(op == 13 for op, _ in peer.calls), 1)

    def test_bad_put_ack_breaks_once(self):
        for bad in ("put-crc", "put-offset"):
            c, peer, _ = self.connect()
            peer.bad = bad
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "in.bin"
                source.write_bytes(b"a" * 1000)
                with self.assertRaises(client.ProtocolError):
                    c.put_file(source, "A:\\new.bin")
            self.assertEqual(sum(op == 16 for op, _ in peer.calls), 1)
            self.assertFalse(any(op in (17, 18) for op, _ in peer.calls))

    def test_cancel_uses_same_pending_job_then_cleanup(self):
        c, peer, _ = self.connect()
        event = threading.Event()
        peer.event, peer.cancel_at = event, 16
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "in.bin"
            source.write_bytes(b"a" * 1000)
            with self.assertRaises(client.FileError) as error:
                c.put_file(source, "A:\\new.bin", cancel_event=event)
        self.assertEqual(error.exception.result, client.FILE_CANCELLED)
        cancel = next(i for i, (op, _) in enumerate(peer.calls) if op == 20)
        self.assertEqual(peer.calls[cancel + 1][0], 19)
        self.assertEqual(peer.calls[cancel][1], peer.calls[cancel + 1][1])
        self.assertTrue(any(op == 18 for op, _ in peer.calls))
        self.assertNotIn("A:\\new.bin", peer.files)
        self.assertFalse(c.broken)

    def test_late_cancel_does_not_misreport_committed_file(self):
        c, peer, _ = self.connect()
        peer.event, peer.cancel_at, peer.late_commit = threading.Event(), 17, True
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "in.bin"
            source.write_bytes(b"a")
            result = c.put_file(source, "A:\\new.bin", cancel_event=peer.event)
        self.assertTrue(result["committed"])
        self.assertEqual(peer.files["A:\\new.bin"], b"a")
        self.assertFalse(any(op == 18 for op, _ in peer.calls))

    def test_error_cleanup_and_recovery_data(self):
        for code in (-703, -714):
            c, peer, _ = self.connect()
            peer.finish_error = code
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "in.bin"
                source.write_bytes(b"a")
                with self.assertRaises(client.FileError) as error:
                    c.put_file(source, "A:\\new.bin")
            self.assertEqual(error.exception.result, code)
            self.assertFalse(c.broken)
            self.assertEqual(any(op == 18 for op, _ in peer.calls), code != -714)
            if code == -714:
                self.assertIn(b"OWNED.BAK", error.exception.payload)

    def test_old_service_rejected_before_file_commands(self):
        c, peer, _ = self.connect()
        peer.cap = 0
        with self.assertRaises(client.FileError) as error:
            c.list_files("A:\\")
        self.assertEqual(error.exception.result, -715)
        self.assertFalse(any(op >= 11 for op, _ in peer.calls))

    def test_submit_recovery_rejection_latches_without_payload_or_more_requests(self):
        c, peer, transport = self.connect()
        original = peer.handler
        def handler(op, body):
            if op == 13:
                return 0, struct.pack("<Ii", 0, -714)
            return original(op, body)
        transport.handler = handler
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(client.FileError) as error:
                c.get_file("A:\\read.bin", Path(directory) / "read.bin")
            self.assertEqual(error.exception.result, -714)
            self.assertEqual(error.exception.payload, b"")
            self.assertTrue(c._file_recovery)
            before = len(transport.writes)
            with self.assertRaises(client.FileError) as repeated:
                c.list_files("A:\\")
            self.assertEqual(repeated.exception.result, -714)
            self.assertEqual(len(transport.writes), before)
            self.assertFalse(c.broken)
            report = Path(directory) / "recovery.json"
            with patch.object(client, "execute", side_effect=error.exception), patch("builtins.print"):
                self.assertEqual(client.main(["--report", str(report), "file-list", "A:\\"]), 1)
            import json
            saved = json.loads(report.read_text(encoding="utf-8"))
            self.assertTrue(saved["recovery_required"] and saved["outcome_unknown"])
            self.assertEqual(saved["recovery_data_hex"], "")

    def test_interrupt_while_pending_never_overwrites_job_with_abort(self):
        c, peer, _ = self.connect()
        with patch.object(client.time, "sleep", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                c.list_files("A:\\")
        self.assertTrue(c.broken)
        self.assertEqual([op for op, _ in peer.calls if op >= 11], [11, 19])

    def test_upload_detects_local_mutation_and_aborts_before_commit(self):
        c, peer, _ = self.connect()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "in.bin"
            source.write_bytes(b"a" * 1000)
            def progress(done, total):
                if done == 0:
                    source.write_bytes(b"b" * total)
            with self.assertRaisesRegex(ValueError, "changed during upload"):
                c.put_file(source, "A:\\new.bin", progress=progress)
        self.assertNotIn("A:\\new.bin", peer.files)
        self.assertTrue(any(op == 18 for op, _ in peer.calls))
        self.assertFalse(any(op == 17 for op, _ in peer.calls))
        self.assertFalse(c.broken)

    def test_transfer_holds_connection_lock_for_whole_transaction(self):
        c, _, transport = self.connect()
        contender = []
        started, finished = threading.Event(), threading.Event()
        def ping():
            started.set()
            c.ping()
            finished.set()
        def progress(done, total):
            if not contender:
                thread = threading.Thread(target=ping, daemon=True)
                contender.append(thread)
                thread.start()
                self.assertTrue(started.wait(1))
            self.assertFalse(finished.is_set())
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "in.bin"
            source.write_bytes(b"a" * 1000)
            c.put_file(source, "A:\\new.bin", progress=progress)
        contender[0].join(1)
        self.assertTrue(finished.is_set())
        self.assertEqual(transport.operations()[-1], 1)

    def test_recovery_error_is_preserved_in_cli_report(self):
        error = client.FileError(17, -714, 99, b"A:\\OWNED.BAK\x00")
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "error.json"
            with patch.object(client, "execute", side_effect=error), patch("builtins.print"):
                self.assertEqual(client.main(["--report", str(report), "file-list", "A:\\"]), 1)
            import json
            result = json.loads(report.read_text(encoding="utf-8"))
        self.assertTrue(result["recovery_required"] and result["outcome_unknown"])
        self.assertEqual(result["file_job_id"], 99)
        self.assertEqual(bytes.fromhex(result["recovery_data_hex"]), error.payload)

    def test_finish_budget_allows_long_verification_without_extending_rpc_timeout(self):
        c, peer, transport = self.connect()
        original = peer.handler
        clock = [0.0]
        def handler(op, body):
            if op == 19 and peer.current[1] == 17 and peer.current[3] < 8:
                peer.current[3] += 1
                return 0, pack(peer.current[0], 2, 0, 0)
            return original(op, body)
        transport.handler = handler
        def sleep(_):
            if peer.current[1] == 17:
                clock[0] += 100.0
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "in.bin"
            source.write_bytes(b"")
            with patch.object(client.time, "monotonic", side_effect=lambda: clock[0]), \
                 patch.object(client.time, "sleep", side_effect=sleep):
                result = c.put_file(source, "A:\\new.bin")
        self.assertTrue(result["committed"])
        self.assertEqual(clock[0], 800.0)
        self.assertEqual(c.timeout, 0.1)
        self.assertFalse(c.broken)

    def test_nonfinish_job_retains_shorter_budget(self):
        c, peer, transport = self.connect()
        original = peer.handler
        clock = [0.0]
        def handler(op, body):
            if op == 19:
                return 0, pack(peer.current[0], 2, 0, 0)
            return original(op, body)
        transport.handler = handler
        def sleep(_):
            clock[0] += 100.0
        with patch.object(client.time, "monotonic", side_effect=lambda: clock[0]), \
             patch.object(client.time, "sleep", side_effect=sleep):
            with self.assertRaisesRegex(TimeoutError, "File job deadline"):
                c.list_files("A:\\")
        self.assertTrue(c.broken)
        self.assertEqual(c.timeout, 0.1)
        self.assertFalse(any(op == 18 for op, _ in peer.calls))

    def test_cli_file_layout(self):
        p = client.parser()
        args = p.parse_args(["file-put", "local.bin", "A:\\remote.bin", "--replace"])
        self.assertTrue(args.replace)
        self.assertEqual(args.remote, "A:\\remote.bin")
        self.assertEqual(p.parse_args(["file-get", "A:\\x", "local"]).command, "file-get")
        self.assertEqual(p.parse_args(["file-list", "A:\\"]).command, "file-list")


if __name__ == "__main__":
    unittest.main()
