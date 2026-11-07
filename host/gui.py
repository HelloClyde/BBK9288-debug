"""Small Tk desktop for the experimental BBK 9288 debug client.

All serial traffic belongs to one worker. Tk only receives immutable results;
neither a button click nor window shutdown can start a concurrent RPC.
"""

from __future__ import annotations

import json
from collections import deque
from pathlib import Path
import queue
import shutil
import sys
import tempfile
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
from typing import Any, Callable

from .client import CAP_FILES, Client, FileError, remote_path, scan_code


ROOT = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent.parent


class KeyQueue:
    """A small FIFO for complete taps, never an unbounded OS key-repeat backlog."""
    def __init__(self, limit: int = 16):
        self.limit = limit
        self.items: deque[str] = deque()

    def push(self, key: str) -> bool:
        if len(self.items) >= self.limit:
            return False
        self.items.append(key)
        return True

    def pop(self) -> str | None:
        return self.items.popleft() if self.items else None

    def clear(self) -> None:
        self.items.clear()


class DebugWindow:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("9288 Debug — 实验版")
        width = min(940, int(root.winfo_screenwidth() * 0.85))
        height = min(780, int(root.winfo_screenheight() * 0.85))
        self.root.geometry(f"{width}x{height}")
        self.root.minsize(min(900, width), min(560, height))
        self.requests: queue.Queue[tuple[str, tuple[Any, ...]]] = queue.Queue(maxsize=2)
        self.results: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.connected = False
        self.busy = False
        self.busy_operation = ""
        self.key_queue = KeyQueue()
        self.file_supported = False
        self.cancel_event = threading.Event()
        self.closing = False
        self.last_image: Path | None = None
        self.photo: Any = None
        self.port = tk.StringVar(value="自动发现 USB CDC")
        self.baud = tk.StringVar(value="38400")
        self.state = tk.StringVar(value="未连接")
        self.address = tk.StringVar(value="0x02000000")
        self.length = tk.StringVar(value="256")
        self.scale = tk.IntVar(value=2)
        self.keyboard = tk.BooleanVar(value=False)
        self.remote = tk.StringVar(value="A:\\")
        self.transfer = tk.StringVar(value="文件传输需要支持 CAP_FILES 的新版机端服务")
        self.image_caption = tk.StringVar(value="等待设备截图 · 320 × 240")
        self.action_widgets: list[ttk.Widget] = []
        self.key_widgets: list[ttk.Widget] = []
        self.file_widgets: list[ttk.Widget] = []
        self._build()
        self.worker = threading.Thread(target=self._worker, name="9288-serial", daemon=True)
        self.worker.start()
        self.root.after(60, self._poll)
        self.root.protocol("WM_DELETE_WINDOW", self._quit)

    def _build(self) -> None:
        viewport = tk.Canvas(self.root, highlightthickness=0)
        scroll = ttk.Scrollbar(self.root, orient="vertical", command=viewport.yview)
        scroll.pack(side="right", fill="y")
        viewport.pack(side="left", fill="both", expand=True)
        viewport.configure(yscrollcommand=scroll.set)
        shell = ttk.Frame(viewport, padding=14)
        window = viewport.create_window((0, 0), window=shell, anchor="nw")
        shell.bind("<Configure>", lambda _event: viewport.configure(scrollregion=viewport.bbox("all")))
        viewport.bind("<Configure>", lambda event: viewport.itemconfigure(window, width=event.width))
        self.root.bind("<MouseWheel>", lambda event: viewport.yview_scroll(-int(event.delta / 120), "units"))
        ttk.Label(shell, text="9288 Debug", font=("Microsoft YaHei UI", 18, "bold")).pack(anchor="w")
        ttk.Label(shell, text="实验版 · USB、截图及 GUI 按键已做真机验证；文件传输需新版服务", foreground="#a25b00").pack(anchor="w", pady=(2, 10))
        top = ttk.Frame(shell)
        top.pack(fill="x")
        ttk.Label(top, text="端口").pack(side="left")
        self.port_box = ttk.Combobox(top, textvariable=self.port, width=29)
        self.port_box.pack(side="left", padx=(7, 5))
        ttk.Button(top, text="刷新端口", command=self._ports).pack(side="left")
        ttk.Label(top, text="波特率").pack(side="left", padx=(14, 6))
        ttk.Combobox(top, textvariable=self.baud, values=("38400", "115200", "9600"), width=9).pack(side="left")
        self.connect_button = ttk.Button(top, text="连接", command=self._connect)
        self.connect_button.pack(side="left", padx=(12, 5))
        self.disconnect_button = ttk.Button(top, text="断开", command=lambda: self._submit("disconnect"), state="disabled")
        self.disconnect_button.pack(side="left")
        ttk.Label(top, textvariable=self.state).pack(side="left", padx=12)

        tabs = ttk.Notebook(shell)
        tabs.pack(fill="both", expand=True, pady=(10, 0))
        debug_page, file_page = ttk.Frame(tabs), ttk.Frame(tabs)
        tabs.add(debug_page, text="画面与按键")
        tabs.add(file_page, text="文件传输")
        body = ttk.Frame(debug_page)
        body.pack(fill="both", expand=True, pady=(13, 10))
        screen = ttk.LabelFrame(body, text="设备画面", padding=8)
        screen.pack(side="left", fill="both", expand=True)
        self.canvas = tk.Canvas(screen, width=640, height=480, background="#dfe5d1", highlightthickness=1, highlightbackground="#8f9884", takefocus=True)
        self.canvas.pack(fill="both", expand=True)
        self.canvas.create_text(320, 240, text="等待原尺寸截图", fill="#63705c", font=("Microsoft YaHei UI", 16), tags="placeholder")
        self.canvas.bind("<Configure>", self._redraw)
        self.canvas.bind("<Button-1>", lambda _event: self.canvas.focus_set())
        self.canvas.bind("<KeyPress>", self._key_event)
        ttk.Label(screen, textvariable=self.image_caption).pack(anchor="w", pady=(5, 0))
        display_controls = ttk.Frame(screen)
        display_controls.pack(fill="x", pady=(7, 0))
        self._button(display_controls, "刷新截图", lambda: self._submit("screenshot")).pack(side="left")
        ttk.Button(display_controls, text="另存截图", command=self._save_image).pack(side="left", padx=6)
        ttk.Label(display_controls, text="缩放").pack(side="left", padx=(8, 2))
        for factor in (1, 2):
            ttk.Radiobutton(display_controls, text=f"{factor}×", variable=self.scale, value=factor, command=self._redraw).pack(side="left")

        controls = ttk.Frame(body, padding=(12, 0, 0, 0))
        controls.pack(side="left", fill="y")
        ttk.Label(controls, text="按键", font=("Microsoft YaHei UI", 11, "bold")).pack(anchor="w")
        arrows = ttk.Frame(controls)
        arrows.pack(pady=(7, 4))
        for label, key, row, col in (("↑", "UP", 0, 1), ("←", "LEFT", 1, 0), ("确定", "ENTER", 1, 1), ("→", "RIGHT", 1, 2), ("↓", "DOWN", 2, 1)):
            button = self._button(arrows, label, lambda k=key: self._press(k), width=6)
            self.key_widgets.append(button)
            button.grid(row=row, column=col, padx=2, pady=2)
        for label, key in (("返回", "ESC"), ("开始 / 菜单", "F1"), ("目录", "F5"), ("上翻页", "PAGEUP"), ("下翻页", "PAGEDOWN"), ("删除", "DELETE"), ("空格", "SPACE")):
            button = self._button(controls, label, lambda k=key: self._press(k))
            self.key_widgets.append(button)
            button.pack(fill="x", pady=2)
        ttk.Checkbutton(controls, text="电脑键盘输入", variable=self.keyboard, command=self._keyboard_focus).pack(anchor="w", pady=(8, 1))
        ttk.Label(controls, text="勾选后点击画面\n支持字母、数字及方向键", foreground="#666666").pack(anchor="w")
        self._button(controls, "查询状态", lambda: self._submit("status")).pack(fill="x", pady=(13, 3))
        self._button(controls, "停止机端服务", lambda: self._submit("stop")).pack(fill="x", pady=3)

        ram = ttk.LabelFrame(debug_page, text="RAM 读取", padding=8)
        ram.pack(fill="x")
        ttk.Label(ram, text="起始地址").pack(side="left")
        ttk.Entry(ram, textvariable=self.address, width=17).pack(side="left", padx=(6, 12))
        ttk.Label(ram, text="字节数").pack(side="left")
        ttk.Entry(ram, textvariable=self.length, width=10).pack(side="left", padx=6)
        self._button(ram, "读取并保存…", self._read).pack(side="left", padx=7)
        ttk.Label(ram, text="仅读取机端允许的 RAM 区域", foreground="#666666").pack(side="left", padx=8)
        files = ttk.LabelFrame(file_page, text="文件传输 · GBK 路径", padding=8)
        files.pack(fill="both", expand=True, pady=(8, 0))
        file_top = ttk.Frame(files)
        file_top.pack(fill="x")
        ttk.Label(file_top, text="机端路径").pack(side="left")
        ttk.Entry(file_top, textvariable=self.remote).pack(side="left", fill="x", expand=True, padx=6)
        for text, command in (("上传…", self._upload), ("下载…", self._download)):
            button = self._button(file_top, text, command)
            self.file_widgets.append(button)
            button.pack(side="left", padx=2)
        self.cancel_button = ttk.Button(file_top, text="取消传输", command=self._cancel_transfer, state="disabled")
        self.cancel_button.pack(side="left", padx=2)
        ttk.Label(files, text="填写已知完整路径，例如 A:\\系统\\程序\\9288MemoryProbe.exe。目录必须已存在。\n"
                  "上传时若以 \\ 结尾，会追加电脑文件名；下载必须填写完整文件路径。\n"
                  "本版不提供目录浏览。上传不会自动运行程序，默认不覆盖同名文件。",
                  wraplength=760, justify="left").pack(anchor="w", pady=(10, 16))
        self.progress = ttk.Progressbar(files, mode="determinate", maximum=100)
        self.progress.pack(fill="x")
        ttk.Label(files, textvariable=self.transfer).pack(anchor="w", pady=(3, 0))
        self.log = tk.Text(shell, height=6, wrap="word", state="disabled", font=("Consolas", 9))
        self.log.pack(fill="x", pady=(10, 0))
        self._log("连接时会核对 9288 专用协议身份。按键回执表示已接受，画面变化请刷新截图核对。")
        self._set_controls()

    def _button(self, parent: ttk.Widget, text: str, command: Callable[[], Any], **kwargs: Any) -> ttk.Button:
        button = ttk.Button(parent, text=text, command=command, **kwargs)
        self.action_widgets.append(button)
        return button

    def _log(self, text: str) -> None:
        self.log.configure(state="normal")
        self.log.insert("end", f"{time.strftime('%H:%M:%S')}  {text}\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def _set_controls(self) -> None:
        available = self.connected and not self.busy and not self.closing
        for widget in self.action_widgets:
            widget.configure(state="normal" if available else "disabled")
        for widget in self.key_widgets:
            allowed = self.connected and not self.closing and (not self.busy or self.busy_operation == "press")
            widget.configure(state="normal" if allowed else "disabled")
        for widget in self.file_widgets:
            widget.configure(state="normal" if available and self.file_supported else "disabled")
        self.cancel_button.configure(state="normal" if self.busy_operation in ("file-list", "file-get", "file-put") and self.busy else "disabled")
        self.connect_button.configure(state="normal" if not self.connected and not self.busy and not self.closing else "disabled")
        self.disconnect_button.configure(state="normal" if self.connected and not self.busy and not self.closing else "disabled")

    def _ports(self) -> None:
        try:
            from serial.tools import list_ports
            ports = list(list_ports.comports())
        except ImportError:
            messagebox.showerror("缺少依赖", "请运行：python -m pip install -r requirements.txt", parent=self.root)
            return
        self.port_box.configure(values=["自动发现 USB CDC", *[p.device for p in ports]])
        self._log("可用端口：" + ("；".join(f"{p.device} — {p.description}" for p in ports) or "未发现串口"))

    def _connect(self) -> None:
        port = self.port.get().strip()
        if port == "自动发现 USB CDC" or not port:
            port = None
        try:
            baud = int(self.baud.get())
            if not 1200 <= baud <= 1000000:
                raise ValueError
        except ValueError:
            messagebox.showerror("波特率无效", "请输入 1200 到 1000000 的整数。", parent=self.root)
            return
        self._submit("connect", port, baud)

    def _submit(self, operation: str, *args: Any) -> None:
        if self.busy or self.closing:
            return
        if operation != "connect" and not self.connected:
            return
        if operation != "press":
            self.key_queue.clear()
        self.busy = True
        self.busy_operation = operation
        if operation in ("file-list", "file-get", "file-put"):
            self.cancel_event.clear()
            self.progress["value"] = 0
        self.state.set("处理中…")
        self._set_controls()
        self.requests.put_nowait((operation, args))

    def _press(self, key: str) -> None:
        if self.busy:
            if self.connected and not self.closing and self.busy_operation == "press":
                if not self.key_queue.push(key):
                    self._log("按键队列已满（16）；未追加按键。")
            return
        self._submit("press", key)

    def _upload(self) -> None:
        filename = filedialog.askopenfilename(parent=self.root, title="选择要上传的文件")
        if not filename:
            return
        target = self.remote.get()
        if target.endswith(("\\", "/")):
            target += Path(filename).name
        try:
            target, _ = remote_path(target)
        except ValueError as error:
            messagebox.showerror("路径无效", str(error), parent=self.root)
            return
        self._submit("file-put", filename, target, False)

    def _download(self) -> None:
        try:
            path, _ = remote_path(self.remote.get())
        except ValueError as error:
            messagebox.showerror("路径无效", str(error), parent=self.root)
            return
        filename = filedialog.asksaveasfilename(parent=self.root, title="下载并校验文件", initialfile=path.rsplit("\\", 1)[-1])
        if filename:
            self._submit("file-get", path, filename)

    def _cancel_transfer(self) -> None:
        self.cancel_event.set()
        self.transfer.set("正在等待机端清理完成；已提交的文件以最终回执为准…")

    def _keyboard_focus(self) -> None:
        if self.keyboard.get():
            self.canvas.focus_set()

    def _key_event(self, event: tk.Event) -> str | None:
        if not self.keyboard.get() or not self.connected:
            return None
        names = {"Up": "UP", "Down": "DOWN", "Left": "LEFT", "Right": "RIGHT", "Return": "ENTER", "Escape": "ESC", "BackSpace": "BACKSPACE", "Delete": "DELETE", "space": "SPACE", "Prior": "PAGEUP", "Next": "PAGEDOWN"}
        key = names.get(event.keysym)
        if key is None and len(event.char) == 1 and event.char.isascii() and event.char.isalnum():
            key = event.char.upper()
        if key:
            self._press(key)
            return "break"
        return None

    def _read(self) -> None:
        try:
            address = int(self.address.get(), 0)
            size = int(self.length.get(), 0)
            if not 0 <= address <= 0xFFFFFFFF or not 1 <= size <= 65536 or address + size > 0x100000000:
                raise ValueError
        except ValueError:
            messagebox.showerror("读取范围无效", "地址支持 0x 十六进制；一次读取 1 到 65536 字节。", parent=self.root)
            return
        filename = filedialog.asksaveasfilename(parent=self.root, title="保存 RAM 数据", defaultextension=".bin", filetypes=(("二进制数据", "*.bin"), ("所有文件", "*.*")))
        if filename:
            self._submit("read", address, size, filename)

    def _save_image(self) -> None:
        if self.last_image is None:
            messagebox.showinfo("没有截图", "先连接设备并刷新截图。", parent=self.root)
            return
        filename = filedialog.asksaveasfilename(parent=self.root, title="保存当前截图", defaultextension=".png", filetypes=(("PNG 图片", "*.png"),))
        if filename:
            try:
                if Path(filename).resolve() != self.last_image.resolve():
                    shutil.copyfile(self.last_image, filename)
                self._log(f"已保存当前截图：{filename}")
            except OSError as error:
                messagebox.showerror("保存失败", str(error), parent=self.root)

    def _redraw(self, _event: tk.Event | None = None) -> None:
        if self.last_image is None:
            return
        from PIL import Image, ImageTk
        with Image.open(self.last_image) as original:
            resized = original.resize((original.width * self.scale.get(), original.height * self.scale.get()), Image.Resampling.NEAREST)
            self.photo = ImageTk.PhotoImage(resized)
        self.canvas.delete("all")
        self.canvas.create_image(self.canvas.winfo_width() // 2, self.canvas.winfo_height() // 2, image=self.photo, anchor="center")

    def _worker(self) -> None:
        client: Client | None = None
        while True:
            operation, args = self.requests.get()
            if operation == "quit":
                if client:
                    try:
                        client.close()
                    except Exception:
                        pass
                self.results.put(("closed", None))
                return
            try:
                if operation == "connect":
                    if client:
                        client.close()
                    client = Client(port=args[0], baud=args[1], timeout=5)
                    identity = client.ping()
                    result: Any = {"identity": identity.decode("ascii") if isinstance(identity, bytes) else identity,
                                   "status": client.status()}
                elif operation == "disconnect":
                    if client:
                        client.close()
                    client = None
                    result = "连接已断开；机端服务保持原状态。"
                else:
                    if client is None:
                        raise RuntimeError("设备尚未连接")
                    if operation == "press":
                        result = client.press(scan_code(args[0]))
                    elif operation == "status":
                        result = client.status()
                    elif operation == "stop":
                        result = client.stop()
                        client.close()
                        client = None
                    elif operation == "screenshot":
                        directory = ROOT / "captures"
                        directory.mkdir(exist_ok=True)
                        path = directory / f"screen-{time.strftime('%Y%m%d-%H%M%S')}-{time.time_ns() % 1000000:06d}.png"
                        client.screenshot(path)
                        result = path
                    elif operation == "read":
                        address, size, output = args
                        data = bytearray()
                        while len(data) < size:
                            count = min(512, size - len(data))
                            chunk = client.read_memory(address + len(data), count)
                            if len(chunk) != count:
                                raise RuntimeError(f"RAM 返回长度错误：{len(chunk)}，预期 {count}")
                            data.extend(chunk)
                        destination = Path(output)
                        with tempfile.NamedTemporaryFile(dir=destination.parent, prefix=destination.name + ".part-", delete=False) as temporary:
                            temporary.write(data)
                            staged = Path(temporary.name)
                        try:
                            staged.replace(destination)
                        except BaseException:
                            staged.unlink(missing_ok=True)
                            raise
                        result = f"已读取 {size} 字节并保存：{destination}"
                    elif operation in ("file-list", "file-get", "file-put"):
                        def progress(done, total):
                            self.results.put(("progress", (done, total)))
                        if operation == "file-list":
                            result = client.list_files(args[0], cancel_event=self.cancel_event)
                        elif operation == "file-get":
                            result = client.get_file(args[0], args[1], progress=progress, cancel_event=self.cancel_event)
                        else:
                            result = client.put_file(args[0], args[1], replace=args[2], progress=progress,
                                                     cancel_event=self.cancel_event)
                    else:
                        raise RuntimeError(f"未知操作：{operation}")
                self.results.put(("ok", (operation, result)))
            except FileError as error:
                self.results.put(("file-error", (operation, args, error)))
            except Exception as error:
                # A failed RPC may already have changed the device. Never replay
                # it or issue another command through a potentially broken link.
                if client:
                    try:
                        client.close()
                    except Exception:
                        pass
                client = None
                self.results.put(("error", (operation, str(error))))

    def _poll(self) -> None:
        try:
            while True:
                kind, payload = self.results.get_nowait()
                if kind == "progress":
                    done, total = payload
                    self.progress["value"] = done * 100 / total if total else 100
                    self.transfer.set(f"{done:,} / {total:,} 字节" + (" · 数据已传完，正在机端读回校验，请等待（可取消）" if done == total else ""))
                    continue
                self.busy = False
                self.busy_operation = ""
                if kind == "closed":
                    self.root.destroy()
                    return
                if kind == "error":
                    self.connected = False
                    self.key_queue.clear()
                    operation, detail = payload
                    self._log(f"{operation} 失败：{detail}。已断开；未自动重试。")
                elif kind == "file-error":
                    operation, args, error = payload
                    self.key_queue.clear()
                    self.transfer.set(str(error))
                    self._log(f"文件操作未成功：{error}；连接保持，未自动重放。")
                    if error.result == -714:
                        self.file_supported = False
                        messagebox.showerror("文件提交需要恢复", str(error), parent=self.root)
                    elif operation == "file-put" and error.result == -705 and not args[2] and not self.closing:
                        if messagebox.askyesno("目标文件已存在", f"覆盖机端文件？\n{args[1]}\n机端会保留备份直到校验提交完成。", parent=self.root):
                            self._submit("file-put", args[0], args[1], True)
                    elif error.result == -715:
                        self.transfer.set("此机端服务尚不支持该文件操作；上传/下载需要新版服务。")
                else:
                    operation, result = payload
                    if operation == "connect":
                        self.connected = True
                        self.file_supported = bool(result["status"]["caps"] & CAP_FILES)
                        self.transfer.set("支持文件事务；仅操作 A:\\，上传和下载均校验 CRC32。" if self.file_supported else
                                          "当前机端不支持文件传输，请先更新至 CAP_FILES 新版服务。")
                    elif operation in ("disconnect", "stop"):
                        self.connected = False
                        self.file_supported = False
                        self.key_queue.clear()
                    if operation == "screenshot":
                        self.last_image = Path(result)
                        try:
                            from PIL import Image
                            with Image.open(result) as screenshot:
                                self.image_caption.set(f"{screenshot.width} × {screenshot.height} · {Path(result).name}")
                            self._redraw()
                        except Exception as error:
                            self._log(f"截图已保存，显示失败：{error}")
                        self._log(f"截图：{result}")
                    elif operation == "press":
                        self._log("按键已接受；刷新截图核对设备画面。")
                    elif operation in ("file-get", "file-put"):
                        self.progress["value"] = 100
                        self.transfer.set(f"{'上传提交' if operation == 'file-put' else '下载校验'}完成：{result['size']:,} 字节 · CRC32 {result['crc32']}")
                        self._log(json.dumps(result, ensure_ascii=False))
                    else:
                        self._log(json.dumps(result, ensure_ascii=False, default=str) if not isinstance(result, str) else result)
                self.state.set("已连接" if self.connected else "未连接")
                if self.connected and not self.busy and not self.closing:
                    queued = self.key_queue.pop()
                    if queued is not None:
                        self._submit("press", queued)
                self._set_controls()
        except queue.Empty:
            pass
        if self.closing:
            # Existing work runs to its bounded serial timeout; quit follows it
            # in the same queue so closing never races an active exchange.
            self.root.after(100, self._poll)
        else:
            self.root.after(60, self._poll)

    def _quit(self) -> None:
        if self.closing:
            return
        self.closing = True
        self.key_queue.clear()
        self.cancel_event.set()
        self.state.set("正在关闭连接…")
        self._set_controls()
        self.requests.put_nowait(("quit", ()))


def main() -> None:
    root = tk.Tk()
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        root.withdraw()
        messagebox.showerror("缺少依赖", "请在 9288-debug 目录运行：\npython -m pip install -r requirements.txt", parent=root)
        root.destroy()
        return
    DebugWindow(root)
    root.mainloop()


if __name__ == "__main__":
    main()
