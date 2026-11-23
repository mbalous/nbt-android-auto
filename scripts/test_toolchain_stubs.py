"""Tests for symbols generated from the open QNX sysroot."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ToolchainStubTests(unittest.TestCase):
    def test_resource_functions_are_exported_but_callback_fields_are_not(self):
        with tempfile.TemporaryDirectory() as directory:
            subprocess.run([sys.executable, str(ROOT / "toolchain/runtime/mkstub.py"),
                            str(ROOT / "toolchain/sysroot/usr/include"), directory],
                           check=True, capture_output=True)
            source = (Path(directory) / "libc.c").read_text()
            for name in ("dispatch_create", "dispatch_block", "dispatch_handler", "dispatch_timeout",
                         "dispatch_destroy", "dispatch_context_alloc", "dispatch_context_free",
                         "resmgr_attach", "resmgr_detach", "iofunc_func_init", "iofunc_attr_init",
                         "pthread_create"):
                with self.subTest(name=name):
                    self.assertIn("void %s(void) {}" % name, source)
            self.assertNotIn("void _unblock(void) {}", source)


if __name__ == "__main__":
    unittest.main()
