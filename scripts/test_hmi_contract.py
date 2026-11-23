"""Tests for the HMI link-only stub contract checks."""
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import check_hmi_contract as check

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = json.loads((ROOT / "toolchain/hmi-stubs/contract.json").read_text())


def target():
    return {"class": "ELF32", "machine": "Intel 80386", "data": "2's complement, little endian",
            "type": "DYN (Shared object file)"}


def plugin():
    strong = set(CONTRACT["libc_imports"])
    for symbols in CONTRACT["stubs"].values():
        strong.update(symbols)
    imports = [{"name": name, "binding": "GLOBAL", "visibility": "DEFAULT",
                "type": "OBJECT" if name == "environ" else "FUNC"} for name in sorted(strong)]
    imports += [{"name": name, "binding": "WEAK", "visibility": "DEFAULT", "type": "NOTYPE"}
                for name in CONTRACT["weak_imports"]]
    exports = [{"name": name} for name in CONTRACT["plugin_exports"] if name != "pluginAppInfo"]
    exports.append({"name": "pluginAppInfo", "type": "OBJECT", "binding": "GLOBAL",
                    "visibility": "DEFAULT", "size": 20})
    return dict(target(), soname=None, needed=CONTRACT["needed"], search_paths=[], imports=imports, exports=exports)


class HmiContractTests(unittest.TestCase):
    def test_plugin_contract_rejects_missing_or_unexpected_imports_and_exports(self):
        baseline = plugin()
        check.check_plugin(baseline, CONTRACT)
        for field in ("imports", "exports"):
            altered = copy.deepcopy(baseline)
            altered[field].pop(0)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "mismatch"):
                check.check_plugin(altered, CONTRACT)
        altered = copy.deepcopy(baseline)
        altered["imports"].append({"name": "unexpected", "binding": "GLOBAL",
                                   "visibility": "DEFAULT", "type": "FUNC"})
        with self.assertRaisesRegex(ValueError, "unexpected"):
            check.check_plugin(altered, CONTRACT)
        altered = copy.deepcopy(baseline)
        altered["exports"][-1]["size"] = 16
        with self.assertRaisesRegex(ValueError, "pluginAppInfo"):
            check.check_plugin(altered, CONTRACT)

    def test_rejects_dependency_or_search_path_drift(self):
        for key, value in (("soname", "unexpected.so"), ("needed", ["libc.so.3"]),
                           ("search_paths", [{"tag": "RUNPATH", "value": "/tmp"}])):
            altered = plugin()
            altered[key] = value
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "SONAME, dependencies or search paths"):
                check.check_plugin(altered, CONTRACT)

    def test_stub_soname_and_exports_are_checked(self):
        name = "libCoreFramework.A.so"
        symbols = CONTRACT["stubs"][name]
        elf = dict(target(), soname=name, needed=[], imports=[],
                   exports=[{"name": name, "type": "FUNC", "binding": "GLOBAL"} for name in symbols])
        check.check_stub(name, elf, symbols)
        elf["soname"] = "wrong.so"
        with self.assertRaisesRegex(ValueError, "SONAME"):
            check.check_stub(name, elf, symbols)
        elf["soname"] = name
        elf["exports"].pop()
        with self.assertRaisesRegex(ValueError, "exports mismatch"):
            check.check_stub(name, elf, symbols)

    def test_artifacts_reject_renamed_stub_copies(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stub = root / "libAppFramework.A.so"
            stub.write_bytes(b"stub")
            artifacts = root / "programs"
            artifacts.mkdir()
            (artifacts / "plugin.so").write_bytes(b"plugin")
            check.check_no_stubs([artifacts], [stub])
            (artifacts / "renamed.so").write_bytes(b"stub")
            with self.assertRaisesRegex(ValueError, "Link-only stub"):
                check.check_no_stubs([artifacts], [stub])

    def test_negative_link_fixture_requires_the_expected_linker_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "build").mkdir()
            source = root / "source/work"
            (source / "src").mkdir(parents=True)
            (source / "toolchain/hmi-stubs").mkdir(parents=True)
            (source / "toolchain/hmi-stubs/app.c").write_text("void _ZN3bmw9PluginApp5startEv(void) {}\n")
            (source / "build-open.sh").write_text("#!/bin/sh\n")
            with mock.patch.object(check, "ROOT", root), mock.patch.object(check.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 1,
                    "undefined reference to `bmw::PluginApp::start()'")
                check.check_missing_symbol_fails(root / "source")
                run.return_value = subprocess.CompletedProcess([], 1, "compiler not found")
                with self.assertRaisesRegex(ValueError, "did not fail the linker as expected"):
                    check.check_missing_symbol_fails(root / "source")


if __name__ == "__main__":
    unittest.main()
