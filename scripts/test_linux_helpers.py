"""Focused tests for Linux/WSL VM and vehicle helper scripts (offline / dry-run only)."""
import base64
import io
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import qnx_vm_setup
import update_nbt_aa_package
import upload_nbt_aa_test

ROOT = Path(__file__).resolve().parent.parent
HWVIDEO_DIR = ROOT / "experiments" / "hwvideo-bringup"


class QnxVmSetupTests(unittest.TestCase):
    def test_validate_fingerprint(self):
        self.assertEqual(qnx_vm_setup.validate_fingerprint("MD5:00:11:22:33:44:55:66:77:88:99:aa:bb:cc:dd:ee:ff"), "md5")
        self.assertEqual(
            qnx_vm_setup.validate_fingerprint("SHA256:" + "A" * 43),
            "sha256",
        )
        with self.assertRaises(SystemExit):
            qnx_vm_setup.validate_fingerprint("SHA256:short")

    def test_known_hosts_helper_accepts_matching_and_rejects_mismatch(self):
        blob = b"fake-rsa-key-material"
        key_b64 = base64.b64encode(blob).decode("ascii")
        expected = qnx_vm_setup.compute_key_fingerprint(key_b64, "sha256")
        with tempfile.TemporaryDirectory() as tmp:
            dest = Path(tmp) / "pending"
            buf = io.StringIO()
            with mock.patch.object(sys, "stdout", buf):
                rc = qnx_vm_setup.main([
                    "_known-hosts-helper",
                    "--expected", expected,
                    "--destination", str(dest),
                    "--expected-host", "192.168.56.10",
                    "192.168.56.10", "ssh-rsa", key_b64,
                ])
            self.assertEqual(rc, 0)
            self.assertEqual(buf.getvalue(), f"192.168.56.10 ssh-rsa {key_b64}\n")
            self.assertEqual(stat.S_IMODE(dest.stat().st_mode), 0o600)

            bad_dest = Path(tmp) / "bad"
            rc_bad = qnx_vm_setup.main([
                "_known-hosts-helper",
                "--expected", "SHA256:" + "B" * 43,
                "--destination", str(bad_dest),
                "--expected-host", "192.168.56.10",
                "192.168.56.10", "ssh-rsa", key_b64,
            ])
            self.assertEqual(rc_bad, 1)
            self.assertFalse(bad_dest.exists())

    def test_write_ssh_config_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = Path(tmp) / "ssh_config"
            key = Path(tmp) / "id_rsa"
            qnx_vm_setup.write_ssh_config(cfg, key, "nbtbuild", "192.168.56.10", "22")
            self.assertEqual(stat.S_IMODE(cfg.stat().st_mode), 0o600)
            self.assertIn("StrictHostKeyChecking yes", cfg.read_text(encoding="utf-8"))
            with self.assertRaises(SystemExit):
                qnx_vm_setup.write_ssh_config(cfg, key, "nbtbuild", "192.168.56.10", "22")

    def test_verify_elf32_i386(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = Path(tmp) / "good"
            header = bytearray(52)
            header[0:4] = b"\x7fELF"
            header[4] = 1  # ELF32
            header[5] = 1  # little-endian
            header[18] = 3  # EM_386
            good.write_bytes(header)
            qnx_vm_setup.verify_elf32_i386(good)

            bad = Path(tmp) / "bad"
            header[4] = 2  # ELF64
            bad.write_bytes(header)
            with self.assertRaises(SystemExit):
                qnx_vm_setup.verify_elf32_i386(bad)

    def test_verify_vm_ssh_config_enforces_host_and_user(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg, known = Path(tmp) / "ssh_config", Path(tmp) / "known_hosts"
            cfg.touch()
            known.touch()
            with mock.patch.object(
                qnx_vm_setup.subprocess, "run",
                return_value=subprocess.CompletedProcess([], 0, "hostname 192.168.56.10\nport 22\nuser REPLACE_ME\n", ""),
            ):
                with self.assertRaisesRegex(SystemExit, "Set the VM username"):
                    qnx_vm_setup.verify_vm_ssh_config(cfg, known)

            with mock.patch.object(
                qnx_vm_setup.subprocess, "run",
                return_value=subprocess.CompletedProcess([], 0, "hostname 10.0.0.5\nport 22\nuser nbtbuild\n", ""),
            ):
                with self.assertRaisesRegex(SystemExit, "restricted to the QNX VM"):
                    qnx_vm_setup.verify_vm_ssh_config(cfg, known)


class DeployAndExperimentHelpersTests(unittest.TestCase):
    def test_update_and_upload_dry_run_and_ram_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            buf = io.StringIO()
            with mock.patch.object(sys, "stdout", buf):
                self.assertEqual(update_nbt_aa_package.main(["--source", tmp, "--dry-run"]), 0)
            self.assertIn("[dry-run]", buf.getvalue())

            buf = io.StringIO()
            with mock.patch.object(sys, "stdout", buf):
                self.assertEqual(upload_nbt_aa_test.main(["--source", tmp, "--dry-run"]), 0)
            self.assertIn("/fs/ram3/nbt-video-test", buf.getvalue())

            with self.assertRaisesRegex(SystemExit, "must be under /fs/ram3/ or /tmp/"):
                upload_nbt_aa_test.main(["--source", tmp, "--remote-dir", "/var/opt/hmi", "--dry-run"])

    def test_hwvideo_scripts_dry_run_and_validation(self):
        collect = subprocess.run(
            [sys.executable, str(HWVIDEO_DIR / "collect_nbt_video_route.py"), "--dry-run", "--sections", "procs"],
            capture_output=True, text=True,
        )
        self.assertEqual(collect.returncode, 0, collect.stderr)
        self.assertIn("[dry-run]", collect.stdout)

        run_ok = subprocess.run(
            [
                sys.executable, str(HWVIDEO_DIR / "test_nbt_native_video.py"),
                "run", "--device", "NmeDeviceScreen", "--seconds", "8", "--dry-run",
            ],
            capture_output=True, text=True,
        )
        self.assertEqual(run_ok.returncode, 0, run_ok.stderr)
        self.assertIn("NmeDeviceScreen", run_ok.stdout)

        run_bad_dir = subprocess.run(
            [
                sys.executable, str(HWVIDEO_DIR / "test_nbt_native_video.py"),
                "cleanup", "--remote-dir", "/var/opt/hmi", "--dry-run",
            ],
            capture_output=True, text=True,
        )
        self.assertNotEqual(run_bad_dir.returncode, 0)

        run_bad_dev = subprocess.run(
            [
                sys.executable, str(HWVIDEO_DIR / "test_nbt_native_video.py"),
                "run", "--device", "Bad;rm", "--dry-run",
            ],
            capture_output=True, text=True,
        )
        self.assertNotEqual(run_bad_dev.returncode, 0)


if __name__ == "__main__":
    unittest.main()
