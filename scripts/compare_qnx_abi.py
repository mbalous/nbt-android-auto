#!/usr/bin/env python3
"""Inventory QNX ELF files and compare open and SDP builds without executing them."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import sys

from abi_provenance import digest_file
from build_all import PROGRAMS
from build_open import OPEN_PROGRAMS


def parse_readelf(text):
    headers = {}
    for key in ("Class", "Data", "Type", "Machine"):
        match = re.search(r"^\s*" + key + r":\s*(.+)$", text, re.MULTILINE)
        if not match:
            raise ValueError("readelf output missing ELF header field " + key)
        headers[key.lower()] = match.group(1).strip()
    interpreter = re.search(r"\[Requesting program interpreter:\s*(.*?)\]", text)
    needed = re.findall(r"\(NEEDED\).*?\[([^]]+)\]", text)
    sonames = re.findall(r"\(SONAME\).*?\[([^]]+)\]", text)
    if len(sonames) > 1:
        raise ValueError("ELF has multiple SONAME entries")
    search_paths = [{"tag": tag, "value": value} for tag, value in
                    re.findall(r"\((RPATH|RUNPATH)\).*?\[([^]]*)\]", text)]
    imports, exports, sizes = [], [], {}
    in_dynamic = False
    for line in text.splitlines():
        if line.startswith("Symbol table '"):
            in_dynamic = line.startswith("Symbol table '.dynsym'")
            continue
        if line.startswith("Version ") or line.startswith("Relocation section"):
            in_dynamic = False
        if not in_dynamic:
            continue
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        _, _, size, kind, bind, visibility, section, name = fields[:8]
        if bind not in ("GLOBAL", "WEAK", "UNIQUE") or not name:
            continue
        symbol = {"name": name, "type": kind, "binding": bind, "visibility": visibility}
        if kind in ("OBJECT", "TLS", "COMMON"):
            symbol["size"] = int(size, 0)
        if section == "UND":
            imports.append(symbol)
        elif visibility in ("DEFAULT", "PROTECTED"):
            exports.append(symbol)
            sizes.setdefault(name.split("@", 1)[0], set()).add(int(size, 0))
    key = lambda item: json.dumps(item, sort_keys=True)
    relocations = Counter()
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 3 or not re.fullmatch(r"[0-9a-fA-F]+", fields[0]) or not fields[2].startswith("R_"):
            continue
        kind = fields[2]
        name = fields[4] if len(fields) >= 5 and not fields[4].startswith(("+", "-")) else ""
        # RELATIVE relocations have no symbol; addresses and addends are intentionally omitted.
        if kind.endswith("_RELATIVE"):
            name = ""
        relocations[(kind, name)] += 1
    normalized_relocs = []
    for (kind, name), count in sorted(relocations.items()):
        entry = {"type": kind, "symbol": name, "count": count}
        if kind.endswith("_COPY"):
            entry["symbol_sizes"] = sorted(sizes.get(name.split("@", 1)[0], set()))
        normalized_relocs.append(entry)
    versions, definitions = [], []
    section, library = None, None
    for line in text.splitlines():
        if line.startswith("Version needs section"):
            section = "needs"
        elif line.startswith("Version definition section"):
            section = "definitions"
        elif line.startswith("Version symbols section"):
            section = None
        elif section == "needs":
            file_match = re.search(r"\bFile:\s*(\S+)", line)
            name_match = re.search(r"\bName:\s*(\S+)\s+Flags:\s*(.*?)\s+Version:", line)
            if file_match:
                library = file_match.group(1)
            elif name_match and library:
                versions.append({"library": library, "name": name_match.group(1), "flags": name_match.group(2)})
        elif section == "definitions":
            definition = re.search(r"\bFlags:\s*(.*?)\s+Index:.*?\bName:\s*(\S+)", line)
            parent = re.search(r"\bParent \d+:\s*(\S+)", line)
            if definition:
                definitions.append({"name": definition.group(2), "flags": definition.group(1), "parents": []})
            elif parent and definitions:
                definitions[-1]["parents"].append(parent.group(1))
    return {**headers, "interpreter": interpreter.group(1) if interpreter else None, "needed": needed,
            "soname": sonames[0] if sonames else None,
            "search_paths": search_paths, "imports": sorted(imports, key=key), "exports": sorted(exports, key=key),
            "version_needs": sorted(versions, key=key), "version_definitions": sorted(definitions, key=key),
            "relocations": normalized_relocs}


def inspect_elf(path):
    result = subprocess.run(["readelf", "-W", "-h", "-l", "-d", "-r", "-V", "--dyn-syms", str(path)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode or re.search(r"\b(?:Error|Warning):", result.stderr):
        raise ValueError("readelf failed for %s: %s" % (path, result.stderr.strip()))
    return parse_readelf(result.stdout)


def load_json(path):
    if not path.is_file():
        return None
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def inventory(directory, *, role):
    if not directory.is_dir():
        raise ValueError("Not a directory: " + str(directory))
    programs_dir = directory / "programs"
    if not programs_dir.is_dir():
        programs_dir = directory
    candidates = {}
    for folder in (programs_dir, programs_dir / "rt", programs_dir / "plugin", programs_dir / "test"):
        if not folder.is_dir():
            continue
        for path in sorted(folder.iterdir()):
            if not path.is_file() or path.suffix == ".sh":
                continue
            if programs_dir == directory and path.name not in PROGRAMS:
                continue
            if path.name in candidates:
                raise ValueError("Duplicate program name: " + path.name)
            candidates[path.name] = path
    if not candidates:
        raise ValueError("No program binaries found in " + str(directory))
    inputs = load_json(directory / "abi-inputs.json")
    provenance = load_json(directory / "build-provenance.json")
    declared = inputs.get("artifacts", {}) if inputs else {}
    missing_declared = set(declared) - set(candidates)
    if missing_declared:
        raise ValueError("Recorded artifacts missing: " + ", ".join(sorted(missing_declared)))
    result = {}
    for name, path in sorted(candidates.items()):
        sha256 = digest_file(path)
        record = declared.get(name)
        if record and record.get("sha256") != sha256:
            raise ValueError("Artifact hash mismatch: " + name)
        result[name] = {"sha256": sha256, "origin": record.get("origin", "unknown") if record else "unknown",
                        "elf": inspect_elf(path)}
    missing = {}
    for name in sorted(set(PROGRAMS) - set(result)):
        if inputs and name in inputs.get("selected", []):
            missing[name] = "selected-but-missing"
        elif role == "open" and name not in OPEN_PROGRAMS:
            missing[name] = "unsupported"
        else:
            missing[name] = "not-present"
    return {"role": role, "inputs": inputs, "build_provenance": provenance, "programs": result,
            "missing": missing}


def comparison(left, right):
    a, b = left["inputs"], right["inputs"]
    source_status = "observational: missing provenance"
    if a and b:
        keys = ("shared_source_sha256", "nbt_aad_revision", "mbedtls_archive_sha256")
        if not all(a.get(key) and a.get(key) == b.get(key) for key in keys):
            source_status = "observational: shared inputs differ or are incomplete"
        elif a.get("worktree_dirty") is not False or b.get("worktree_dirty") is not False:
            source_status = "observational: shared inputs match but worktree was dirty or unknown"
        elif not a.get("revision") or a.get("revision") != b.get("revision"):
            source_status = "observational: revisions differ or are unknown"
        else:
            source_status = "same-source: matching clean revision and shared input digest"
    reasons = []
    if right.get("role") == "reference":
        reasons.append("local reference")
    if any(item["origin"] == "reused" for side in (left, right) for item in side["programs"].values()):
        reasons.append("reused outputs")
    elif any(item["origin"] != "built" for side in (left, right) for item in side["programs"].values()):
        reasons.append("output origins unknown")
    if reasons:
        detail = source_status[len("observational: "):] if source_status.startswith("observational: ") else source_status
        source_status = "observational: " + "; ".join(reasons + [detail])
    common = sorted(left["programs"].keys() & right["programs"].keys())
    differences = {}
    for name in common:
        l_elf, r_elf = left["programs"][name]["elf"], right["programs"][name]["elf"]
        changes = {}
        for key in l_elf:
            if l_elf[key] == r_elf[key]:
                continue
            if key in ("imports", "exports", "relocations", "version_needs", "version_definitions"):
                l_items = Counter(json.dumps(item, sort_keys=True) for item in l_elf[key])
                r_items = Counter(json.dumps(item, sort_keys=True) for item in r_elf[key])
                changes[key] = {
                    "only_open": [json.loads(item) for item in sorted((l_items - r_items).elements())],
                    "only_other": [json.loads(item) for item in sorted((r_items - l_items).elements())],
                }
            else:
                changes[key] = {"open": l_elf[key], "other": r_elf[key]}
        differences[name] = changes
    return {"source_status": source_status, "common_programs": common,
            "only_open": sorted(left["programs"].keys() - right["programs"].keys()),
            "only_other": sorted(right["programs"].keys() - left["programs"].keys()),
            "differences": differences}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--open", required=True, type=Path, dest="open_dir")
    parser.add_argument("--sdp", type=Path)
    parser.add_argument("--reference", type=Path, help="optional older local payload or program directory")
    parser.add_argument("--output", type=Path, help="JSON output (default: stdout)")
    args = parser.parse_args()
    try:
        opened = inventory(args.open_dir, role="open")
        report = {"schema": 1, "open": opened}
        for role, directory in (("sdp", args.sdp), ("reference", args.reference)):
            if directory:
                other = inventory(directory, role=role)
                report[role] = other
                report[role + "_comparison"] = comparison(opened, other)
        text = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.write_text(text, encoding="utf-8")
        else:
            sys.stdout.write(text)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("ABI inventory failed:", error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
