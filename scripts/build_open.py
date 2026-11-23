"""Build the head-unit programs with the open QNX 6.5.0 toolchain (no QNX SDP, no VM).

Prepares the sources exactly like scripts/build_all.py (pinned NBT-AAD + overlay + patches, Mbed TLS 3.6.7 with
the QNX config) and runs scripts/build-open.sh in the toolchain container (toolchain/qnx-open). The toolchain must be
built once first, see toolchain/README.md.

Output: build/open-build-<time>-<id>/programs (+ plugin-resources), the same layout as a build_all.py run, so
installer/make_payload.py --built can use it. The HMI plugin links against symbol stubs by default; --hmi-libs
selects real BMW framework libraries. Both open build modes remain experimental.
The open resource-manager declarations used by accessory_window remain experimental and require runtime validation.

Example:
  python3 scripts/build_open.py --reuse .local/release-payload/NBT-AndroidAuto-Python-2026.11.10.2/payload
  python3 installer/make_payload.py 2026.11.16-open --built build/open-build-<time>-<id> \
      --pem-dir .local/release-payload/NBT-AndroidAuto-Python-2026.11.10.2/payload/rt
"""
import argparse
import shutil
import subprocess
import sys
import tarfile
import time
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from abi_provenance import capture_inputs, write_inputs  # noqa: E402
from build_all import (MBEDTLS_SHA256, NBT_AAD_REVISION, PROGRAMS, ROOT, Archive, add_mbedtls, add_nbt_aad, add_repo_sources,
                       write_provenance)  # noqa: E402

OPEN_PROGRAMS = ("nbt_android_auto", "native_service", "decode_runner", "fifo_http", "accessory_window", "nbt_knob",
                 "nbt_hwvideo", "nbt_cluster", "audioconn", "nbt_key", "nbt_reset", "libNbtAndroidAuto.so")
HMI_LIBRARIES = ("libAppFramework.A.so", "libCoreFramework.A.so")


def select_programs(value):
    if value is None:
        return OPEN_PROGRAMS
    names = value.split()
    if not names:
        raise SystemExit("--programs must name at least one program")
    unknown = set(names) - set(OPEN_PROGRAMS)
    if unknown:
        raise SystemExit("Unknown open-build program(s): " + ", ".join(sorted(unknown)))
    return tuple(dict.fromkeys(names))


def copy_built_programs(source, destination, selected):
    for name in selected:
        path = source / name
        if not path.is_file() or path.stat().st_size == 0:
            raise SystemExit("Open build did not produce a nonempty " + name)
        shutil.copy(path, destination / name)


def hmi_link_mode(programs, selected, requested):
    if "libNbtAndroidAuto.so" in selected:
        return requested
    if (programs / "libNbtAndroidAuto.so").is_file():
        return "reused"
    return "missing"


def hmi_build_options(libraries):
    if libraries is None:
        return "stubs", []
    directory = libraries.resolve()
    if not directory.is_dir() or any(not (directory / name).is_file() or
                                     (directory / name).stat().st_size == 0 for name in HMI_LIBRARIES):
        raise SystemExit("--hmi-libs must contain nonempty " + " and ".join(HMI_LIBRARIES))
    if ":" in str(directory):
        raise SystemExit("--hmi-libs path cannot contain ':' for a Docker bind mount")
    return "real", ["-v", str(directory) + ":/hmi-libs:ro", "-e", "HMI_LIBS=/hmi-libs"]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nbt-aad", type=Path, default=ROOT / ".local/reference-nbt-aad",
                        help="NBT-AAD checkout at commit %s" % NBT_AAD_REVISION[:7])
    parser.add_argument("--programs", help="space-separated list (default: all supported programs including HMI plugin)")
    hmi = parser.add_mutually_exclusive_group()
    hmi.add_argument("--hmi-libs", type=Path, help="local folder with real BMW AppFramework and CoreFramework libraries")
    hmi.add_argument("--hmi-stubs", action="store_true", help="link the plugin with generated BMW stubs (default)")
    parser.add_argument("--reuse", type=Path,
                        help="installer payload folder (rt/, plugin/) to take programs from that were not built")
    parser.add_argument("--output-dir", type=Path,
                        help="new output directory inside build/ (default: unique timestamped directory)")
    args = parser.parse_args()

    selected = select_programs(args.programs)
    if args.hmi_libs is not None and "libNbtAndroidAuto.so" not in selected:
        raise SystemExit("--hmi-libs requires selecting libNbtAndroidAuto.so")
    requested_mode, hmi_options = hmi_build_options(args.hmi_libs)
    run_id = "open-build-" + time.strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8]
    build_dir = (ROOT / "build").resolve()
    out = (ROOT / args.output_dir).resolve() if args.output_dir else build_dir / run_id
    if out == build_dir or build_dir not in out.parents:
        raise SystemExit("--output-dir must be a new directory inside build/")
    out.mkdir(parents=True, exist_ok=False)
    work = out / "work"
    work.mkdir()
    archive_path = out / "source.tar"
    archive = Archive(archive_path)
    add_mbedtls(archive)
    add_nbt_aad(archive, args.nbt_aad.resolve())
    add_repo_sources(archive)
    archive.close()
    with tarfile.open(archive_path) as tar:
        tar.extractall(work)
    shutil.copy(ROOT / "scripts/build-open.sh", work / "build-open.sh")
    inputs = capture_inputs(ROOT, out, toolchain="open-experimental", selected=selected,
                            nbt_aad=NBT_AAD_REVISION, mbedtls_sha256=MBEDTLS_SHA256)

    env = ["-e", "PROGRAMS=" + " ".join(selected), "-e", "HMI_MODE=" + requested_mode, *hmi_options]
    rel = work.relative_to(ROOT).as_posix()
    command = ["docker", "run", "--rm", "--platform", "linux/amd64",
               "-v", "%s/build/qnx-open:/opt/qnx-open" % ROOT, "-v", "%s:/repo" % ROOT, "-w", "/repo/" + rel,
               "-e", "PATH=/opt/qnx-open/bin:/usr/bin:/bin", *env, "qnx-open-host", "sh", "build-open.sh"]
    print("building in the toolchain container:", rel, flush=True)
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = result.stdout.decode(errors="replace")
    (out / "build.log").write_text(log, encoding="utf-8")
    print(log[-3000:])
    if result.returncode != 0 or "BUILD_OPEN_DONE" not in log:
        raise SystemExit("build failed, full log: " + str(out / "build.log"))

    programs = out / "programs"
    programs.mkdir()
    copy_built_programs(work / "out", programs, selected)
    reused = []
    if args.reuse:
        for name, folder in PROGRAMS.items():
            source = args.reuse / folder / name
            if not (programs / name).exists() and source.is_file():
                if source.stat().st_size == 0:
                    raise SystemExit("Cannot reuse empty program: " + str(source))
                shutil.copy(source, programs / name)
                reused.append(name)
                print("reused from", args.reuse.name + ":", name)
    resources = out / "plugin-resources"
    subprocess.run([sys.executable, str(ROOT / "src/idrive/make_aa_entry_resources.py"), str(resources)], check=True)
    shutil.copy(ROOT / "scripts/nbt-aa-boot.sh", programs / "boot.sh")
    shutil.copy(ROOT / "scripts/nbt-aa-restart.sh", programs / "restart.sh")
    hmi_link = hmi_link_mode(programs, selected, requested_mode)
    write_provenance(out, "open-experimental", hmi_link, run_id)
    write_inputs(out, inputs, reused=reused, hmi_link=hmi_link)
    print("programs:", programs)
    print("plugin XML/manifest/texts:", resources)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
