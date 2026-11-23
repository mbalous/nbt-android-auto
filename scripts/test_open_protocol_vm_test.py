"""Regression tests for the paired offline protocol runner."""
import io
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

import test_open_protocol_vm as probe
import test_open_session_vm as session
from abi_provenance import shared_source_digest


class ProtocolRunnerTests(unittest.TestCase):
    def test_prepared_sources_require_matching_clean_build_and_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build, reference, output = root / "build", root / "reference", root / "output"
            build.mkdir()
            output.mkdir()
            (reference / "milestone-7/tests").mkdir(parents=True)
            (reference / "milestone-7/tests/test.cpp").write_text("test\r\n")
            (reference / "LICENSE.md").write_text("license")
            archive = build / "source.tar"
            with tarfile.open(archive, "w") as tar:
                for name in ("reference/src/example.cpp", "reference/include/example.h",
                             "src/android_auto/knob/ref/dsi_client.cpp",
                             "src/android_auto/knob/ref/dsi_client.hpp"):
                    data = name.encode()
                    info = tarfile.TarInfo(name)
                    info.size = len(data)
                    tar.addfile(info, io.BytesIO(data))
            inputs = {"toolchain": "open-experimental", "worktree_dirty": False, "revision": "revision",
                      "toolchain_install_sha256": "toolchain",
                      "shared_source_sha256": shared_source_digest(archive), "nbt_aad_revision": "revision"}
            (build / "abi-inputs.json").write_text(json.dumps(inputs))
            with mock.patch.object(probe, "digest_tree", return_value="toolchain"), \
                    mock.patch.object(probe, "run", side_effect=["revision\n", "", "milestone-7/tests/test.cpp\0"]):
                files = probe.prepare(build, reference, output)
            self.assertEqual(files["reference/tests/test.cpp"], b"test\n")
            self.assertIn("dsi/dsi_client.cpp", files)
            inputs["shared_source_sha256"] = "incorrect"
            (build / "abi-inputs.json").write_text(json.dumps(inputs))
            with self.assertRaisesRegex(ValueError, "matching prepared sources"):
                probe.prepare(build, reference, output)

    def test_bounded_runner_reports_failure_and_terminates_hung_child(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "bounded"
            subprocess.run(["cc", "-Wall", "-DBOUND_SECONDS=1", str(probe.ROOT / "toolchain/probes/run_bounded.c"),
                            "-o", str(executable)], check=True)
            for command, expected in ((["/bin/true"], 0), (["/bin/false"], 1), (["/bin/sleep", "5"], 1)):
                with self.subTest(command=command):
                    result = subprocess.run([str(executable), *command], capture_output=True, timeout=4)
                    self.assertEqual(result.returncode, expected)

    def test_session_requires_matching_source_script_and_toolchain(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build, work = root / "build", root / "work"
            (build / "work").mkdir(parents=True)
            work.mkdir()
            script = build / "work/build-open.sh"
            script.write_bytes(b"build")
            source = build / "source.tar"
            def write_source(qnx_script=b"qnx script", extra=None):
                with tarfile.open(source, "w") as archive:
                    for name, data in (("reference/src/test.cpp", b"source"), ("build-qnx.sh", qnx_script)):
                        info = tarfile.TarInfo(name)
                        info.size = len(data)
                        archive.addfile(info, io.BytesIO(data))
                    if extra:
                        archive.addfile(extra)
            write_source()
            inputs = {"toolchain": "open-experimental", "worktree_dirty": False, "revision": "a" * 40,
                      "shared_source_sha256": shared_source_digest(source),
                      "build_script_sha256": hashlib.sha256(b"build").hexdigest(),
                      "toolchain_install_sha256": "toolchain"}
            (build / "abi-inputs.json").write_text(json.dumps(inputs))
            with mock.patch.object(session, "digest_tree", return_value="toolchain"), \
                    mock.patch.object(session, "run", return_value="qnx script"):
                session.prepare(build, work, source)
                self.assertEqual((work / "reference/src/test.cpp").read_bytes(), b"source")
                write_source(qnx_script=b"changed")
                with self.assertRaisesRegex(ValueError, "QNX build script does not match"):
                    session.prepare(build, work, source)
                link = tarfile.TarInfo("reference/link")
                link.type, link.linkname = tarfile.SYMTYPE, "../../outside"
                write_source(extra=link)
                with self.assertRaisesRegex(ValueError, "Invalid source archive path"):
                    session.prepare(build, work, source)
                write_source()
                script.write_bytes(b"changed")
                with self.assertRaisesRegex(ValueError, "must match clean provenance"):
                    session.prepare(build, work, source)


if __name__ == "__main__":
    unittest.main()
