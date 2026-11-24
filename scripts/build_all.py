"""Build every program of the installed package from this repository in the QNX 6.5.0 VM.

What it does:
  1. checks the NBT-AAD checkout (must match NBT_AAD_REVISION, unmodified)
  2. downloads Mbed TLS 3.6.7 once (SHA-256 checked, cached in .local/), applies the legacy-UTCTime patch and the
     QNX configuration
  3. prepares the NBT-AAD core: upstream milestone-7 + scripts/aa_session_overlay.py + patches/*.patch + our
     Mbed TLS crypto backend (src/android_auto/aa_cryptor_mbedtls.cpp)
  4. sends everything plus src/ and scripts/build-qnx.sh to the VM and runs the build there
  5. fetches the programs into build/qnx-build-<time>/ and creates the plugin's text/image files
  6. optionally compares the result with an installer payload (byte by byte, also with symbols stripped)

Requirements: Python 3.8+, git, OpenSSH (ssh/scp) to the VM, the VM user must not be root.
Example:
  git clone https://github.com/Sq00ky/Supra-iDrive6-Android-Auto .local/reference-nbt-aad
  git -C .local/reference-nbt-aad checkout cd1d9f87e330f90109068d8ccc3f7fd61f945d89
  python scripts/build_all.py --host qnx-build --hmi-libs /home/nbtbuild/hmi-libs --compare installer/payload
"""
import argparse
import hashlib
import io
import json
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import uuid
from pathlib import Path

from aa_session_overlay import harden_session
from abi_provenance import capture_inputs, write_inputs
from mbedtls_legacy_time import patch_x509

ROOT = Path(__file__).resolve().parents[1]
NBT_AAD_REVISION = "cd1d9f87e330f90109068d8ccc3f7fd61f945d89"
MBEDTLS_VERSION = "3.6.7"
MBEDTLS_SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
MBEDTLS_URL = ("https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-%s/mbedtls-%s.tar.bz2"
               % (MBEDTLS_VERSION, MBEDTLS_VERSION))
# Mbed TLS options the QNX 6.5 gcc 4.4 cannot build
MBEDTLS_DISABLED = ["MBEDTLS_AESNI_C", "MBEDTLS_HAVE_ASM", "MBEDTLS_PADLOCK_C"]
# payload binaries built here (installer/payload/<name>)
PROGRAMS = {"nbt_android_auto": "rt", "native_service": "rt", "decode_runner": "rt", "fifo_http": "rt",
            "accessory_window": "rt", "nbt_knob": "rt", "nbt_hwvideo": "rt", "nbt_cluster": "rt", "audioconn": "rt",
            "libNbtAndroidAuto.so": "plugin", "nbt_reset": "rt", "nbt_key": "test",
            "nbt_a2i_beep": "test"}


def git(*args, cwd=None):
    return subprocess.run(["git", *args], cwd=cwd, check=True, stdout=subprocess.PIPE).stdout.decode().strip()


class Archive:
    """Deterministic tar archive (fixed owner, mode and an old timestamp the VM clock accepts)."""

    def __init__(self, path):
        self.tar = tarfile.open(path, "w", format=tarfile.GNU_FORMAT)

    def add(self, name, data, mode=0o600):
        info = tarfile.TarInfo(name)
        info.size, info.mode, info.mtime = len(data), mode, 946684800
        self.tar.addfile(info, io.BytesIO(data))

    def close(self):
        self.tar.close()


def text(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def add_mbedtls(archive):
    cache = ROOT / ".local" / ("mbedtls-%s.tar.bz2" % MBEDTLS_VERSION)
    cache.parent.mkdir(exist_ok=True)
    if not cache.exists():
        print("downloading", MBEDTLS_URL)
        urllib.request.urlretrieve(MBEDTLS_URL, cache)
    if hashlib.sha256(cache.read_bytes()).hexdigest() != MBEDTLS_SHA256:
        raise SystemExit("Mbed TLS archive: SHA-256 mismatch (%s)" % cache)
    top = "mbedtls-" + MBEDTLS_VERSION
    with tarfile.open(cache) as source:
        for item in source:
            path = Path(item.name)
            if path.is_absolute() or ".." in path.parts or path.parts[0] != top:
                raise SystemExit("Mbed TLS archive: unexpected path " + item.name)
            if not item.isfile():
                continue
            data = source.extractfile(item).read()
            if item.name == top + "/library/x509.c":
                data = patch_x509(data.decode()).encode()
            if item.name.endswith("/include/mbedtls/mbedtls_config.h"):
                config = data.decode()
                for option in MBEDTLS_DISABLED:
                    config, count = re.subn(r"^#define " + option + r"\s*$", "/* QNX GCC 4.4: disabled " + option + " */",
                                            config, flags=re.M)
                    if count != 1:
                        raise SystemExit("Mbed TLS config: expected exactly one " + option)
                data = config.encode()
            archive.add(item.name, data, 0o700 if item.mode & 0o100 else 0o600)


def add_nbt_aad(archive, checkout):
    if git("-C", str(checkout), "rev-parse", "HEAD") != NBT_AAD_REVISION:
        raise SystemExit("NBT-AAD checkout must be at commit " + NBT_AAD_REVISION)
    if git("-C", str(checkout), "status", "--porcelain"):
        raise SystemExit("NBT-AAD checkout must be unmodified (the patches are applied to a copy)")
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        base = checkout / "milestone-7"
        for folder in ("src", "include", "third_party/speexdsp"):
            for path in (base / folder).rglob("*"):
                if path.is_file():
                    target = work / "milestone-7" / path.relative_to(base)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(text(path))
        src = work / "milestone-7" / "src"
        # 1. checked edits of the session (stricter protocol phases, TLS error handling)
        session = src / "aa_session.cpp"
        session.write_bytes(harden_session(session.read_bytes().decode()).encode())
        # 2. Mbed TLS crypto backend instead of upstream's OpenSSL one
        (src / "aa_cryptor.cpp").write_bytes(text(ROOT / "src/android_auto/aa_cryptor_mbedtls.cpp"))
        (src / "aa_cryptor.hpp").write_bytes(text(ROOT / "src/android_auto/aa_cryptor.hpp"))
        (src / "video_acks.hpp").write_bytes(text(ROOT / "src/android_auto/video_acks.hpp"))
        # 3. patches/*.patch (paths milestone-7/src/...)
        for patch in sorted((ROOT / "patches").glob("*.patch")):
            subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=work, check=True)
            print("applied", patch.name)
        # 4. original HU_NBT audio output: A2I input_stereo_3 instead of the libasound shim the car does not have
        #    (car-verified 2026-11-15 with connection 0xb8, see docs/AA-AUDIO-PLAN.md)
        for name in ("asound_device.cpp", "asound_device.hpp"):
            (src / "audio" / name).write_bytes(text(ROOT / "src/android_auto/audio_a2i" / name))
        for path in sorted((work / "milestone-7").rglob("*")):
            if path.is_file():
                archive.add("reference/" + path.relative_to(work / "milestone-7").as_posix(), path.read_bytes())


def hmi_stub_sources(contract):
    filenames = {"libAppFramework.A.so": "app.c", "libCoreFramework.A.so": "core.c",
                 "libcpp-ne.so.4": "cpp.c"}
    stubs = contract.get("stubs") if isinstance(contract, dict) else None
    if not isinstance(stubs, dict) or set(stubs) != set(filenames):
        raise ValueError("HMI contract must specify exactly the three stub libraries")
    sources, seen = {}, set()
    for library, filename in filenames.items():
        symbols = stubs[library]
        if not isinstance(symbols, list) or not symbols or any(
                not isinstance(symbol, str) or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol)
                for symbol in symbols):
            raise ValueError("Invalid HMI stub symbols for " + library)
        if len(set(symbols)) != len(symbols) or seen.intersection(symbols):
            raise ValueError("Duplicate HMI stub symbol in " + library)
        seen.update(symbols)
        sources[filename] = "".join("void %s(void) {}\n" % symbol for symbol in symbols).encode("ascii")
    return sources


def add_repo_sources(archive):
    for folder in ("src/android_auto", "src/idrive"):
        for path in sorted((ROOT / folder).rglob("*")):
            if folder == "src/idrive" and path.relative_to(ROOT / folder).parts[:1] == ("hmi-stubs",):
                continue
            if path.is_file() and path.suffix in (".c", ".cpp", ".h", ".hpp"):
                archive.add(path.relative_to(ROOT).as_posix(), text(path))
    contract = text(ROOT / "toolchain/hmi-stubs/contract.json")
    for filename, source in hmi_stub_sources(json.loads(contract)).items():
        archive.add("toolchain/hmi-stubs/" + filename, source)
    archive.add("toolchain/hmi-stubs/contract.json", contract)
    archive.add("build-qnx.sh", text(ROOT / "scripts/build-qnx.sh"), 0o700)


def add_expected(archive, payload):
    for name, folder in PROGRAMS.items():
        path = payload / folder / name
        if path.is_file():
            archive.add("expected/" + name, path.read_bytes())


class OpenSSH:
    def __init__(self):
        self.ssh = "ssh"
        self.scp = "scp"

    def local_path(self, path, forward_slashes=False):
        return Path(path).as_posix() if forward_slashes else str(path)

    def options(self, config, known_hosts, identity):
        options = []
        if config.is_file():
            options += ["-F", self.local_path(config)]
        if known_hosts.is_file():
            options += ["-o", "UserKnownHostsFile=" + self.local_path(known_hosts, forward_slashes=True)]
        if identity:
            options += ["-i", self.local_path(identity)]
        return options + ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes"]

    def copy(self, options, source, destination):
        source = self.local_path(source) if isinstance(source, Path) else source
        destination = self.local_path(destination) if isinstance(destination, Path) else destination
        return [self.scp, "-O", "-q", *options, source, destination]


def hmi_link_mode(log, requested):
    modes = re.findall(r"^QNX_HMI_LINK=(real|stubs|missing)\s*$", log, re.MULTILINE)
    if len(modes) != 1 or modes[0] not in (("stubs",) if requested == "stubs" else ("real", "missing")):
        raise SystemExit("VM reported missing or unexpected HMI link mode; expected " + requested)
    return modes[0]


def hmi_libs_assignment(path):
    if path == "$HOME" or path.startswith("$HOME/"):
        return ' HMI_LIBS="$HOME"' + shlex.quote(path[5:]) if path != "$HOME" else ' HMI_LIBS="$HOME"'
    return " HMI_LIBS=" + shlex.quote(path)


def write_provenance(out, toolchain, mode, build_id):
    plugin = out / "programs" / "libNbtAndroidAuto.so"
    digest = hashlib.sha256(plugin.read_bytes()).hexdigest() if plugin.is_file() else None
    metadata = {"schema": 1, "toolchain": toolchain, "hmi_link": mode, "build_id": build_id,
                "plugin_sha256": digest}
    (out / "build-provenance.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


def fetch_programs(client, options, host, folder, programs, mode):
    for name in PROGRAMS:
        if name == "libNbtAndroidAuto.so" and mode == "missing":
            print("not built:", name)
            continue
        fetched = subprocess.run(client.copy(options, "%s:%s/out/%s" % (host, folder, name), programs / name),
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if fetched.returncode != 0:
            raise SystemExit("Failed to fetch %s; VM build files retained at %s:\n%s" %
                             (name, folder, fetched.stdout.decode(errors="replace")))
        if not (programs / name).is_file() or (programs / name).stat().st_size == 0:
            raise SystemExit("Fetched %s is missing or empty; VM build files retained at %s" % (name, folder))


def create_vm_build_directory(remote, folder):
    created = remote("umask 077; mkdir -p .nbt-build && mkdir " + folder)
    if created.returncode != 0:
        raise SystemExit("Cannot create unique VM build directory %s:\n%s" %
                         (folder, created.stdout.decode(errors="replace")))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="qnx-build", help="ssh host of the QNX VM (default: qnx-build)")
    parser.add_argument("--ssh-config", type=Path, default=ROOT / ".local/ssh_config")
    parser.add_argument("--known-hosts", type=Path, default=ROOT / ".local/known_hosts")
    parser.add_argument("--identity", type=Path, help="ssh private key for the VM (if not set in the ssh config)")
    parser.add_argument("--nbt-aad", type=Path, default=ROOT / ".local/reference-nbt-aad",
                        help="NBT-AAD checkout at commit %s" % NBT_AAD_REVISION[:7])
    hmi = parser.add_mutually_exclusive_group()
    hmi.add_argument("--hmi-libs", help="folder IN THE VM with libAppFramework.A.so and libCoreFramework.A.so")
    hmi.add_argument("--hmi-stubs", action="store_true", help="link the HMI plugin against link-only BMW stubs")
    parser.add_argument("--compare", type=Path, help="installer payload folder to compare with (e.g. installer/payload)")
    args = parser.parse_args()

    client = OpenSSH()
    ssh = client.options(args.ssh_config, args.known_hosts, args.identity)

    def remote(command, timeout=60):
        return subprocess.run([client.ssh, *ssh, args.host, command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout)

    probe = remote('test "`uname -s`" = QNX && test "`uname -r`" = 6.5.0 && test "`id -u`" != 0 && echo VM_OK')
    if probe.returncode != 0 or b"VM_OK" not in probe.stdout:
        raise SystemExit("The host is not a QNX 6.5.0 VM with a non-root user:\n" + probe.stdout.decode(errors="replace"))

    run_id = "qnx-build-" + time.strftime("%Y%m%dT%H%M%S") + "-" + uuid.uuid4().hex[:8]
    out = ROOT / "build" / run_id
    out.mkdir(parents=True)
    archive_path = out / "source.tar"
    archive = Archive(archive_path)
    add_mbedtls(archive)
    add_nbt_aad(archive, args.nbt_aad.resolve())
    add_repo_sources(archive)
    if args.compare:
        add_expected(archive, args.compare.resolve())
    archive.close()
    inputs = capture_inputs(ROOT, out, toolchain="qnx-sdp", selected=PROGRAMS,
                            nbt_aad=NBT_AAD_REVISION, mbedtls_sha256=MBEDTLS_SHA256)

    folder = ".nbt-build/" + run_id
    create_vm_build_directory(remote, folder)
    try:
        subprocess.run(client.copy(ssh, archive_path, "%s:%s/source.tar" % (args.host, folder)), check=True, timeout=300)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise SystemExit("Upload failed; VM build directory retained at %s" % folder) from error
    print("building in the VM:", folder, flush=True)
    mode = "stubs" if args.hmi_stubs else "real"
    hmi_libs = hmi_libs_assignment(args.hmi_libs or "$HOME/.nbt-build/hmi-libs") if mode == "real" else ""
    require_real = " HMI_REQUIRE_REAL=1" if args.hmi_libs is not None else ""
    result = remote("cd %s && tar xf source.tar && HMI_MODE=%s%s%s sh build-qnx.sh" %
                    (folder, mode, hmi_libs, require_real), timeout=1800)
    log = result.stdout.decode(errors="replace")
    (out / "build.log").write_text(log, encoding="utf-8")
    print(log[log.find("== nbt_android_auto"):] if "== nbt_android_auto" in log else log[-3000:])
    if result.returncode != 0 or "BUILD_QNX_DONE" not in log:
        cleaned = remote("rm -rf " + folder)
        if cleaned.returncode != 0:
            print("VM cleanup failed for %s: %s" % (folder, cleaned.stdout.decode(errors="replace")), file=sys.stderr)
        raise SystemExit("VM build failed, full log: " + str(out / "build.log"))

    actual_mode = hmi_link_mode(log, mode)
    programs = out / "programs"
    programs.mkdir()
    fetch_programs(client, ssh, args.host, folder, programs, actual_mode)
    # every run made a new folder in the VM and nothing removed it: the VM disk filled up ("final link failed: Bad file
    # descriptor", user 2026-11-12). The programs are fetched now, so the folder goes.
    cleaned = remote("rm -rf " + folder)
    if cleaned.returncode != 0:
        raise SystemExit("VM build succeeded but cleanup failed for %s:\n%s" %
                         (folder, cleaned.stdout.decode(errors="replace")))
    resources = out / "plugin-resources"
    subprocess.run([sys.executable, str(ROOT / "src/idrive/make_aa_entry_resources.py"), str(resources)], check=True)
    shutil.copy(ROOT / "scripts/nbt-aa-boot.sh", programs / "boot.sh")
    shutil.copy(ROOT / "scripts/nbt-aa-restart.sh", programs / "restart.sh")
    write_provenance(out, "qnx-sdp", actual_mode, run_id)
    write_inputs(out, inputs, reused=(), hmi_link=actual_mode)
    print("programs:", programs)
    print("plugin XML/manifest/texts:", resources)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
