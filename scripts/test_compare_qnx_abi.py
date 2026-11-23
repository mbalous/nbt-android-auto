"""Focused tests for ABI inventory, provenance, and comparison."""
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest

import abi_provenance
import compare_qnx_abi as abi


class CompareAbiTests(unittest.TestCase):
    def test_normalizes_symbols_and_copy_relocations(self):
        output = """ELF Header:
  Class: ELF32
  Data: little endian
  Type: EXEC
  Machine: Intel 80386
 [Requesting program interpreter: /usr/lib/ldqnx.so.2]
 0x1 (NEEDED) Shared library: [liba.so]
 0x1 (NEEDED) Shared library: [libb.so]
 0x3 (SONAME) Library soname: [test.so]
 0x2 (RUNPATH) Library runpath: [/a:/b]
080511c0  00000b05 R_386_COPY 080511c0 errno
080510a0  00001407 R_386_JUMP_SLOT 00000000 foo@VER
Symbol table '.dynsym' contains 3 entries:
 Num: Value Size Type Bind Vis Ndx Name
  1: 00000000 0 FUNC GLOBAL DEFAULT UND foo@VER (2)
  2: 080511c0 4 OBJECT GLOBAL DEFAULT 24 errno
Version needs section '.gnu.version_r' contains 1 entry:
  000000: Version: 1  File: liba.so  Cnt: 1
  0x0010: Name: VER  Flags: none  Version: 2
"""
        result = abi.parse_readelf(output)
        self.assertEqual(result["needed"], ["liba.so", "libb.so"])
        self.assertEqual(result["soname"], "test.so")
        self.assertEqual(result["search_paths"], [{"tag": "RUNPATH", "value": "/a:/b"}])
        self.assertEqual(result["imports"][0]["name"], "foo@VER")
        self.assertEqual(result["exports"][0]["size"], 4)
        self.assertEqual(result["version_needs"], [{"library": "liba.so", "name": "VER", "flags": "none"}])
        self.assertIn({"type": "R_386_COPY", "symbol": "errno", "count": 1, "symbol_sizes": [4]},
                      result["relocations"])
        self.assertEqual(result, abi.parse_readelf(output.replace("080511c0", "080522d0")))
        with self.assertRaisesRegex(ValueError, "multiple SONAME"):
            abi.parse_readelf(output + " 0x3 (SONAME) Library soname: [other.so]\n")

    def test_real_elf_and_bad_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "test.c"
            source.write_text("extern int puts(const char *); int main(void) { return puts(\"ok\"); }\n")
            binary = root / "native_service"
            subprocess.run(["cc", str(source), "-o", str(binary)], check=True)
            report = abi.inventory(root, role="open")
            self.assertIn("native_service", report["programs"])
            self.assertEqual(report["missing"]["accessory_window"], "not-present")
            self.assertEqual(report["missing"]["nbt_key"], "not-present")
            self.assertEqual(report["missing"]["nbt_a2i_beep"], "unsupported")
            self.assertTrue(any("puts" in symbol["name"] for symbol in
                                report["programs"]["native_service"]["elf"]["imports"]))
            binary.write_bytes(b"not an ELF")
            with self.assertRaisesRegex(ValueError, "readelf failed"):
                abi.inventory(root, role="open")

    def test_hashes_origins_and_missing_provenance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "programs").mkdir()
            binary = root / "programs" / "native_service"
            binary.write_bytes(Path("/bin/true").read_bytes())
            metadata = {"artifacts": {"native_service": {"sha256": abi_provenance.digest_file(binary),
                                                           "origin": "reused"}},
                        "selected": []}
            (root / "abi-inputs.json").write_text(json.dumps(metadata))
            found = abi.inventory(root, role="open")
            self.assertEqual(found["programs"]["native_service"]["origin"], "reused")
            self.assertIn("reused", abi.comparison(found, found)["source_status"])
            binary.write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "Artifact hash mismatch"):
                abi.inventory(root, role="open")

    def test_reference_payload_ignores_nonprogram_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "plugin").mkdir()
            (root / "plugin" / "NbtAndroidAuto.xml").write_text("<resources/>")
            (root / "plugin" / "libNbtAndroidAuto.so").write_bytes(Path("/bin/true").read_bytes())
            found = abi.inventory(root, role="reference")
            self.assertEqual(list(found["programs"]), ["libNbtAndroidAuto.so"])

    def test_matching_inputs_do_not_hide_dirty_worktree(self):
        inputs = {"shared_source_sha256": "a", "nbt_aad_revision": "b",
                  "mbedtls_archive_sha256": "c", "revision": "d", "worktree_dirty": True}
        build = {"inputs": inputs, "programs": {}, "missing": {}}
        self.assertIn("dirty", abi.comparison(build, build)["source_status"])
        inputs["worktree_dirty"] = False
        self.assertTrue(abi.comparison(build, build)["source_status"].startswith("same-source"))
        other = {"inputs": dict(inputs, shared_source_sha256="changed"), "programs": {}, "missing": {}}
        self.assertIn("differ", abi.comparison(build, other)["source_status"])
        reference = {"inputs": inputs, "programs": {}, "missing": {}, "role": "reference"}
        self.assertIn("observational: local reference", abi.comparison(build, reference)["source_status"])
        unknown = {"inputs": inputs, "programs": {"x": {"origin": "unknown", "elf": {}}}, "missing": {}}
        self.assertIn("origins unknown", abi.comparison(build, unknown)["source_status"])

    def test_comparison_reports_only_changed_symbols_and_keeps_dependency_order(self):
        symbol = {"name": "same"}
        left = {"inputs": None, "programs": {"x": {"origin": "built",
                "elf": {"imports": [symbol, {"name": "open"}], "needed": ["a", "b"]}}}}
        right = {"inputs": None, "programs": {"x": {"origin": "built",
                 "elf": {"imports": [symbol, {"name": "sdp"}], "needed": ["b", "a"]}}}}
        changes = abi.comparison(left, right)["differences"]["x"]
        self.assertEqual(changes["imports"], {"only_open": [{"name": "open"}], "only_other": [{"name": "sdp"}]})
        self.assertEqual(changes["needed"], {"open": ["a", "b"], "other": ["b", "a"]})

    def test_shared_source_digest_ignores_scripts_and_expected_binaries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            def archive(path, source, script, stub=b"stub"):
                with tarfile.open(path, "w") as tar:
                    for name, data in (("src/a.c", source), ("build-qnx.sh", script),
                                       ("expected/program", b"private"),
                                       ("toolchain/hmi-stubs/app.c", stub)):
                        info = tarfile.TarInfo(name)
                        info.size = len(data)
                        tar.addfile(info, io.BytesIO(data))
            a, b = root / "a.tar", root / "b.tar"
            archive(a, b"source", b"one")
            archive(b, b"source", b"two")
            self.assertEqual(abi_provenance.shared_source_digest(a), abi_provenance.shared_source_digest(b))
            archive(b, b"changed", b"one")
            self.assertNotEqual(abi_provenance.shared_source_digest(a), abi_provenance.shared_source_digest(b))
            archive(b, b"source", b"one", b"changed-stub")
            self.assertNotEqual(abi_provenance.shared_source_digest(a), abi_provenance.shared_source_digest(b))

    def test_build_provenance_binds_built_and_reused_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "programs").mkdir()
            (root / "programs" / "native_service").write_bytes(b"built")
            (root / "programs" / "accessory_window").write_bytes(b"reused")
            (root / "build.log").write_text("ABI_TOOLCHAIN_INFO_BEGIN\ncompiler version\nABI_TOOLCHAIN_INFO_END\n")
            abi_provenance.write_inputs(root, {"selected": ["native_service"]},
                                        reused=["accessory_window"], hmi_link="stubs")
            metadata = json.loads((root / "abi-inputs.json").read_text())
            self.assertEqual(metadata["compiler_linker_info"], "compiler version")
            self.assertEqual(metadata["hmi_link"], "stubs")
            self.assertEqual(metadata["artifacts"]["native_service"]["origin"], "built")
            self.assertEqual(metadata["artifacts"]["accessory_window"]["origin"], "reused")
            self.assertEqual(metadata["artifacts"]["accessory_window"]["sha256"],
                             abi_provenance.digest_file(root / "programs" / "accessory_window"))

    def test_toolchain_tree_digest_tracks_files_and_symlinks(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "file").write_bytes(b"one")
            (root / "link").symlink_to("file")
            first = abi_provenance.digest_tree(root)
            (root / "file").write_bytes(b"two")
            self.assertNotEqual(first, abi_provenance.digest_tree(root))
            (root / "file").write_bytes(b"one")
            (root / "link").unlink()
            (root / "link").symlink_to("other")
            self.assertNotEqual(first, abi_provenance.digest_tree(root))


if __name__ == "__main__":
    unittest.main()
