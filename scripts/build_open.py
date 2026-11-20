"""Build the head-unit programs with the open QNX 6.5.0 toolchain (no QNX SDP, no VM).

Prepares the sources exactly like scripts/build_all.py (NBT-AAD at 4328560 + overlay + patches, Mbed TLS 3.6.7 with
the QNX config) and runs scripts/build-open.sh in the toolchain container (toolchain/qnx-open). The toolchain must be
built once first, see toolchain/README.md.

Output: build/open-build-<time>/programs (+ plugin-resources), the same layout as a build_all.py run, so
installer/make_payload.py --built can use it. The HMI plugin is linked against experimental symbol stubs.
accessory_window needs QNX resource-manager headers, which the open sysroot does not have; --reuse copies it
from an existing installer payload instead.

Example:
  python3 scripts/build_open.py --reuse .local/release-payload/NBT-AndroidAuto-Python-2026.11.10.2/payload
  python3 installer/make_payload.py 2026.11.16-open --built build/open-build-<time> \
      --pem-dir .local/release-payload/NBT-AndroidAuto-Python-2026.11.10.2/payload/rt
"""
import argparse
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_all import PROGRAMS, ROOT, Archive, add_mbedtls, add_nbt_aad, add_repo_sources  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nbt-aad", type=Path, default=ROOT / ".local/reference-nbt-aad",
                        help="NBT-AAD checkout at commit 4328560")
    parser.add_argument("--programs", help="space-separated list (default: all supported programs including HMI plugin)")
    parser.add_argument("--reuse", type=Path,
                        help="installer payload folder (rt/, plugin/) to take programs from that were not built")
    args = parser.parse_args()

    run_id = "open-build-" + time.strftime("%Y%m%dT%H%M%S")
    out = ROOT / "build" / run_id
    work = out / "work"
    work.mkdir(parents=True)
    archive_path = out / "source.tar"
    archive = Archive(archive_path)
    add_mbedtls(archive)
    add_nbt_aad(archive, args.nbt_aad.resolve())
    add_repo_sources(archive)
    archive.close()
    with tarfile.open(archive_path) as tar:
        tar.extractall(work)
    shutil.copy(ROOT / "scripts/build-open.sh", work / "build-open.sh")

    env = ["-e", "PROGRAMS=" + args.programs] if args.programs else []
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
    for path in sorted((work / "out").iterdir()):
        shutil.copy(path, programs / path.name)
    if args.reuse:
        for name, folder in PROGRAMS.items():
            source = args.reuse / folder / name
            if not (programs / name).exists() and source.is_file():
                shutil.copy(source, programs / name)
                print("reused from", args.reuse.name + ":", name)
    resources = out / "plugin-resources"
    subprocess.run([sys.executable, str(ROOT / "src/idrive/make_aa_entry_resources.py"), str(resources)], check=True)
    shutil.copy(ROOT / "scripts/nbt-aa-boot.sh", programs / "boot.sh")
    shutil.copy(ROOT / "scripts/nbt-aa-restart.sh", programs / "restart.sh")
    print("programs:", programs)
    print("plugin XML/manifest/texts:", resources)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
