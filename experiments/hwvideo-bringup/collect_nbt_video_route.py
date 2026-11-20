#!/usr/bin/env python3
"""Read-only vehicle inventory of the stock video route via an open SSH session.

Runs experiments/hwvideo-bringup/Inspect-NbtVideoRoute.sh section by section through
tools/nbt_request.py (paired with tools/nbt_ssh_session.py). Never retries automatically.
"""
import argparse
import datetime as dt
import json
from pathlib import Path
import subprocess
import sys

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parent.parent
REQUEST_SCRIPT = ROOT / "tools" / "nbt_request.py"
INSPECT_SCRIPT = SCRIPT_DIR / "Inspect-NbtVideoRoute.sh"
STATUS_FILE = ROOT / ".local" / "ssh-session" / "status.json"
ALL_SECTIONS = (
    "procs", "libs", "devs", "graphics", "cfg-cinemo",
    "cfg-mme", "cfg-generic", "cfg-search", "use", "slog",
)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--python", default=sys.executable, help="Python executable for tools/nbt_request.py")
    parser.add_argument("--sections", nargs="+", choices=ALL_SECTIONS, default=list(ALL_SECTIONS))
    parser.add_argument("--out-dir", type=Path, help="directory for collected section outputs")
    parser.add_argument("--dry-run", action="store_true", help="print commands without contacting the vehicle")
    args = parser.parse_args(argv)

    out_dir = args.out_dir or (
        ROOT / "build" / "native-video-route" / ("vehicle-" + dt.datetime.now().strftime("%Y%m%dT%H%M%S"))
    )
    if not args.dry_run:
        if not STATUS_FILE.is_file():
            raise SystemExit(
                f"SSH session status missing at {STATUS_FILE}. "
                "Start tools/nbt_ssh_session.py in your own terminal first."
            )
        state = json.loads(STATUS_FILE.read_text(encoding="utf-8")).get("state")
        if state not in ("ready", "busy"):
            raise SystemExit(
                f"SSH session is '{state}', not ready. "
                "Run: python3 tools/nbt_ssh_session.py (type the password yourself)."
            )
        out_dir.mkdir(parents=True, exist_ok=True)

    for section in args.sections:
        command = f"tr -d '\\015' | sh -s -- {section}"
        out = out_dir / f"{section}.txt"
        print(f"== {section} -> {out}")
        if args.dry_run:
            print(f"   [dry-run] {command}  (stdin: {INSPECT_SCRIPT})")
            continue
        result = subprocess.run([
            args.python, "-I", str(REQUEST_SCRIPT), command,
            "--input-file", str(INSPECT_SCRIPT),
            "--timeout", "40",
            "--output-file", str(out),
        ])
        if result.returncode != 0:
            print(f"WARNING: {section} failed (exit {result.returncode}); not retried automatically.",
                  file=sys.stderr)
    if not args.dry_run:
        print(f"Done. Evidence directory: {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
