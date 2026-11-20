# Changelog

Versions are named after the build date of the installer payload.

## Unreleased

The pre-merge browser audio route and A2I output were tested on a BMW F07 (2014, original HU_NBT). The integration
with Valinet's fork passed a limited parked check after a USB cable swap. It is not fully vehicle-validated or part of
a released installer payload.

- **Android Auto sound over USB, no Bluetooth needed** for music, navigation voice and system sounds. The original
  NBT has no `libasound`, so the NBT-AAD browser-audio mixer now writes to the A2I endpoint `input_stereo_3`
  (`src/android_auto/audio_a2i`). The new `audioconn` (`nbt_audioconn`) asks the audio ConnectionMaster
  (FBlock 0x03) for connection 0xb8, the request the iDrive HMI itself sends (decoded from `HmiMain3D`), and removes
  it again after silence and after every session. This route can be selected with `aa-audio`; the default is now
  Valinet's `f6` main source. `touch .../NbtAndroidAutoRT/bt-audio` forces Bluetooth.
  Details, findings and rollback: `docs/AA-AUDIO-PLAN.md`.

## 2026.11.17 (fork valinet/nbt-android-auto)

Tested on a BMW F30 with NBT. Technical details: [docs/FORK-CHANGES.md](docs/FORK-CHANGES.md).

- **Bluetooth / AUX audio fixed:** after Android Auto had run, other sources no longer stay silent until an HMI reboot
  (the decoder switched off io-nme's audio for everyone).
- **Audio source:** the phone starting playback switches the car to Android Auto's source; leaving it pauses the
  phone, returning resumes it. (With the cable setup the source is AUX, and choosing AUX brings Android Auto to the
  front.)
- **Steering wheel:** roller = next / previous, push = play / pause, only on Android Auto's source; no song skip at boot; the roller stays
  with iDrive's contact list for 4 s after the phone button.
- **Controller:** BACK and tilt left / right stay in Android Auto, also with split screen on; NAV brings Android Auto to
  the front; NAV pressed again in Android Auto switches to the phone's audio (AUX) or play / pauses it. These act only
  while a session runs.
- **Start at boot** button (on / off); the service starts right away instead of after 15 s.
- **Parking assistant:** Android Auto comes back when it closes.
- **Night mode:** Android Auto (Google Maps, Waze) goes dark when the headlights are on.
- **Android Auto sound over USB:** music, prompts and system sounds play through the car as their own source (no
  cable); NAV in Android Auto switches to it. The cable setup stays available (`media-jack`).
- **Restart system** button and `scripts/nbt_reset.py` (reboot from a laptop).
- One hold screen instead of 8; USB reads wait 8 s instead of 2 s; Bluetooth audio keeps working while Android Auto
  runs.
- Docs: [docs/WORKFLOW.md](docs/WORKFLOW.md) (commands), [docs/SSH-ACCESS-SERIAL.md](docs/SSH-ACCESS-SERIAL.md) (free
  SSH unlock over the debug serial port).

## 2026.11.10.2

Tested on the car (BMW F07 2014, OnePlus 7 Pro), including install with the installer.

- **Fewer drop-outs:** picking the "exit" icon in the Android Auto rail no longer ends the session ("connected but
  nothing works"); the projection is handed back to the phone after half a second.
- **Clean session end:** before stopping, the service now says goodbye to the phone (ByeBye). After a service restart
  the phone no longer stays stuck until Android Auto is force-stopped.
- **No false "replug the phone":** a session that ran for at least a minute no longer counts as a failed attempt.
- **More reliable menu entry:** the plugin sends at most one hold command per second (two commands 50 ms apart in the
  middle of an iDrive page animation were ignored).
- **Cluster:** smoother road shields and digits (anti-aliased), distances like "5.4" show correctly (the cluster font
  has no comma), the distance bar appears only below 1 km.
- **Diagnostics:** the restart snapshot now includes the session, decoder and video logs of the current boot.
- Fix: updating `nbt_cluster` while it was running failed ("Resource busy").
- Investigated and documented, not shipped: hardware decoding (`-wd 1`, more delay) and a map picture in the cluster
  (the 6WB cluster shows bitmaps only in the top line), see `docs/CLUSTER-HUDTERMINAL.md`.

## 2026.11.09

First release candidate: wired Android Auto with hardware video, iDrive controller, menu entry with autostart, cluster
turn-by-turn guidance, installer for Windows and Python.
