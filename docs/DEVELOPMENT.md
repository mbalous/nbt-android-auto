# Development guide

This page explains how the pieces are built and how to work on the car safely. **One command builds every program of
the installed package** in the QNX VM: `python scripts/build_all.py` (see [Building everything](#building-everything)).
See below for the historical binary comparison with installer 2026.11.10.2.

## What you need

- **QNX Software Development Platform 6.5.0 (x86)** in a virtual machine (VirtualBox works). It can be downloaded
  from QNX; it is not part of this repository. The compiler is `qcc -V4.4.2,gcc_ntox86` (C) / `gcc_ntox86_cpp` (C++).
  See [Setting up QNX 6.5.0](#setting-up-qnx-650) below. **Binaries for supported vehicle deployment must be built in
  this QNX VM.**
- A **Linux or WSL** host with Python 3.10+, OpenSSH (`ssh`, `scp`, `ssh-keygen`), `git`, and (optional, for the
  experimental open toolchain) Docker. For the installer GUI, install `tkinter` (e.g. `python3-tk`) and use X11/Wayland
  or WSLg; `--cli` works headless.
- An NBT with SSH root access for testing. A few proprietary libraries have to be copied **from your own NBT** for
  linking (see below). Never commit them.

On WSL, native Linux networking must be able to reach both the QNX VM (host-only/bridged address) and the NBT ENET
link-local interface (`169.254.199.99`, e.g. via WSL mirrored networking).

Private files belong in `.local/` and build output in `build/`; both are ignored by git.

## Setting up QNX 6.5.0

**Experimental open toolchain (CI / prototyping only):** [toolchain/README.md](../toolchain/README.md) builds most
programs with an open GCC 4.4.2 toolchain in Docker (no SDP, no VM), including `accessory_window` and the HMI plugin
(linked against stub HMI libraries by default). `./flash-aa-open.sh --build-only` runs this
experimental build; its `--install` / `--install-only` options are an explicit, unsupported experimental override. Use
the QNX 6.5.0 VM below for supported vehicle deployment binaries.

1. Download **QNX 6.5.0** from QNX.
2. Install it in a VM. This video shows everything you need, step by step:
   [QNX 6.5.0 setup on YouTube](https://www.youtube.com/watch?v=3yjVaIwzPsc&t=182s).
3. **Tip: adjust the BIOS date of the VM** (set it back to an earlier date). In VirtualBox you can do this with an
   offset in milliseconds while the VM is off, for example one year back:

   ```sh
   VBoxManage modifyvm "QNX" --biossystemtimeoffset -31536000000
   ```

## QNX VM

The development VM uses a host-only network and an unprivileged build user (`nbtbuild`). The historical static
addresses were VM `192.168.56.10` and host `192.168.56.1`; DHCP can assign a different VM address. Check the VM's
actual address at its console (`ifconfig en0`) and the host-only network before connecting. The build VM is not the car.

If using the historical static configuration, after each VM start, as root on the VM console:

```sh
slay dhcp.client; ifconfig en0 192.168.56.10; /usr/sbin/sshd
```

Do not run that command blindly on a differently configured network: stopping DHCP and changing the address disrupts
existing connections. On a fresh QNX installation, create a non-root build account and set a nonempty password
interactively; verify the VM's SSH authentication settings. Start `sshd` on the VM if it is not already listening.

### Trust and connection

Copy `config/ssh_config.example` to `.local/ssh_config` and set `HostName` to the actual VM address, `User` to the build
user, and `IdentityFile` to your private key. Keep private keys and passwords out of the repository. The build scripts
use key authentication in batch mode, even if an interactive password login works.

Verify the host key independently on the VM console before trusting it:

```sh
ssh-keygen -lf /etc/ssh/ssh_host_rsa_key.pub
```

For Linux or WSL, scan the VM address into a temporary candidate file, then compare its fingerprint with the console
output (use the same fingerprint format on both sides):

```sh
mkdir -p .local
umask 077
VM_IP=192.168.56.10  # Replace with the VM's actual address.
candidate=$(mktemp .local/qnx-host-key.XXXXXX)
ssh-keyscan -T 5 -t rsa "$VM_IP" > "$candidate"
ssh-keygen -E md5 -lf "$candidate"
```

Only after the fingerprints match, add the candidate key to `.local/known_hosts`; preserve any existing entries and
investigate conflicts rather than overwriting them. A key obtained by scanning the network alone is not verified.
The build scripts use `.local/ssh_config` and `.local/known_hosts` by default; ordinary `ssh` uses `~/.ssh/config` and
`~/.ssh/known_hosts` unless told otherwise. Do not commit either known-hosts file.

QNX OpenSSH 5.2 may offer only the legacy RSA/SHA-1 host-key signature. If negotiation fails, enable
`HostKeyAlgorithms +ssh-rsa` in the `qnx-build` host block. Fedora 44 WSL also required
`PubkeyAcceptedAlgorithms +ssh-rsa` even for password login; it can also be needed for RSA key authentication.
Add `MACs +hmac-sha1` only if no MAC matches. Keep these exceptions limited to the VM. No key-exchange downgrade was
needed for the observed VM.

Some Linux distributions also block SHA-1 signatures in OpenSSL: Fedora 44 WSL required the following process-scoped
override, in addition to the host-specific SSH options. Run this from the repository after verifying the host key:

```sh
OPENSSL_ENABLE_SHA1_SIGNATURES=1 ssh -F .local/ssh_config \
  -o UserKnownHostsFile=.local/known_hosts -o StrictHostKeyChecking=yes qnx-build 'id; uname -a'
```

If the client reports `no matching host key type found` or `error in libcrypto`, check both SSH options and the
distribution's crypto policy. The override weakens signature policy for that process; do not enable it globally.
Prefer upgrading the VM's SSH server to support modern host-key signatures when possible.

One WSL setup also uses `~/.local/bin/ssh` as a local wrapper to apply the OpenSSL override when the first argument is
the VM's literal IP or `user@IP`, with the SSH algorithm options in `~/.ssh/config` for that IP. This is local machine
state, not repository setup: it does not cover aliases or commands with options before the destination. An existing
Bash shell may need `hash -r` once to forget a cached path to the system `ssh`; a new shell does not. Scripts that
invoke `ssh` with options first, and `scp`, need the explicit process-scoped override instead.

Helper script for Linux and WSL (`scripts/qnx_vm_setup.py`):

- `python3 scripts/qnx_vm_setup.py init-trust --verified-fingerprint 'MD5:...'` verifies and pins the VM host key into
  `.local/known_hosts` after you compare it on the VM console (`ssh-keygen -lf /etc/ssh/ssh_host_rsa_key.pub`).
- `python3 scripts/qnx_vm_setup.py setup` creates `~/.ssh/nbt_qnx_build_rsa` (with `0700`/`0600` permissions), writes
  `.local/ssh_config` without overwriting an existing config, and runs `bootstrap` to install your public key for the
  build user (password typed interactively into `ssh`, never stored).
- `python3 scripts/qnx_vm_setup.py probe` checks the VM identity and `qcc` toolchain;
  `python3 scripts/qnx_vm_setup.py smoke-build` builds and runs `src/hello_nbt.cpp` in the VM and verifies the ELF32
  i386 artifact.

These subcommands default to `192.168.56.10` (override with `--vm-host`) and, for key bootstrapping, restrict
`authorized_keys` to `192.168.56.1` (override with `--allow-from`). Older VM protocol-test scripts also require
`192.168.56.10`. Do not bypass target checks without verifying the VM identity.

Environment for every build shell in the VM:

```sh
export QNX_HOST=/usr/qnx650/host/qnx6/x86
export QNX_TARGET=/usr/qnx650/target/qnx6
export PATH=$QNX_HOST/usr/bin:$PATH
```

## Building everything

Which program is built from which files: [src/README.md](../src/README.md).

### Once

1. **NBT-AAD** (the Android Auto protocol core) at the exact commit the patches are made for:

   ```sh
   git clone https://github.com/Sq00ky/Supra-iDrive6-Android-Auto .local/reference-nbt-aad
   git -C .local/reference-nbt-aad checkout cd1d9f87e330f90109068d8ccc3f7fd61f945d89
   ```

   Keep this checkout unmodified; the build applies our changes to a copy. When bumping the pinned revision, update
   `NBT_AAD_REVISION` in `scripts/build_all.py` (all other Python build/test scripts import it from there), plus
   `flash-aa-open.sh`, `.github/workflows/ci.yml`, and docs, then check `scripts/aa_session_overlay.py` and
   `patches/*.patch` against the new commit.
2. **For the default build**, copy two HMI libraries from your own NBT for the plugin: `libAppFramework.A.so` and `libCoreFramework.A.so` from
   the HMI's library directory on the head unit (read only) into a folder in the VM, e.g. `~/.nbt-build/hmi-libs/`.
   They are BMW software: never commit or publish them. Without them the default build skips the plugin.
   Alternatively, `--hmi-stubs` builds the plugin without these files; this path is not yet vehicle-qualified.
3. SSH from your PC to the VM works with a key (see [QNX VM](#qnx-vm)); the build runs as the normal VM user, not root.
   Mbed TLS 3.6.7 is downloaded automatically on the first run (SHA-256 checked).

### Every build

```sh
python3 scripts/build_all.py --host qnx-build --hmi-libs '$HOME/.nbt-build/hmi-libs' --compare installer/payload
```

For a complete SDP build without BMW libraries or a car:

```sh
python3 scripts/build_all.py --host qnx-build --hmi-stubs
```

The stubs provide imported symbol names solely for linking and are not packaged. The plugin still requires the
real BMW libraries at runtime. Successful linking does not establish ABI or runtime compatibility, and the SDP
stub-linked path is **not yet qualified for vehicle deployment**.
For all four SDP/open and real/stub combinations, see the [HMI build matrix](../toolchain/README.md#build-matrix).

On Fedora WSL (or other Linux distributions whose OpenSSL blocks SHA-1 signatures), apply the
[process-scoped SHA-1 override](#trust-and-connection) to the build process so both its `ssh` and `scp` children
inherit it:

```sh
OPENSSL_ENABLE_SHA1_SIGNATURES=1 python3 scripts/build_all.py --host qnx-build
```

For a checkout or worktree without its own `.local` inputs, pass `--ssh-config`, `--known-hosts`, and `--nbt-aad`
pointing to the existing files and pinned reference checkout. If the key path is missing or trust fails, fix the VM
configuration; do not disable host-key checks. A successful VM build does not install or test anything on the car.

`--identity <key>` if your SSH config does not name the VM key; `--compare` is optional (an installer payload, for
example unpacked from a release ZIP). The run takes about 5 minutes (most of it Mbed TLS) and leaves:

- `build/qnx-build-<time>-<id>/programs/` - all programs for the head unit, plus `boot.sh` / `restart.sh`
- `build/qnx-build-<time>-<id>/plugin-resources/` - plugin XML, manifest and the text/image ZIPs
- `build/qnx-build-<time>-<id>/build.log` - the full VM log with the comparison table
- `build/qnx-build-<time>-<id>/build-provenance.json` - toolchain, HMI link mode and plugin hash

Each build creates a unique VM directory. An upload or artifact download failure retains that directory and reports
its path so the failure can be investigated; the program does not treat an interrupted download as a successful build.
If the plugin was skipped, install the two HMI libraries on the VM or opt in to `--hmi-stubs` and rebuild.

What `build_all.py` puts together (on your PC) and `scripts/build-qnx.sh` compiles (in the VM):

| Step | What |
|---|---|
| Mbed TLS 3.6.7 | release archive + `scripts/mbedtls_legacy_time.py` (old phone certificate dates) + QNX config (no AES-NI/ASM/PadLock), static libraries |
| NBT-AAD core `libnbt_aa_core.a` | upstream `milestone-7/src` + `scripts/aa_session_overlay.py` + `patches/*.patch` + our `aa_cryptor_mbedtls.cpp` (Mbed TLS instead of OpenSSL) |
| `nbt_android_auto` | `nbt_android_auto.c` (`-DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE`), `aa_video_capture.cpp`, `tls_usb_probe.cpp` + core + Mbed TLS, `-lsocket -lm -lusbdi` |
| `native_service` | C, `-O2` |
| `accessory_window` | C, no optimisation |
| `decode_runner` | C++, `-O2` |
| `fifo_http` | C++, no optimisation |
| `nbt_knob`, `nbt_hwvideo`, `nbt_cluster` | C++ `-O1`, with the MOST/DSI client from `knob/ref/` |
| `libNbtAndroidAuto.so` | C++ shared library without exceptions/RTTI, linked against the two HMI libraries or opt-in stubs |

**Verified on 2026-11-11** against installer 2026.11.10.2: `native_service`, `decode_runner`, `fifo_http`,
`accessory_window`, `nbt_knob`, `nbt_cluster` and `libNbtAndroidAuto.so` were byte-identical; the session executable,
which had a different name at the time, was
identical apart from the symbol table (file names). `nbt_hwvideo` differs on purpose: the source has one more
read-only diagnostic option (`--registry-all`, 2026-11-08) than the shipped program.

## Installer and release

1. `python3 installer/make_payload.py <version> --built build/qnx-build-<time>-<id> [--pem-dir <folder>]`
   collects the QNX VM build into `installer/payload/` and writes `manifest.json` with SHA-256 per file. Always pass
   the explicit `--built` directory produced by `scripts/build_all.py` so there is no ambiguity about which build is
   packaged. `--pem-dir` defaults to `installer/payload/rt` (the PEM files are read before the folder is replaced; on a
   fresh checkout take them from `rt/` of a release payload).
   The manifest records build provenance. Builds made before provenance was added must be rebuilt before packaging.
   Installing an SDP stub-linked payload requires `--allow-qnx-hmi-stubs` in the CLI or explicit GUI confirmation;
   this is an accidental-deployment guard, not a signature or proof of safety.
2. Run the Python installer directly on Linux or WSL (`pip3 install "paramiko>=3.4,<4"` — paramiko must stay below 4
   because the NBT only offers `ssh-rsa`). No platform-packaged installer binaries are built.
3. Test against the VM first: the installer has hidden options `--key <ssh key>` and `--root <test dir>` to install
   below a test folder instead of the system. Then test on a car.

Release assets: the Python installer + payload archive and SHA-256 sums.

The older test scripts `scripts/Test-QnxTls.py`, `Test-AndroidAutoCore.py` and `Test-AndroidAutoSession.py` still run
the offline TLS and protocol tests (simulated phone) in the VM.

## Continuous integration

GitHub Actions runs on pull requests and pushes to `main`. It cross-builds the supported programs with the open QNX
toolchain and compiles and runs the host-compatible H.264 framing, capture output, FIFO HTTP bridge and PNG packet tests
on Linux, both normally and with AddressSanitizer and UndefinedBehaviorSanitizer. It also checks Python syntax and runs
the TLS inspection and audio safety tests. Run the host checks locally with:

```sh
python3 scripts/test_host.py
python3 scripts/test_host.py --sanitize
python3 -m pip install cryptography==44.0.3 "paramiko>=3.4,<4"
python3 -m compileall -q scripts installer tools experiments
python3 -m unittest discover -s scripts -p 'test_*.py' -v
```

Run the Python checks in the environment where `cryptography` is installed. If the sanitizer build fails because its
runtime is missing, use a supported compiler and install its matching ASan/UBSan runtimes; a normal host test is not a
substitute for the sanitizer run.

The open cross-build can use link-only stubs and includes an experimental `accessory_window`; it does not establish ABI compatibility or
vehicle safety. See the [open versus SDP comparison](../toolchain/COMPARISON.md) for measured coverage and remaining
validation. The complete SDP build and VM tests still require the toolchain and inputs described above; installer
builds also require a release payload. MemorySanitizer is not enabled: it requires an instrumented toolchain and
dependencies and would not cover the QNX target on hosted Linux.

## Working on the car

`tools/nbt_ssh_session.py` keeps one SSH session open (you type the root password in your own terminal; it is never
stored) and `tools/nbt_request.py` sends single commands or scripts through it:

```sh
python tools/nbt_ssh_session.py
python tools/nbt_request.py "tr -d '\015' | sh -s list" --input-file scripts/car-dev-leftovers.sh
```

Pin the NBT host key in `.local/nbt_known_hosts` first. The NBT shell lacks many tools (`head`, `printf`, `env`,
`which`); use `sed Nq`, `echo`, `ls -R`. Logs: `sloginfo`, `/fs/ram3/nbt-aa-boot.log`, the package `logs/` folder.

### Safety rules (learned the hard way)

- **NEVER run `/sbin/shutdown` on the head unit**, including through scripts or automation. It causes a boot lock that
  can only be cleared by disconnecting the vehicle battery. A diagnostic reset must go through the vehicle's ZGW
  (central gateway); see the [NBT reset guide](../src/idrive/reboot/README.md).
- **Never stop or restart stock processes** such as `io-nme` or `HmiMain3D`. Stop only your own processes, with SIGTERM.
- **Do not revive the WAL sprite overlay** (`experiments/wal-overlay/`): unpaced sprite uploads deadlocked the
  `WalServer` and crashed the NVIDIA driver, which restarted the head unit.
- **No standalone EGL experiments** on the car; one crashed it. Never copy VM-only libraries to the car.
- Write files as `.tmp`, compare, then rename. Do not blindly repeat a command whose result is unknown.
- Test new things in RAM (`/fs/ram3/...`) first; it is gone after a restart.
- For an ordinary hang (not the `/sbin/shutdown` boot lock), hold the power button for about 30 s to restart iDrive.

## Open topics

See [CONTRIBUTING.md](../CONTRIBUTING.md#where-help-is-needed).
