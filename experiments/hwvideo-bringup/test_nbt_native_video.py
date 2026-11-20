#!/usr/bin/env python3
"""Staging and bounded test of a native video device through stock Cinemo (RAM-only).

Uses tools/nbt_request.py (paired with tools/nbt_ssh_session.py). Do not run `run` before
collecting and analyzing the stock video route inventory (collect_nbt_video_route.py).
"""
import argparse
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
REQUEST_SCRIPT = ROOT / "tools" / "nbt_request.py"
STATUS_FILE = ROOT / ".local" / "ssh-session" / "status.json"
LIBS = "LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/cinemo:/lib:/usr/lib"

DEVICE_RE = re.compile(r"^Nme[A-Za-z0-9]+$")
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9_=&?.:/,\-]*$")
RAM_DIR_RE = re.compile(r"^/(fs/ram3|tmp)/[A-Za-z0-9_-][A-Za-z0-9_./-]*$")
ABS_PATH_RE = re.compile(r"^/[A-Za-z0-9_./-]+$")


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bounded_int(low: int, high: int):
    def parse(value: str) -> int:
        num = int(value)
        if not (low <= num <= high):
            raise argparse.ArgumentTypeError(f"must be between {low} and {high}")
        return num
    return parse


def send_request(python: str, command: str, input_file: Path | None = None,
                 output_file: Path | None = None, timeout: int = 30, dry_run: bool = False) -> None:
    if dry_run:
        print(f"[dry-run] {command}")
        if input_file is not None:
            print(f"          stdin: {input_file}")
        return
    argv = [python, "-I", str(REQUEST_SCRIPT), command, "--timeout", str(timeout)]
    if input_file is not None:
        argv += ["--input-file", str(input_file)]
    if output_file is not None:
        argv += ["--output-file", str(output_file)]
    result = subprocess.run(argv)
    if result.returncode != 0:
        raise SystemExit(f"Remote command failed (exit {result.returncode}). Not retried automatically: {command}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("stage", "run", "cleanup", "status"))
    parser.add_argument("--python", default=sys.executable)
    parser.add_argument("--artifacts", type=Path, default=ROOT / "build" / "native-video-test",
                        help="local directory with video_runner and pattern clips")
    parser.add_argument("--device", default="", help="video device class from stock inventory (e.g. NmeDeviceScreen)")
    parser.add_argument("--device-id", default="", help="URL-parameter style device identifier")
    parser.add_argument("--layer", type=bounded_int(-1, 64), default=-1)
    parser.add_argument("--x", type=bounded_int(-1, 4096), default=-1)
    parser.add_argument("--y", type=bounded_int(-1, 4096), default=-1)
    parser.add_argument("--left", type=bounded_int(-1, 4096), default=-1)
    parser.add_argument("--top", type=bounded_int(-1, 4096), default=-1)
    parser.add_argument("--clip", choices=("pattern-800x480.mp4", "pattern-800x480.h264"), default="pattern-800x480.mp4")
    parser.add_argument("--seconds", type=bounded_int(1, 20), default=8)
    parser.add_argument("--repeat", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--remote-dir", default="/fs/ram3/nbt-video-test")
    parser.add_argument("--stock-bin", default="/opt/mm/bin/NmeCmdLine")
    parser.add_argument("--stock-cinemo-dir", default="/opt/mm/lib/cinemo")
    args = parser.parse_args(argv)

    if not RAM_DIR_RE.match(args.remote_dir):
        raise SystemExit(f"Remote directory must be under /fs/ram3/ or /tmp/: {args.remote_dir}")
    if not ABS_PATH_RE.match(args.stock_bin) or not ABS_PATH_RE.match(args.stock_cinemo_dir):
        raise SystemExit("Stock paths must be valid absolute paths.")
    if args.device and not DEVICE_RE.match(args.device):
        raise SystemExit(f"Invalid --device name: {args.device}")
    if not DEVICE_ID_RE.match(args.device_id):
        raise SystemExit(f"Invalid --device-id value: {args.device_id}")

    if not args.dry_run:
        if not STATUS_FILE.is_file():
            raise SystemExit(f"SSH session status missing at {STATUS_FILE}. Start tools/nbt_ssh_session.py first.")
        state = json.loads(STATUS_FILE.read_text(encoding="utf-8")).get("state")
        if state not in ("ready", "busy"):
            raise SystemExit(f"SSH session is '{state}'. Start tools/nbt_ssh_session.py yourself first.")

    remote = args.remote_dir
    artifacts = args.artifacts.resolve()
    evidence = artifacts / ("vehicle-" + dt.datetime.now().strftime("%Y%m%dT%H%M%S"))

    if args.action == "status":
        send_request(
            args.python,
            f"ls -l {remote} 2>&1; pidin ar | grep -e NmeCmdLine -e video_runner | grep -v grep; echo STATUS_END",
            dry_run=args.dry_run,
        )
    elif args.action == "stage":
        names = ("video_runner", "pattern-800x480.mp4", "pattern-800x480.h264")
        if not args.dry_run:
            missing = [str(artifacts / n) for n in names if not (artifacts / n).is_file()]
            if missing:
                raise SystemExit("Missing artifact(s):\n  " + "\n  ".join(missing))
        send_request(
            args.python,
            f"mkdir {remote} && chmod 700 {remote} && cp {args.stock_bin} "
            f"{args.stock_cinemo_dir}/cinemo_classes.xml {args.stock_cinemo_dir}/cinemo_links.xml {remote}/ "
            f"&& ls -l {remote}",
            dry_run=args.dry_run,
        )
        for name in names:
            send_request(args.python, f"cat > {remote}/{name}", input_file=artifacts / name,
                         timeout=60, dry_run=args.dry_run)
        send_request(args.python, f"chmod 700 {remote}/video_runner {remote}/NmeCmdLine; ls -l {remote}",
                     dry_run=args.dry_run)
        if not args.dry_run:
            evidence.mkdir(parents=True, exist_ok=True)
            for name in names:
                back = evidence / f"{name}.readback"
                send_request(args.python, f"cat {remote}/{name}", output_file=back, timeout=60)
                same = sha256_file(back) == sha256_file(artifacts / name)
                print(f"{name}: read-back identical = {same}")
                if not same:
                    raise SystemExit(f"Read-back mismatch for {name}; do not run tests. Run `cleanup`.")
    elif args.action == "run":
        if not args.device:
            raise SystemExit("`run` requires --device (from the stock inventory).")
        client = f"-a0 -wd 0 -wi 0 -wm 0 -vd {args.device}"
        if args.device_id:
            client += f" -vi '{args.device_id}'"
        if args.layer >= 0:
            client += f" -vl {args.layer}"
        if args.x >= 0:
            client += f" -x {args.x}"
        if args.y >= 0:
            client += f" -y {args.y}"
        if args.left >= 0:
            client += f" -xx {args.left}"
        if args.top >= 0:
            client += f" -yy {args.top}"
        if args.repeat:
            client += " -r"
        client += f" -i {remote}/{args.clip}"
        command = (
            f"cd {remote} && mkdir run.lock && {{ {LIBS} ./video_runner {args.seconds} ./NmeCmdLine {client} "
            f"> run.log 2>&1; echo RUNNER_RC=$?; rm -r run.lock; sed 80q run.log; }}"
        )
        print(f"Client arguments: {client}")
        out_file = evidence / "run-output.txt"
        if not args.dry_run:
            evidence.mkdir(parents=True, exist_ok=True)
        send_request(args.python, command, output_file=out_file, timeout=args.seconds + 20, dry_run=args.dry_run)
        if not args.dry_run and out_file.is_file():
            sys.stdout.write(out_file.read_text(encoding="utf-8", errors="replace"))
    elif args.action == "cleanup":
        send_request(
            args.python,
            f"cd {remote} && rm -f video_runner pattern-800x480.mp4 pattern-800x480.h264 NmeCmdLine "
            f"cinemo_classes.xml cinemo_links.xml run.log; rm -r run.lock 2>/dev/null; "
            f"cd / && rm -r {remote} && echo CLEANUP_OK",
            dry_run=args.dry_run,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
