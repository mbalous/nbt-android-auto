"""Tests for explicitly enabled persistent incident diagnostics."""
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
import unittest

from build_all import NBT_AAD_REVISION

ROOT = Path(__file__).resolve().parents[1]


class ServiceLockTests(unittest.TestCase):
    def test_exclusion_and_stale_recovery(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "lock.c"
            source.write_text("""
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "service_lock.h"
int main(void) {
    int recovered = 0;
    int fd = acquire_service_lock(&recovered);
    if (fd < 0) return 2;
    printf("%d\\n", recovered);
    fflush(stdout);
    getchar();
    unlink("service.lock");
    close(fd);
    return 0;
}
""")
            binary = root / "lock"
            subprocess.run(["cc", "-I", str(ROOT / "src/android_auto"), str(source),
                            "-o", str(binary)], check=True)
            first = subprocess.Popen([str(binary)], cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     text=True)
            try:
                self.assertEqual(first.stdout.readline().strip(), "0")
                blocked = subprocess.run([str(binary)], cwd=root, input="", text=True)
                self.assertEqual(blocked.returncode, 2)
                first.send_signal(signal.SIGKILL)
                first.wait()
                recovered = subprocess.run([str(binary)], cwd=root, input="", text=True,
                                           capture_output=True, check=True)
                self.assertEqual(int(recovered.stdout), first.pid)
                (root / "service.lock").write_text("")
                ambiguous = subprocess.run([str(binary)], cwd=root, input="", text=True)
                self.assertEqual(ambiguous.returncode, 2)
            finally:
                if first.poll() is None:
                    first.kill()
                    first.wait()
                first.stdin.close()
                first.stdout.close()


class IncidentTests(unittest.TestCase):
    def test_opt_in_and_bounded_snapshots(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pkg, ram, runtime, bin_dir = (root / n for n in ("pkg", "ram", "runtime", "bin"))
            for path in (pkg, ram, runtime, bin_dir):
                path.mkdir()
            for name in ("pidin", "sloginfo"):
                stub = bin_dir / name
                stub.write_text("#!/bin/sh\nexit 0\n")
                stub.chmod(0o700)
            script = (ROOT / "scripts/nbt-aa-incident.sh").read_text()
            script = script.replace("P=/var/opt/hmi/data/rw/NbtAndroidAutoRT", "P=" + shlex.quote(str(pkg)))
            script = script.replace("R=/fs/ram3/nbt-aa-native", "R=" + shlex.quote(str(runtime)))
            script = script.replace("/fs/ram3/", str(ram) + "/")
            incident = root / "incident.sh"
            incident.write_text(script)
            env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ["PATH"])

            def run():
                subprocess.run(["sh", str(incident), "../../bad-name", "1", "fixture"], env=env, check=True)

            run()
            self.assertFalse((pkg / "incidents").exists())
            self.assertFalse((pkg / "events.log").exists())
            (pkg / "incident-logging").touch()
            snapshots = pkg / "incidents"
            snapshots.mkdir()
            for i in range(41):
                f = snapshots / f"old-{i:02}.txt"
                f.write_text("old")
                os.utime(f, (1000 + i, 1000 + i))
            (pkg / "events.log").write_bytes(b"x" * 131073)
            run()
            files = list(snapshots.glob("*.txt"))
            self.assertEqual(len(files), 40)
            created = [f for f in files if "bad-name" in f.name]
            self.assertEqual(len(created), 1)
            self.assertLessEqual(created[0].stat().st_size, 256 * 1024)
            self.assertEqual((pkg / "events.log.1").stat().st_size, 131073)
            self.assertLess((pkg / "events.log").stat().st_size, 1024)

    def test_boot_history_opt_in_and_safe_names(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pkg, ram = root / "pkg", root / "ram"
            pkg.mkdir()
            ram.mkdir()
            source = (ROOT / "scripts/nbt-aa-boot.sh").read_text()
            section = source[source.index("L=$P/logs\n"):source.index("# optional passive MOST trace")]
            section = section.replace("/fs/ram3/", str(ram) + "/")
            script = root / "rotation.sh"
            script.write_text("P=" + shlex.quote(str(pkg)) + "\n" + section)
            logs = pkg / "logs"
            logs.mkdir()
            (logs / "states.txt").write_text("baseline")
            subprocess.run(["sh", str(script)], check=True)
            self.assertFalse((pkg / "history").exists())
            self.assertTrue((pkg / "logs.prev/states.txt").exists())
            (pkg / "extended-log-history").touch()
            (pkg / "logs/boot-number").write_text("../../unsafe\n")
            history = pkg / "history"
            history.mkdir()
            for i in range(20):
                d = history / f"boot-{i}"
                d.mkdir()
                os.utime(d, (1000 + i, 1000 + i))
            subprocess.run(["sh", str(script)], check=True)
            self.assertLessEqual(len(list(history.glob("boot-*"))), 15)
            self.assertTrue((ram / "nbt-aa-booted").exists())
            self.assertFalse((root / "unsafe").exists())


class FlashTests(unittest.TestCase):
    def test_build_default_never_contacts_car(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "flash-aa-open.sh"
            script.write_bytes((ROOT / "flash-aa-open.sh").read_bytes())
            release = root / ".local/release-payload/NBT-AndroidAuto-Python-2026.11.10.2/payload/plugin"
            release.mkdir(parents=True)
            (release / "libNbtAndroidAuto.so").touch()
            (root / ".local/reference-nbt-aad/.git").mkdir(parents=True)
            toolchain = root / "build/qnx-open/bin/i486-pc-nto-qnx6.5.0-gcc"
            toolchain.parent.mkdir(parents=True)
            toolchain.write_text("#!/bin/sh\nexit 0\n")
            toolchain.chmod(0o700)
            programs = root / "build/fake/programs"
            programs.mkdir(parents=True)
            for name in ("audioconn", "nbt_android_auto", "native_service"):
                (programs / name).touch()
            (root / "installer/payload").mkdir(parents=True)
            (root / "installer/payload/manifest.json").touch()
            python = root / ".venv/bin/python"
            python.parent.mkdir(parents=True)
            python.write_text("#!/bin/sh\nexit 0\n")
            python.chmod(0o700)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            commands = {
                "git": f'case "$*" in *"rev-parse HEAD"*) echo {NBT_AAD_REVISION};; '
                       '*"rev-parse --abbrev-ref"*) echo test;; *"rev-parse --short"*) echo abc123;; esac\n',
                "docker": "exit 0\n",
                "python3": 'echo "programs: $FAKE_PROGRAMS"\n',
                "ping": 'touch "$CAR_CONTACTED"; exit 1\n',
                "sleep": "exit 0\n",
                "ifconfig": "exit 0\n",
            }
            for name, body in commands.items():
                command = bin_dir / name
                command.write_text("#!/bin/sh\n" + body)
                command.chmod(0o700)
            marker = root / "car-contacted"
            env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ["PATH"],
                       FAKE_PROGRAMS=str(programs), CAR_CONTACTED=str(marker))
            subprocess.run(["bash", str(script)], env=env, check=True, stdout=subprocess.PIPE)
            self.assertFalse(marker.exists())
            result = subprocess.run(["bash", str(script), "--install-only"], env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(marker.exists())


if __name__ == "__main__":
    unittest.main()
