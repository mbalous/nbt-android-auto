"""Exercise diagnostic tools without the QNX SDK or a head unit."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import nbt_pull_logs
import nbt_pull_phone_logs

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


class LogSummaryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.dest = Path(temp.name) / "pull"
        out = patch.object(nbt_pull_logs, "OUT", Path(temp.name))
        out.start()
        self.addCleanup(out.stop)

    def write(self, name, content):
        path = self.dest / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def test_running_capture_is_separate_from_old_usb_failures_and_system_warnings(self):
        ram = "car/fs/ram3/"
        self.write(ram + "nbt-aa-state", "state=running\nphone=1\nsession=1\n")
        self.write(ram + "nbt-aa-native/capture-1.log", "AA_VIDEO_CAPTURE_BEGIN\nAA_STALL no video frame\n")
        self.write(ram + "nbt-aa-native/capture-4.log", "AA_USB_IN_FAILED rc=0 status=02000005 bytes=0\n")
        self.write("live/sloginfo.txt", "EHCI - Error on Control Transfer ( abc ) - Retrying transfer(count 1)\n"
                   "EHCI - Error on Control Transfer ( abc )\n"
                   "screen: win_post_ref: couldn't allocate display handle for window 11\n")

        text = nbt_pull_logs.summary(self.dest)
        _, active, system, other = text.split("\n## ")[-4:]
        self.assertIn("capture-1.log", active)
        self.assertIn("1x video stall", active)
        self.assertNotIn("USB transfer failed", active)
        self.assertIn("1x EHCI transfer error", system)
        self.assertIn("1x display-handle warning", system)
        self.assertIn("1x USB transfer failed", other)
        self.assertIn("capture-4.log", other)
        self.assertNotIn("video stall", other)
        self.assertEqual(nbt_pull_logs.summary(self.dest), text)

    def test_running_state_does_not_make_an_already_ended_capture_active(self):
        ram = "car/fs/ram3/"
        self.write(ram + "nbt-aa-state", "state=running\nphone=1\nsession=1\n")
        self.write(ram + "nbt-aa-native/capture-1.log", "AA_VIDEO_CAPTURE_BEGIN\n"
                   "AA_USB_IN_FAILED rc=0 status=02000005 bytes=0\nAA_VIDEO_CAPTURE_END\n")

        _, active, _, other = nbt_pull_logs.summary(self.dest).split("\n## ")[-4:]
        self.assertIn("no matching in-progress capture", active)
        self.assertNotIn("USB transfer failed", active)
        self.assertIn("1x USB transfer failed", other)

    def test_capture_already_shutting_down_is_not_active(self):
        ram = "car/fs/ram3/"
        self.write(ram + "nbt-aa-state", "state=running\nphone=1\nsession=1\n")
        self.write(ram + "nbt-aa-native/capture-1.log", "AA_VIDEO_CAPTURE_BEGIN\n"
                   "AA_LIVE_WRITER_FAILED errno=32 uptime_ms=1234\n"
                   "AA_VIDEO_CAPTURE_STOP reason=session_inactive local_shutdown=capture_error uptime_ms=1334\n")

        _, active, _, other = nbt_pull_logs.summary(self.dest).split("\n## ")[-4:]
        self.assertIn("no matching in-progress capture", active)
        self.assertIn("1x video FIFO writer failed", other)


class PhoneLogTests(unittest.TestCase):
    def test_saves_read_only_snapshot_and_clock_bracket(self):
        serial = "192.0.2.1:12345"
        payload = b"1700000000.001  100  100 I UsbDeviceManager: accessory mode\n"
        with tempfile.TemporaryDirectory() as work, patch.object(nbt_pull_phone_logs, "OUT", Path(work)), patch.object(
            nbt_pull_phone_logs.subprocess, "run",
            side_effect=[subprocess.CompletedProcess([], 0, stdout=data, stderr=b"")
                         for data in (b"device\r\n", b"1700000000.123456789\r\n", payload,
                                      b"USB_STATE=DISCONNECTED\n")],
        ) as adb, patch.object(nbt_pull_phone_logs.time, "time_ns", side_effect=[1700000000000000000, 1700000000200000000]):
            folder, warnings = nbt_pull_phone_logs.pull("adb", serial)
            self.assertEqual(warnings, [])
            self.assertEqual(list(Path(work).iterdir()), [folder])
            self.assertEqual((folder / "logcat.txt").read_bytes(), payload)
            self.assertEqual((folder / "usb-service.txt").read_bytes(), b"USB_STATE=DISCONNECTED\n")
            self.assertEqual(json.loads((folder / "clock.json").read_text()), {
                "phone_epoch_seconds": 1700000000,
                "phone_epoch_ns": 1700000000123456789,
                "phone_clock_resolution_ns": 1,
                "host_epoch_ns_before": 1700000000000000000,
                "host_epoch_ns_after": 1700000000200000000,
            })
            if os.name == "posix":
                self.assertEqual(folder.stat().st_mode & 0o777, 0o700)
                self.assertEqual((folder / "logcat.txt").stat().st_mode & 0o777, 0o600)
            self.assertEqual([call.args[0][:3] for call in adb.call_args_list], [["adb", "-s", serial]] * 4)
            self.assertEqual(adb.call_args_list[2].args[0][3:], [
                "logcat", "-d", "-t", "5000", "-b", "main", "-b", "system", "-b", "events", "-v", "epoch",
            ])
            self.assertEqual(adb.call_args.args[0][3:], ["shell", "dumpsys", "usb"])

    def test_saves_logcat_when_usb_history_is_unavailable(self):
        serial = "192.0.2.1:12345"
        with tempfile.TemporaryDirectory() as work, patch.object(nbt_pull_phone_logs, "OUT", Path(work)), patch.object(
            nbt_pull_phone_logs.subprocess, "run",
            side_effect=[subprocess.CompletedProcess([], 0, stdout=data, stderr=b"")
                         for data in (b"device\n", b"1700000000.%N\n", b"log data\n")]
            + [subprocess.CompletedProcess([], 1, stdout=b"", stderr=b"USB service unavailable")],
        ):
            folder, (warning,) = nbt_pull_phone_logs.pull("adb", serial)
            self.assertIn("USB service unavailable", warning)
            self.assertEqual((folder / "logcat.txt").read_bytes(), b"log data\n")
            self.assertIn(warning, (folder / "usb-service-error.txt").read_text())
            clock = json.loads((folder / "clock.json").read_text())
            self.assertEqual(clock["phone_clock_resolution_ns"], 1000000000)

    def test_watch_preserves_filtered_events_after_the_general_snapshot(self):
        serial = "192.0.2.1:12345"
        event = b"1700000000.001  100  100 I UsbDeviceManager: USB_STATE=DISCONNECTED\n"
        with tempfile.TemporaryDirectory() as work, patch.object(nbt_pull_phone_logs, "OUT", Path(work)), patch.object(
            nbt_pull_phone_logs.subprocess, "run",
            side_effect=[subprocess.CompletedProcess([], 0, stdout=data, stderr=b"")
                         for data in (b"device\n", b"1700000000.%N\n", b"log data\n", b"USB history\n")]
            + [subprocess.TimeoutExpired("adb", 600, output=event)],
        ) as adb:
            folder, warnings = nbt_pull_phone_logs.pull("adb", serial, watch_seconds=600)
            self.assertEqual(warnings, [])
            self.assertEqual((folder / "usb-events.txt").read_bytes(), event)
            self.assertEqual((folder / "logcat.txt").read_bytes(), b"log data\n")
            command = adb.call_args.args[0]
            self.assertEqual(command[:4], ["adb", "-s", serial, "logcat"])
            self.assertNotIn("-d", command)
            self.assertIn("*:S", command)
            self.assertIn("UsbDeviceManager:V", command)
            self.assertEqual(adb.call_args.kwargs["timeout"], 600)

    def test_watch_preserves_partial_events_and_warns_when_adb_disconnects(self):
        with tempfile.TemporaryDirectory() as work, patch.object(nbt_pull_phone_logs, "OUT", Path(work)), patch.object(
            nbt_pull_phone_logs.subprocess, "run",
            side_effect=[subprocess.CompletedProcess([], 0, stdout=data, stderr=b"")
                         for data in (b"device\n", b"1700000000.%N\n", b"log data\n", b"USB history\n")]
            + [subprocess.CompletedProcess([], 1, stdout=b"partial watch\n", stderr=b"device offline")],
        ):
            folder, (warning,) = nbt_pull_phone_logs.pull("adb", "192.0.2.1:12345", watch_seconds=600)
            self.assertIn("ended before 600 seconds: device offline", warning)
            self.assertEqual((folder / "usb-events.txt").read_bytes(), b"partial watch\n")
            self.assertIn(warning, (folder / "usb-events-error.txt").read_text())

    def test_offline_phone_does_not_create_snapshot(self):
        with tempfile.TemporaryDirectory() as work, patch.object(nbt_pull_phone_logs, "OUT", Path(work)), patch.object(
            nbt_pull_phone_logs.subprocess, "run",
            return_value=subprocess.CompletedProcess([], 1, stdout=b"", stderr=b"device offline"),
        ) as adb:
            with self.assertRaisesRegex(RuntimeError, "device offline"):
                nbt_pull_phone_logs.pull("adb", "192.0.2.1:12345")
            adb.assert_called_once()
            self.assertEqual(list(Path(work).iterdir()), [])
