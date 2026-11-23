#!/usr/bin/env python3
"""Compare open GCC 4.4.2 and SDP ABI probes on a trusted host-only QNX VM."""
import argparse
import io
import ipaddress
from pathlib import Path
import subprocess
import tarfile
import uuid

from abi_provenance import digest_file
from build_all import ROOT

PROBE = ROOT / "toolchain/probes/abi_probe.cpp"
RESOURCE_PROBE = ROOT / "toolchain/probes/resource_abi_probe.c"
RUN = """\
set -eu
export QNX_HOST=/usr/qnx650/host/qnx6/x86 QNX_TARGET=/usr/qnx650/target/qnx6
export PATH="$QNX_HOST/usr/bin:$PATH"
qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x abi_probe.cpp -o probe_sdp
qcc -V4.4.2,gcc_ntox86 -Wall resource_abi_probe.c -o resource_sdp
chmod 700 probe_open probe_sdp resource_open resource_sdp
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
"""


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm-address", required=True, help="expected VM address on 192.168.56.0/24")
    parser.add_argument("--ssh-config", type=Path, default=ROOT / ".local/ssh_config")
    parser.add_argument("--known-hosts", type=Path, default=ROOT / ".local/known_hosts")
    parser.add_argument("--identity", type=Path)
    args = parser.parse_args()
    try:
        address = ipaddress.ip_address(args.vm_address)
    except ValueError:
        parser.error("--vm-address must be a numeric IP address")
    if address not in ipaddress.ip_network("192.168.56.0/24"):
        raise SystemExit("VM address must be in the documented host-only network")
    if not args.ssh_config.is_file() or not args.known_hosts.is_file():
        raise SystemExit("A VM SSH config and independently verified known-hosts file are required")
    options = ["-F", str(args.ssh_config.resolve()),
               "-o", "UserKnownHostsFile=" + str(args.known_hosts.resolve()),
               "-o", "GlobalKnownHostsFile=none", "-o", "StrictHostKeyChecking=yes",
               "-o", "BatchMode=yes", "-o", "ClearAllForwardings=yes", "-o", "ProxyCommand=none",
               "-o", "ProxyJump=none", "-o", "PermitLocalCommand=no", "-o", "ForwardAgent=no",
               "-o", "ConnectTimeout=8"]
    if args.identity:
        options += ["-i", str(args.identity.resolve()), "-o", "IdentitiesOnly=yes"]
    config = run(["ssh", *options, "-G", "qnx-build"]).splitlines()
    for expected in ("hostname " + args.vm_address, "user nbtbuild", "port 22"):
        if expected not in config:
            raise SystemExit("VM SSH target did not match: " + expected)
    check = ('test "$(uname -s)" = QNX && test "$(uname -r)" = 6.5.0 && '
             'test "$(uname -p)" = x86 && test "$(id -u)" != 0 && echo QNX_VM_OK')
    if "QNX_VM_OK" not in run(["ssh", *options, "qnx-build", check], timeout=30):
        raise SystemExit("QNX VM identity check failed")

    run_id = "open-abi-probe-" + uuid.uuid4().hex[:12]
    out = ROOT / "build" / run_id
    out.mkdir(parents=True, exist_ok=False)
    print("Evidence:", out, flush=True)
    source = out / "abi_probe.cpp"
    source.write_bytes(PROBE.read_bytes())
    resource = out / "resource_abi_probe.c"
    resource.write_bytes(RESOURCE_PROBE.read_bytes())
    run(["sh", str(ROOT / "toolchain/qnx-open"), "i486-pc-nto-qnx6.5.0-g++",
         "-O1", "-Wall", "-D_QNX_SOURCE", "-DNBT_OPEN_TOOLCHAIN", "-std=gnu++0x",
         "/repo/build/" + run_id + "/abi_probe.cpp", "-lqnxopen", "-o",
         "/repo/build/" + run_id + "/probe_open"], timeout=180, log=out / "cross-build.log")
    run(["sh", str(ROOT / "toolchain/qnx-open"), "i486-pc-nto-qnx6.5.0-gcc", "-Wall",
         "/repo/build/" + run_id + "/resource_abi_probe.c", "-o",
         "/repo/build/" + run_id + "/resource_open"], timeout=180, log=out / "resource-cross-build.log")
    (out / "source-sha256.txt").write_text(digest_file(source) + "  abi_probe.cpp\n", encoding="ascii")
    archive_path = out / "source.tar"
    with tarfile.open(archive_path, "w", format=tarfile.USTAR_FORMAT) as archive:
        add_file(archive, "abi_probe.cpp", source.read_bytes(), 0o600)
        add_file(archive, "probe_open", (out / "probe_open").read_bytes(), 0o700)
        add_file(archive, "resource_abi_probe.c", resource.read_bytes(), 0o600)
        add_file(archive, "resource_open", (out / "resource_open").read_bytes(), 0o700)
        add_file(archive, "run.sh", RUN.encode(), 0o600)
    remote = ".nbt-build/" + run_id
    run(["ssh", *options, "qnx-build", "umask 077; mkdir -p .nbt-build && mkdir " + remote], timeout=30)
    try:
        run(["scp", "-O", *options, str(archive_path), "qnx-build:" + remote + "/source.tar"])
        output = run(["ssh", *options, "qnx-build", "cd " + remote + " && tar xf source.tar && sh run.sh"],
                     timeout=180, log=out / "vm-tests.log")
        if not all(marker in output for marker in ("ABI_SDP_DONE", "ABI_OPEN_DONE", "ABI_PROBES_MATCH",
                                                    "RESOURCE_ABI_PROBES_MATCH")):
            raise RuntimeError("VM output missing probe completion markers")
        for name in ("probe_sdp", "sdp.txt", "open.txt", "resource_sdp", "resource_sdp.txt", "resource_open.txt"):
            run(["scp", "-O", *options, "qnx-build:" + remote + "/" + name, str(out / name)])
        (out / "SHA256SUMS").write_text(
            "".join(digest_file(out / name) + "  " + name + "\n"
                    for name in ("abi_probe.cpp", "probe_open", "probe_sdp",
                                 "resource_abi_probe.c", "resource_open", "resource_sdp")), encoding="ascii")
        run(["ssh", *options, "qnx-build",
             "cd " + remote + " && rm source.tar abi_probe.cpp probe_open probe_sdp run.sh sdp.txt open.txt"
             " resource_abi_probe.c resource_open resource_sdp resource_sdp.txt resource_open.txt"
             " && cd .. && rmdir " + run_id], timeout=30)
    except (RuntimeError, subprocess.TimeoutExpired):
        print("VM files retained at", remote, flush=True)
        raise
    print("Paired probes match; this does not establish vehicle compatibility.")


if __name__ == "__main__":
    raise SystemExit(main())
