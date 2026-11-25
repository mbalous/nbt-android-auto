"""Tests for explicitly enabled persistent incident diagnostics."""
import os
import json
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
import time
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
    def test_session_summaries_are_opt_in_attributed_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pkg, ram, runtime = (root / name for name in ("pkg", "ram", "runtime"))
            for path in (pkg, ram, runtime):
                path.mkdir()
            script = (ROOT / "scripts/nbt-aa-incident.sh").read_text()
            script = script.replace("P=/var/opt/hmi/data/rw/NbtAndroidAutoRT", "P=" + shlex.quote(str(pkg)))
            script = script.replace("R=/fs/ram3/nbt-aa-native", "R=" + shlex.quote(str(runtime)))
            script = script.replace("/fs/ram3/", str(ram) + "/")
            (pkg / "incident.sh").write_text(script)
            source = root / "summary.c"
            source.write_text(
                "#define RUNTIME_DIR " + json.dumps(str(runtime)) + "\n"
                "#define PERSIST_DIR " + json.dumps(str(pkg)) + "\n"
                "#define main native_service_main\n"
                '#include "' + str(ROOT / "src/android_auto/nbt_native_service.c") + '"\n'
                "#undef main\n"
                "int main(int argc, char **argv) { (void)argc; "
                "session_summary((unsigned)atoi(argv[1]), argv[2], atoi(argv[3])); return 0; }\n"
            )
            binary = root / "summary"
            subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", str(source), "-o", str(binary)], check=True)
            summaries = pkg / "session-summaries"

            def run(number, started=1):
                subprocess.run([str(binary), str(number), "outcome-%d" % number, str(started)],
                               cwd=runtime, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

            def wait_for(predicate):
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if predicate():
                        return
                    time.sleep(0.02)
                self.fail("timed out waiting for session summary")

            def has_record(number):
                for path in summaries.glob("*.txt"):
                    try:
                        if "outcome-%d\n" % number in path.read_text():
                            return True
                    except FileNotFoundError:
                        continue
                return False

            run(1)
            self.assertFalse(summaries.exists())
            (pkg / "incident-logging").touch()
            (runtime / "capture-1.log").write_text(
                "AA_USB_IN_FAILED session=first completion_rc=19 actual_valid=0\n"
                "AA_VIDEO_CAPTURE_STOP reason=usb_in\n")
            run(1)
            (runtime / "capture-1.log").write_text("AA_USB_IN_FAILED session=reused\n")
            wait_for(lambda: list(summaries.glob("*.txt")))
            first = next(summaries.glob("*.txt")).read_text()
            self.assertIn("session=first", first)
            self.assertNotIn("session=reused", first)
            self.assertIn("actual_valid=0", first)
            run(2, 0)
            run(3)
            wait_for(lambda: len(list(summaries.glob("*.txt"))) >= 3)
            records = [p.read_text() for p in summaries.glob("*.txt")]
            self.assertTrue(any("outcome-2" in r and "capture=not_started" in r for r in records))
            self.assertTrue(any("outcome-3" in r and "capture=unavailable" in r for r in records))
            for number in range(4, 21):
                run(number, 0)
                wait_for(lambda: has_record(number))
            wait_for(lambda: len(list(summaries.glob("*.txt"))) <= 15 and
                     not list(runtime.glob("session-summary-??????")) and not list(summaries.glob("*.tmp")))
            self.assertEqual(len(list(summaries.glob("*.txt"))), 15)
            self.assertTrue(all(p.stat().st_size <= 4096 for p in summaries.glob("*.txt")))
            self.assertFalse(list(runtime.glob("session-summary-??????")))

    def test_session_summary_storage_failure_is_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pkg, runtime = root / "pkg", root / "runtime"
            pkg.mkdir()
            runtime.mkdir()
            (pkg / "incident-logging").touch()
            (pkg / "session-summaries").write_text("not a directory")
            source = runtime / "session-summary-123456"
            source.write_text("test")
            script = (ROOT / "scripts/nbt-aa-incident.sh").read_text()
            script = script.replace("P=/var/opt/hmi/data/rw/NbtAndroidAutoRT", "P=" + shlex.quote(str(pkg)))
            script = script.replace("R=/fs/ram3/nbt-aa-native", "R=" + shlex.quote(str(runtime)))
            result = subprocess.run(["sh", "-c", script, "incident.sh", "session-summary", str(source)],
                                    text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("cannot create session summary directory", result.stderr)
            self.assertFalse(source.exists())

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
            (pkg / "logs/states.txt").write_text("current")
            subprocess.run(["sh", str(script)], check=True)
            self.assertEqual((pkg / "logs/states.txt").read_text(), "current")
            self.assertEqual((pkg / "logs.prev/states.txt").read_text(), "baseline")
            (pkg / "extended-log-history").touch()
            subprocess.run(["sh", str(script)], check=True)
            self.assertFalse((pkg / "history").exists())
            self.assertEqual((pkg / "logs/states.txt").read_text(), "current")
            (ram / "nbt-aa-booted").unlink()
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
            self.assertEqual((pkg / "logs.prev/states.txt").read_text(), "current")
            (pkg / "logs/states.txt").write_text("new boot")
            (pkg / "extended-log-history").unlink()
            subprocess.run(["sh", str(script)], check=True)
            self.assertEqual((pkg / "logs/states.txt").read_text(), "new boot")
            (ram / "nbt-aa-booted").unlink()
            subprocess.run(["sh", str(script)], check=True)
            self.assertEqual((pkg / "logs.prev/states.txt").read_text(), "new boot")

    def test_unfinished_session_records_are_cleaned_only_on_boot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pkg, ram = root / "pkg", root / "ram"
            pkg.mkdir()
            ram.mkdir()
            summaries = pkg / "session-summaries"
            summaries.mkdir()
            unfinished = summaries / "20261125-154200-session-summary-Ab1234.txt.tmp"
            record = summaries / "20261125-154200-session-summary-Ab1234.txt"
            source = (ROOT / "scripts/nbt-aa-boot.sh").read_text()
            section = source[source.index("# A power loss can leave"):source.index("mkdir -p $R")]
            section = section.replace("/fs/ram3/", str(ram) + "/")
            script = root / "cleanup.sh"
            script.write_text("P=" + shlex.quote(str(pkg)) + "\n" + section)
            unfinished.touch()
            record.touch()
            (ram / "nbt-aa-booted").touch()
            subprocess.run(["sh", str(script)], check=True)
            self.assertTrue(unfinished.exists())
            (ram / "nbt-aa-booted").unlink()
            subprocess.run(["sh", str(script)], check=True)
            self.assertFalse(unfinished.exists())
            self.assertTrue(record.exists())


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
