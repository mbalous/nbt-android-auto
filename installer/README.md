# NBT Android Auto Installer

Installs Android Auto on an original BMW NBT (QNX), adding an **Android Auto** entry under **Multimedia**. The installer
needs SSH access to the NBT: its IP address and root password. The password is not stored.

## Before installation: enable SSH access

The stock NBT does not accept SSH logins, so SSH must first be unlocked.

- **How the developer did it:** with **FeatureInstaller**. This can take **several attempts**, and the last step may
  report **errors even when SSH was enabled**. Check the connection afterwards, for example with
  `ssh root@169.254.199.99`.
- **Root password:** see the [SSH access guide (supramkv.com)](https://www.supramkv.com/threads/ssh-access-guide-supra-nbtevo-headunit.24190/).
  The thread is about the NBT Evo in the Toyota Supra; the root password is given there.

Enabling SSH changes the device configuration. As with everything here, you do it at your own risk.

## Usage (Linux and WSL)

If upgrading from a version before the executable rename, uninstall the previous package first and restart iDrive.
The installer does not migrate old files automatically.

1. Connect the Linux or WSL laptop to the car with an ENET cable and switch on the ignition. The usual NBT address is
   `169.254.199.99`. On WSL, ensure the ENET link-local interface (`169.254.x.x`) is reachable from Linux (for example,
   with WSL mirrored networking) and use `--cli` if no X11/WSLg display is available.
2. Install the Python dependency (once):
   ```sh
   pip3 install "paramiko>=3.4,<4"   # paramiko 4+ no longer supports the NBT's old ssh-rsa
   ```
3. Start the installer (the `payload/` folder must be next to `nbt_aa_installer.py`):
   ```sh
   python3 installer/nbt_aa_installer.py                  # GUI (requires tkinter + X11/Wayland/WSLg)
   python3 installer/nbt_aa_installer.py --cli install    # terminal (works headless on Linux/WSL)
   ```
4. Enter the IP address and password, then install.
5. Restart iDrive.
6. Connect the phone over USB and select **Multimedia → Android Auto**.

## Android Auto features on the NBT

- **Picture mode** on the Android Auto page:
  - **Widescreen** (default) uses the full width.
  - **16:9** is sharper and leaves the iDrive split screen visible on the right.

  Changes take effect the next time the phone connects.
- **Control** with the iDrive controller (turn, press, tilt, BACK).
- **Instrument-cluster navigation** (6WB) while Google Maps guides a route:
  - Arrow, distance bar and street name.
  - Motorway and road shields.
  - Lanes.

  BMW navigation takes priority when it is guiding a route.
- **Restart Android Auto** on the Android Auto page restarts only the Android Auto service, not the NBT. It first saves
  a diagnostic snapshot, which **Save diagnostic logs** includes.

In **Widescreen** mode, switch off the iDrive split screen (OPTION → split screen off). Otherwise, tilting right twice
quickly opens the side panel.

**Uninstall** removes exactly the two installed folders. **Save diagnostic logs** downloads the logs as a ZIP, for
example to attach to a bug report.

## Files written to the NBT

| Location | Content |
|---|---|
| `/var/opt/hmi/data/rw/NbtAndroidAutoRT/` | Service, cluster navigation, certificates, `boot.sh`, `restart.sh`, `vid.dpi`, `logs/`, `snapshots/` |
| `/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto/` | iDrive plugin (menu entry) |

Each file is first written as `.tmp`, read back and compared byte for byte, and only then renamed. No other system
files are changed.

## Verified SSH host-key checking

For verified SSH host-key checking, pass an OpenSSH `known_hosts` file containing the independently verified NBT key:

```sh
python3 installer/nbt_aa_installer.py --cli install --host 169.254.199.99 --known-hosts /path/to/known_hosts
```

Unknown or changed keys are rejected before authentication. Verify the fingerprint through a trusted channel first;
`ssh-keyscan` alone does not establish identity. The same option works for the GUI and other CLI actions. Without it,
the installer accepts an unknown host key and logs its fingerprint after connecting.

## Safer car testing

The merged build has had limited parked testing; see the [test checklist](../CAR-TEST-aa-dev.md). Park safely,
use stable vehicle power and confirm that the head unit is an original NBT before installation.
Read-only inspection should establish the existing installation state, free space
and whether another Android Auto solution or similarly named service is present. Keep the existing installation and
settings backed up, and prepare a known-working recovery path where possible.

The installer's preflight creates its target folders and a temporary write-test file. Each payload file is verified
before rename, but an interrupted installation can leave a mixture of files. If the result is uncertain, stop and
inspect before retrying or restarting. Uninstall deletes the two folders above and stops `native_service` and
`nbt_cluster` by name; check that those processes belong to this package first. It cannot guarantee recovery if SSH or
the head unit is no longer available. Logs may contain vehicle and phone details; keep them private.

## For developers

- Build vehicle binaries in the QNX 6.5.0 VM (`python3 scripts/build_all.py`), then assemble the payload from that
  specific build directory:
  `python3 installer/make_payload.py <version> --built build/qnx-build-<time>-<id>`
  collects the build files in `installer/payload/`. `manifest.json` contains the SHA-256 for each file. The folder is
  not checked in. The packager needs build provenance, so rebuild older build directories first.
- `python3 scripts/build_all.py --hmi-stubs` builds the HMI plugin without car libraries using the SDP VM.
  This path is not yet vehicle-qualified. The installer records its link mode and requires
  `--allow-qnx-hmi-stubs` for CLI installation or explicit GUI confirmation. This opt-in is not proof of safety
  or a tamper-resistant approval. The real BMW libraries must still be present on the head unit.
- The installer is distributed and run directly with Python 3 on Linux and WSL (no packaged OS binaries).
- For testing in a QNX VM, the hidden `--key <ssh-key>` and `--root <test-directory>` options install under a test
  directory instead of the system paths.
