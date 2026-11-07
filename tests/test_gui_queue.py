"""Headless GUI scheduling checks; no Tk window, serial port or device is opened."""
import queue
import threading
import unittest
try:
    from host.gui import DebugWindow, KeyQueue
except ImportError:
    DebugWindow = KeyQueue = None


class Value:
    def __init__(self):
        self.value = None

    def set(self, value):
        self.value = value


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
        w.state, w.transfer = Value(), Value()
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
        key, file = Widget(), Widget()
        w.action_widgets, w.key_widgets, w.file_widgets = [key, file], [key], [file]
        w.file_supported = False
        w.cancel_button, w.connect_button, w.disconnect_button = Widget(), Widget(), Widget()
        DebugWindow._set_controls(w)
        self.assertEqual(file.state, "disabled")
        self.assertEqual(key.state, "normal")
        w.busy, w.busy_operation = True, "file-get"
        DebugWindow._set_controls(w)
        self.assertEqual(key.state, "disabled")
        self.assertEqual(w.cancel_button.state, "normal")


if __name__ == "__main__":
    unittest.main()
