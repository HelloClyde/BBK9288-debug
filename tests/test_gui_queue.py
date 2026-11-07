"""Headless GUI scheduling checks; no Tk window, serial port or device is opened."""
import queue
import threading
import unittest
from unittest.mock import patch
try:
    from host import gui
    from host.client import CAP_APP_REFRESH, CAP_FILES, FileError
    from host.gui import DebugWindow, KeyQueue
except ImportError:
    DebugWindow = KeyQueue = None


class Value:
    def __init__(self):
        self.value = None

    def set(self, value):
        self.value = value

    def get(self):
        return self.value


class Widget:
    def __init__(self):
        self.state = None

    def configure(self, *, state):
        self.state = state


@unittest.skipIf(DebugWindow is None, "Tkinter module unavailable")
class GuiQueueTests(unittest.TestCase):
    def window(self):
        w = DebugWindow.__new__(DebugWindow)
        w.connected, w.closing, w.busy = True, False, False
        w.busy_operation = ""
        w.key_queue = KeyQueue()
        w.requests, w.results = queue.Queue(maxsize=2), queue.Queue()
        w.cancel_event = threading.Event()
        w.progress = {}
        w.state, w.transfer, w.refresh_state = Value(), Value(), Value()
        w.refresh_supported = False
        w.refresh_widgets = []
        w.root = type("Root", (), {"after": lambda *_: None})()
        w.log_messages = []
        w._log = w.log_messages.append
        w._set_controls = lambda: None
        return w

    def test_fifo_is_bounded_and_only_key_work_accepts_more_keys(self):
        w = self.window()
        w._press("UP")
        self.assertEqual(w.requests.get_nowait(), ("press", ("UP",)))
        for i in range(20):
            w._press(str(i))
        self.assertEqual([w.key_queue.pop() for _ in range(16)], [str(i) for i in range(16)])
        self.assertIsNone(w.key_queue.pop())
        self.assertEqual(len(w.log_messages), 4)
        w.busy_operation = "file-put"
        w._press("ENTER")
        self.assertIsNone(w.key_queue.pop())
        self.assertTrue(w.requests.empty())

    def test_new_operation_and_disconnect_drop_old_keys(self):
        for operation in ("status", "disconnect", "file-get"):
            w = self.window()
            w.key_queue.push("ENTER")
            w._submit(operation, "argument")
            self.assertIsNone(w.key_queue.pop())
            self.assertEqual(w.requests.get_nowait(), (operation, ("argument",)))

    def test_cancel_sets_event_without_rpc_or_command_replacement(self):
        w = self.window()
        w.busy, w.busy_operation = True, "file-put"
        w._cancel_transfer()
        self.assertTrue(w.cancel_event.is_set())
        self.assertTrue(w.requests.empty())
        self.assertTrue(w.busy)

    def test_progress_does_not_finish_active_work(self):
        w = self.window()
        w.busy, w.busy_operation = True, "file-put"
        w.results.put(("progress", (480, 960)))
        w._poll()
        self.assertTrue(w.busy)
        self.assertEqual(w.busy_operation, "file-put")
        self.assertEqual(w.progress["value"], 50)
        w.results.put(("progress", (960, 960)))
        w._poll()
        self.assertTrue(w.busy)
        self.assertIn("数据已传完，正在机端读回校验", w.transfer.value)
        self.assertIn("可取消", w.transfer.value)

    def test_old_capability_keeps_file_buttons_disabled(self):
        w = self.window()
        key, file, refresh = Widget(), Widget(), Widget()
        w.action_widgets, w.key_widgets, w.file_widgets = [key, file, refresh], [key], [file]
        w.refresh_widgets = [refresh]
        w.file_supported = False
        w.cancel_button, w.connect_button, w.disconnect_button = Widget(), Widget(), Widget()
        DebugWindow._set_controls(w)
        self.assertEqual(file.state, "disabled")
        self.assertEqual(key.state, "normal")
        self.assertEqual(refresh.state, "disabled")
        w.refresh_supported = True
        DebugWindow._set_controls(w)
        self.assertEqual(refresh.state, "normal")
        w.busy, w.busy_operation = True, "file-get"
        DebugWindow._set_controls(w)
        self.assertEqual(key.state, "disabled")
        self.assertEqual(refresh.state, "disabled")
        self.assertEqual(w.cancel_button.state, "normal")

    def test_refresh_capability_is_updated_on_connect_and_status(self):
        w = self.window()
        w.results.put(("ok", ("connect", {"identity": "device", "status": {"caps": CAP_FILES}})))
        w._poll()
        self.assertFalse(w.refresh_supported)
        self.assertIn("不支持刷新", w.refresh_state.value)
        w.results.put(("ok", ("status", {"caps": CAP_FILES | CAP_APP_REFRESH})))
        w._poll()
        self.assertTrue(w.refresh_supported)
        self.assertIn("原生桌面", w.refresh_state.value)

    def test_upload_request_carries_checkbox_choice_and_preserves_it_for_replace(self):
        w = self.window()
        w.auto_refresh, w.remote = Value(), Value()
        w.auto_refresh.set(True)
        w.remote.set("A:\\系统\\程序\\")
        with patch.object(gui.filedialog, "askopenfilename", return_value="demo.exe"):
            w._upload()
        self.assertEqual(w.requests.get_nowait(), ("file-put", ("demo.exe", "A:\\系统\\程序\\demo.exe", False, True)))
        error = FileError(15, -705)
        w.results.put(("file-error", ("file-put", ("demo.exe", "A:\\系统\\程序\\demo.exe", False, True), error)))
        with patch.object(gui.messagebox, "askyesno", return_value=True):
            w._poll()
        self.assertEqual(w.requests.get_nowait(), ("file-put", ("demo.exe", "A:\\系统\\程序\\demo.exe", True, True)))

    def test_worker_keeps_committed_upload_success_when_refresh_fails(self):
        w = self.window()
        w.requests = queue.Queue()
        w.requests.put(("connect", (None, 38400)))
        w.requests.put(("file-put", ("demo.exe", "A:\\系统\\程序\\demo.exe", False, True)))
        w.requests.put(("quit", ()))

        class FakeClient:
            broken = False
            uploads = 0
            refreshes = 0
            closed = False

            def ping(self):
                return "device"

            def status(self):
                return {"caps": CAP_FILES | CAP_APP_REFRESH}

            def put_file(self, _local, remote, **_kwargs):
                self.uploads += 1
                return {"remote": remote, "size": 5, "crc32": "12345678", "committed": True}

            def refresh_apps(self, **_kwargs):
                self.refreshes += 1
                raise FileError(21, -702)

            def close(self):
                self.closed = True

        client = FakeClient()
        with patch.object(gui, "Client", return_value=client):
            w._worker()
        self.assertEqual((client.uploads, client.refreshes), (1, 1))
        results = []
        while not w.results.empty():
            results.append(w.results.get_nowait())
        self.assertEqual([kind for kind, _ in results], ["ok", "ok", "closed"])
        result = results[1][1][1]
        self.assertTrue(result["committed"])
        self.assertFalse(result["refresh"]["ok"])
        self.assertEqual(result["refresh"]["file_result"], -702)
        self.assertTrue(client.closed)

    def test_poll_shows_upload_committed_and_separate_refresh_failure(self):
        for lost in (False, True):
            with self.subTest(connection_lost=lost):
                w = self.window()
                w.key_queue.push("ENTER")
                result = {"size": 5, "crc32": "12345678", "committed": True,
                          "refresh": {"ok": False, "connection_lost": lost, "error": "busy"}}
                w.results.put(("ok", ("file-put", result)))
                w._poll()
                self.assertIn("上传提交完成", w.transfer.value)
                self.assertIn("文件已提交，请勿重复上传", w.transfer.value)
                self.assertEqual(w.connected, not lost)
                if lost:
                    self.assertIsNone(w.key_queue.pop())

    def test_worker_preserves_committed_result_even_if_broken_transport_close_fails(self):
        w = self.window()
        w.requests = queue.Queue()
        for request in (("connect", (None, 38400)),
                        ("file-put", ("demo.exe", "A:\\系统\\程序\\demo.exe", False, True)),
                        ("quit", ())):
            w.requests.put(request)

        class FakeClient:
            broken = False

            def ping(self):
                return "device"

            def status(self):
                return {"caps": CAP_FILES | CAP_APP_REFRESH}

            def put_file(self, _local, remote, **_kwargs):
                return {"remote": remote, "size": 5, "crc32": "12345678", "committed": True}

            def refresh_apps(self, **_kwargs):
                self.broken = True
                raise TimeoutError("refresh reply lost")

            def close(self):
                raise OSError("transport already disconnected")

        with patch.object(gui, "Client", return_value=FakeClient()):
            w._worker()
        self.assertEqual(w.results.get_nowait()[0], "ok")
        kind, (_operation, result) = w.results.get_nowait()
        self.assertEqual(kind, "ok")
        self.assertTrue(result["committed"] and result["refresh"]["connection_lost"])
        self.assertEqual(w.results.get_nowait()[0], "closed")

    def test_refresh_busy_error_does_not_disconnect_or_submit_more_work(self):
        w = self.window()
        w.results.put(("file-error", ("refresh-apps", (), FileError(21, -702))))
        w._poll()
        self.assertTrue(w.connected)
        self.assertIn("原生桌面", w.refresh_state.value)
        self.assertTrue(w.requests.empty())

    def test_post_or_profile_refresh_errors_warn_that_scan_may_have_happened(self):
        for result in (-703, -709):
            with self.subTest(result=result):
                w = self.window()
                w.results.put(("file-error", ("refresh-apps", (), FileError(21, result))))
                w._poll()
                self.assertTrue(w.connected)
                self.assertIn("可能已完成部分刷新", w.refresh_state.value)
                self.assertIn("不要自动重放或重新上传", w.refresh_state.value)
                self.assertTrue(w.requests.empty())

    def test_committed_upload_with_uncertain_refresh_keeps_success_and_warning(self):
        w = self.window()
        result = {"size": 5, "crc32": "12345678", "committed": True,
                  "refresh": {"ok": False, "partial_refresh_possible": True}}
        w.results.put(("ok", ("file-put", result)))
        w._poll()
        self.assertIn("上传提交完成", w.transfer.value)
        self.assertIn("可能已完成部分刷新", w.transfer.value)


if __name__ == "__main__":
    unittest.main()
