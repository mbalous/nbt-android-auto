"""Check the session executable's path from build output to installation and launch."""
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest import mock

import build_all

ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


payload = load_module("make_payload", "installer/make_payload.py")
installer = load_module("nbt_aa_installer", "installer/nbt_aa_installer.py")


class SessionPackageTests(unittest.TestCase):
    def test_build_package_and_runtime_use_same_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            built, pem_dir, out = root / "build", root / "pems", root / "payload"
            for source, _, _, _ in payload.built_files(built, pem_dir):
                if source.is_relative_to(root):
                    source.parent.mkdir(parents=True, exist_ok=True)
                    source.write_bytes(b"fixture")

            with mock.patch.object(payload, "OUT", out), mock.patch(
                    "sys.argv", ["make_payload.py", "test", "--built", str(built), "--pem-dir", str(pem_dir)]):
                payload.main()

            manifest = json.loads((out / "manifest.json").read_text())
            session = next(item for item in manifest["files"] if item["source"] == "rt/nbt_android_auto")
            executable = Path(session["source"]).name
            self.assertEqual(session["target"], installer.RT_DIR + "/" + executable)
            self.assertEqual((out / session["source"]).read_bytes(), b"fixture")
            self.assertEqual(build_all.PROGRAMS[executable], "rt")

            build_script = (ROOT / "scripts/build-qnx.sh").read_text()
            self.assertRegex(build_script, r"-o \$OUT/" + re.escape(executable) + r"\b")
            boot = (out / "rt/boot.sh").read_text()
            copy_list = re.search(r"for f in (.+); do", boot).group(1).split()
            self.assertIn(executable, copy_list)
            service = (ROOT / "src/android_auto/nbt_native_service.c").read_text()
            for option in ("--switch-aa", "--capture-video"):
                self.assertIn('{"./' + executable + '", "' + option + '"', service)

            class Nbt:
                def __init__(self):
                    self.files = {installer.RT_DIR + "/previous-session": b"existing"}

                def put(self, data, target, mode):
                    self.files[target] = data

                def run(self, command, **kwargs):
                    return 0, b"", b""

            nbt = Nbt()
            with mock.patch.object(installer, "payload_dir", return_value=out), mock.patch.object(
                    installer, "preflight"):
                installer.install(nbt, lambda _: None)
            self.assertEqual(nbt.files[session["target"]], b"fixture")
            self.assertEqual(nbt.files[installer.RT_DIR + "/previous-session"], b"existing")


if __name__ == "__main__":
    unittest.main()
