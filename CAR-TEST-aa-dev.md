# Experimental at-car test checklist

The merged build passed a limited parked check after a cable swap. Full vehicle validation is still pending.
Further vehicle tests require explicit authorization and a recovery plan. The open toolchain is experimental.

Several fixes are ported from [valinet/nbt-android-auto](https://github.com/valinet/nbt-android-auto) (car-tested on an
F30 with the original NBT, mostly in split screen; we run full screen): steering roller, parking-assistant return,
focus-event pool, decoder audio fix, USB robustness, Bluetooth-audio fix, night mode.

## Initial parked test

The QNX SDP-built payload installed and verified on an original NBT; after one head-unit reset, the user reported
stock iDrive and radio were responsive. Android Auto briefly displayed its logo and then exited while the phone still
detected it. Captured logs show successful TLS and service discovery, incoming video frames, and software decoding
of approximately 60 frames in several sessions before USB input failed with `status=02000005`. QNX 6.5 headers
identify this as `USBD_STATUS_CMP_ERR | USBD_STATUS_DEV_NOANSWER`, not a timeout. The decoder logs show normal exits
for those sessions. The service reported `replug_required` after its automatic recovery attempts; one logged USB
reset returned error 19. Neither a phone disconnect nor a decoder crash is proven by these observations.

The service can automatically attempt up to three USB resets after failed sessions. Stop testing after an unexpected
failure, preserve diagnostic logs privately before restarting, and diagnose before choosing a separately authorized
retest or recovery action. Do not publish logs: they may contain vehicle and phone details.

## Parked retest

With a different USB cable and no redeploy, the user confirmed picture, iDrive knob input, and Android Auto audio
without Bluetooth. The service had also been restarted in split-screen mode since the failing capture, so the cable
swap was not an isolated comparison. Two read-only snapshots, 34 seconds apart, showed the same running session
with advancing video and stereo PCM, 20 forwarded input events with no drops reported by the knob bridge, and no
Android Auto bulk-transfer errors in the active capture log. Numbered logs from earlier sessions remained in the
same directory; compare modification times and current process state before attributing failures to the live session.

QNX still logged EHCI and `serusb` control-transfer errors, USB Ethernet (`ecm_vendor_command`) errors, and screen
display-handle warnings while projection ran. VideoApp also rejected the initial `StopVideo` cleanup request but
acknowledged the subsequent build and `StartVideo` on the first attempt. No interruption was observed in the live
session during the snapshots. Return to stock audio, camera behavior, and driving remain unverified.

The USB failure later recurred with the replacement cable. The decoder had run for about 5 minutes 15 seconds and
exited normally after the bulk input error. One reconnect failed before projection; the next resumed video and
stereo audio. The recovered session then failed after about 3 minutes 12 seconds, with bulk input and output both
reporting `status=02000005`. Both EHCI bulk failures were logged in the same second; the full-width mode change came
afterward. The cable swap did not eliminate the failures. Control-transfer warnings were also present during working
projection; do not treat them as evidence of a failed Android Auto bulk transfer.

On a later occurrence the user reported that the phone was disconnected and physically replugged the cable.
Record manual unplug/replug separately from automatic recovery when comparing logs.
In a subsequent read-only watch, a bulk read failed after receiving 4,608 bytes and the service started draining.
The accessory was still listed at the same USB address in cached samples about two seconds apart, including the
first sample after failure. This does not rule out a brief disconnect or a stale enumeration entry.
The [QNX USB API reference](https://www.qnx.com/developers/docs/6.5.0SP1.update/com.qnx.doc.ddk_en_usb/usbd_urb_status.html)
defines `DEV_NOANSWER` as no response to an IN token or no handshake for an OUT transfer, not proof of device
removal. Failure after partial transfer is not grounds to replay the transfer or assume a clean protocol boundary.

In one timestamped QNX snapshot, two EHCI bulk error bursts each went from retry count 1 to count 32
and a final error in 8 ms. A separate burst logged retries without a final error in that snapshot.
`nbt_pull_logs.py` uses `sloginfo -t` to preserve milliseconds. Compare time differences within the system
log before correlating it with phone events: QNX log timestamps can retain the boot-era date.
The EHCI debug words were not decoded; the retry timing does not identify a faulty component.

After another automatic recovery (about 20 seconds as reported by the user), the saved capture showed two distinct
endings: session 3's video FIFO writer got `EPIPE` (no reader), then the head unit requested and received a ByeBye.
The logs do not establish why the FIFO reader went away. Session 4 logged bulk IN `DEV_NOANSWER` after receiving
10,240 bytes; session 5 was streaming video and stereo audio at the time of the pull.

The next reported dropout recovered quickly. Session 5 failed with the same bulk IN status after 1,024 bytes in
the failed transfer and about 6 minutes 7 seconds of decoding. Session 6 failed during startup; session 7
resumed streaming. The HMI and Android Auto supervisor PIDs stayed the same, and the session 5 decoder exited
normally. The intervening phone USB reset log showed error 19, not a successful reset.

New builds log the capture's USB device address, generation and bulk endpoints. Failure and shutdown lines include
monotonic timestamps and whether QNX reported removal of that specific device instance. `removal_seen=0` means
the application had not observed a removal at that point, not that the connection was healthy. In a failure line,
`rc` covers setup, submission and waiting; `completion_rc` is the separate `usbd_urb_status()` result (`-1` if
no callback arrived). QNX can report device removal through this result even if the removal callback has not arrived.
Successful zero-byte bulk reads are ignored; failed transfers and short bulk writes still terminate the capture without
replay. `AA_BYEBYE requested` identifies the local trigger, and `AA_VIDEO_CAPTURE_STOP` records the pump's
stop reason before waiting for its threads. Only the protocol log confirms whether a ByeBye response arrived.
With `incident-logging`, the supervisor records the first child it observed exiting (not necessarily the root
cause), its PID and exit status or signal, and writes the session-end snapshot after capture teardown.
These changes have not been tested on the car and do not change the USB reset policy. For a parked comparison,
keep the replacement cable and car port fixed, try another known-good wired Android Auto phone, and save both
head-unit and phone logs if projection drops again. Run each comparison past the previous dropout and keep phone
and vehicle data private.

For a separate USB read-size A/B comparison with an authorized diagnostic build, leave `usb-rx-16k` absent for the
64 KiB baseline, then enable it and restart only the Android Auto service for the 16 KiB variant. Check
`read_size=` in each capture's `AA_USB_CAPTURE_TARGET` line. Run both variants on the same phone, cable, port and
settings, for at least 10 minutes per run or until a bulk error; repeat before drawing a conclusion. The smaller
size matches the reference transport's request length, not its queue depth. Observed failures also occurred below
16 KiB, so those logs do not establish a size limit. Remove the flag and restart the service to restore the
baseline. This changes neither bulk OUT nor the USB error or reset policy.

For phone-side evidence, use wireless ADB on a trusted local network so the Android Auto cable stays connected
to the head unit. Pairing alone is not a connection: use the **IP address & port** on Android's main Wireless
debugging screen with `adb connect`, not the separate pairing port, and verify `adb devices` reports `device`.
After a dropout, run `python3 scripts/nbt_pull_phone_logs.py <address:port>` from the same environment where ADB
is connected. It saves a bounded log snapshot, Android USB service history and a clock sample under
`.local/phone-logs/` without clearing the phone's log. On a busy phone the general log snapshot may cover only
seconds; the USB service history can still retain earlier gadget state transitions. Pull the head-unit logs at
the same time; compare clocks before ordering events. Keep both captures private and treat a change in accessory
PID or USB debugging state as a changed test condition.

For a parked comparison, start `python3 scripts/nbt_pull_phone_logs.py <address:port> --watch-seconds 660`
before the run. It takes the usual snapshot, then records selected USB and Android Auto tags in `usb-events.txt`
for up to 11 minutes or 10,000 entries. The extra minute leaves room to observe recovery after a 10-minute run.
This only reads logs; it does not change USB modes or clear logcat. An early ADB exit preserves partial events
and records a warning. A quiet watch or missing tag is not proof that the USB link stayed healthy.

In the first paired capture, session 13 logged `DEV_NOANSWER` after about 7 minutes 14 seconds of decoding. A
subsequent attempt had already failed before the phone logged accessory read `EIO` and `USB_STATE=DISCONNECTED`
several seconds later. The QNX system log also shows two final EHCI bulk errors, followed 9 seconds after the
second error by the stock media service processing the accessory removal (at one-second log precision). The
phone then switched back through normal USB mode before returning to accessory mode and streaming in session 15.
Both logs place the reported removal after the bulk failures; neither rules out an earlier brief bus or device
controller fault before a removal was reported.

A second synchronized watch observed the running session 16 fail with `DEV_NOANSWER` after a partial 5,632-byte
read. About 13 seconds after the NBT first showed it draining, Android logged accessory read `EIO` and
`USB_STATE=DISCONNECTED`. By then an automatic recovery reset attempt had started; its log later reported
error 19. Session 18 resumed projection after the phone re-entered accessory mode. This establishes that the
observed Android disconnect did not start the session 16 failure, not which USB endpoint or physical component
caused it. The diagnostic build was not installed during these observations.

The user also reported that a right tilt moves both Android Auto and the visible iDrive panel in split-screen mode.
The knob bridge receives copies of hardware events; its existing BACK workaround tries to restore iDrive focus
after release, not consume the original tilt. The overlay leaves the right side visible without resizing iDrive.
For a future parked test of exclusive input, a right tilt must move Android Auto without moving or opening the
iDrive side panel; after leaving Android Auto, the same tilt must work normally in iDrive.

## Before any car test

- If wired projection connects and immediately drops, try a short, known-good USB data cable directly connected to
  the head unit's USB port before changing software. Keep the phone, port, and settings fixed to isolate the cable.
- Park safely and use stable vehicle power. Confirm the head unit is the original NBT and check free space, existing
  installations and relevant process names read-only before installing.
- Verify the SSH host key and use the installer's `--known-hosts` option. Preserve any existing package and settings
  and have a recovery path ready. Uninstall is best-effort and requires working SSH; it is not guaranteed recovery.
- A partial installer failure is not automatically rolled back. Stop and inspect the actual state before any retry or
  reboot. Avoid stopping or restarting stock HMI or audio processes.
- Make initial checks while stationary. Stop on hangs, repeated restarts or abnormal audio, and keep diagnostic logs
  private; they may contain phone and vehicle details.

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
(sessions and end reasons, incidents by kind, crashes / freezes, and suspected reboots while Android Auto ran).
The summary separates the reported running session's capture, QNX system warnings, and other pulled logs, including
leftover session files. A system warning alone does not establish an Android Auto fault. Read-only on the car.

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
- `usb-rx-16k` — diagnostic-only 16 KiB bulk IN requests instead of 64 KiB; read on Android Auto service start.

Keep a known-working installer payload for rollback before any future vehicle test.
