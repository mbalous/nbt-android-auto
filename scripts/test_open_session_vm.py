#!/usr/bin/env python3
"""Compare the offline AA session/TLS simulator with open GCC and QNX SDP."""
import argparse
import importlib.util
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import uuid

from abi_provenance import digest_file, digest_tree, shared_source_digest
from build_all import ROOT
from test_open_abi_vm import add_file, run, vm_options


def fixtures():
    spec = importlib.util.spec_from_file_location("qnx_tls_fixture", ROOT / "scripts/Test-QnxTls.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.fixtures()


def prepare(build, work, source):
    inputs = json.loads((build / "abi-inputs.json").read_text())
    script = build / "work/build-open.sh"
    if (inputs.get("toolchain") != "open-experimental" or inputs.get("worktree_dirty") is not False or
            not re.fullmatch(r"[0-9a-f]{40}", inputs.get("revision", "")) or
            shared_source_digest(source) != inputs.get("shared_source_sha256") or
            digest_file(script) != inputs.get("build_script_sha256") or
            digest_tree(ROOT / "build/qnx-open") != inputs.get("toolchain_install_sha256")):
        raise ValueError("Build sources, build script and installed open toolchain must match clean provenance")
    qnx_script = run(["git", "-C", str(ROOT), "show", inputs["revision"] + ":scripts/build-qnx.sh"])
    qnx_script = qnx_script.encode().replace(b"\r\n", b"\n")
    seen = set()
    with tarfile.open(source) as archive:
        for item in archive:
            name = PurePosixPath(item.name)
            allowed = (item.name == "build-qnx.sh" or item.name.startswith(
                ("src/", "reference/", "mbedtls-", "toolchain/hmi-stubs/", "expected/")))
            if (not item.isfile() or not allowed or name.is_absolute() or ".." in name.parts or
                    name.as_posix() != item.name or item.name in seen):
                raise ValueError("Invalid source archive path")
            seen.add(item.name)
            data = archive.extractfile(item).read()
            if item.name == "build-qnx.sh" and data != qnx_script:
                raise ValueError("QNX build script does not match the recorded clean revision")
            path = work.joinpath(*name.parts)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    if "build-qnx.sh" not in seen:
        raise ValueError("QNX build script missing from source archive")
    (work / "build-open.sh").write_bytes(script.read_bytes())
    return inputs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm-address", required=True)
    parser.add_argument("--ssh-config", type=Path, default=ROOT / ".local/ssh_config")
    parser.add_argument("--known-hosts", type=Path, default=ROOT / ".local/known_hosts")
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--open-build", type=Path, required=True)
    args = parser.parse_args()
    options = vm_options(args.vm_address, args.ssh_config, args.known_hosts, args.identity)
    build = args.open_build.resolve()
    run_id = "open-session-probe-" + uuid.uuid4().hex[:12]
    out = ROOT / "build" / run_id
    work = out / "work"
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    work.mkdir()
    print("Evidence:", out, flush=True)
    shutil.copyfile(build / "source.tar", out / "source.tar")
    inputs = prepare(build, work, out / "source.tar")
    (out / "abi-inputs.json").write_text(json.dumps(inputs, indent=2, sort_keys=True) + "\n")
    tls = "mbedtls-3.6.7/library"
    libs = "%s/libmbedtls.a %s/libmbedx509.a %s/libmbedcrypto.a" % (tls, tls, tls)
    include = "-Imbedtls-3.6.7/include -Ireference/src -Ireference/include"
    open_command = ("set -eu; cd /repo/build/%s/work; PROGRAMS=nbt_android_auto sh build-open.sh; "
                    "i486-pc-nto-qnx6.5.0-g++ -O1 -Wall -D_QNX_SOURCE -std=gnu++0x %s "
                    "src/android_auto/test_session_protocol.cpp libnbt_aa_core.a %s "
                    "-lsocket -lm -lqnxopen -o /repo/build/%s/open-session" %
                    (run_id, include, libs, run_id))
    run(["sh", str(ROOT / "toolchain/qnx-open"), "sh", "-c", open_command],
        timeout=600, log=out / "open-build.log")
    extra = fixtures()
    extra["run_bounded.c"] = (ROOT / "toolchain/probes/run_bounded.c").read_bytes()
    extra["open-session"] = (out / "open-session").read_bytes()
    script = """\
set -eu
export QNX_HOST=/usr/qnx650/host/qnx6/x86 QNX_TARGET=/usr/qnx650/target/qnx6
export PATH="$QNX_HOST/usr/bin:$PATH"
HMI_MODE=stubs sh build-qnx.sh > sdp-build.log 2>&1 || { tail -n 40 sdp-build.log; exit 1; }
qcc -V4.4.2,gcc_ntox86 -Wall -DBOUND_SECONDS=120 run_bounded.c -o run_bounded
qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x %(include)s src/android_auto/test_session_protocol.cpp libnbt_aa_core.a %(libs)s -lsocket -lm -o sdp-session
chmod 700 open-session sdp-session run_bounded
./run_bounded ./sdp-session > sdp-session.log 2>&1 || { tail -n 40 sdp-session.log; exit 1; }
./run_bounded ./open-session > open-session.log 2>&1 || { tail -n 40 open-session.log; exit 1; }
grep '^AA_PROTOCOL_.*OK' sdp-session.log > sdp-results.txt
grep '^AA_PROTOCOL_.*OK' open-session.log > open-results.txt
cmp sdp-results.txt open-results.txt
cat sdp-results.txt
echo AA_SESSION_PAIR_MATCH
""" % {"include": include, "libs": libs}
    extra["run-session.sh"] = script.encode()
    aux = out / "session-inputs.tar"
    with tarfile.open(aux, "w", format=tarfile.GNU_FORMAT) as archive:
        for name, data in sorted(extra.items()):
            add_file(archive, name, data, 0o700 if name == "open-session" else 0o600)
    aux.chmod(0o600)
    (out / "SHA256SUMS").write_text(
        "".join(digest_file(path) + "  " + path.name + "\n"
                for path in (out / "source.tar", out / "open-session", aux)), encoding="ascii")
    remote = ".nbt-build/" + run_id
    run(["ssh", *options, "qnx-build", "umask 077; mkdir -p .nbt-build && mkdir " + remote], timeout=30)
    try:
        for path, target in ((out / "source.tar", "source.tar"), (aux, "session-inputs.tar")):
            run(["scp", "-O", *options, str(path), "qnx-build:" + remote + "/" + target], timeout=180)
        output = run(["ssh", *options, "qnx-build",
                      "cd " + remote + " && tar xf source.tar && tar xf session-inputs.tar && sh run-session.sh"],
                     timeout=900, log=out / "vm-tests.log")
        if not all(marker in output for marker in
                   ("AA_PROTOCOL_NEGATIVE_OK", "AA_PROTOCOL_SIMULATOR_OK", "AA_SESSION_PAIR_MATCH")):
            raise RuntimeError("VM output missing session completion markers")
        for name in ("sdp-session", "sdp-session.log", "open-session.log", "sdp-results.txt", "open-results.txt"):
            run(["scp", "-O", *options, "qnx-build:" + remote + "/" + name, str(out / name)], timeout=30)
        with (out / "SHA256SUMS").open("a", encoding="ascii") as sums:
            for name in ("sdp-session", "sdp-session.log", "open-session.log", "sdp-results.txt", "open-results.txt"):
                sums.write(digest_file(out / name) + "  " + name + "\n")
        run(["ssh", *options, "qnx-build", "rm -rf " + remote], timeout=60)
    except (RuntimeError, subprocess.TimeoutExpired):
        print("VM files retained at", remote, flush=True)
        raise
    print("Paired offline AA session/TLS simulator matched; no vehicle services were contacted.")


if __name__ == "__main__":
    raise SystemExit(main())
