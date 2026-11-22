#!/bin/bash
#
# flash-aa-open.sh - experimental open-toolchain build (and optional unsupported car install).
#
# Supported vehicle deployments should use the QNX 6.5.0 VM/SDP build (`scripts/build_all.py`).
# This script uses the experimental Docker-based open QNX toolchain (which still reuses an
# existing `accessory_window` binary from a prior payload).
#
# Usage:
#   ./flash-aa-open.sh                # build only; never connects to the car
#   ./flash-aa-open.sh --build-only   # build the payload, stop before the car (do this at home)
#   ./flash-aa-open.sh --install      # build, then install on the car (experimental/unsupported)
#   ./flash-aa-open.sh --install-only # install the payload built earlier (experimental/unsupported)
#
# Steps: toolchain (once, ~15 min) -> build programs -> installer/payload -> car link -> SSH check
#        -> installer --cli install (asks for the NBT root password) -> next steps.

set -u
REPO="$(cd "$(dirname "$0")" && pwd)"
if [ -x "$REPO/.venv/bin/python" ]; then
  VENV_PY="$REPO/.venv/bin/python"
else
  VENV_PY="${PYTHON:-python3}"
fi
NBT="169.254.199.99"
RELEASE="$REPO/.local/release-payload/NBT-AndroidAuto-Python-2026.11.10.2/payload"
NBT_AAD="$REPO/.local/reference-nbt-aad"
NBT_AAD_REV="cd1d9f87e330f90109068d8ccc3f7fd61f945d89"
INSTALLER="$REPO/installer/nbt_aa_installer.py"
PAYLOAD="$REPO/installer/payload"
STAMP="$PAYLOAD/.built-by-open-toolchain"

say()  { printf '\n\033[1;36m==> %s\033[0m\n' "$1"; }
warn() { printf '\n\033[1;33m!!  %s\033[0m\n' "$1"; }
die()  { printf '\n\033[1;31mXX  %s\033[0m\n' "$1" >&2; exit 1; }

DO_BUILD=1; DO_INSTALL=0
case "${1:-}" in
  "") ;;
  --build-only) DO_INSTALL=0 ;;
  --install) DO_INSTALL=1 ;;
  --install-only) DO_BUILD=0; DO_INSTALL=1 ;;
  -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
  *) die "Unknown option: $1 (use --build-only, --install, --install-only or nothing)" ;;
esac
[ "$#" -le 1 ] || die "Only one option may be specified."

# ---------------------------------------------------------------- 0. sanity
command -v "$VENV_PY" >/dev/null 2>&1 || die "Python not found ($VENV_PY). Create .venv or install python3."
"$VENV_PY" -c 'import paramiko' 2>/dev/null || die "paramiko missing in $VENV_PY (pip install \"paramiko>=3.4,<4\")."
[ -f "$RELEASE/plugin/libNbtAndroidAuto.so" ] || die "Reference payload missing: $RELEASE (needed to reuse accessory_window and PEMs)."

# ---------------------------------------------------------------- 1-3. build
if [ "$DO_BUILD" = 1 ]; then
  command -v docker >/dev/null || die "Docker is not installed."
  docker info >/dev/null 2>&1 || die "Docker is not running. Start the Docker daemon and re-run."
  branch="$(git -C "$REPO" rev-parse --abbrev-ref HEAD)"
  git -C "$REPO" merge-base --is-ancestor 7ef5884 HEAD 2>/dev/null \
    || die "Branch '$branch' does not contain the audio commit 7ef5884."
  say "Building from branch '$branch' ($(git -C "$REPO" rev-parse --short HEAD))"

  if [ ! -x "$REPO/build/qnx-open/bin/i486-pc-nto-qnx6.5.0-gcc" ]; then
    say "1/3 Open QNX toolchain not built yet - building it once (about 15-30 minutes)"
    "$REPO/toolchain/setup.sh" || die "Toolchain setup failed (logs: build/toolchain-stage*.log)."
  else
    say "1/3 Open QNX toolchain present (build/qnx-open)"
  fi
  docker image inspect qnx-open-host >/dev/null 2>&1 \
    || docker build --platform linux/amd64 -t qnx-open-host "$REPO/toolchain" >/dev/null \
    || die "Could not build the toolchain container image."

  [ -d "$NBT_AAD/.git" ] || die "NBT-AAD checkout missing: git clone https://github.com/Sq00ky/Supra-iDrive6-Android-Auto $NBT_AAD"
  [ "$(git -C "$NBT_AAD" rev-parse HEAD)" = "$NBT_AAD_REV" ] \
    || die "NBT-AAD must be at $NBT_AAD_REV: git -C $NBT_AAD checkout $NBT_AAD_REV"

  say "2/3 Building the head-unit programs (a few minutes)"
  out="$(cd "$REPO" && python3 scripts/build_open.py --reuse "$RELEASE" 2>&1)" || { printf '%s\n' "$out" | tail -30; die "Build failed."; }
  built="$(printf '%s\n' "$out" | sed -n 's|^programs: \(.*\)/programs$|\1|p' | tail -1)"
  [ -d "$built/programs" ] || die "Build finished but its output folder was not found."
  for f in audioconn nbt_android_auto native_service; do
    [ -f "$built/programs/$f" ] || die "Build output misses $f."
  done
  printf '    built: %s\n' "${built#$REPO/}"

  say "3/3 Assembling installer/payload"
  version="audio-open-$(date +%Y.%m.%d)-$(git -C "$REPO" rev-parse --short HEAD)"
  "$VENV_PY" "$REPO/installer/make_payload.py" "$version" --built "$built" --pem-dir "$RELEASE/rt" >/dev/null \
    || die "make_payload failed."
  printf '%s\n%s\n' "$version" "${built#$REPO/}" > "$STAMP"
  say "Payload ready: installer/payload ($version)"
fi

[ "$DO_INSTALL" = 1 ] || { echo; echo "Build only - stopping before the car. For an experimental install run: ./flash-aa-open.sh --install-only"; exit 0; }

# ---------------------------------------------------------------- 4. check the payload
[ -f "$PAYLOAD/manifest.json" ] || die "No installer/payload yet. Run ./flash-aa-open.sh --build-only first."
[ -f "$STAMP" ] || die "installer/payload was not made by this script (no open-toolchain stamp). Rebuild: ./flash-aa-open.sh --build-only"
"$VENV_PY" - "$PAYLOAD" <<'EOF' || die "Payload check failed (a file changed after the build). Rebuild with --build-only."
import hashlib, json, sys
from pathlib import Path
p = Path(sys.argv[1]); m = json.loads((p / "manifest.json").read_text())
for f in m["files"]:
    if hashlib.sha256((p / f["source"]).read_bytes()).hexdigest() != f["sha256"]:
        sys.exit("hash mismatch: " + f["source"])
names = {Path(f["source"]).name for f in m["files"]}
missing = {"audioconn", "nbt_android_auto", "native_service", "libNbtAndroidAuto.so"} - names
if missing: sys.exit("missing: " + ", ".join(sorted(missing)))
print("    payload %s: %d files, hashes OK" % (m["version"], len(m["files"])))
EOF

# ---------------------------------------------------------------- 5. link to the car
say "Checking Linux/WSL network link to the car"
if command -v ip >/dev/null 2>&1; then
  ip -4 addr show 2>/dev/null | grep -q '169\.254\.' || warn "No 169.254.x.x link-local IP seen on Linux interfaces - ensure ENET is up (or WSL mirrored networking is enabled)."
fi
ok=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  if ping -c1 -W2 "$NBT" >/dev/null 2>&1; then ok=1; break; fi
  printf '    ping %s ... no reply (try %s/10)\n' "$NBT" "$i"; sleep 2
done
[ "$ok" = 1 ] || die "Can't reach the NBT at $NBT. Check: ignition ON, ENET cable seated, 169.254.x.x link-local address up. Then: ./flash-aa-open.sh --install-only"
say "Car is reachable at $NBT."

# ---------------------------------------------------------------- 6. verify SSH port 22
if "$VENV_PY" -c "import socket, sys; s = socket.create_connection((sys.argv[1], 22), 2); s.close()" "$NBT" 2>/dev/null; then
  say "SSH (port 22) is open on the NBT."
else
  die "SSH port 22 is not open on $NBT. Unlock SSH first (see README.md#getting-ssh-access or docs/SSH-ACCESS-SERIAL.md)."
fi

# ---------------------------------------------------------------- 7. install
say "Installing the EXPERIMENTAL open-toolchain build - you'll be asked for the NBT root password"
warn "Unsupported experimental deployment path: supported vehicle binaries come from scripts/build_all.py (QNX VM)."
( cd "$REPO/installer" && "$VENV_PY" "$INSTALLER" --cli install --host "$NBT" ) \
  || die "Installer reported a failure (it verifies each file). Logs: cd '$REPO/installer' && '$VENV_PY' nbt_aa_installer.py --cli logs --host $NBT --output '$REPO/.local/aa-diag-open.zip'"

# ---------------------------------------------------------------- 8. done + next steps
say "Install finished and verified."
cat <<EOF

NEXT (at the car):
  1. Restart iDrive: hold the volume/power knob ~30 s until the screen reboots.
  2. Plug the phone into the car USB port. On screen: Multimedia -> Android Auto. Accept on the phone.
  3. Check video, iDrive knob and cluster arrows first.
  4. Play music with Bluetooth disconnected -> it should come from the car speakers.

IF SOMETHING'S OFF:
  - Save logs:               cd "$REPO/installer" && "$VENV_PY" nbt_aa_installer.py --cli logs --host $NBT --output "$REPO/.local/aa-diag-open.zip"
  - Force Bluetooth audio:   ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa root@$NBT 'touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/bt-audio'   (then restart iDrive)
  - Radio stuck muted:       ssh -o HostKeyAlgorithms=+ssh-rsa -o PubkeyAcceptedAlgorithms=+ssh-rsa root@$NBT '/var/opt/hmi/data/rw/NbtAndroidAutoRT/audioconn -r 0xb8'
  - Remove everything:       cd "$REPO/installer" && "$VENV_PY" nbt_aa_installer.py --cli uninstall --host $NBT
EOF
