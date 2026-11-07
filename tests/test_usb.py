"""Compile and run the production USB driver against a register/FIFO model."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from compiler_paths import native_gcc, target_compiler

ROOT=Path(__file__).resolve().parents[1]

class USBModelTests(unittest.TestCase):
    def test_strict_target_compile(self):
        cc=target_compiler()
        if not cc.is_file():self.skipTest("Target compiler is required")
        with tempfile.TemporaryDirectory() as directory:
            for enabled,guarded in ((0,0),(0,1),(1,0),(1,1)):
                with self.subTest(platform_gate=enabled,timer_guarded=guarded):
                    subprocess.run([str(cc),"--target=s1c33-none-elf","-Os","-ffreestanding","-fno-builtin",
                                    "-Wall","-Wextra","-Werror",f"-DD9288_USB_PLATFORM_VERIFIED={enabled}",
                                    f"-DD9288_USB_TIMER_GUARDED={guarded}","-c",str(ROOT/"device/usb_cdc.c"),
                                    "-o",str(Path(directory)/f"usb-{enabled}-{guarded}.o")],
                                   check=True,capture_output=True,text=True)
    def test_default_and_experimental_controller_models(self):
        gcc=native_gcc()
        if not Path(gcc).is_file(): self.skipTest("GCC is required for the native controller model")
        env=dict(os.environ); env["PATH"]=str(Path(gcc).parent)+os.pathsep+env.get("PATH","")
        with tempfile.TemporaryDirectory() as directory:
            for enabled,guarded in ((0,0),(0,1),(1,0),(1,1)):
                with self.subTest(platform_gate=enabled,timer_guarded=guarded):
                    exe=Path(directory)/f"usb-{enabled}-{guarded}.exe"
                    subprocess.run([gcc,"-std=c99","-O2","-Wall","-Wextra","-Werror",
                                    f"-DD9288_USB_PLATFORM_VERIFIED={enabled}",
                                    f"-DD9288_USB_TIMER_GUARDED={guarded}",
                                    str(ROOT/"tests/usb_cdc_model.c"),"-o",str(exe)],
                                   check=True,capture_output=True,text=True,env=env)
                    r=subprocess.run([str(exe)],check=True,capture_output=True,text=True,env=env,timeout=15)
                    self.assertIn("PASS",r.stdout)

if __name__=="__main__": unittest.main()
