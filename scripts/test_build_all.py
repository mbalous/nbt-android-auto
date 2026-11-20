"""Regression tests for the QNX build transport and artifact handling."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import build_all


class QnxBuildTests(unittest.TestCase):
    def test_windows_openssh_converts_local_paths_and_keeps_host_key_checks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config, known, identity = (root / name for name in ("config", "known_hosts", "key"))
            for path in (config, known, identity):
                path.touch()
            with mock.patch.object(build_all.shutil, "which", return_value="/usr/bin/wslpath"), \
                    mock.patch.object(Path, "is_file", return_value=True), \
                    mock.patch.object(build_all.subprocess, "check_output",
                                      side_effect=lambda command, **kwargs: "\\\\wsl.localhost\\Distro\\" +
                                      Path(command[-1]).name + "\n"):
                client = build_all.OpenSSH(windows=True)
                options = client.options(config, known, identity)
                self.assertEqual(options, [
                    "-F", "\\\\wsl.localhost\\Distro\\config",
                    "-o", "UserKnownHostsFile=//wsl.localhost/Distro/known_hosts",
                    "-i", "\\\\wsl.localhost\\Distro\\key",
                    "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes"])
                command = client.copy(options, root / "source.tar", "qnx-build:.nbt-build/test/source.tar")
                self.assertEqual(command[-2:], ["\\\\wsl.localhost\\Distro\\source.tar",
                                                 "qnx-build:.nbt-build/test/source.tar"])
                command = client.copy(options, "qnx-build:.nbt-build/test/out/app", root / "app")
                self.assertEqual(command[-2:], ["qnx-build:.nbt-build/test/out/app",
                                                 "\\\\wsl.localhost\\Distro\\app"])

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
                                     "SKIPPED: no libAppFramework.A.so / libCoreFramework.A.so")
            run.assert_not_called()

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
