#!/usr/bin/env python3
"""Save Android logs, USB history and a clock sample from an already connected phone."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".local" / "phone-logs"
WATCH_TAGS = ("UsbDeviceManager", "UsbPortManager", "UsbService", "UsbGadgetAidl", "UsbGadgetHidl",
              "android.hardware.usb.gadget-service.qti", "CAR.GAL.GAL.LITE", "CAR.CONMAN.LITE", "CAR.SERVICE")


def adb_read(adb, serial, *args):
    result = subprocess.run([adb, "-s", serial, *args], capture_output=True, timeout=30)
    if result.returncode:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise RuntimeError("adb %s failed: %s" % (args[0], detail or "exit %d" % result.returncode))
    return result.stdout


def write_private(path, data):
    fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "wb") as file:
        file.write(data)


def watch_usb(adb, serial, seconds):
    command = [adb, "-s", serial, "logcat", "-T", "1", "--max-count=10000", "-b", "main", "-b", "system",
               "-v", "epoch", "*:S", *(tag + ":V" for tag in WATCH_TAGS)]
    try:
        result = subprocess.run(command, capture_output=True, timeout=seconds)
    except subprocess.TimeoutExpired as exc:
        return exc.stdout or b"", None
    detail = result.stderr.decode("utf-8", "replace").strip()
    warning = "USB event watch ended before %d seconds" % seconds
    if result.returncode:
        warning += ": " + (detail or "adb exit %d" % result.returncode)
    else:
        warning += " (ADB exited or reached the 10,000-entry limit)"
    return result.stdout, warning


def pull(adb, serial, watch_seconds=0):
    if adb_read(adb, serial, "get-state").strip() != b"device":
        raise RuntimeError("ADB transport is not online")
    before = time.time_ns()
    clock = adb_read(adb, serial, "shell", "date", "+%s.%N").strip()
    after = time.time_ns()
    match = re.fullmatch(rb"([0-9]+)(?:\.([0-9]{1,9})|\.%N)?", clock)
    if not match:
        raise RuntimeError("Could not read the phone's epoch clock")
    seconds, fraction = int(match[1]), match[2] or b""
    phone_ns = seconds * 10**9 + int(fraction.ljust(9, b"0"))
    logs = adb_read(adb, serial, "logcat", "-d", "-t", "5000", "-b", "main", "-b", "system", "-b", "events",
                    "-v", "epoch")
    if not logs.strip():
        raise RuntimeError("Phone logcat returned no data")
    warnings = []
    try:
        usb = adb_read(adb, serial, "shell", "dumpsys", "usb")
        if not usb.strip():
            raise RuntimeError("dumpsys usb returned no data")
    except (RuntimeError, subprocess.TimeoutExpired) as exc:
        usb = None
        warnings.append("USB history unavailable: %s" % exc)

    OUT.mkdir(parents=True, mode=0o700, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    with tempfile.TemporaryDirectory(prefix=".collect-", dir=OUT) as work:
        # Publish only a complete snapshot, with restrictive permissions inherited from mkdtemp.
        folder = Path(work) / ("phone-" + stamp + "-" + Path(work).name.removeprefix(".collect-"))
        folder.mkdir(mode=0o700)
        write_private(folder / "clock.json", (json.dumps({
            "phone_epoch_seconds": seconds,
            "phone_epoch_ns": phone_ns,
            "phone_clock_resolution_ns": 10**(9 - len(fraction)),
            "host_epoch_ns_before": before,
            "host_epoch_ns_after": after,
        }, indent=2) + "\n").encode())
        write_private(folder / "logcat.txt", logs)
        if usb is None:
            write_private(folder / "usb-service-error.txt", (warnings[0] + "\n").encode())
        else:
            write_private(folder / "usb-service.txt", usb)
        if watch_seconds:
            events, warning = watch_usb(adb, serial, watch_seconds)
            write_private(folder / "usb-events.txt", events)
            if warning:
                write_private(folder / "usb-events-error.txt", (warning + "\n").encode())
                warnings.append(warning)
        return folder.rename(OUT / folder.name), warnings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial", help="online transport from adb devices (wireless: the connect IP:port)")
    parser.add_argument("--watch-seconds", type=int, default=0,
                        help="also record filtered USB/Android Auto events for up to 1800 seconds (default: off)")
    args = parser.parse_args()
    if not 0 <= args.watch_seconds <= 1800:
        parser.error("--watch-seconds must be between 0 and 1800")
    adb = shutil.which("adb")
    if not adb:
        parser.exit(1, "adb not found. Install Android SDK Platform-Tools or put it on PATH.\n")
    try:
        folder, warnings = pull(adb, args.serial, args.watch_seconds)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        parser.exit(1, "%s\n" % exc)
    print("Private phone logs: %s" % folder)
    for warning in warnings:
        print(warning, file=sys.stderr)


if __name__ == "__main__":
    main()
