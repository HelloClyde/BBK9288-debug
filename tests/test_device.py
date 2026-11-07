"""Compile the portable C core and test it with mocked platform callbacks.

These checks run the production parser, dispatcher and host client. USB/MMIO,
OS task lifetime, display hardware and physical input remain unverified.
"""

from __future__ import annotations

import ctypes as C
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import unittest
import zlib

from compiler_paths import native_gcc

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("debug9288_host_for_core", ROOT / "host" / "client.py")
host = importlib.util.module_from_spec(spec)
spec.loader.exec_module(host)
U32 = C.c_uint
U8 = C.c_ubyte


class Screen(C.Structure):
    _fields_ = [(name, U32) for name in ("width", "height", "bpp", "stride", "format", "flags")]


Uptime = C.CFUNCTYPE(U32, C.c_void_p)
Read = C.CFUNCTYPE(C.c_int, C.c_void_p, U32, C.POINTER(U8), U32)
Key = C.CFUNCTYPE(C.c_int, C.c_void_p, U32)
Capture = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(U8), U32, C.POINTER(Screen))
FileRPC = C.CFUNCTYPE(C.c_int,C.c_void_p,U32,C.POINTER(U8),U32,C.POINTER(U8),C.POINTER(U32))
FileReset = C.CFUNCTYPE(None,C.c_void_p)


class Platform(C.Structure):
    _fields_ = [("context", C.c_void_p), ("image_id", U32), ("caps", U32),
                ("flags", U32), ("scratch_address", U32), ("snapshot", C.POINTER(U8)),
                ("snapshot_capacity", U32), ("uptime_ms", Uptime), ("read", Read),
                ("key", Key), ("capture", Capture),("file_rpc",FileRPC),("file_reset",FileReset)]


class State(C.Structure):
    _fields_ = [("platform", Platform), ("rx", U8 * 528), ("tx", U8 * 528),
                ("scratch", U8 * 512)] + [(name, U32) for name in (
                    "rx_used", "rx_expected", "discard_remaining", "tx_length", "image_id", "caps",
                    "requests", "errors", "token", "next_token", "snapshot_bytes", "session", "ready",
                    "stop_pending", "stop_requested")] + [("screen", Screen)]


class Core:
    def __init__(self, library):
        self.lib = library
        self.state = State()
        self.snapshot = (U8 * 76800)()
        self.keys = []
        self.reads = []
        self.raw = bytes(i & 255 for i in range(9600))
        self.capture_error = 0
        self.capture_bad_metadata = False
        self.key_error = 0
        self.callbacks = (Uptime(lambda context: 1234), Read(self.read), Key(self.key), Capture(self.capture))
        self.platform = Platform(None, 0x1234ABCD, 0x3F, 1, 0x02010000, self.snapshot, 76800, *self.callbacks)
        self.lib.dbg_init(C.byref(self.state), C.byref(self.platform))
        self.sequence = 0
        self.serial_buffer = bytearray()
        self.closed = False

    def read(self, context, address, output, count):
        if address < 0x02000000 or address + count > 0x02800000:
            return -1
        self.reads.append((address, count))
        C.memmove(output, bytes((address + i) & 255 for i in range(count)), count)
        return 0

    def key(self, context, scan):
        self.keys.append(scan)
        return self.key_error

    def capture(self, context, output, capacity, meta):
        if self.capture_error:
            return self.capture_error
        C.memmove(output, self.raw, len(self.raw))
        meta[0] = Screen(160, 240, 2, 40, 2, 0)
        if self.capture_bad_metadata:
            meta[0].stride = 1
        return len(self.raw)

    def feed(self, data):
        data = bytes(data)
        array = (U8 * len(data)).from_buffer_copy(data)
        return self.lib.dbg_feed(C.byref(self.state), array, len(data))

    def reply(self, consume=True):
        length = C.c_uint()
        pointer = self.lib.dbg_reply(C.byref(self.state), C.byref(length))
        if not pointer:
            return None
        result = C.string_at(pointer, length.value)
        if consume:
            self.lib.dbg_consume_reply(C.byref(self.state))
        return result

    def rpc(self, op, payload=b"", flags=0):
        self.sequence = (self.sequence + 1) & 65535
        packet = host.frame(op, self.sequence, payload, flags)
        assert self.feed(packet) == len(packet)
        result = self.reply()
        assert result is not None
        head = host.HEADER.unpack(result[:12])
        assert head[:4] == (host.MAGIC, 1, op | 128, self.sequence)
        assert len(result) == head[4] + 16
        assert zlib.crc32(result[:-4]) == struct.unpack("<I", result[-4:])[0]
        return head[5], result[12:-4]

    def ping(self):
        assert self.rpc(1) == (0, host.IDENTITY)

    def reset_input_buffer(self):
        self.serial_buffer.clear()
        self.lib.dbg_reset_session(C.byref(self.state))

    def close(self):
        self.closed = True

    def write(self, packet):
        consumed = self.feed(packet)
        reply = self.reply()
        if reply:
            self.serial_buffer.extend(reply)
        return consumed

    def read_serial(self, count):
        count = min(count, 17, len(self.serial_buffer))
        result = bytes(self.serial_buffer[:count])
        del self.serial_buffer[:count]
        return result


class CoreSerial:
    def __init__(self, core):
        self.core = core

    def reset_input_buffer(self):
        self.core.reset_input_buffer()

    def close(self):
        self.core.close()

    def write(self, packet):
        return self.core.write(packet)

    def read(self, count):
        return self.core.read_serial(count)


class DeviceCoreTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = Path(native_gcc())
        if not compiler.is_file():
            discovered = shutil.which("gcc")
            if not discovered:
                raise unittest.SkipTest("A native C compiler is required for production-core tests")
            compiler = Path(discovered)
        build = ROOT / "build" / "tests"
        build.mkdir(parents=True, exist_ok=True)
        output = build / (f"debug-core-{os.getpid()}.dll" if os.name == "nt" else f"debug-core-{os.getpid()}.so")
        command = [str(compiler), "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2", "-ffreestanding",
                   "-fno-builtin", "-shared", "-o", str(output), str(ROOT / "device" / "debug_core.c")]
        if os.name != "nt":
            command.insert(1, "-fPIC")
        environment = dict(os.environ)
        environment["PATH"] = str(compiler.parent) + os.pathsep + environment.get("PATH", "")
        compiled = subprocess.run(command, capture_output=True, text=True, env=environment)
        if compiled.returncode:
            raise RuntimeError(f"C core did not compile:\n{compiled.stdout}\n{compiled.stderr}")
        cls.lib = C.CDLL(str(output))
        cls.lib.dbg_init.argtypes = [C.POINTER(State), C.POINTER(Platform)]
        cls.lib.dbg_init.restype = None
        cls.lib.dbg_feed.argtypes = [C.POINTER(State), C.POINTER(U8), C.c_uint]
        cls.lib.dbg_feed.restype = C.c_uint
        cls.lib.dbg_reply.argtypes = [C.POINTER(State), C.POINTER(C.c_uint)]
        cls.lib.dbg_reply.restype = C.POINTER(U8)
        cls.lib.dbg_consume_reply.argtypes = [C.POINTER(State)]
        cls.lib.dbg_consume_reply.restype = None
        cls.lib.dbg_reset_session.argtypes = [C.POINTER(State)]
        cls.lib.dbg_reset_session.restype = None

    def setUp(self):
        self.core = Core(self.lib)

    def test_unhandshaken_requests_have_no_side_effects(self):
        for op, body in ((2, b""), (4, struct.pack("<I", 28)), (8, b""),
                         (9, struct.pack("<I", 0x02010000) + b"bad")):
            self.assertEqual(self.core.rpc(op, body), (4, b""))
        self.assertFalse(self.core.state.session)
        self.assertEqual(self.core.keys, [])
        self.assertFalse(self.core.state.stop_requested)
        self.assertEqual(bytes(self.core.state.scratch), bytes(512))

    def test_ping_status_and_callbacks(self):
        c = self.core
        c.ping()
        status, body = c.rpc(2)
        self.assertEqual(status, 0)
        fields = dict(zip(host.STATUS_FIELDS, struct.unpack("<8I", body)))
        self.assertEqual(fields["image_id"], 0x1234ABCD)
        self.assertEqual(fields["caps"], 0x3F)
        self.assertEqual(fields["uptime_ms"], 1234)
        self.assertEqual(fields["flags"], 3)
        self.assertEqual(c.rpc(4, struct.pack("<I", 28)), (0, b""))
        self.assertEqual(c.keys, [28])

    def test_bad_crc_drops_without_execution(self):
        c = self.core
        c.ping()
        packet = bytearray(host.frame(4, 2, struct.pack("<I", 28)))
        packet[-1] ^= 0x80
        self.assertEqual(c.feed(packet), len(packet))
        self.assertIsNone(c.reply())
        self.assertEqual(c.keys, [])
        self.assertEqual(c.state.errors, 1)
        self.assertEqual(c.state.requests, 1)

    def test_overlong_body_is_discarded_without_embedded_commands(self):
        c = self.core
        embedded = host.frame(1, 4)
        body = embedded + bytes(513 - len(embedded))
        packet = host.HEADER.pack(host.MAGIC, 1, 1, 3, 513, 0) + body + bytes(4)
        self.assertEqual(c.feed(packet[:25]), 25)
        self.assertEqual(c.feed(packet[25:]), len(packet) - 25)
        self.assertIsNone(c.reply())
        self.assertFalse(c.state.session)
        self.assertEqual(c.state.errors, 1)
        c.ping()

    def test_split_frames_and_overlapping_magic(self):
        c = self.core
        packet = b"garbage888" + host.frame(1, 7)
        for byte in packet:
            self.assertEqual(c.feed(bytes([byte])), 1)
        response = c.reply()
        self.assertIsNotNone(response)
        self.assertEqual(response[12:-4], host.IDENTITY)
        self.assertTrue(c.state.session)

    def test_flags_and_wrong_lengths_are_rejected(self):
        c = self.core
        self.assertEqual(c.rpc(1, b"bad"), (1, b""))
        self.assertFalse(c.state.session)
        self.assertEqual(c.rpc(1, flags=1), (1, b""))
        self.assertFalse(c.state.session)
        c.ping()
        for op, data in ((2, b"x"), (3, b""), (4, b""), (5, b"x"), (6, b""),
                         (7, b""), (8, b"x"), (9, b"abcd"), (10, b"x"), (127, b"")):
            with self.subTest(op=op):
                self.assertEqual(c.rpc(op, data), (1, b""))
        self.assertEqual(c.keys, [])

    def test_only_one_request_outstanding(self):
        c = self.core
        first, second = host.frame(1, 1), host.frame(2, 2)
        self.assertEqual(c.feed(first + second), len(first))
        self.assertEqual(c.feed(second), 0)
        self.assertEqual(c.reply(False), c.reply(False))
        self.assertEqual(c.state.requests, 1)
        c.reply()
        self.assertEqual(c.feed(second), len(second))
        self.assertEqual(c.state.requests, 2)

    def test_read_bounds_and_maximum_payload(self):
        c = self.core
        c.ping()
        result = c.rpc(3, struct.pack("<II", 0x02000000, 512))
        self.assertEqual(result, (0, bytes(range(256)) * 2))
        for address, length in ((0x02000000, 0), (0x02000000, 513), (0xFFFFFFFF, 2), (0x00300100, 1)):
            with self.subTest(address=address, length=length):
                self.assertNotEqual(c.rpc(3, struct.pack("<II", address, length))[0], 0)
        self.assertEqual(len(c.reads), 1)

    def test_scratch_write_bounds_and_readback(self):
        c = self.core
        c.ping()
        self.assertEqual(c.rpc(10), (0, struct.pack("<II", 0x02010000, 512)))
        payload = bytes(range(256)) + bytes(range(252))
        self.assertEqual(c.rpc(9, struct.pack("<I", 0x02010000) + payload), (0, b""))
        self.assertEqual(c.rpc(9, struct.pack("<I", 0x020101FC) + b"tail"), (0, b""))
        expected = payload + b"tail"
        self.assertEqual(c.rpc(3, struct.pack("<II", 0x02010000, 512)), (0, expected))
        for address, data in ((0x0200FFFF, b"x"), (0x02010200, b"x"), (0x020101FF, b"xx"),
                              (0xFFFFFFFF, b"xx"), (0x02010000, b"")):
            with self.subTest(address=address):
                self.assertEqual(c.rpc(9, struct.pack("<I", address) + data), (1, b""))
        self.assertEqual(bytes(c.state.scratch), expected)
        self.assertEqual(c.reads, [])

    def test_snapshot_token_bounds_and_frozen_data(self):
        c = self.core
        c.ping()
        status, body = c.rpc(5)
        self.assertEqual(status, 0)
        meta = dict(zip(host.SCREEN_FIELDS, struct.unpack("<8I", body)))
        token = meta["token"]
        self.assertEqual(c.rpc(5), (2, b""))
        c.raw = bytes(9600)
        self.assertEqual(c.rpc(6, struct.pack("<III", token, 0, 512)), (0, bytes(range(256)) * 2))
        for bad_token, offset, length in ((token + 1, 0, 1), (token, 9600, 1),
                                          (token, 9599, 2), (token, 0, 513), (token, 0, 0),
                                          (token, 0xFFFFFFFF, 1)):
            self.assertEqual(c.rpc(6, struct.pack("<III", bad_token, offset, length)), (1, b""))
        self.assertEqual(c.rpc(7, struct.pack("<I", token + 1)), (1, b""))
        self.assertEqual(c.rpc(7, struct.pack("<I", token)), (0, b""))
        self.assertEqual(c.rpc(6, struct.pack("<III", token, 0, 1)), (1, b""))
        self.assertFalse(c.state.snapshot_bytes)

    def test_capture_bad_metadata_and_callback_errors(self):
        c = self.core
        c.ping()
        c.capture_bad_metadata = True
        self.assertEqual(c.rpc(5), (3, b""))
        self.assertFalse(c.state.token)
        c.capture_error = -2
        self.assertEqual(c.rpc(5), (2, b""))
        c.key_error = -2
        self.assertEqual(c.rpc(4, struct.pack("<I", 28)), (2, b""))

    def test_stop_deferred_until_reply_consumed(self):
        c = self.core
        c.ping()
        packet = host.frame(8, 2)
        self.assertEqual(c.feed(packet), len(packet))
        self.assertTrue(c.state.stop_pending)
        self.assertFalse(c.state.stop_requested)
        self.assertIsNotNone(c.reply(False))
        self.assertFalse(c.state.stop_requested)
        c.reply()
        self.assertTrue(c.state.stop_requested)
        self.assertFalse(c.state.stop_pending)
        self.assertEqual(c.feed(host.frame(1, 3)), 0)

    def test_reset_invalidates_snapshot_authorization_and_partial_frames(self):
        c = self.core
        c.ping()
        old_token = struct.unpack("<8I", c.rpc(5)[1])[0]
        c.feed(host.frame(4, 9, struct.pack("<I", 28))[:14])
        self.lib.dbg_reset_session(C.byref(c.state))
        self.assertFalse(c.state.token)
        self.assertFalse(c.state.rx_used)
        self.assertFalse(c.state.ready)
        self.assertEqual(c.rpc(4, struct.pack("<I", 28)), (4, b""))
        self.assertEqual(c.keys, [])
        c.ping()
        new_token = struct.unpack("<8I", c.rpc(5)[1])[0]
        self.assertNotEqual(old_token, new_token)
        self.assertEqual(c.rpc(6, struct.pack("<III", old_token, 0, 1)), (1, b""))

    def test_python_client_against_production_c_core(self):
        c = self.core
        with host.Client(transport=CoreSerial(c), timeout=0.5) as connection:
            self.assertEqual(connection.status()["caps"], 0x3F)
            self.assertEqual(connection.read_memory(0x02000000, 1100), bytes(i & 255 for i in range(1100)))
            scratch = connection.scratch()
            connection.write_memory(scratch["address"], b"host-to-production-core")
            self.assertEqual(connection.read_memory(scratch["address"], 23), b"host-to-production-core")
            connection.press(28)
            meta, data = connection.capture_screen()
            self.assertEqual((meta["width"], meta["height"]), (160, 240))
            self.assertEqual(data, c.raw)
            self.assertFalse(c.state.token)
            connection.stop()
            self.assertTrue(c.state.stop_requested)
        self.assertEqual(c.keys, [28])

    def install_files(self, callback=None, reset=None):
        c = self.core
        c.file_calls = []
        c.file_resets = []
        def routed(context, op, body, size, output, output_size):
            c.file_calls.append((op, C.string_at(body, size)))
            if callback:
                return callback(output, output_size)
            C.memmove(output, b"file-reply", 10)
            output_size[0] = 10
            return 0
        c.file_callbacks = (FileRPC(routed), FileReset(reset or (lambda context: c.file_resets.append(True))))
        c.platform.caps |= 64
        c.platform.file_rpc, c.platform.file_reset = c.file_callbacks
        self.lib.dbg_init(C.byref(c.state), C.byref(c.platform))
        return c

    def test_files_need_both_callbacks_and_handshake(self):
        c = self.core
        c.platform.caps |= 64
        self.lib.dbg_init(C.byref(c.state), C.byref(c.platform))
        self.assertFalse(c.state.caps & 64)
        c = self.install_files()
        self.assertTrue(c.state.caps & 64)
        self.assertEqual(c.rpc(15, b"payload"), (4, b""))
        self.assertFalse(c.file_calls)
        c.ping()
        for op in range(11, 21):
            self.assertEqual(c.rpc(op, b"payload"), (0, b"file-reply"))
        self.assertEqual(c.file_calls, [(op, b"payload") for op in range(11, 21)])
        self.assertEqual(c.rpc(21, b"payload"), (1, b""))

    def test_file_callback_status_and_output_bound(self):
        for result, status in ((-1, 1), (-2, 2), (-3, 3)):
            c = self.install_files(lambda output, size, result=result: result)
            c.ping()
            self.assertEqual(c.rpc(19, b"1234"), (status, b""))
        def oversized(output, size):
            size[0] = 513
            return 0
        c = self.install_files(oversized)
        c.ping()
        self.assertEqual(c.rpc(19, b"1234"), (3, b""))

    def test_transport_reset_signals_file_cleanup(self):
        c = self.install_files()
        c.ping()
        self.assertFalse(c.file_resets)
        self.lib.dbg_reset_session(C.byref(c.state))
        self.assertEqual(c.file_resets, [True])
        self.assertEqual(c.rpc(19, b"1234"), (4, b""))
        self.assertFalse(c.file_calls)


if __name__ == "__main__":
    unittest.main()
