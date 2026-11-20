# Experimental at-car test checklist

This merged build has not been tested on a vehicle. This checklist is for a future, explicitly authorized test;
do not install or operate the vehicle as part of the merge. The open toolchain is experimental.

Several fixes are ported from [valinet/nbt-android-auto](https://github.com/valinet/nbt-android-auto) (car-tested on an
F30 with the original NBT, mostly in split screen; we run full screen): steering roller, parking-assistant return,
focus-event pool, decoder audio fix, USB robustness, Bluetooth-audio fix, night mode.

## 2. Tests (one per reported bug)

| # | Bug | Do this | Pass |
|---|-----|---------|------|
| 1 | Reverse camera doesn't return | AA in front, playing. Shift R, wait 5 s, shift D. | AA comes back by itself within ~2 s |
| 1b | …but not when you left AA | Press MEDIA (leave AA), shift R, then D. | AA does **not** pop back |
| 2a | Steering volume | AA music playing. Steering-wheel volume up/down, also iDrive knob volume. | AA music gets louder/quieter |
| 2b | Steering roller (right side) | AA music playing. Roll up / down one step, push it. | Next / previous track, play/pause |
| 2c | Roller after phone button | Press the phone button, scroll the contact list. | List scrolls, songs do **not** skip |
| 2d | Skip buttons (if fitted) | Press next / prev. | May not work yet: see §3 |
| 3 | Track in HUD / cluster | AA music, no navigation. Skip to a new song. | Song title shows ~8 s in the cluster nav area (and HUD if it mirrors it) |
| 4 | Freeze | Drive normally. | Note time if it freezes |
| 5 | Source compatibility | Use volume / sources as usual. | No unexpected rear-audio warnings or radio muting |
| 6 | AA exits, audio keeps playing | Drive normally, popups/PDC etc. | Note time if it happens |
| 7 | "Connected" but won't open | Use AA a long time, many popups; then Multimedia → Android Auto. | Opens (focus-event pool: iDrive ignored each focus event after 11 uses) |
| 7a | BMW icon does not return to iDrive | While parked, select the BMW icon, use iDrive, then reopen AA via Multimedia; repeat. | iDrive remains visible; AA stays connected and reopens with video and working controls. |
| 7b | BMW icon and parking assistant | Leave AA with the BMW icon, then test the parking assistant. | iDrive stays in front after the parking view closes. |
| 8 | iDrive reboot | — | If it happens, logs now survive the reboot |
| 10 | Bluetooth / radio sound after AA | Use AA, then switch to Bluetooth audio or radio. | Sound plays (decoder no longer mutes io-nme) |
| 11 | Night mode | Headlights on. | Android Auto / Maps go dark |

## 3. Steering-wheel skip buttons — one-time mapping (only if 2d failed)
nbt_knob logs steering-wheel messages. Press next / prev a few times, then inspect the log and
writes `mfl.map` — no rebuild needed:
```
/fs/ram3/nbt-aa-native/knob.log                       ([MFL] lines)
/var/opt/hmi/data/rw/NbtAndroidAutoRT/mfl.map         e.g.  0402 01 87   (87 next, 88 prev, 85 play/pause)
```
nbt_knob reads `mfl.map`, `wheel-off` and `ptt-assistant` only when it starts: after changing them, restart the Android
Auto service (Android Auto page -> restart) or the head unit.

## 4. Logs: pull after driving
With the explicit `incident-logging` flag, the car keeps `events.log` and `incidents/` (a snapshot with
the QNX system log each time something goes wrong: program crash, session end, no picture, freeze, replug, iDrive
restart, service recovered, user restart; newest 40). The separate `extended-log-history` flag keeps up to 15 boot
histories and approximately 20 MB. Baseline `logs/`, `logs.prev/` and restart snapshots remain enabled as before.
Logs may contain phone and vehicle details.
After a drive, ENET cable in, ignition on:
```bash
python3 scripts/nbt_pull_logs.py
```
It asks for the NBT root password once, saves everything to `.local/car-logs/<time>/` and prints `summary.md`
(sessions and end reasons, incidents by kind, crashes / freezes, suspected reboots while Android Auto ran, error
patterns). Read-only on the car.

## 5. Microphone, calls and voice

Experimental wired car-microphone integration is deferred. Keep the phone paired for existing Bluetooth calling and
microphone behavior.

## Switches (files in `/var/opt/hmi/data/rw/NbtAndroidAutoRT/`)
- `aa-volume-off` — keep AA gain fixed (if volume behaves oddly / double attenuation).
- `no-hud-track` — no track line in the cluster.
- `wheel-off` — steering roller stays with iDrive only.
- `ptt-assistant` — long press of the steering-wheel voice button also sends the Assistant key over the cable.
- `tls-allow-date-errors` — opt in to accepting expired or not-yet-valid certificates; all other TLS checks remain.
- `incident-logging` — opt in to bounded incident snapshots and event logs.
- `extended-log-history` — opt in to bounded boot history.

Keep a known-working installer payload for rollback before any future vehicle test.
