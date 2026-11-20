#!/usr/bin/env python3
"""Linux/WSL helper for QNX 6.5.0 build VM trust, SSH bootstrap, probe, and smoke-build.

Uses native Linux OpenSSH and Python 3 (no Node.js required) while preserving host-key verification,
non-overwrite guarantees, key permissions, VM identity checks, and ELF32 x86 validation.
"""
import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parent.parent
LOCAL_DIR = ROOT / ".local"
DEFAULT_CONFIG = LOCAL_DIR / "ssh_config"
DEFAULT_KNOWN_HOSTS = LOCAL_DIR / "known_hosts"
DEFAULT_VM_HOST = "192.168.56.10"
DEFAULT_VM_PORT = "22"
DEFAULT_VM_USER = "nbtbuild"
DEFAULT_KEY_PATH = Path.home() / ".ssh" / "nbt_qnx_build_rsa"
DEFAULT_HOST_ALIAS = "qnx-build"

MD5_RE = re.compile(r"^MD5:([0-9a-f]{2}:){15}[0-9a-f]{2}$")
SHA256_RE = re.compile(r"^SHA256:[A-Za-z0-9+/]{43}$")
PUBKEY_RE = re.compile(r"^ssh-rsa ([A-Za-z0-9+/=]+)(?: .*)?$")

PROBE_SCRIPT = """\
set -eu
test "$(uname -s)" = QNX
test "$(uname -r)" = 6.5.0
test "$(uname -p)" = x86
test "$(id -u)" != 0
uname -a
# SSH does not load the interactive QNX profile. Select the observed SDK explicitly.
QNX_HOST=/usr/qnx650/host/qnx6/x86
QNX_TARGET=/usr/qnx650/target/qnx6
PATH="$QNX_HOST/usr/bin:$PATH"
export QNX_HOST QNX_TARGET PATH
test -d "$QNX_TARGET"
command -v qcc
qcc -V
"""


def sha256_hex(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def compute_key_fingerprint(key_b64: str, mode: str) -> str:
    blob = base64.b64decode(key_b64.encode("ascii"), validate=True)
    if mode == "sha256":
        digest = base64.b64encode(hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")
        return "SHA256:" + digest
    digest = hashlib.md5(blob).hexdigest()
    return "MD5:" + ":".join(digest[i:i + 2] for i in range(0, len(digest), 2))


def validate_fingerprint(fingerprint: str) -> str:
    if MD5_RE.match(fingerprint):
        return "md5"
    if SHA256_RE.match(fingerprint):
        return "sha256"
    raise SystemExit("Supply MD5:... or SHA256:... verified at the VM console.")


def require_tools(*names: str) -> None:
    missing = [name for name in names if not shutil.which(name)]
    if missing:
        raise SystemExit("Required OpenSSH tool(s) missing: " + ", ".join(missing))


def base_ssh_options(config: Path, known_hosts: Path) -> list[str]:
    return [
        "-F", str(config),
        "-o", f"UserKnownHostsFile={known_hosts}",
        "-o", "GlobalKnownHostsFile=none",
        "-o", "StrictHostKeyChecking=yes",
        "-o", "ClearAllForwardings=yes",
        "-o", "ForwardAgent=no",
        "-o", "ForwardX11=no",
        "-o", "PermitLocalCommand=no",
        "-o", "ProxyCommand=none",
        "-o", "ProxyJump=none",
    ]


def verify_vm_ssh_config(config: Path, known_hosts: Path, alias: str = DEFAULT_HOST_ALIAS,
                         expected_host: str = DEFAULT_VM_HOST, expected_port: str = DEFAULT_VM_PORT) -> list[str]:
    if not config.is_file() or not known_hosts.is_file():
        raise SystemExit(
            "First configure .local/ssh_config and verify the VM host key in .local/known_hosts. "
            "See docs/DEVELOPMENT.md."
        )
    options = base_ssh_options(config, known_hosts)
    result = subprocess.run(["ssh", *options, "-G", alias], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("Invalid SSH configuration:\n" + result.stderr.strip())
    lines = set(result.stdout.splitlines())
    if f"hostname {expected_host}" not in lines or f"port {expected_port}" not in lines:
        raise SystemExit(f"This workflow is restricted to the QNX VM at {expected_host}:{expected_port}.")
    if "user REPLACE_ME" in lines:
        raise SystemExit("Set the VM username in .local/ssh_config first.")
    return options


def verify_elf32_i386(binary: Path) -> None:
    data = binary.read_bytes()
    if (len(data) < 52 or data[0:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1
            or data[18] != 3 or data[19] != 0):
        raise SystemExit("Artifact is not ELF32 little-endian Intel 80386.")


def cmd_known_hosts_helper(args: argparse.Namespace) -> int:
    """Internal OpenSSH KnownHostsCommand callback: emit and record only the expected VM key."""
    if args.host != args.expected_host or args.key_type != "ssh-rsa" or not args.key or args.key == "NONE":
        return 0
    if not re.fullmatch(r"[A-Za-z0-9+/=]+", args.key):
        return 1
    mode = "sha256" if args.expected.startswith("SHA256:") else "md5"
    actual = compute_key_fingerprint(args.key, mode)
    if actual != args.expected:
        print("Console fingerprint mismatch.", file=sys.stderr)
        return 1
    line = f"{args.host} {args.key_type} {args.key}\n"
    dest = Path(args.destination)
    if dest.exists():
        if dest.read_text(encoding="utf-8") != line:
            return 1
    else:
        fd = os.open(dest, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(line)
    sys.stdout.write(line)
    return 0


def cmd_init_trust(args: argparse.Namespace) -> int:
    require_tools("ssh", "ssh-keygen")
    hash_mode = validate_fingerprint(args.verified_fingerprint)
    known = args.known_hosts.resolve()
    known.parent.mkdir(parents=True, exist_ok=True)
    if known.exists():
        raise SystemExit(f"{known} already exists; review changes manually.")
    pending = known.parent / f"hostkey-{uuid.uuid4().hex}.pending"
    helper_cmd = (
        f'"{sys.executable}" "{Path(__file__).resolve()}" _known-hosts-helper '
        f'--expected "{args.verified_fingerprint}" --destination "{pending}" '
        f'--expected-host "{args.vm_host}" %H %t %K'
    )
    try:
        # OpenSSH 5.2 on QNX needs a MAC override unavailable in modern ssh-keyscan.
        # KnownHostsCommand emits the key only AFTER checking the expected fingerprint.
        subprocess.run(
            [
                "ssh", "-F", "/dev/null",
                "-o", "HostKeyAlgorithms=+ssh-rsa",
                "-o", "MACs=+hmac-sha1",
                "-o", "BatchMode=yes",
                "-o", "PreferredAuthentications=none",
                "-o", "PubkeyAuthentication=no",
                "-o", "PasswordAuthentication=no",
                "-o", "KbdInteractiveAuthentication=no",
                "-o", "StrictHostKeyChecking=yes",
                "-o", f"UserKnownHostsFile={pending}",
                "-o", "GlobalKnownHostsFile=none",
                "-o", f"KnownHostsCommand={helper_cmd}",
                "-o", "ConnectTimeout=5",
                f"root@{args.vm_host}", "true",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if not pending.is_file():
            raise SystemExit("Could not verify VM host key.")
        details = subprocess.run(
            ["ssh-keygen", "-E", hash_mode, "-lf", str(pending)],
            capture_output=True, text=True,
        )
        if details.returncode != 0:
            raise SystemExit("Cannot calculate host fingerprint.")
        parts = details.stdout.strip().split()
        if len(parts) < 2 or parts[1] != args.verified_fingerprint:
            raise SystemExit("VM host key mismatch.")
        pending.replace(known)
        os.chmod(known, 0o600)
        print(f"Verified VM host key saved to {known}.")
        return 0
    finally:
        if pending.exists():
            pending.unlink()


def ensure_keypair(key_path: Path) -> Path:
    key_dir = key_path.parent
    if not key_path.exists():
        key_dir.mkdir(parents=True, mode=0o700, exist_ok=True)
        os.chmod(key_dir, 0o700)
        result = subprocess.run(
            ["ssh-keygen", "-q", "-t", "rsa", "-b", "3072", "-N", "", "-C", "nbt-qnx-linux", "-f", str(key_path)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise SystemExit("SSH key generation failed:\n" + result.stderr.strip())
        os.chmod(key_path, 0o600)
    pub_path = Path(str(key_path) + ".pub")
    if not pub_path.is_file():
        raise SystemExit("Public key missing; existing private key was not overwritten.")
    mode = stat.S_IMODE(key_path.stat().st_mode)
    if mode & 0o077:
        os.chmod(key_path, 0o600)
    return pub_path


def write_ssh_config(config_path: Path, key_path: Path, user: str, vm_host: str, vm_port: str) -> None:
    if config_path.exists():
        raise SystemExit(
            f"{config_path} already exists and was not overwritten. "
            "Run `python3 scripts/qnx_vm_setup.py bootstrap` if appropriate."
        )
    config_path.parent.mkdir(parents=True, exist_ok=True)
    content = f"""\
Host {DEFAULT_HOST_ALIAS}
    HostName {vm_host}
    User {user}
    Port {vm_port}
    IdentityFile "{key_path}"
    IdentitiesOnly yes
    StrictHostKeyChecking yes
    ForwardAgent no
    ForwardX11 no
    ClearAllForwardings yes
    ConnectTimeout 5
    ServerAliveInterval 15
    ServerAliveCountMax 2
    HostKeyAlgorithms +ssh-rsa
    PubkeyAcceptedAlgorithms +ssh-rsa
    MACs +hmac-sha1
"""
    fd = os.open(config_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(content)


def cmd_bootstrap(args: argparse.Namespace) -> int:
    require_tools("ssh")
    config = args.ssh_config.resolve()
    known = args.known_hosts.resolve()
    pub_path = Path(str(args.key.expanduser()) + ".pub")
    status_path = LOCAL_DIR / "bootstrap-status.json"
    result_path = LOCAL_DIR / "bootstrap-result.txt"
    LOCAL_DIR.mkdir(parents=True, exist_ok=True)
    try:
        if not pub_path.is_file():
            raise SystemExit(f"Dedicated public key missing: {pub_path}")
        public = pub_path.read_text(encoding="utf-8").strip()
        match = PUBKEY_RE.match(public)
        if not match:
            raise SystemExit("Unexpected public key format (expected ssh-rsa).")
        options = verify_vm_ssh_config(config, known, expected_host=args.vm_host, expected_port=args.vm_port)
        from_clause = f'from="{args.allow_from}",' if args.allow_from else ""
        entry = (
            f"{from_clause}no-agent-forwarding,no-port-forwarding,no-X11-forwarding,no-pty "
            f"ssh-rsa {match.group(1)} nbt-qnx-vm-build"
        )
        remote_script = f"""\
set -eu
test "$(uname -s)" = QNX
test "$(uname -r)" = 6.5.0
umask 077
cd "$HOME"
mkdir -p .ssh
chmod 700 .ssh
touch .ssh/authorized_keys
chmod 600 .ssh/authorized_keys
entry='{entry}'
if grep -F -x "$entry" .ssh/authorized_keys >/dev/null 2>&1; then
    :
else
    printf '%s\\n' "$entry" >> .ssh/authorized_keys
fi
printf 'NBT_KEY_READY\\n'
"""
        print("QNX VM SSH setup: enter the password for the configured build user.")
        print("Password input is read directly by ssh and is not stored.")
        install = subprocess.run(
            [
                "ssh", *options,
                "-o", "BatchMode=no",
                "-o", "PubkeyAuthentication=no",
                "-o", "PreferredAuthentications=password",
                "-o", "NumberOfPasswordPrompts=1",
                DEFAULT_HOST_ALIAS, "sh -s",
            ],
            input=remote_script,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        result_path.write_text(install.stdout, encoding="utf-8")
        sys.stdout.write(install.stdout)
        if install.returncode != 0 or "NBT_KEY_READY" not in install.stdout:
            raise SystemExit("SSH key installation failed. No password was stored.")
        verify = subprocess.run(
            [
                "ssh", *options,
                "-o", "BatchMode=yes",
                "-o", "PreferredAuthentications=publickey",
                DEFAULT_HOST_ALIAS, "id",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        with result_path.open("a", encoding="utf-8") as handle:
            handle.write(verify.stdout)
        sys.stdout.write(verify.stdout)
        if verify.returncode != 0:
            raise SystemExit("Public key verification failed.")
        status_path.write_text(json.dumps({
            "state": "ready",
            "completedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        }, indent=2) + "\n", encoding="utf-8")
        print("SSH key authentication is ready.")
        return 0
    except SystemExit as exc:
        status_path.write_text(json.dumps({
            "state": "failed",
            "reason": str(exc),
            "completedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        }, indent=2) + "\n", encoding="utf-8")
        raise


def cmd_setup(args: argparse.Namespace) -> int:
    require_tools("ssh", "scp", "ssh-keygen")
    known = args.known_hosts.resolve()
    if not known.is_file():
        raise SystemExit(
            f"Verified VM host key missing at {known}. "
            "Run `python3 scripts/qnx_vm_setup.py init-trust --verified-fingerprint <fp>` first."
        )
    if args.expected_fingerprint:
        hash_mode = validate_fingerprint(args.expected_fingerprint)
        fp = subprocess.run(["ssh-keygen", "-E", hash_mode, "-lf", str(known)], capture_output=True, text=True)
        parts = fp.stdout.strip().split()
        if fp.returncode != 0 or len(parts) < 2 or parts[1] != args.expected_fingerprint:
            raise SystemExit("VM host key does not match the expected fingerprint.")
    if args.check_only:
        print("OpenSSH tools and VM host key verified. No changes made.")
        return 0
    key_path = args.key.expanduser()
    ensure_keypair(key_path)
    write_ssh_config(args.ssh_config.resolve(), key_path, args.user, args.vm_host, args.vm_port)
    print(f"SSH key and {args.ssh_config} prepared. Bootstrapping authorized_keys in the VM...")
    return cmd_bootstrap(args)


def cmd_probe(args: argparse.Namespace) -> int:
    require_tools("ssh")
    options = verify_vm_ssh_config(
        args.ssh_config.resolve(), args.known_hosts.resolve(),
        expected_host=args.vm_host, expected_port=args.vm_port,
    )
    options += ["-o", "BatchMode=yes", "-o", "PreferredAuthentications=publickey"]
    result = subprocess.run(["ssh", *options, DEFAULT_HOST_ALIAS, "sh -s"], input=PROBE_SCRIPT, text=True)
    if result.returncode != 0:
        raise SystemExit("VM/toolchain probe failed.")
    return 0


def cmd_smoke_build(args: argparse.Namespace) -> int:
    require_tools("ssh", "scp")
    options = verify_vm_ssh_config(
        args.ssh_config.resolve(), args.known_hosts.resolve(),
        expected_host=args.vm_host, expected_port=args.vm_port,
    )
    options += ["-o", "BatchMode=yes", "-o", "PreferredAuthentications=publickey"]
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    remote_dir = ".nbt-build/" + run_id
    source_path = ROOT / "src" / "hello_nbt.cpp"
    source = source_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    delimiter = "NBT_SOURCE_" + uuid.uuid4().hex
    script = PROBE_SCRIPT + f"""\
umask 077
cd "$HOME"
mkdir -p .nbt-build
mkdir '{remote_dir}'
cd '{remote_dir}'
cat > hello_nbt.cpp <<'{delimiter}'
{source}
{delimiter}
qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O2 -Wall -Wextra hello_nbt.cpp -o hello_nbt
./hello_nbt
printf 'NBT_VM_TEST_OK\\n'
"""
    result = subprocess.run(
        ["ssh", *options, DEFAULT_HOST_ALIAS, "sh -s"],
        input=script,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    sys.stdout.write(result.stdout)
    lines = result.stdout.splitlines()
    if result.returncode != 0 or "NBT_VM_TEST_OK" not in lines or "Hello World from QNX x86!" not in lines:
        raise SystemExit("Compilation or VM execution failed. No artifact accepted.")
    build_dir = ROOT / "build" / run_id
    build_dir.mkdir(parents=True, exist_ok=True)
    binary = build_dir / "hello_nbt"
    fetch = subprocess.run(["scp", "-O", *options, f"{DEFAULT_HOST_ALIAS}:{remote_dir}/hello_nbt", str(binary)])
    if fetch.returncode != 0:
        raise SystemExit("Artifact download failed.")
    verify_elf32_i386(binary)
    record = {
        "builtUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "sourceSha256": sha256_hex(source_path),
        "binarySha256": sha256_hex(binary),
        "compilerTarget": "4.4.2,gcc_ntox86_cpp",
        "vmTestPassed": True,
        "nbtTestPassed": False,
        "remoteDirectory": remote_dir,
    }
    (build_dir / "manifest.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    (build_dir / "vm-test.txt").write_text(result.stdout, encoding="utf-8")
    print(f"Verified VM artifact: {binary}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--ssh-config", type=Path, default=DEFAULT_CONFIG)
    common.add_argument("--known-hosts", type=Path, default=DEFAULT_KNOWN_HOSTS)
    common.add_argument("--vm-host", default=DEFAULT_VM_HOST)
    common.add_argument("--vm-port", default=DEFAULT_VM_PORT)

    p_trust = sub.add_parser("init-trust", parents=[common],
                             help="Verify and pin the QNX VM host key against a console-verified fingerprint")
    p_trust.add_argument("--verified-fingerprint", required=True,
                         help="MD5:... or SHA256:... fingerprint read at the local VM console")
    p_trust.set_defaults(func=cmd_init_trust)

    p_setup = sub.add_parser("setup", parents=[common],
                             help="Create dedicated SSH key, write .local/ssh_config, and bootstrap authorized_keys")
    p_setup.add_argument("--key", type=Path, default=DEFAULT_KEY_PATH)
    p_setup.add_argument("--user", default=DEFAULT_VM_USER)
    p_setup.add_argument("--allow-from", default="192.168.56.1",
                         help="restrict authorized_keys to this source IP (empty to omit from=)")
    p_setup.add_argument("--expected-fingerprint",
                         help="optional SHA256:... or MD5:... fingerprint to re-check against known_hosts")
    p_setup.add_argument("--check-only", action="store_true",
                         help="verify OpenSSH tools and known_hosts without modifying files")
    p_setup.set_defaults(func=cmd_setup)

    p_boot = sub.add_parser("bootstrap", parents=[common],
                            help="Install the dedicated public key into the QNX VM build user's authorized_keys")
    p_boot.add_argument("--key", type=Path, default=DEFAULT_KEY_PATH)
    p_boot.add_argument("--allow-from", default="192.168.56.1",
                        help="restrict authorized_keys to this source IP (empty to omit from=)")
    p_boot.set_defaults(func=cmd_bootstrap)

    p_probe = sub.add_parser("probe", parents=[common], help="Check QNX 6.5.0 VM identity and qcc toolchain")
    p_probe.set_defaults(func=cmd_probe)

    p_smoke = sub.add_parser("smoke-build", parents=[common],
                             help="Build and run src/hello_nbt.cpp in the QNX VM and verify the ELF32 artifact")
    p_smoke.set_defaults(func=cmd_smoke_build)

    p_helper = sub.add_parser("_known-hosts-helper", help=argparse.SUPPRESS)
    p_helper.add_argument("--expected", required=True)
    p_helper.add_argument("--destination", required=True)
    p_helper.add_argument("--expected-host", required=True)
    p_helper.add_argument("host")
    p_helper.add_argument("key_type")
    p_helper.add_argument("key")
    p_helper.set_defaults(func=cmd_known_hosts_helper)

    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
