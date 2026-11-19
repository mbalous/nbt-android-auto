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

## Usage

1. Connect the laptop to the car with an ENET cable and switch on the ignition. The usual NBT address is `169.254.199.99`.
2. Start the installer:
   - Windows: `NBT-AndroidAuto-Installer.exe`
   - macOS: `NBT-AndroidAuto-Installer.app`. On first use, right-click and choose Open because the app is unsigned.
3. Enter the IP address and password, then click **Install / Update**.
4. Restart iDrive.
5. Connect the phone over USB and select **Multimedia → Android Auto**.

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

## Without a packaged app (Windows, macOS, Linux)

```
pip3 install "paramiko>=3.4,<4"   # paramiko 4+ no longer supports the NBT's old ssh-rsa
python3 nbt_aa_installer.py                  # window
python3 nbt_aa_installer.py --cli install    # terminal
```

The `payload/` folder must be next to the script.

## For developers

- `python installer/make_payload.py [version]` collects the car-tested build files in `installer/payload/`.
  `manifest.json` contains the SHA-256 for each file. The folder is not checked in.
- `installer/build_windows.ps1` builds the Windows executable. `installer/build_mac.sh` builds the app on a Mac.
- For testing in a QNX VM, the hidden `--key <ssh-key>` and `--root <test-directory>` options install under a test
  directory instead of the system paths.
