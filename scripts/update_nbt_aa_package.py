#!/usr/bin/env python3
"""Update the installed Android Auto package on the car through an already-open SSH session.

Uses tools/nbt_request.py (paired with tools/nbt_ssh_session.py). Each file is uploaded to RAM,
copied as .tmp, byte-compared with cmp -s, chmod'd, and atomically renamed. The running service
is not stopped; new files take effect on the next iDrive start.
"""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
REQUEST_SCRIPT = ROOT / "tools" / "nbt_request.py"
RT_DIR = "/var/opt/hmi/data/rw/NbtAndroidAutoRT"
PLUGIN_DIR = "/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto"
STAGE_DIR = "/fs/ram3/nbt-aa-update"


def package_files(source: Path) -> list[tuple[Path, str, str]]:
    return [
        (source / "native_service", f"{RT_DIR}/native_service", "700"),
        (source / "nbt_android_auto", f"{RT_DIR}/nbt_android_auto", "700"),
        (source / "nbt_knob", f"{RT_DIR}/nbt_knob", "700"),
        (source / "nbt_hwvideo", f"{RT_DIR}/nbt_hwvideo", "700"),
        (source / "decode_runner", f"{RT_DIR}/decode_runner", "700"),
        (source / "nbt_cluster", f"{RT_DIR}/nbt_cluster", "700"),
        (source / "NbtAndroidAuto" / "libNbtAndroidAuto.so", f"{PLUGIN_DIR}/libNbtAndroidAuto.so", "755"),
        (source / "NbtAndroidAuto" / "NbtAndroidAuto.xml", f"{PLUGIN_DIR}/NbtAndroidAuto.xml", "644"),
        (source / "NbtAndroidAuto" / "NbtAndroidAuto_ALL_Texts.zip",
         f"{PLUGIN_DIR}/NbtAndroidAuto_ALL_Texts.zip", "644"),
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
    parser.add_argument("--source", type=Path, required=True, help="local build directory containing binaries and NbtAndroidAuto/")
    parser.add_argument("--python", default=sys.executable, help="Python executable for tools/nbt_request.py")
    parser.add_argument("--res720", choices=("keep", "on", "off"), default="keep")
    parser.add_argument("--dry-run", action="store_true", help="print remote commands without contacting the car")
    args = parser.parse_args(argv)

    source = args.source.resolve()
    files = package_files(source)
    scripts = [
        (ROOT / "scripts" / "nbt-aa-boot.sh", f"{RT_DIR}/boot.sh"),
        (ROOT / "scripts" / "nbt-aa-restart.sh", f"{RT_DIR}/restart.sh"),
    ]
    if not args.dry_run:
        missing = [str(local) for local, _, _ in files if not local.is_file()]
        missing += [str(local) for local, _ in scripts if not local.is_file()]
        if missing:
            raise SystemExit("Missing local file(s):\n  " + "\n  ".join(missing))

    send_request(
        args.python,
        f"mkdir -p {STAGE_DIR} && test -d {RT_DIR} && test -d {PLUGIN_DIR} && echo TARGETS_OK",
        timeout=20, dry_run=args.dry_run,
    )
    for local, target, mode in files:
        name = local.name
        print(f"update {name} -> {target}")
        send_request(
            args.python,
            f"cat > {STAGE_DIR}/{name} && cp {STAGE_DIR}/{name} {target}.tmp "
            f"&& cmp -s {STAGE_DIR}/{name} {target}.tmp && chmod {mode} {target}.tmp "
            f"&& mv {target}.tmp {target} && cmp -s {STAGE_DIR}/{name} {target} && echo OK {name}",
            input_file=local, timeout=90, dry_run=args.dry_run,
        )
    for local, target in scripts:
        name = Path(target).name
        send_request(
            args.python,
            f"tr -d '\\015' > {STAGE_DIR}/{name} && cp {STAGE_DIR}/{name} {target}.tmp "
            f"&& cmp -s {STAGE_DIR}/{name} {target}.tmp && chmod 700 {target}.tmp "
            f"&& mv {target}.tmp {target} && echo OK {name}",
            input_file=local, timeout=30, dry_run=args.dry_run,
        )
    if args.res720 == "on":
        send_request(args.python, f"touch {RT_DIR}/res720 && echo res720 ON", timeout=20, dry_run=args.dry_run)
    elif args.res720 == "off":
        send_request(args.python, f"rm -f {RT_DIR}/res720 && echo res720 OFF", timeout=20, dry_run=args.dry_run)
    send_request(args.python, f"rm -r {STAGE_DIR}; ls -l {RT_DIR} {PLUGIN_DIR}", timeout=20, dry_run=args.dry_run)
    print("Done. Active after the next iDrive start.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
