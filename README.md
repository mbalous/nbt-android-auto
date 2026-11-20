# NBT Android Auto

Wired Android Auto for the **original BMW NBT head unit** (CIC successor, 2013–2016, QNX 6.5 on x86), without an
additional Android Auto interface. The phone's picture uses the NBT's video path, the iDrive controller handles input,
and compatible instrument clusters can display navigation guidance.

This fork brings together [JayJay3071's original project](https://github.com/JayJay3071/nbt-android-auto),
[Valinet's fork](https://github.com/valinet/nbt-android-auto) and
[Adityadroid's fork](https://github.com/adityadroid/nbt-android-auto), preserving their commit histories. It combines
the `f6` audio and iDrive work with recovery, diagnostics, volume and experimental build tooling, while keeping
security-sensitive compatibility options and additional persistent logging opt-in.

> **Status: experimental community prototype.** The original project was tested on a BMW F07 and Valinet's fork on a
> BMW F30; **this merged version has not been tested in a car**. Host CI does not verify QNX ABI compatibility or
> vehicle safety. NBT Evo is not supported. Read the [disclaimer](#disclaimer) first.

🎬 **Earlier prototype:** [see it running in the car (Instagram video)](https://www.instagram.com/reel/DfkW1wYsB__/)

See [docs/FORK-CHANGES.md](docs/FORK-CHANGES.md) for the Valinet-derived functionality,
[docs/WORKFLOW.md](docs/WORKFLOW.md) for build and debug commands, and
[CAR-TEST-aa-dev.md](CAR-TEST-aa-dev.md) for a future vehicle-test checklist.

## Features

- **Android Auto over USB**: wired Android Auto over the phone's USB connection; compatibility varies by phone.
- **Video**: 800x480 through the stock Cinemo/VideoApp path. Earlier vehicle tests reported about 23 fps and 27 % CPU.
  Two picture modes: **wide** (full width) and **16:9** (sharper, iDrive split screen stays visible on the right).
- **iDrive controller**: turn = rotate, tilt = D-pad, press = OK, BACK = back; BACK and tilts stay in Android Auto, also
  with iDrive split screen on. MENU / MEDIA / COM / OPTION leave Android Auto, which keeps running in the background.
  **NAV** brings Android Auto to the front; pressed again in Android Auto, it switches to Android Auto's sound (or
  play / pause).
- **Android Auto sound in the car**: over USB, into main audio source `f6` (no separate audio cable). When the phone starts playing,
  the car switches to it; choosing another source in iDrive pauses the phone; NAV in Android Auto switches back and
  resumes. The browser audio route is available via `aa-audio`, and `bt-audio` selects Bluetooth. Factory AUX remains
  unchanged; the former `media-jack` mode is no longer supported. Software volume applies to both USB routes.
- **Steering wheel**: roller up / down = next / previous track, roller push = play / pause, only while Android Auto's
  audio source plays. After the phone button the roller stays with iDrive's contact list for 4 s.
- **Menu entry** *Multimedia → Android Auto*: shows the status, starts the service as soon as the HMI boots and brings a
  running session to the front. **Start at boot** (on / off) brings Android Auto to the front once per boot.
- **Parking assistant**: when it closes, Android Auto comes back if it was on screen before.
- **Navigation in the instrument cluster** (6WB cluster) while Google Maps guides a route: arrow, distance bar, street
  name, motorway/road shields and lanes (lanes not yet confirmed with a real phone). BMW navigation takes priority when it guides itself.
- **Now playing and recovery**: track metadata, session and video recovery, and configurable steering-wheel key mappings
  from the integrated forks.
- **Restart Android Auto** button on the Android Auto page restarts only the service (not the NBT) and saves a
  diagnostic snapshot first. **Restart system** reboots the head unit (also from a laptop: `scripts/nbt_reset.py`).

## Known limitations

- Calls and the microphone still use Bluetooth. Details: [docs/AA-AUDIO-PLAN.md](docs/AA-AUDIO-PLAN.md).
- The experimental wired car-microphone integration and BMW coding viewer are not included.
- If the USB audio output fails, the next session falls back to Bluetooth. Prompts over other sources are disabled
  after a browser-route failure until reboot.
- Software volume on `f6` may be attenuated again by the car; use `aa-volume-off` if necessary. Neither this behavior
  nor the merged service's advisory locking has been vehicle-tested.
- **800x480 only.** The hardware capture (`dev-nvideocapture`) is limited to WVGA. 720p was tested: not sharper, more
  input lag.
- The controller has a noticeable delay because of the video buffer.
- No map picture in the instrument cluster: the 6WB cluster shows pictures only in its top line (turn arrow, shields).
- The merged version has not yet been tested in a car.

What changed between versions: [CHANGELOG.md](CHANGELOG.md).

## Requirements

- BMW with the **original NBT** (not NBT Evo, not CIC). NBT Evo uses a different DSI library and is not supported.
- **SSH root access to the NBT** and its root password. The installer does not set this up for you, see
  [Getting SSH access](#getting-ssh-access).
- A laptop connected to the car with an **ENET cable** (the NBT usually answers at `169.254.199.99`).
- A USB cable for the phone.

## Getting SSH access

The stock NBT does not accept SSH logins, so SSH has to be unlocked (patched) once before you can install anything.

- **How it was done on the developer's car:** with **FeatureInstaller**. It can take **several attempts**. The last
  step sometimes reports **errors even though SSH access was enabled**, so try to connect (for example
  `ssh root@169.254.199.99`) before you assume it failed.
- **Free alternative:** enable SSH yourself through the debug serial port on the head unit's Jacinto board, with a USB
  serial adapter: [docs/SSH-ACCESS-SERIAL.md](docs/SSH-ACCESS-SERIAL.md). No FeatureInstaller, and nobody else
  connects to your car remotely to install things.
- **Root password:** the default is `ts&SK412`. It can also be looked up in this forum thread:
  [SSH access guide (supramkv.com)](https://www.supramkv.com/threads/ssh-access-guide-supra-nbtevo-headunit.24190/).
  The thread is about the NBT Evo in the Toyota Supra; the root password is given there.

Unlocking SSH changes the head unit's configuration. As with everything here, you do it at your own risk.

## Install

If upgrading from a version before the executable rename, **uninstall the previous package first** using the installer,
then restart iDrive before installing the new package. The installer does not migrate old files automatically.

Prebuilt installers may be published on [Releases](../../releases); check that the release actually contains the version
you want. Older upstream or fork releases do not contain this merged version. To build a payload from this branch, see
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md); no prebuilt release of the merged version is currently available.

1. Use a matching release installer if available, or build a payload and run the Python installer:
   `pip install "paramiko>=3.4,<4"`, then `python installer/nbt_aa_installer.py` with the `payload/` folder next to it.
2. Connect the ENET cable, ignition on, enter IP address and root password, click **Install / Update**.
3. Restart the iDrive.
4. Plug in the phone and choose **Multimedia → Android Auto**.

The installer writes only two folders (`/var/opt/hmi/data/rw/NbtAndroidAutoRT/` and
`/var/opt/hmi/data/rw/Plugins/NbtAndroidAuto/`), verifies every file byte by byte and can uninstall them again. It
never stores your password. Details: [installer/README.md](installer/README.md).

**Unsigned binaries:** the Windows .exe is not code-signed yet, so Windows 11 *Smart App Control* may block it. Use the
Python version in that case; please do not switch off Smart App Control for this. The macOS app is not notarized: open
it with right-click → Open.

## Configuration and diagnostics

Optional switches are files in `/var/opt/hmi/data/rw/NbtAndroidAutoRT/`. Changes take effect after restarting the
Android Auto service unless noted otherwise.

| File | Effect |
|---|---|
| `aa-audio` | Use the browser USB audio route instead of the default `f6` route. |
| `bt-audio` | Use Bluetooth audio instead of USB audio. |
| `aa-volume-off` | Disable software media-volume adjustment. |
| `prompts-off` | Disable prompts over other car audio sources. |
| `tls-allow-date-errors` | Allow expired or not-yet-valid phone certificates. Strict date validation is the default; other certificate checks remain enforced. |
| `incident-logging` | Enable bounded persistent incident snapshots and event logs. |
| `extended-log-history` | Enable bounded persistent boot-log history, starting at the next head-unit boot. |

Baseline logs and restart snapshots remain enabled without these flags. Logs can contain phone and vehicle details;
review them before sharing. More switches and a future test checklist are in [CAR-TEST-aa-dev.md](CAR-TEST-aa-dev.md).

## Reporting problems

Use **Save diagnostic logs** in the installer and attach the ZIP to a
[bug report](../../issues/new?template=bug_report.md). Please include car model, build year, NBT software level and
phone model.

## How it works (short)

```
Phone ──USB/AOA──▶ nbt_android_auto (Android Auto protocol, TLS, H.264)
                     │ annexb.fifo
                     ▼
                   fifo_http ──HTTP──▶ decode_runner ──▶ io-nme / Cinemo NmeDeviceScreen (stock hardware video)
                                                         ▲
nbt_hwvideo ── MOST/DSI (VideoApp D7/D8) ────────────────┘   switches the stock video layer to our stream
nbt_knob    ── watches iDrive controller events (MOST, CCS) ──▶ input.fifo ──▶ Android Auto keys
nbt_cluster ── navigation state from the phone ──▶ instrument cluster (HUDTerminal)
NbtAndroidAuto (HMI plugin) ── menu entry, status page, autostart via boot.sh
```

More: [docs/HW-VIDEO-PATH.md](docs/HW-VIDEO-PATH.md), [docs/CLUSTER-HUDTERMINAL.md](docs/CLUSTER-HUDTERMINAL.md),
[docs/ANDROID-AUTO.md](docs/ANDROID-AUTO.md), [docs/FORK-CHANGES.md](docs/FORK-CHANGES.md),
[docs/WORKFLOW.md](docs/WORKFLOW.md). Older experiments (PNG pipeline, WAL sprites, first hello-world plugin)
are kept in [docs/history/](docs/history/) for reference.

## Repository layout

| Path | Content |
|---|---|
| `src/android_auto/` | Android Auto side: USB/AOA, TLS, video capture, service, decoder runner, test programs |
| `src/android_auto/knob/` | iDrive controller (`nbt_knob`), hardware video switch (`nbt_hwvideo`), cluster guidance (`nbt_cluster`) |
| `src/idrive/` | HMI plugin (menu entry and Android Auto page) and its resources |
| `installer/` | Installer app (Python/Tk, Windows/macOS/Linux) and payload builder |
| `scripts/` | Build, test and diagnostic scripts (VM and car) |
| `tools/` | SSH session helper for development on the car |
| `patches/` | Small patches to the upstream NBT-AAD core |
| `toolchain/` | Experimental open QNX toolchain, headers and link-only stubs |
| `docs/` | Technical documentation; `docs/history/` = development log |
| `experiments/` | Past experiments, not part of the product (some crashed the head unit), kept for reference |

**Where to start reading the code:** [src/README.md](src/README.md) maps every program on the head unit to its source
files and shows how they work together.

## Contributing

Help is welcome, especially with testing on different phones, NBT software levels and cars. The QNX VM build uses
`scripts/build_all.py` and requires the inputs described in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
An [open toolchain](toolchain/README.md) is also available but is experimental and reuses an existing
`accessory_window` binary. `./flash-aa-open.sh` builds only by default; installation requires `--install` or
`--install-only`. Start with [CONTRIBUTING.md](CONTRIBUTING.md).

## Support and contact

Please use this fork's [issues](../../issues) for questions, suggestions and bug reports.

## Credits and license

Thank you to JayJay3071, Valinet and Adityadroid for their work on the original project and forks. Parts of the Android
Auto core and controller code come from **NBT-AAD** by nanaisu
([Sq00ky/Supra-iDrive6-Android-Auto](https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)).

This project is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE): free for private and other
noncommercial use, **no commercial use** (for example selling it pre-installed). Third-party notices are listed in
[NOTICE.md](NOTICE.md).

Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)

## Disclaimer

This is an unofficial hobby project. It is not affiliated with, endorsed by or connected to BMW AG, Google LLC or
Harman. BMW, iDrive and Android Auto are trademarks of their respective owners.

You modify your car's head unit **at your own risk**. There is no warranty of any kind; the authors are not liable for
damage to your car, head unit, phone or anything else, or for warranty claims you may lose. During development, wrong
experiments crashed and restarted the head unit; this integration has not yet been tested in a car.
Do not operate the phone or the system while driving in a way that distracts you; follow your local
traffic laws.
