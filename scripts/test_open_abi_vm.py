#!/usr/bin/env python3
"""Compare open GCC 4.4.2 and SDP ABI probes on a trusted host-only QNX VM."""
import argparse
import io
import ipaddress
import json
from pathlib import Path
import subprocess
import tarfile
import uuid

from abi_provenance import digest_file
from build_all import ROOT, hmi_stub_sources

PROBE = ROOT / "toolchain/probes/abi_probe.cpp"
SPAWN_HEADER = ROOT / "src/idrive/qnx_spawn_compat.h"
RESOURCE_PROBE = ROOT / "toolchain/probes/resource_abi_probe.c"
RESOURCE_LOCAL_PROBE = ROOT / "toolchain/probes/resource_local_probe.c"
RESOURCE_MESSAGE_PROBE = ROOT / "toolchain/probes/resource_message_probe.c"
HMI_PROBE = ROOT / "toolchain/probes/hmi_loader_probe.c"
HMI_CONTRACT = ROOT / "toolchain/hmi-stubs/contract.json"
HMI_MATRIX = (("open-real", "open-experimental", "real"), ("open-stubs", "open-experimental", "stubs"),
              ("sdp-real", "qnx-sdp", "real"), ("sdp-stubs", "qnx-sdp", "stubs"))
RUN = """\
set -eu
export QNX_HOST=/usr/qnx650/host/qnx6/x86 QNX_TARGET=/usr/qnx650/target/qnx6
export PATH="$QNX_HOST/usr/bin:$PATH"
qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x abi_probe.cpp -o probe_sdp
qcc -V4.4.2,gcc_ntox86 -Wall resource_abi_probe.c -o resource_sdp
qcc -V4.4.2,gcc_ntox86 -Wall resource_local_probe.c -o resource_local_sdp
qcc -V4.4.2,gcc_ntox86 -Wall resource_message_probe.c -o resource_message_sdp
chmod 700 probe_open probe_sdp resource_open resource_sdp resource_local_open resource_local_sdp resource_message_open resource_message_sdp
test ! -e abi-probe-file-that-must-not-exist
./probe_sdp > sdp.txt
./probe_open > open.txt
cat sdp.txt
echo ABI_SDP_DONE
cat open.txt
echo ABI_OPEN_DONE
cmp sdp.txt open.txt
echo ABI_PROBES_MATCH
./resource_sdp > resource_sdp.txt
./resource_open > resource_open.txt
cat resource_sdp.txt
cmp resource_sdp.txt resource_open.txt
echo RESOURCE_ABI_PROBES_MATCH
./resource_local_sdp > resource_local_sdp.txt
local_flag=$(sed -n 's/^local-only flag=\\([0-9][0-9]*\\) attached timeouts=3$/\\1/p' resource_local_sdp.txt)
test -n "$local_flag"
./resource_local_open "$local_flag" > resource_local_open.txt
cat resource_local_sdp.txt
cmp resource_local_sdp.txt resource_local_open.txt
echo RESOURCE_LOCAL_PROBES_MATCH
./resource_message_sdp "$local_flag" > resource_message_sdp.txt
./resource_message_open "$local_flag" > resource_message_open.txt
cat resource_message_sdp.txt
cmp resource_message_sdp.txt resource_message_open.txt
echo RESOURCE_MESSAGE_PROBES_MATCH
"""
HMI_RUN = """\
qcc -V4.4.2,gcc_ntox86 -shared -fPIC -Wl,-soname,libAppFramework.A.so hmi-app.c -o libAppFramework.A.so
qcc -V4.4.2,gcc_ntox86 -shared -fPIC -Wl,-soname,libCoreFramework.A.so hmi-core.c -o libCoreFramework.A.so
qcc -V4.4.2,gcc_ntox86 -Wall hmi_loader_probe.c -o hmi_loader_sdp
chmod 700 hmi_loader_sdp hmi_loader_open
export LD_PRELOAD=
export LD_LIBRARY_PATH="$PWD"
for plugin in hmi-open-real.so hmi-open-stubs.so hmi-sdp-real.so hmi-sdp-stubs.so; do
    for loader in hmi_loader_sdp hmi_loader_open; do
        ./"$loader" "./$plugin" > "$loader-$plugin.txt"
        cmp hmi_loader_sdp-hmi-open-real.so.txt "$loader-$plugin.txt"
    done
done
cat hmi_loader_sdp-hmi-open-real.so.txt
echo HMI_MATRIX_PROBES_MATCH
"""
HMI_REAL_RUN = """\
for library in libAppFramework.A.so libCoreFramework.A.so; do
    test -r "$HOME/.nbt-build/hmi-libs/$library"
    openssl dgst -sha256 "$HOME/.nbt-build/hmi-libs/$library"
done > real-hmi-libraries.sha256
export LD_LIBRARY_PATH="$HOME/.nbt-build/hmi-libs"
for plugin in hmi-open-real.so hmi-open-stubs.so hmi-sdp-real.so hmi-sdp-stubs.so; do
    for loader in hmi_loader_sdp hmi_loader_open; do
        ./"$loader" "./$plugin" > "real-$loader-$plugin.txt"
        cmp real-hmi_loader_sdp-hmi-open-real.so.txt "real-$loader-$plugin.txt"
    done
done
cat real-hmi_loader_sdp-hmi-open-real.so.txt
echo HMI_REAL_MATRIX_PROBES_MATCH
"""


def hmi_matrix_builds(directories):
    if len(directories) != len(HMI_MATRIX):
        raise ValueError("Exactly four HMI builds are required")
    results, common = [], None
    for directory, (name, toolchain, mode) in zip(directories, HMI_MATRIX):
        metadata = (directory / "abi-inputs.json").read_bytes()
        inputs = json.loads(metadata)
        record = inputs.get("artifacts", {}).get("libNbtAndroidAuto.so", {})
        binary = directory / "programs/libNbtAndroidAuto.so"
        if (inputs.get("toolchain") != toolchain or inputs.get("hmi_link") != mode or
                inputs.get("worktree_dirty") is not False or record.get("origin") != "built" or
                "libNbtAndroidAuto.so" not in inputs.get("selected", []) or
                not binary.is_file() or digest_file(binary) != record.get("sha256")):
            raise ValueError("Invalid or mismatched " + name + " build: " + str(directory))
        shared = tuple(inputs.get(key) for key in ("revision", "shared_source_sha256",
                                                    "nbt_aad_revision", "mbedtls_archive_sha256"))
        if not all(shared) or (common is not None and shared != common):
            raise ValueError("HMI builds must have matching clean revisions and shared inputs")
        common = shared
        results.append(("hmi-" + name + ".so", binary, record["sha256"], metadata))
    return results


def hmi_test_sources(contract):
    generated = hmi_stub_sources(contract)
    result = {}
    for filename, library in (("app.c", "libAppFramework.A.so"), ("core.c", "libCoreFramework.A.so")):
        source = generated[filename]
        if source.count(b" {}\n") != len(contract["stubs"][library]):
            raise ValueError("Unexpected generated HMI stub format")
        result["hmi-" + filename] = b"#include <stdlib.h>\n" + source.replace(b" {}\n", b" { abort(); }\n")
    return result


def run(command, *, timeout=120, log=None):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    if log is not None:
        log.write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError("%s failed (%d):\n%s" %
                           (command[0], result.returncode, result.stdout.decode(errors="replace")[-6000:]))
    return result.stdout.decode(errors="replace")


def add_file(archive, name, data, mode):
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.mtime = len(data), mode, 946684800
    archive.addfile(info, io.BytesIO(data))


def vm_options(vm_address, ssh_config, known_hosts, identity=None):
    try:
        address = ipaddress.ip_address(vm_address)
    except ValueError:
        raise SystemExit("--vm-address must be a numeric IP address")
    if address not in ipaddress.ip_network("192.168.56.0/24"):
        raise SystemExit("VM address must be in the documented host-only network")
    if not ssh_config.is_file() or not known_hosts.is_file():
        raise SystemExit("A VM SSH config and independently verified known-hosts file are required")
    options = ["-F", str(ssh_config.resolve()),
               "-o", "UserKnownHostsFile=" + str(known_hosts.resolve()),
               "-o", "GlobalKnownHostsFile=none", "-o", "StrictHostKeyChecking=yes",
               "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes", "-o", "ProxyCommand=none",
               "-o", "ProxyJump=none", "-o", "PermitLocalCommand=no", "-o", "ForwardAgent=no",
               "-o", "ConnectTimeout=8"]
    if identity:
        options += ["-i", str(identity.resolve()), "-o", "IdentitiesOnly=yes"]
    config = run(["ssh", *options, "-G", "qnx-build"]).splitlines()
    for expected in ("hostname " + vm_address, "user nbtbuild", "port 22"):
        if expected not in config:
            raise SystemExit("VM SSH target did not match: " + expected)
    check = ('test "$(uname -s)" = QNX && test "$(uname -r)" = 6.5.0 && '
             'test "$(uname -p)" = x86 && test "$(id -u)" != 0 && echo QNX_VM_OK')
    if "QNX_VM_OK" not in run(["ssh", *options, "qnx-build", check], timeout=30):
        raise SystemExit("QNX VM identity check failed")
    return options


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm-address", required=True, help="expected VM address on 192.168.56.0/24")
    parser.add_argument("--ssh-config", type=Path, default=ROOT / ".local/ssh_config")
    parser.add_argument("--known-hosts", type=Path, default=ROOT / ".local/known_hosts")
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--hmi-builds", nargs=4, type=Path,
                        metavar=("OPEN_REAL", "OPEN_STUBS", "SDP_REAL", "SDP_STUBS"),
                        help="also load four clean, same-source plugins against synthetic HMI fixtures")
    parser.add_argument("--hmi-real-vm", action="store_true",
                        help="also load plugins using the VM's local real HMI libraries")
    args = parser.parse_args()
    if args.hmi_real_vm and not args.hmi_builds:
        parser.error("--hmi-real-vm requires --hmi-builds")
    matrix = hmi_matrix_builds(args.hmi_builds) if args.hmi_builds else []
    options = vm_options(args.vm_address, args.ssh_config, args.known_hosts, args.identity)

    run_id = "open-abi-probe-" + uuid.uuid4().hex[:12]
    out = ROOT / "build" / run_id
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    print("Evidence:", out, flush=True)
    source = out / "abi_probe.cpp"
    source.write_bytes(PROBE.read_bytes())
    header = out / "qnx_spawn_compat.h"
    header.write_bytes(SPAWN_HEADER.read_bytes())
    resource = out / "resource_abi_probe.c"
    resource.write_bytes(RESOURCE_PROBE.read_bytes())
    local_resource = out / "resource_local_probe.c"
    local_resource.write_bytes(RESOURCE_LOCAL_PROBE.read_bytes())
    message_resource = out / "resource_message_probe.c"
    message_resource.write_bytes(RESOURCE_MESSAGE_PROBE.read_bytes())
    run(["sh", str(ROOT / "toolchain/qnx-open"), "i486-pc-nto-qnx6.5.0-g++",
         "-O1", "-Wall", "-D_QNX_SOURCE", "-DNBT_OPEN_TOOLCHAIN", "-std=gnu++0x",
         "/repo/build/" + run_id + "/abi_probe.cpp", "-lqnxopen", "-o",
         "/repo/build/" + run_id + "/probe_open"], timeout=180, log=out / "cross-build.log")
    run(["sh", str(ROOT / "toolchain/qnx-open"), "i486-pc-nto-qnx6.5.0-gcc", "-Wall",
         "/repo/build/" + run_id + "/resource_abi_probe.c", "-o",
         "/repo/build/" + run_id + "/resource_open"], timeout=180, log=out / "resource-cross-build.log")
    run(["sh", str(ROOT / "toolchain/qnx-open"), "i486-pc-nto-qnx6.5.0-gcc", "-Wall",
         "-DNBT_OPEN_TOOLCHAIN", "/repo/build/" + run_id + "/resource_local_probe.c", "-o",
         "/repo/build/" + run_id + "/resource_local_open"], timeout=180, log=out / "resource-local-cross-build.log")
    run(["sh", str(ROOT / "toolchain/qnx-open"), "i486-pc-nto-qnx6.5.0-gcc", "-Wall",
         "-DNBT_OPEN_TOOLCHAIN", "/repo/build/" + run_id + "/resource_message_probe.c", "-o",
         "/repo/build/" + run_id + "/resource_message_open"], timeout=180, log=out / "resource-message-cross-build.log")
    extra_files = []
    if matrix:
        (out / "hmi_loader_probe.c").write_bytes(HMI_PROBE.read_bytes())
        (out / "hmi-contract.json").write_bytes(HMI_CONTRACT.read_bytes())
        for name, data in hmi_test_sources(json.loads((out / "hmi-contract.json").read_bytes())).items():
            (out / name).write_bytes(data)
        for name, binary, expected, metadata in matrix:
            (out / name).write_bytes(binary.read_bytes())
            if digest_file(out / name) != expected:
                raise ValueError("HMI plugin changed while copying: " + str(binary))
            (out / (name + ".abi-inputs.json")).write_bytes(metadata)
        run(["sh", str(ROOT / "toolchain/qnx-open"), "i486-pc-nto-qnx6.5.0-gcc", "-Wall",
             "/repo/build/" + run_id + "/hmi_loader_probe.c", "-o",
             "/repo/build/" + run_id + "/hmi_loader_open"], timeout=180, log=out / "hmi-cross-build.log")
        extra_files = ["hmi_loader_probe.c", "hmi-app.c", "hmi-core.c", "hmi_loader_open"]
        extra_files += [item[0] for item in matrix]
    (out / "source-sha256.txt").write_text(
        "".join(digest_file(path) + "  " + path.name + "\n" for path in (source, header)), encoding="ascii")
    archive_path = out / "source.tar"
    with tarfile.open(archive_path, "w", format=tarfile.USTAR_FORMAT) as archive:
        add_file(archive, "abi_probe.cpp", source.read_bytes(), 0o600)
        add_file(archive, "qnx_spawn_compat.h", header.read_bytes(), 0o600)
        add_file(archive, "probe_open", (out / "probe_open").read_bytes(), 0o700)
        add_file(archive, "resource_abi_probe.c", resource.read_bytes(), 0o600)
        add_file(archive, "resource_open", (out / "resource_open").read_bytes(), 0o700)
        add_file(archive, "resource_local_probe.c", local_resource.read_bytes(), 0o600)
        add_file(archive, "resource_local_open", (out / "resource_local_open").read_bytes(), 0o700)
        add_file(archive, "resource_message_probe.c", message_resource.read_bytes(), 0o600)
        add_file(archive, "resource_message_open", (out / "resource_message_open").read_bytes(), 0o700)
        for name in extra_files:
            add_file(archive, name, (out / name).read_bytes(), 0o700 if name == "hmi_loader_open" else 0o600)
        add_file(archive, "run.sh", (RUN + (HMI_RUN if matrix else "") +
                                     (HMI_REAL_RUN if args.hmi_real_vm else "")).encode(), 0o600)
    remote = ".nbt-build/" + run_id
    run(["ssh", *options, "qnx-build", "umask 077; mkdir -p .nbt-build && mkdir " + remote], timeout=30)
    try:
        run(["scp", "-O", *options, str(archive_path), "qnx-build:" + remote + "/source.tar"])
        output = run(["ssh", *options, "qnx-build", "cd " + remote + " && tar xf source.tar && sh run.sh"],
                     timeout=180, log=out / "vm-tests.log")
        markers = ["ABI_SDP_DONE", "ABI_OPEN_DONE", "ABI_PROBES_MATCH",
                   "RESOURCE_ABI_PROBES_MATCH", "RESOURCE_LOCAL_PROBES_MATCH", "RESOURCE_MESSAGE_PROBES_MATCH"]
        if matrix:
            markers.append("HMI_MATRIX_PROBES_MATCH")
        if args.hmi_real_vm:
            markers.append("HMI_REAL_MATRIX_PROBES_MATCH")
        if not all(marker in output for marker in markers):
            raise RuntimeError("VM output missing probe completion markers")
        fetched = ["probe_sdp", "sdp.txt", "open.txt", "resource_sdp", "resource_sdp.txt", "resource_open.txt",
                   "resource_local_sdp", "resource_local_sdp.txt", "resource_local_open.txt",
                   "resource_message_sdp", "resource_message_sdp.txt", "resource_message_open.txt"]
        if matrix:
            fetched += ["hmi_loader_sdp"]
            fetched += [loader + "-" + item[0] + ".txt" for item in matrix
                        for loader in ("hmi_loader_sdp", "hmi_loader_open")]
        if args.hmi_real_vm:
            fetched += ["real-hmi-libraries.sha256"]
            fetched += ["real-" + loader + "-" + item[0] + ".txt" for item in matrix
                        for loader in ("hmi_loader_sdp", "hmi_loader_open")]
        for name in fetched:
            run(["scp", "-O", *options, "qnx-build:" + remote + "/" + name, str(out / name)])
        hashed = ["abi_probe.cpp", "qnx_spawn_compat.h", "probe_open", "probe_sdp",
                  "resource_abi_probe.c", "resource_open", "resource_sdp",
                  "resource_local_probe.c", "resource_local_open", "resource_local_sdp",
                  "resource_message_probe.c", "resource_message_open", "resource_message_sdp"]
        if matrix:
            hashed += ["hmi-contract.json", "hmi_loader_sdp"] + extra_files
            hashed += [item[0] + ".abi-inputs.json" for item in matrix]
        (out / "SHA256SUMS").write_text(
            "".join(digest_file(out / name) + "  " + name + "\n" for name in hashed),
            encoding="ascii")
        cleanup = ["source.tar", "abi_probe.cpp", "qnx_spawn_compat.h", "probe_open", "run.sh",
                   "resource_abi_probe.c", "resource_open", "resource_local_probe.c", "resource_local_open",
                   "resource_message_probe.c", "resource_message_open"]
        cleanup += fetched
        if matrix:
            cleanup += extra_files + ["libAppFramework.A.so", "libCoreFramework.A.so"]
        run(["ssh", *options, "qnx-build",
             "cd " + remote + " && rm " + " ".join(cleanup) + " && cd .. && rmdir " + run_id], timeout=30)
    except (RuntimeError, subprocess.TimeoutExpired):
        print("VM files retained at", remote, flush=True)
        raise
    print("Paired probes match; this does not establish vehicle compatibility.")


if __name__ == "__main__":
    raise SystemExit(main())
