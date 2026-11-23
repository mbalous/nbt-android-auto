"""Record comparison inputs separately from installer provenance."""
import hashlib
import json
import os
import re
import subprocess
import tarfile
from pathlib import Path


def digest_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def shared_source_digest(archive_path):
    digest = hashlib.sha256()
    with tarfile.open(archive_path) as archive:
        members = sorted((item for item in archive if item.isfile() and
                          item.name.startswith(("src/", "reference/", "mbedtls-", "toolchain/hmi-stubs/"))),
                         key=lambda item: item.name)
        if not members or len({item.name for item in members}) != len(members):
            raise ValueError("Source archive is empty or contains duplicate shared paths")
        for item in members:
            data = archive.extractfile(item).read()
            digest.update(item.name.encode("utf-8") + b"\0")
            digest.update(hashlib.sha256(data).digest())
    return digest.hexdigest()


def digest_tree(directory):
    digest = hashlib.sha256()
    if not directory.is_dir():
        raise ValueError("Toolchain installation is missing: " + str(directory))
    for path in sorted(directory.rglob("*")):
        if path.is_symlink():
            value = "link:" + os.readlink(path)
        elif path.is_file():
            value = "file:" + digest_file(path)
        else:
            continue
        digest.update(path.relative_to(directory).as_posix().encode("utf-8") + b"\0" + value.encode() + b"\n")
    return digest.hexdigest()


def capture_inputs(root, out, *, toolchain, selected, nbt_aad, mbedtls_sha256):
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root, check=True,
                              capture_output=True, text=True).stdout.strip()
    status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=normal"], cwd=root,
                            check=True, capture_output=True, text=True).stdout
    inputs = [root / "scripts" / ("build-open.sh" if toolchain == "open-experimental" else "build-qnx.sh")]
    if toolchain == "open-experimental":
        for folder in ("runtime", "sysroot", "patches"):
            inputs.extend(path for path in (root / "toolchain" / folder).rglob("*") if path.is_file())
        inputs.extend(root / "toolchain" / name for name in
                      ("Dockerfile", "setup.sh", "build-toolchain.sh", "build-runtime.sh"))
    files = {path.relative_to(root).as_posix(): digest_file(path) for path in sorted(inputs)}
    if toolchain == "open-experimental":
        build_script_hash = digest_file(out / "work" / "build-open.sh")
        installed_hash = digest_tree(root / "build/qnx-open")
    else:
        with tarfile.open(out / "source.tar") as archive:
            build_script_hash = hashlib.sha256(archive.extractfile("build-qnx.sh").read()).hexdigest()
        installed_hash = None
    return {
        "schema": 1,
        "toolchain": toolchain,
        "revision": revision,
        "worktree_dirty": bool(status),
        "shared_source_sha256": shared_source_digest(out / "source.tar"),
        "nbt_aad_revision": nbt_aad,
        "mbedtls_archive_sha256": mbedtls_sha256,
        "selected": sorted(selected),
        "build_script_sha256": build_script_hash,
        "toolchain_source_sha256": files,
        "toolchain_install_sha256": installed_hash,
    }


def write_inputs(out, metadata, *, reused, hmi_link):
    log = (out / "build.log").read_text(encoding="utf-8")
    version = re.search(r"^ABI_TOOLCHAIN_INFO_BEGIN\n(.*?)^ABI_TOOLCHAIN_INFO_END$",
                        log, re.MULTILINE | re.DOTALL)
    artifacts = {}
    for path in sorted((out / "programs").iterdir()):
        if path.is_file() and path.name in set(metadata["selected"]) | set(reused):
            artifacts[path.name] = {"sha256": digest_file(path),
                                    "origin": "reused" if path.name in reused else "built"}
    metadata = dict(metadata, compiler_linker_info=version.group(1).strip() if version else None,
                    hmi_link=hmi_link, artifacts=artifacts)
    (out / "abi-inputs.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
