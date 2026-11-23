"""Regression tests for the QNX build transport and artifact handling."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

import build_all


class QnxBuildTests(unittest.TestCase):
    def test_native_openssh_options_keep_host_key_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config, known, identity = (root / name for name in ("config", "known_hosts", "key"))
            for path in (config, known, identity):
                path.touch()
            client = build_all.OpenSSH()
            options = client.options(config, known, identity)
            self.assertEqual(options, [
                "-F", str(config),
                "-o", "UserKnownHostsFile=" + known.as_posix(),
                "-i", str(identity),
                "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes"])
            command = client.copy(options, root / "source.tar", "qnx-build:.nbt-build/test/source.tar")
            self.assertEqual(command[:3], ["scp", "-O", "-q"])
            self.assertEqual(command[-2:], [str(root / "source.tar"),
                                            "qnx-build:.nbt-build/test/source.tar"])

    def test_native_openssh_preserves_paths(self):
        client = build_all.OpenSSH()
        self.assertEqual(client.ssh, "ssh")
        self.assertEqual(client.copy([], Path("/tmp/source"), "qnx-build:target")[-2:],
                         ["/tmp/source", "qnx-build:target"])

    def test_failed_directory_creation_stops_before_upload(self):
        remote = mock.Mock(return_value=subprocess.CompletedProcess([], 1, b"already exists"))
        with self.assertRaisesRegex(SystemExit, "Cannot create unique VM build directory"):
            build_all.create_vm_build_directory(remote, ".nbt-build/qnx-build-test")
        remote.assert_called_once_with("umask 077; mkdir -p .nbt-build && mkdir .nbt-build/qnx-build-test")

    def test_failed_artifact_fetch_is_not_reported_as_success(self):
        client = build_all.OpenSSH()
        with mock.patch.object(build_all, "PROGRAMS", {"nbt_android_auto": "rt"}), \
                mock.patch.object(build_all.subprocess, "run",
                                  return_value=subprocess.CompletedProcess([], 1, b"connection lost")):
            with self.assertRaisesRegex(SystemExit, "(?s)VM build files retained.*connection lost"):
                build_all.fetch_programs(client, [], "qnx-build", ".nbt-build/test", Path("/tmp"), "")

    def test_only_explicitly_skipped_plugin_is_optional(self):
        client = build_all.OpenSSH()
        with mock.patch.object(build_all, "PROGRAMS", {"libNbtAndroidAuto.so": "plugin"}), \
                mock.patch.object(build_all.subprocess, "run") as run:
            build_all.fetch_programs(client, [], "qnx-build", ".nbt-build/test", Path("/tmp"),
                                     "missing")
            run.assert_not_called()

    def test_vm_link_mode_must_match_request_and_be_unambiguous(self):
        self.assertEqual(build_all.hmi_link_mode("QNX_HMI_LINK=stubs\n", "stubs"), "stubs")
        self.assertEqual(build_all.hmi_link_mode("QNX_HMI_LINK=missing\n", "real"), "missing")
        for log, requested in (("", "stubs"), ("QNX_HMI_LINK=missing\n", "stubs"),
                               ("QNX_HMI_LINK=real\n", "stubs"),
                               ("QNX_HMI_LINK=real\nQNX_HMI_LINK=stubs\n", "real")):
            with self.subTest(log=log), self.assertRaises(SystemExit):
                build_all.hmi_link_mode(log, requested)

    def test_hmi_build_flags_are_mutually_exclusive(self):
        result = subprocess.run([sys.executable, str(Path(build_all.__file__)), "--hmi-stubs",
                                 "--hmi-libs", "/tmp/libs"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not allowed with argument", result.stderr)

    def test_hmi_library_path_preserves_home_expansion_without_shell_injection(self):
        self.assertEqual(build_all.hmi_libs_assignment("$HOME/.nbt-build/hmi-libs"),
                         ' HMI_LIBS="$HOME"/.nbt-build/hmi-libs')
        self.assertEqual(build_all.hmi_libs_assignment("/tmp/lib; echo unsafe"),
                         " HMI_LIBS='/tmp/lib; echo unsafe'")

    def test_provenance_binds_plugin_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / "programs").mkdir()
            (out / "programs" / "libNbtAndroidAuto.so").write_bytes(b"plugin")
            build_all.write_provenance(out, "qnx-sdp", "stubs", "test-build")
            record = json.loads((out / "build-provenance.json").read_text())
            self.assertEqual(record["hmi_link"], "stubs")
            self.assertEqual(record["plugin_sha256"], hashlib.sha256(b"plugin").hexdigest())

    def test_build_archive_includes_both_shared_stub_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sources.tar"
            archive = build_all.Archive(path)
            build_all.add_repo_sources(archive)
            archive.close()
            with tarfile.open(path) as tar:
                for name in ("app.c", "core.c", "cpp.c"):
                    self.assertIn("toolchain/hmi-stubs/" + name, tar.getnames())
                self.assertNotIn("src/idrive/hmi-stubs/app.c", tar.getnames())

    def test_stub_compiler_failure_does_not_skip_plugin_or_fall_back(self):
        script = (build_all.ROOT / "scripts/build-qnx.sh").read_text()
        block = script.split('echo "== HMI plugin libNbtAndroidAuto.so"', 1)[1].split(
            'echo "== nbt_reset', 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "src/idrive").mkdir(parents=True)
            env = dict(os.environ, W=str(root), OUT=str(root / "out"), HMI_MODE="stubs", CC="false")
            result = subprocess.run(["sh", "-ec", block], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("QNX_HMI_LINK=", result.stdout)
            self.assertNotIn("SKIPPED:", result.stdout)

    def test_stub_plugin_retains_coreframework_compiler_helper(self):
        script = (build_all.ROOT / "scripts/build-qnx.sh").read_text()
        block = script.split('echo "== HMI plugin libNbtAndroidAuto.so"', 1)[1].split(
            'echo "== nbt_reset', 1)[0]
        for mode in ("real", "stubs"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / "src/idrive").mkdir(parents=True)
                (root / "out").mkdir()
                (root / "bin").mkdir()
                (root / "libs").mkdir()
                for name in ("libAppFramework.A.so", "libCoreFramework.A.so"):
                    (root / "libs" / name).touch()
                qcc = root / "bin/qcc"
                qcc.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$CAPTURE"\n'
                               'while [ "$#" -gt 0 ]; do\n'
                               '  if [ "$1" = -o ]; then shift; : > "$1"; break; fi\n'
                               '  shift\ndone\n')
                qcc.chmod(0o700)
                capture = root / "args"
                env = dict(os.environ, W=str(root), OUT=str(root / "out"), HMI_MODE=mode, CC="qcc",
                           HMI_LIBS=str(root / "libs"), CAPTURE=str(capture),
                           PATH=str(root / "bin") + os.pathsep + os.environ["PATH"])
                result = subprocess.run(["sh", "-ec", block], env=env, capture_output=True, text=True, check=True)
                plugin = next(line for line in capture.read_text().splitlines() if "aa_entry_plugin.cpp" in line)
                self.assertEqual("-Wl,-u,__divdi3" in plugin, mode == "stubs")
                self.assertIn("QNX_HMI_LINK=" + mode, result.stdout)

    def test_empty_artifact_is_not_reported_as_success(self):
        client = build_all.OpenSSH()
        with tempfile.TemporaryDirectory() as directory:
            (Path(directory) / "nbt_android_auto").touch()
            with mock.patch.object(build_all, "PROGRAMS", {"nbt_android_auto": "rt"}), \
                    mock.patch.object(build_all.subprocess, "run",
                                      return_value=subprocess.CompletedProcess([], 0, b"")):
                with self.assertRaisesRegex(SystemExit, "missing or empty"):
                    build_all.fetch_programs(client, [], "qnx-build", ".nbt-build/test", Path(directory), "")


if __name__ == "__main__":
    unittest.main()
