"""Windows GUI entry point with a bounded, device-free frozen smoke test."""
from __future__ import annotations

import json
from pathlib import Path
import sys


def smoke_test(report: Path) -> None:
    import tkinter as tk
    from host import gui

    def forbid_transport(*_args, **_kwargs):
        raise AssertionError("The GUI smoke test must not open a device")

    gui.Client = forbid_transport
    root = tk.Tk()
    window = gui.DebugWindow(root)
    root.update()
    if not root.winfo_viewable() or window.connected or window.busy:
        raise AssertionError("Frozen GUI did not open in the disconnected state")
    versions = {"tcl": str(root.tk.call("info", "patchlevel")),
                "tk": str(root.tk.call("package", "require", "Tk"))}
    root.after(200, window._quit)
    root.after(5000, root.destroy)
    root.mainloop()
    window.worker.join(2)
    if window.worker.is_alive():
        raise AssertionError("GUI worker did not shut down")
    report.write_text(json.dumps({"result": "PASS", "gui_visible": True,
                                 "serial_opened": False, "worker_stopped": True,
                                 "versions": versions}, indent=2) + "\n", encoding="utf-8")


def main() -> None:
    if len(sys.argv) == 3 and sys.argv[1] == "--smoke-test-report":
        report = Path(sys.argv[2]).resolve()
        report.parent.mkdir(parents=True, exist_ok=True)
        smoke_test(report)
    elif len(sys.argv) != 1:
        raise SystemExit("Unsupported client arguments")
    else:
        from host.gui import main as gui_main
        gui_main()


if __name__ == "__main__":
    main()
