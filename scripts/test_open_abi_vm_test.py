"""Tests for the optional, isolated VM HMI loader matrix."""
import hashlib
import json
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest

import test_open_abi_vm as probe


class HmiProbeTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directories = []
        for name, toolchain, mode in probe.HMI_MATRIX:
            directory = Path(self.tmp.name) / name
            (directory / "programs").mkdir(parents=True)
            data = name.encode()
            (directory / "programs/libNbtAndroidAuto.so").write_bytes(data)
            inputs = {
                "revision": "revision", "shared_source_sha256": "source",
                "nbt_aad_revision": "aad", "mbedtls_archive_sha256": "tls",
                "toolchain": toolchain, "hmi_link": mode, "worktree_dirty": False,
                "selected": ["libNbtAndroidAuto.so"],
                "artifacts": {"libNbtAndroidAuto.so": {
                    "origin": "built", "sha256": hashlib.sha256(data).hexdigest()}},
            }
            (directory / "abi-inputs.json").write_text(json.dumps(inputs))
            self.directories.append(directory)

    def change(self, index, **changes):
        path = self.directories[index] / "abi-inputs.json"
        inputs = json.loads(path.read_text())
        inputs.update(changes)
        path.write_text(json.dumps(inputs))

    def test_matrix_requires_clean_matching_builds_and_plugin_hashes(self):
        self.assertEqual(len(probe.hmi_matrix_builds(self.directories)), 4)
        for changes in ({"worktree_dirty": True}, {"hmi_link": "real"},
                        {"shared_source_sha256": "different"}, {"selected": []}):
            with self.subTest(changes=changes):
                path = self.directories[1] / "abi-inputs.json"
                original = path.read_text()
                self.change(1, **changes)
                with self.assertRaises(ValueError):
                    probe.hmi_matrix_builds(self.directories)
                path.write_text(original)
        (self.directories[0] / "programs/libNbtAndroidAuto.so").write_bytes(b"changed")
        with self.assertRaises(ValueError):
            probe.hmi_matrix_builds(self.directories)
        with self.assertRaises(ValueError):
            probe.hmi_matrix_builds(self.directories[:3])

    def test_generated_fixture_aborts_on_every_contract_symbol(self):
        contract = json.loads(probe.HMI_CONTRACT.read_text())
        sources = probe.hmi_test_sources(contract)
        for filename, library in (("hmi-app.c", "libAppFramework.A.so"),
                                  ("hmi-core.c", "libCoreFramework.A.so")):
            for symbol in contract["stubs"][library]:
                self.assertIn(("void %s(void) { abort(); }\n" % symbol).encode(), sources[filename])
            self.assertNotIn(b" {}\n", sources[filename])
            first = contract["stubs"][library][0]
            source = sources[filename] + ("\nint main(void) { %s(); return 0; }\n" % first).encode()
            executable = Path(self.tmp.name) / filename
            subprocess.run(["cc", "-x", "c", "-o", str(executable), "-"], input=source, check=True)
            result = subprocess.run([str(executable)], cwd=self.tmp.name, capture_output=True)
            self.assertEqual(result.returncode, -signal.SIGABRT)

    def test_real_vm_libraries_require_four_builds(self):
        result = subprocess.run([sys.executable, str(Path(probe.__file__)), "--vm-address",
                                 "192.168.56.101", "--hmi-real-vm"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--hmi-real-vm requires --hmi-builds", result.stderr)


if __name__ == "__main__":
    unittest.main()
