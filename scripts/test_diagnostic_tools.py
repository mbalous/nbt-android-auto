"""Compile portable portions of the developer tools without the QNX SDK."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / "src"


class DiagnosticToolTests(unittest.TestCase):
    def test_loader_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            compiler = os.environ.get("CXX", "c++")
            for name in ("android_auto/tls_capabilities.cpp", "idrive/check_plugin.cpp"):
                executable = work / Path(name).stem
                subprocess.run([compiler, "-std=gnu++0x", "-Wall", "-Wextra",
                                str(SOURCE / name), "-ldl", "-o", str(executable)], check=True)
            result = subprocess.run([work / "tls_capabilities", work / "missing.so"],
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 4)
            self.assertIn("SSL_LOAD_FAILED", result.stderr)
            result = subprocess.run([work / "check_plugin"], timeout=5)
            self.assertEqual(result.returncode, 2)
