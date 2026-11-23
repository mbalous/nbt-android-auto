#!/usr/bin/env python3
"""Check the open HMI plugin and link-only stubs against the reviewed ELF contract."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from abi_provenance import digest_file
from compare_qnx_abi import inspect_elf

ROOT = Path(__file__).resolve().parents[1]
LINKER_SYMBOLS = {"__bss_start", "_btext", "_edata", "_end"}


def check_set(label, actual, expected):
    actual, expected = set(actual), set(expected)
    if actual != expected:
        raise ValueError("%s mismatch: missing=%s unexpected=%s" %
                         (label, sorted(expected - actual), sorted(actual - expected)))


def check_target(elf, label):
    if (elf["class"] != "ELF32" or elf["machine"] != "Intel 80386" or
            elf["data"] != "2's complement, little endian" or not elf["type"].startswith("DYN ")):
        raise ValueError(label + " is not a little-endian ELF32 i386 shared object")


def check_stub(name, elf, symbols):
    check_target(elf, name)
    if elf["soname"] != name or elf["needed"] or elf["imports"]:
        raise ValueError(name + " has an unexpected SONAME, dependency or import")
    exports = [item for item in elf["exports"] if item["name"] not in LINKER_SYMBOLS]
    check_set(name + " exports", (item["name"] for item in exports), symbols)
    if any(item["binding"] != "GLOBAL" or item["type"] != "FUNC" for item in exports):
        raise ValueError(name + " has a non-function or non-global stub export")


def check_plugin(elf, contract):
    check_target(elf, "plugin")
    if elf["soname"] is not None or elf["needed"] != contract["needed"] or elf["search_paths"]:
        raise ValueError("plugin SONAME, dependencies or search paths differ from the contract")
    required = set(contract["libc_imports"])
    for symbols in contract["stubs"].values():
        required.update(symbols)
    strong = [item for item in elf["imports"] if item["binding"] == "GLOBAL"]
    weak = [item for item in elf["imports"] if item["binding"] == "WEAK"]
    check_set("plugin strong imports", (item["name"] for item in strong), required)
    check_set("plugin weak imports", (item["name"] for item in weak), contract["weak_imports"])
    if len(strong) + len(weak) != len(elf["imports"]):
        raise ValueError("plugin has imports with unexpected binding")
    for item in strong:
        expected_type = "OBJECT" if item["name"] == "environ" else "FUNC"
        if item["type"] != expected_type or item["visibility"] != "DEFAULT":
            raise ValueError("plugin import type or visibility differs: " + item["name"])
    check_set("plugin exports", (item["name"] for item in elf["exports"]), contract["plugin_exports"])
    entries = [item for item in elf["exports"] if item["name"] == "pluginAppInfo"]
    if entries != [{"name": "pluginAppInfo", "type": "OBJECT", "binding": "GLOBAL",
                    "visibility": "DEFAULT", "size": 20}]:
        raise ValueError("pluginAppInfo export has an unexpected ELF type or size")


def check_no_stubs(paths, stub_paths):
    names = {path.name for path in stub_paths}
    hashes = {digest_file(path) for path in stub_paths}
    for directory in paths:
        if not directory.is_dir():
            raise ValueError("Missing artifact directory: " + str(directory))
        for path in directory.rglob("*"):
            if path.name in names or (path.is_file() and digest_file(path) in hashes):
                raise ValueError("Link-only stub found in artifact directory: " + str(path))


def check_missing_symbol_fails(build):
    source = build / "work"
    missing_symbol = "_ZN3bmw9PluginApp5startEv"
    declaration = "void " + missing_symbol + "(void) {}\n"
    with tempfile.TemporaryDirectory(prefix="hmi-negative-", dir=ROOT / "build") as directory:
        work = Path(directory)
        shutil.copytree(source / "src", work / "src")
        shutil.copytree(source / "toolchain/hmi-stubs", work / "toolchain/hmi-stubs")
        shutil.copy(source / "build-open.sh", work / "build-open.sh")
        (work / "out").mkdir()
        (work / "hmi-stubs").mkdir()
        app = work / "toolchain/hmi-stubs/app.c"
        text = app.read_text(encoding="utf-8")
        if text.count(declaration) != 1:
            raise ValueError("Negative link fixture cannot remove the expected stub symbol")
        app.write_text(text.replace(declaration, ""), encoding="utf-8")
        rel = work.relative_to(ROOT).as_posix()
        command = ["docker", "run", "--rm", "--platform", "linux/amd64",
                   "-v", "%s/build/qnx-open:/opt/qnx-open" % ROOT, "-v", "%s:/repo" % ROOT,
                   "-w", "/repo/" + rel, "-e", "PATH=/opt/qnx-open/bin:/usr/bin:/bin",
                   "-e", "PROGRAMS=libNbtAndroidAuto.so", "qnx-open-host", "sh", "build-open.sh"]
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        if (result.returncode == 0 or
                not any(symbol in result.stdout for symbol in (missing_symbol, "bmw::PluginApp::start()")) or
                "undefined reference" not in result.stdout or "BUILD_OPEN_DONE" in result.stdout):
            raise ValueError("Missing stub symbol did not fail the linker as expected:\n" + result.stdout[-3000:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build_dir", type=Path)
    parser.add_argument("--negative-link-test", action="store_true",
                        help="also prove that a missing HMI symbol fails the open plugin link")
    args = parser.parse_args()
    try:
        contract = json.loads((ROOT / "toolchain/hmi-stubs/contract.json").read_text(encoding="utf-8"))
        build = args.build_dir
        stubs = build / "work/hmi-stubs"
        for name, symbols in contract["stubs"].items():
            check_stub(name, inspect_elf(stubs / name), symbols)
        check_plugin(inspect_elf(build / "programs/libNbtAndroidAuto.so"), contract)
        check_no_stubs((build / "programs", build / "plugin-resources"),
                       [stubs / name for name in contract["stubs"]])
        if args.negative_link_test:
            check_missing_symbol_fails(build)
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print("HMI contract check failed:", error, file=sys.stderr)
        return 1
    print("HMI link-only stub and plugin ELF contracts match; runtime compatibility is not established.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
