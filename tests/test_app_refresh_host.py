"""Application-list refresh wire checks and committed-upload result handling."""
import contextlib
import io
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch

from host import client
from test_host import Transport
from test_host_files import Files, pack


PROGRAM = "A:\\系统\\程序\\demo.EXE"


class RefreshPeer(Files):
    def __init__(self):
        super().__init__()
        self.cap |= client.CAP_APP_REFRESH
        self.refresh_rejection = 0
        self.refresh_result = 0
        self.refresh_data = pack(1, 12, 13, 8, 9, 0)
        self.scans = 0

    def handler(self, op, body):
        if op != client.APP_REFRESH:
            return super().handler(op, body)
        self.calls.append((op, bytes(body)))
        assert body == b""
        if self.transaction is not None or self.refresh_rejection:
            return 0, struct.pack("<Ii", 0, self.refresh_rejection or -702)
        self.next_job += 1
        self.current = [self.next_job, op, body, 0, False]
        return 0, struct.pack("<Ii", self.next_job, 0)

    def command(self, op, body):
        if op != client.APP_REFRESH:
            return super().command(op, body)
        self.scans += 1
        return self.refresh_result, self.refresh_data


class AppRefreshHostTests(unittest.TestCase):
    def connect(self):
        peer = RefreshPeer()
        transport = Transport(peer.handler, fragment=7)
        connection = client.Client(transport=transport, timeout=5)
        self.addCleanup(connection.close)
        # No real scheduling or short-wall-clock threshold is part of these
        # synchronous transport tests; deadline behavior is checked separately.
        sleep = patch.object(client.time, "sleep", return_value=None)
        sleep.start()
        self.addCleanup(sleep.stop)
        return connection, peer, transport

    def test_refresh_submits_empty_request_once_and_reports_firmware_counts(self):
        connection, peer, _ = self.connect()
        result = connection.refresh_apps()
        self.assertEqual(result, dict(zip(client.APP_REFRESH_FIELDS, (1, 12, 13, 8, 9, 0))))
        self.assertEqual(peer.scans, 1)
        self.assertEqual([op for op, _ in peer.calls], [2, 21, 19, 19])
        self.assertFalse(connection._file_transaction_owned)
        self.assertFalse(connection.broken)

    def test_old_capability_rejects_without_sending_refresh(self):
        connection, peer, _ = self.connect()
        peer.cap = client.CAP_FILES
        with self.assertRaises(client.FileError) as error:
            connection.refresh_apps()
        self.assertEqual((error.exception.opcode, error.exception.result), (21, -715))
        self.assertEqual([op for op, _ in peer.calls], [2])
        self.assertFalse(connection.broken)

    def test_local_transaction_and_recovery_reject_without_any_rpc(self):
        for recovery, result in ((False, -702), (True, -714)):
            with self.subTest(recovery=recovery):
                connection, peer, _ = self.connect()
                connection._file_recovery = recovery
                connection._file_transaction_owned = True
                with self.assertRaises(client.FileError) as error:
                    connection.refresh_apps()
                self.assertEqual(error.exception.result, result)
                self.assertEqual(peer.calls, [])

    def test_native_busy_or_profile_rejection_does_not_abort_or_replay(self):
        for result in (-702, -709):
            with self.subTest(result=result):
                connection, peer, _ = self.connect()
                peer.refresh_rejection = result
                with self.assertRaises(client.FileError) as error:
                    connection.refresh_apps()
                self.assertEqual(error.exception.result, result)
                self.assertEqual([op for op, _ in peer.calls], [2, 21])
                self.assertEqual(peer.scans, 0)
                self.assertFalse(connection.broken)

    def test_malformed_success_report_breaks_connection_without_replay(self):
        for data in (pack(1, 2), pack(2, 12, 13, 8, 9, 0),
                     pack(1, 129, 13, 8, 9, 0), pack(1, 12, 13, 13, 9, 0),
                     pack(1, 12, 13, 8, 9, 1)):
            with self.subTest(data=data):
                connection, peer, _ = self.connect()
                peer.refresh_data = data
                with self.assertRaises(client.ProtocolError):
                    connection.refresh_apps()
                self.assertTrue(connection.broken)
                self.assertEqual(sum(op == 21 for op, _ in peer.calls), 1)

    def test_refresh_cancel_polls_same_job_and_does_not_submit_abort(self):
        connection, peer, _ = self.connect()
        event = threading.Event()
        peer.event, peer.cancel_at = event, 21
        with self.assertRaises(client.FileError) as error:
            connection.refresh_apps(cancel_event=event)
        self.assertEqual(error.exception.result, -710)
        self.assertEqual([op for op, _ in peer.calls], [2, 21, 19, 20, 19])
        self.assertEqual(peer.scans, 0)
        self.assertFalse(connection.broken)

    def test_upload_refresh_runs_only_after_committed_program_transfer(self):
        connection, peer, _ = self.connect()
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.exe"
            source.write_bytes(b"program")
            result = client.upload_with_refresh(connection, source, PROGRAM, refresh_apps=True)
        self.assertTrue(result["committed"] and result["refresh"]["ok"])
        commands = [op for op, _ in peer.calls]
        self.assertLess(commands.index(17), commands.index(21))
        self.assertEqual(sum(op == 15 for op in commands), 1)
        self.assertEqual(peer.files[PROGRAM], b"program")

    def test_committed_upload_remains_success_when_refresh_is_busy_or_unsupported(self):
        for unsupported in (False, True):
            with self.subTest(unsupported=unsupported):
                connection, peer, _ = self.connect()
                if unsupported:
                    peer.cap = client.CAP_FILES
                else:
                    peer.refresh_rejection = -702
                with tempfile.TemporaryDirectory() as directory:
                    source = Path(directory) / "test.exe"
                    source.write_bytes(b"committed")
                    result = client.upload_with_refresh(connection, source, PROGRAM, refresh_apps=True)
                self.assertTrue(result["committed"])
                self.assertFalse(result["refresh"]["ok"])
                self.assertEqual(result["refresh"]["file_result"], -715 if unsupported else -702)
                self.assertEqual(peer.files[PROGRAM], b"committed")
                self.assertEqual(sum(op == 15 for op, _ in peer.calls), 1)
                self.assertFalse(any(op == 18 for op, _ in peer.calls))

    def test_no_refresh_for_nonprogram_files_or_when_option_is_off(self):
        for remote, requested in (("A:\\demo.exe", True), ("A:\\系统\\程序\\data.bin", True),
                                  ("A:\\系统\\程序\\sub\\demo.exe", True), (PROGRAM, False)):
            with self.subTest(remote=remote, requested=requested):
                connection, peer, _ = self.connect()
                with tempfile.TemporaryDirectory() as directory:
                    source = Path(directory) / "test"
                    source.write_bytes(b"x")
                    result = client.upload_with_refresh(connection, source, remote, refresh_apps=requested)
                self.assertTrue(result["committed"])
                self.assertFalse(any(op == 21 for op, _ in peer.calls))
                self.assertEqual("refresh" in result, requested)

    def test_refresh_transport_failure_preserves_committed_result_and_marks_disconnect(self):
        connection, peer, _ = self.connect()
        def fail_refresh(**_kwargs):
            connection._broken = True
            raise TimeoutError("reply deadline")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.exe"
            source.write_bytes(b"saved")
            with patch.object(connection, "refresh_apps", side_effect=fail_refresh):
                result = client.upload_with_refresh(connection, source, PROGRAM, refresh_apps=True)
        self.assertTrue(result["committed"])
        self.assertTrue(result["refresh"]["connection_lost"])
        self.assertTrue(result["refresh"]["outcome_unknown"])
        self.assertTrue(result["refresh"]["partial_refresh_possible"])
        self.assertEqual(peer.files[PROGRAM], b"saved")
        self.assertEqual(sum(op == 15 for op, _ in peer.calls), 1)

    def test_post_or_postscan_profile_failure_warns_of_partial_refresh(self):
        for failure in (-703, -709):
            with self.subTest(result=failure):
                connection, peer, _ = self.connect()
                peer.refresh_result = failure
                with tempfile.TemporaryDirectory() as directory:
                    source = Path(directory) / "test.exe"
                    source.write_bytes(b"saved")
                    result = client.upload_with_refresh(connection, source, PROGRAM, refresh_apps=True)
                self.assertTrue(result["committed"])
                self.assertTrue(result["refresh"]["outcome_unknown"])
                self.assertTrue(result["refresh"]["partial_refresh_possible"])
                self.assertIn("partially refreshed", result["refresh"]["warning"])
                self.assertEqual(peer.scans, 1)
                self.assertEqual(sum(op == 21 for op, _ in peer.calls), 1)

    def test_standalone_cli_partial_failure_differs_from_preexecution_busy(self):
        for failure in (-703, -702):
            with self.subTest(result=failure):
                connection, peer, _ = self.connect()
                peer.refresh_result = failure
                output = io.StringIO()
                with patch.object(client, "Client", return_value=connection), contextlib.redirect_stdout(output):
                    code = client.main(["refresh-apps"])
                report = json.loads(output.getvalue())
                self.assertEqual(code, 1)
                self.assertFalse(report["ok"])
                self.assertEqual(report["outcome_unknown"], failure == -703)
                self.assertEqual(report.get("partial_refresh_possible", False), failure == -703)
                if failure == -703:
                    self.assertIn("partially refreshed", report["refresh_warning"])

    def test_cli_upload_json_retains_success_when_separate_refresh_fails(self):
        connection, peer, _ = self.connect()
        peer.refresh_rejection = -702
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.exe"
            source.write_bytes(b"saved")
            output = io.StringIO()
            with patch.object(client, "Client", return_value=connection), contextlib.redirect_stdout(output):
                code = client.main(["file-put", str(source), PROGRAM, "--refresh-apps"])
        report = json.loads(output.getvalue())
        self.assertEqual(code, 0)
        self.assertTrue(report["ok"] and report["result"]["committed"])
        self.assertFalse(report["result"]["refresh"]["ok"])

    def test_cli_refresh_command_and_program_path_classification(self):
        self.assertEqual(client.parser().parse_args(["refresh-apps"]).command, "refresh-apps")
        self.assertTrue(client.is_program_path("a:/系统/程序/demo.ExE"))
        self.assertFalse(client.is_program_path("A:\\其他\\程序\\demo.exe"))


if __name__ == "__main__":
    unittest.main()
