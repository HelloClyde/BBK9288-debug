"""Offline transport and protocol checks; these do not validate physical USB."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import threading
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "host" / "client.py"
spec = importlib.util.spec_from_file_location("debug9288_client", SOURCE)
client = importlib.util.module_from_spec(spec)
spec.loader.exec_module(client)


class Transport:
    def __init__(self, handler=None, *, identity=client.IDENTITY, fragment=17):
        self.handler = handler or (lambda op, body: (0, b""))
        self.identity, self.fragment = identity, fragment
        self.incoming = bytearray()
        self.writes = []
        self.closed = False
        self.transform = None
        self.short_write = False

    def reset_input_buffer(self):
        self.incoming.clear()

    def close(self):
        self.closed = True

    def write(self, packet):
        self.writes.append(bytes(packet))
        if self.short_write:
            return len(packet) - 1
        magic, version, op, sequence, length, flags = client.HEADER.unpack(packet[:12])
        assert (magic, version, flags) == (client.MAGIC, 1, 0)
        assert length == len(packet) - 16
        body = packet[12:-4]
        status, result = (0, self.identity) if op == 1 else self.handler(op, body)
        reply = client.frame(op | 128, sequence, result, status)
        if self.transform:
            reply = self.transform(reply)
        self.incoming.extend(reply)
        return len(packet)

    def read(self, count):
        count = min(count, self.fragment, len(self.incoming))
        result = bytes(self.incoming[:count])
        del self.incoming[:count]
        return result

    def operations(self):
        return [p[5] for p in self.writes]


class HostTests(unittest.TestCase):
    def connect(self, handler=None, **kwargs):
        transport = Transport(handler, **kwargs)
        connection = client.Client(transport=transport, timeout=0.1)
        self.addCleanup(connection.close)
        return connection, transport

    def test_frame_crc_and_maximum_payload(self):
        import zlib
        packet = client.frame(3, 65535, b"a" * 512)
        self.assertEqual(len(packet), 528)
        self.assertEqual(client.U32.unpack(packet[-4:])[0], zlib.crc32(packet[:-4]))
        with self.assertRaises(ValueError):
            client.frame(3, 1, b"a" * 513)

    def test_fragmented_reply(self):
        c, _ = self.connect(fragment=1)
        self.assertEqual(c.ping(), client.IDENTITY.decode())

    def test_identity_mismatch_closes(self):
        t = Transport(identity=b"H1DEBUG/1 V1.41 RESIDENT")
        with self.assertRaisesRegex(client.ProtocolError, "identity mismatch"):
            client.Client(transport=t, timeout=0.01)
        self.assertTrue(t.closed)

    def test_crc_mismatch_breaks_without_replay(self):
        c, t = self.connect()
        t.transform = lambda packet: packet[:-1] + bytes([packet[-1] ^ 1])
        with self.assertRaisesRegex(client.ProtocolError, "CRC32"):
            c.press(28)
        before = len(t.writes)
        with self.assertRaisesRegex(client.ProtocolError, "desynchronized"):
            c.press(28)
        self.assertEqual(len(t.writes), before)
        self.assertTrue(c.broken)

    def test_wrong_sequence_breaks(self):
        c, t = self.connect()
        def wrong_sequence(packet):
            fields = client.HEADER.unpack(packet[:12])
            return client.frame(fields[2], (fields[3] + 1) & 65535, packet[12:-4], fields[5])
        t.transform = wrong_sequence
        with self.assertRaisesRegex(client.ProtocolError, "sequence"):
            c.status()
        self.assertTrue(c.broken)

    def test_wrong_opcode_breaks(self):
        c, t = self.connect()
        def wrong_opcode(packet):
            fields = client.HEADER.unpack(packet[:12])
            return client.frame(0x83, fields[3], packet[12:-4], fields[5])
        t.transform = wrong_opcode
        with self.assertRaises(client.ProtocolError):
            c.status()
        self.assertTrue(c.broken)

    def test_short_read_breaks(self):
        c, t = self.connect()
        t.transform = lambda packet: packet[:-1]
        with self.assertRaisesRegex(TimeoutError, "outcome may be unknown"):
            c.press(28)
        before = len(t.writes)
        with self.assertRaises(client.ProtocolError):
            c.status()
        self.assertEqual(len(t.writes), before)

    def test_short_write_breaks(self):
        c, t = self.connect()
        t.short_write = True
        with self.assertRaisesRegex(client.ProtocolError, "Incomplete write"):
            c.press(28)
        self.assertTrue(c.broken)

    def test_oversized_remote_payload_rejected(self):
        c, t = self.connect()
        def oversized(packet):
            fields = list(client.HEADER.unpack(packet[:12]))
            fields[4] = 513
            return client.HEADER.pack(*fields)
        t.transform = oversized
        with self.assertRaises(client.ProtocolError):
            c.status()
        self.assertTrue(c.broken)

    def test_oversized_local_payload_not_sent(self):
        c, t = self.connect()
        before = len(t.writes)
        with self.assertRaises(ValueError):
            c.rpc(9, b"a" * 513)
        self.assertEqual(len(t.writes), before)
        self.assertFalse(c.broken)

    def test_device_rejection_keeps_valid_session(self):
        c, _ = self.connect(lambda op, body: (2, b"busy"))
        with self.assertRaises(client.DeviceError) as result:
            c.press(28)
        self.assertEqual(result.exception.status, 2)
        self.assertFalse(c.broken)
        self.assertEqual(c.ping(), client.IDENTITY.decode())

    def test_exact_status_size(self):
        c, _ = self.connect(lambda op, body: (0, b"x" * 31))
        with self.assertRaisesRegex(client.ProtocolError, "size mismatch"):
            c.status()
        self.assertTrue(c.broken)

    def test_read_chunk_boundaries(self):
        requests = []
        def handler(op, body):
            address, count = struct.unpack("<II", body)
            requests.append((address, count))
            return 0, bytes((address + i) & 255 for i in range(count))
        c, _ = self.connect(handler)
        result = c.read_memory(0x02001000, 1100)
        self.assertEqual([n for _, n in requests], [512, 512, 76])
        self.assertEqual([a for a, _ in requests], [0x02001000, 0x02001200, 0x02001400])
        self.assertEqual(result, bytes(i & 255 for i in range(1100)))

    def test_read_short_payload_is_protocol_failure(self):
        c, _ = self.connect(lambda op, body: (0, b"x"))
        with self.assertRaisesRegex(client.ProtocolError, "READ reply size"):
            c.read_memory(0x02000000, 512)
        self.assertTrue(c.broken)

    def test_scratch_writes_are_bounded_and_chunked(self):
        writes = []
        def handler(op, body):
            if op == 10:
                return 0, struct.pack("<II", 0x02010000, 1024)
            self.assertEqual(op, 9)
            writes.append(body)
            return 0, b""
        c, t = self.connect(handler)
        c.write_memory(0x02010000, b"a" * 1024)
        self.assertEqual([len(b) - 4 for b in writes], [508, 508, 8])
        before = len(t.writes)
        with self.assertRaisesRegex(ValueError, "scratch"):
            c.write_memory(0x0200FFFF, b"a")
        self.assertEqual(before, len(t.writes))

    def screen_transport(self, fail=None):
        raw = bytes(range(256)) * 3
        meta = (7, 32, 24, 8, 32, len(raw), 1, 0)
        def handler(op, body):
            if op == 5:
                return 0, struct.pack("<8I", *meta)
            if op == 6:
                token, offset, size = struct.unpack("<III", body)
                self.assertEqual(token, 7)
                if fail:
                    return fail(offset, size)
                return 0, raw[offset:offset + size]
            if op == 7:
                self.assertEqual(client.U32.unpack(body)[0], 7)
                return 0, b""
            self.fail(f"Unexpected operation {op}")
        return self.connect(handler), raw

    def test_frozen_screen_token_reads_and_releases(self):
        (c, t), raw = self.screen_transport()
        meta, result = c.capture_screen()
        self.assertEqual(meta["size"], 768)
        self.assertEqual(result, raw)
        self.assertEqual(t.operations(), [1, 5, 6, 6, 7])

    def test_screen_device_error_releases(self):
        (c, t), _ = self.screen_transport(lambda offset, size: (2, b"busy"))
        with self.assertRaises(client.DeviceError):
            c.capture_screen()
        self.assertEqual(t.operations(), [1, 5, 6, 7])

    def test_screen_invalid_chunk_does_not_send_after_desync(self):
        (c, t), _ = self.screen_transport(lambda offset, size: (0, b"a"))
        with self.assertRaises(client.ProtocolError):
            c.capture_screen()
        self.assertEqual(t.operations(), [1, 5, 6])
        self.assertTrue(c.broken)

    def test_packed_gray2_highbits_and_row_padding(self):
        meta = dict(zip(client.SCREEN_FIELDS, (1, 5, 2, 2, 3, 6, 2, 0)))
        self.assertEqual(client.screen_pixels(meta, bytes([0x1B, 0x40, 0xFF, 0xE4, 0x80, 0xFF])),
                         bytes([0, 85, 170, 255, 85, 255, 170, 85, 0, 170]))

    def test_screen_metadata_limits(self):
        meta = dict(zip(client.SCREEN_FIELDS, (1, 160, 240, 2, 40, 9600, 2, 0)))
        client.validate_screen(meta)
        for key, value in (("bpp", 8), ("size", 9601), ("width", 0), ("token", 0), ("stride", 39)):
            with self.subTest(key=key), self.assertRaises(client.ProtocolError):
                client.validate_screen({**meta, key: value})

    def test_sequence_wrap(self):
        c, t = self.connect()
        c.sequence = 65535
        c.ping()
        self.assertEqual(client.HEADER.unpack(t.writes[-1][:12])[3], 0)

    def test_multiple_threads_serialize_rpc(self):
        c, t = self.connect()
        errors = []
        def ping():
            try:
                for _ in range(20):
                    c.ping()
            except Exception as exc:
                errors.append(exc)
        threads = [threading.Thread(target=ping) for _ in range(3)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(2)
            self.assertFalse(thread.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(len(t.writes), 61)

    def test_help_without_optional_dependencies(self):
        process = subprocess.run([sys.executable, "-S", str(SOURCE), "--help"], capture_output=True, text=True)
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn("screenshot", process.stdout)


if __name__ == "__main__":
    unittest.main()
