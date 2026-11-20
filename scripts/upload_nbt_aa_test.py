#!/usr/bin/env python3
"""Upload RAM-only test binaries to /fs/ram3/nbt-video-test through an already-open SSH session.

Writes only under /fs/ram3/ (RAM, cleared on reboot) via tools/nbt_request.py. Starts nothing.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
REQUEST_SCRIPT = ROOT / "tools" / "nbt_request.py"
RAM_DIR_RE = re.compile(r"^/(fs/ram3|tmp)/[A-Za-z0-9_-][A-Za-z0-9_./-]*$")

BINARIES = [
    ("native_service", "native_service_live"),
    ("nbt_android_auto", "nbt_android_auto"),
    ("nbt_knob", "nbt_knob"),
    ("nbt_hwvideo", "nbt_hwvideo"),
    ("decode_runner", "decode_runner"),
]


def send_request(python: str, command: str, input_file: Path | None = None,
                 timeout: int = 30, dry_run: bool = False) -> None:
    if dry_run:
        print(f"[dry-run] {command}")
        if input_file is not None:
            print(f"          stdin: {input_file}")
        return
    argv = [python, str(REQUEST_SCRIPT), command, "--timeout", str(timeout)]
    if input_file is not None:
        argv += ["--input-file", str(input_file)]
    result = subprocess.run(argv)
    if result.returncode != 0:
        raise SystemExit(f"Remote command failed (exit {result.returncode}): {command}")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True, help="local directory with built binaries")
    parser.add_argument("--video-runner", type=Path, help="optional local video_runner binary")
    parser.add_argument("--audio-dir", type=Path, help="optional local directory with beep1.wav..beep6.wav")
    parser.add_argument("--remote-dir", default="/fs/ram3/nbt-video-test",
                        help="RAM-only target directory on the car (must be under /fs/ram3/ or /tmp/)")
    parser.add_argument("--python", default=sys.executable, help="Python executable for tools/nbt_request.py")
    parser.add_argument("--dry-run", action="store_true", help="print remote commands without contacting the car")
    args = parser.parse_args(argv)

    if not RAM_DIR_RE.match(args.remote_dir):
        raise SystemExit(f"Remote directory must be under /fs/ram3/ or /tmp/: {args.remote_dir}")

    source = args.source.resolve()
    if not args.dry_run:
        missing = [str(source / local_name) for local_name, _ in BINARIES if not (source / local_name).is_file()]
        if args.video_runner and not args.video_runner.is_file():
            missing.append(str(args.video_runner))
        if args.audio_dir:
            for idx in range(1, 7):
                beep = args.audio_dir / f"beep{idx}.wav"
                if not beep.is_file():
                    missing.append(str(beep))
        if missing:
            raise SystemExit("Missing local file(s):\n  " + "\n  ".join(missing))

    target = args.remote_dir
    send_request(args.python, f"mkdir -p {target}; chmod 700 {target}; ls -l {target}",
                 timeout=20, dry_run=args.dry_run)
    for local_name, remote_name in BINARIES:
        local = source / local_name
        size = local.stat().st_size if local.is_file() else 0
        remote = f"{target}/{remote_name}"
        print(f"upload {local_name} -> {remote} ({size} bytes)")
        send_request(
            args.python,
            f"cat > {remote}.tmp && chmod 700 {remote}.tmp && mv {remote}.tmp {remote} && ls -l {remote}",
            input_file=local, timeout=60, dry_run=args.dry_run,
        )
    if args.video_runner:
        send_request(args.python, f"cat > {target}/video_runner && chmod 700 {target}/video_runner",
                     input_file=args.video_runner.resolve(), timeout=60, dry_run=args.dry_run)
    if args.audio_dir:
        for idx in range(1, 7):
            send_request(args.python, f"cat > {target}/beep{idx}.wav",
                         input_file=(args.audio_dir / f"beep{idx}.wav").resolve(), timeout=60, dry_run=args.dry_run)
    send_request(
        args.python,
        f"cp /opt/mm/bin/NmeCmdLine /opt/mm/lib/cinemo/cinemo_classes.xml /opt/mm/lib/cinemo/cinemo_links.xml {target}/ "
        f"&& chmod 700 {target}/NmeCmdLine",
        timeout=30, dry_run=args.dry_run,
    )
    send_request(args.python, f"ls -l {target}", timeout=20, dry_run=args.dry_run)
    if not args.dry_run:
        print("Expected sizes:")
        for local_name, remote_name in BINARIES:
            print(f"  {remote_name:<22} {(source / local_name).stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
