#!/usr/bin/env python3
"""Compare existing offline AA protocol tests built by open GCC and QNX SDP."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import uuid

from abi_provenance import digest_file, digest_tree, shared_source_digest
from build_all import ROOT
from test_open_abi_vm import add_file, run, vm_options


def tests():
    spec = importlib.util.spec_from_file_location("aa_core_vm_tests", ROOT / "scripts/Test-AndroidAutoCore.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.TESTS


def prepare(build, reference, output):
    inputs = json.loads((build / "abi-inputs.json").read_text())
    archive_path = build / "source.tar"
    if (inputs.get("toolchain") != "open-experimental" or inputs.get("worktree_dirty") is not False or
            not inputs.get("revision") or shared_source_digest(archive_path) != inputs.get("shared_source_sha256") or
            digest_tree(ROOT / "build/qnx-open") != inputs.get("toolchain_install_sha256")):
        raise ValueError("A clean open build with matching prepared sources is required")
    revision = run(["git", "-C", str(reference), "rev-parse", "HEAD"]).strip()
    if revision != inputs.get("nbt_aad_revision") or run(
            ["git", "-C", str(reference), "status", "--porcelain", "--untracked-files=normal"]).strip():
        raise ValueError("Reference checkout must be clean and match the build revision")
    files = {}
    with tarfile.open(archive_path) as archive:
        for item in archive:
            path = PurePosixPath(item.name)
            if item.isfile() and item.name.startswith(("reference/src/", "reference/include/")):
                if path.is_absolute() or ".." in path.parts or item.name in files:
                    raise ValueError("Invalid source archive path")
                files[item.name] = archive.extractfile(item).read()
        for name in ("dsi_client.cpp", "dsi_client.hpp"):
            item = archive.getmember("src/android_auto/knob/ref/" + name)
            if not item.isfile():
                raise ValueError("Invalid DSI source archive member")
            files["dsi/" + name] = archive.extractfile(item).read()
    tracked = run(["git", "-C", str(reference), "ls-files", "-z", "--", "milestone-7/tests"]).split("\0")
    for name in filter(None, tracked):
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid reference test path")
        path = reference / name
        if path.is_symlink() or not path.is_file():
            raise ValueError("Invalid reference test file: " + name)
        files["reference/" + relative.relative_to("milestone-7").as_posix()] = path.read_bytes().replace(b"\r\n", b"\n")
    files["LICENSE.md"] = (reference / "LICENSE.md").read_bytes()
    for name in ("run_bounded.c", "dsi_fixture.c", "dsi_probe.cpp"):
        files[name] = (ROOT / "toolchain/probes" / name).read_bytes()
    (output / "abi-inputs.json").write_text(json.dumps(inputs, indent=2, sort_keys=True) + "\n")
    return files


def commands(test_cases, compiler, *, open_compiler=False):
    suffix = " -lqnxopen" if open_compiler else ""
    result = []
    for name, sources in test_cases.items():
        source_args = " ".join("reference/" + source for source in sources)
        result.append("%s -Ireference/src -Ireference/include reference/tests/%s.cpp %s "
                      "-lsocket -lm%s -o %s/%s" %
                      (compiler, name, source_args, suffix, "open" if open_compiler else "sdp", name))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vm-address", required=True)
    parser.add_argument("--ssh-config", type=Path, default=ROOT / ".local/ssh_config")
    parser.add_argument("--known-hosts", type=Path, default=ROOT / ".local/known_hosts")
    parser.add_argument("--identity", type=Path)
    parser.add_argument("--open-build", type=Path, required=True)
    parser.add_argument("--reference", type=Path, default=ROOT / ".local/reference-nbt-aad")
    args = parser.parse_args()
    options = vm_options(args.vm_address, args.ssh_config, args.known_hosts, args.identity)
    run_id = "open-protocol-probe-" + uuid.uuid4().hex[:12]
    out = ROOT / "build" / run_id
    work = out / "work"
    out.mkdir(mode=0o700, parents=True, exist_ok=False)
    work.mkdir()
    print("Evidence:", out, flush=True)
    files = prepare(args.open_build.resolve(), args.reference.resolve(), out)
    for name, data in files.items():
        path = work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (work / "open").mkdir()
    cases = tests()
    open_cc = "i486-pc-nto-qnx6.5.0-g++ -O1 -Wall -D_QNX_SOURCE -std=gnu++0x"
    open_commands = commands(cases, open_cc, open_compiler=True)
    open_commands.append(open_cc + " -Idsi dsi_probe.cpp dsi/dsi_client.cpp -lsocket -lm -lqnxopen -o open/dsi_probe")
    open_script = "set -eu\ncd /repo/build/%s/work\n%s\n" % (
        run_id, "\n".join(open_commands))
    (out / "build-open.sh").write_text(open_script)
    run(["sh", str(ROOT / "toolchain/qnx-open"), "sh", "/repo/build/" + run_id + "/build-open.sh"],
        timeout=300, log=out / "open-build.log")
    sdp_cc = "qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x"
    script = ["set -eu", "export QNX_HOST=/usr/qnx650/host/qnx6/x86 QNX_TARGET=/usr/qnx650/target/qnx6",
              'export PATH="$QNX_HOST/usr/bin:$PATH"', "mkdir sdp",
              "qcc -V4.4.2,gcc_ntox86 -Wall run_bounded.c -o run_bounded"]
    script += commands(cases, sdp_cc)
    script += [sdp_cc + " -Idsi dsi_probe.cpp dsi/dsi_client.cpp -lsocket -lm -o sdp/dsi_probe",
               "qcc -V4.4.2,gcc_ntox86 -shared -fPIC dsi_fixture.c -o libdsi-new.so",
               "qcc -V4.4.2,gcc_ntox86 -shared -fPIC -DOLD_DSI dsi_fixture.c -o libdsi-old.so"]
    for name in cases:
        script += ["./run_bounded ./sdp/%s > sdp/%s.txt" % (name, name),
                   "./run_bounded ./open/%s > open/%s.txt" % (name, name),
                   "cmp sdp/%s.txt open/%s.txt" % (name, name),
                   "echo PROTOCOL_MATCH:%s" % name]
    for version in ("new", "old"):
        script += ["./run_bounded ./sdp/dsi_probe ./libdsi-%s.so > sdp/dsi-%s.txt" % (version, version),
                   "./run_bounded ./open/dsi_probe ./libdsi-%s.so > open/dsi-%s.txt" % (version, version),
                   "grep '^DSI_FIXTURE_OK' sdp/dsi-%s.txt > sdp/dsi-%s-result.txt" % (version, version),
                   "grep '^DSI_FIXTURE_OK' open/dsi-%s.txt > open/dsi-%s-result.txt" % (version, version),
                   "cmp sdp/dsi-%s-result.txt open/dsi-%s-result.txt" % (version, version),
                   "echo DSI_FIXTURE_MATCH:%s" % version]
    results = [name for test in cases for name in
               ("sdp/" + test, "sdp/" + test + ".txt", "open/" + test + ".txt")]
    results += ["sdp/dsi_probe"] + [folder + "/dsi-" + version + ".txt"
                                   for folder in ("sdp", "open") for version in ("new", "old")]
    script.append("tar cf protocol-results.tar " + " ".join(results))
    files["run.sh"] = ("\n".join(script) + "\n").encode()
    for name in cases:
        files["open/" + name] = (work / "open" / name).read_bytes()
    files["open/dsi_probe"] = (work / "open/dsi_probe").read_bytes()
    tarpath = out / "source.tar"
    with tarfile.open(tarpath, "w", format=tarfile.GNU_FORMAT) as archive:
        for name, data in sorted(files.items()):
            add_file(archive, name, data, 0o700 if name.startswith("open/") else 0o600)
    (out / "source-sha256.json").write_text(json.dumps(
        {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())}, indent=2) + "\n")
    remote = ".nbt-build/" + run_id
    run(["ssh", *options, "qnx-build", "umask 077; mkdir -p .nbt-build && mkdir " + remote], timeout=30)
    try:
        run(["scp", "-O", *options, str(tarpath), "qnx-build:" + remote + "/source.tar"], timeout=180)
        output = run(["ssh", *options, "qnx-build", "cd " + remote + " && tar xf source.tar && sh run.sh"],
                     timeout=300, log=out / "vm-tests.log")
        if (not all("PROTOCOL_MATCH:" + name in output for name in cases) or
                not all("DSI_FIXTURE_MATCH:" + version in output for version in ("new", "old"))):
            raise RuntimeError("VM output missing protocol completion markers")
        run(["scp", "-O", *options, "qnx-build:" + remote + "/protocol-results.tar",
             str(out / "protocol-results.tar")], timeout=30)
        (out / "SHA256SUMS").write_text(
            "".join(digest_file(out / name) + "  " + name + "\n"
                    for name in ("source.tar", "protocol-results.tar")), encoding="ascii")
        run(["ssh", *options, "qnx-build", "rm -rf " + remote], timeout=30)
    except (RuntimeError, subprocess.TimeoutExpired):
        print("VM files retained at", remote, flush=True)
        raise
    print("Paired offline protocol tests match; no vehicle services were contacted.")


if __name__ == "__main__":
    raise SystemExit(main())
