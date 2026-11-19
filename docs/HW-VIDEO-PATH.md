# Hardware video path without a USB stick (status 7 November 2026)

## Why

The WAL sprite path (uploading the picture as a texture) is not viable for continuous operation. Evidence:
- Three runs on 7 November:
  - Run 1: about 60 s, then the session ended.
  - Run 2: 800x300 at ~9 fps, reboot after ~30 s.
  - Run 3: 640x240 at ≤6 fps, reboot after ~40 s.
- Runs 2 and 3 both ended with `NVRM: PUSHER RESERVED CMD ERROR / Fifo: Parse Error` in the NVIDIA driver, immediately
  after `create sprite 31281`. All GPU processes then received SIGSTOP, followed by the watchdog and reboot.
  Evidence: `build/aa-entry-20261106/dump-run2/`, `dump-run3/`.
- In run 3, the driver did not appear in the CPU measurements. Load is therefore not the cause: repeatedly replacing a
  large texture triggers the error sooner or later.
- Raw image pipes also use ~28% CPU (`pipe`), and the software decoder (io-nme) ~24%.

## How the HMI starts video (MOST capture, 7 November, phone MTP video)

Capture: `build/video-trace-20261107/trace.log`, recorded with `nbt_knob --trace` (listen only).
Times match the system log (`slog-video.txt`: nvideocapture start 00:13:01 = t≈781, stop 00:13:33 = t≈813).

Participants:

| Address | Role |
|---|---|
| 2021 | HMI |
| 2008 | `VideoApp` with FBlock **D7** (VideoConnectionMaster) |
| 2007 | `VideoApp` with FBlock **D8** (VideoSink CID) |
| 2006 | FBlock 07/11, notified by VideoApp |

Start (observed twice identically, t=738 and t=781):
```
2021->2008 D7/01 fkt=060 op=6 (StartResult)  03 aa 16 01     BuildVideoConnection(handle 0x03aa, source 0x16, sink 0x01)
2008->2006 07/11 fkt=D62 op=0  06 00 00 00 00 00 00 nn        (internal to VideoApp)
2008->all D7/01 fkt=062 op=C  00 00 16 00 00 00 00           Connection table: sink CID <- source 0x16
2008->2021 D7/01 fkt=060 op=D (ResultAck)  03 aa 16 01
2021->2007 D8/01 fkt=C05 op=0 (Set)  16 00                    CurrentVideoSource = 0x16
2021->2007 D8/01 fkt=C03 op=0 (Set)  00 00 00 00 07 80 04 38  VideoViewport (source area) 0,0,1920,1080 -> DisplayManager Src-size
2021->2007 D8/01 fkt=C01 op=6 (StartResult)  03 aa           StartVideo -> dev-nvideocapture starts (800x480, 60 Hz)
2007->2021 D8/01 fkt=301..304 op=C                            Brightness/contrast/color/tint
2007->2021 D8/01 fkt=C01 op=D  03 aa                          ResultAck
2021->2007 D8/01 fkt=C04 op=0 (Set)  01 0e 00 8c 01 40 00 b4  ScreenViewPort 270,140,320,180 (preview)
2021->2007 D8/01 fkt=C04 op=0 (Set)  00 d5 00 00 03 55 01 e0  ScreenViewPort 213,0,853,480 (full screen 16:9)
```
Stop (t=762 and t=813):
```
2021->2007 D8/01 fkt=C02 op=6  35 1d          StopVideo
2021->2008 D7/01 fkt=061 op=6  35 1d 16 01    RemoveVideoConnection(source 0x16, sink 0x01)
2021->2007 D8/01 fkt=C05 op=0  00 00          CurrentVideoSource = none
```
Source 0x16 is the multimedia player (Cinemo output 1, windows `CinemoVideo1_TaskA/B`). VideoApp then sets
Src-pos/Src-size/Visibility on DisplayManager, starts NVIDIA capture and configures the WAL video layer
(`CPVideoSinkWALController`, `WalHmiInterface::setViewport`).

## Plan for Android Auto

1. Keep the decoder on io-nme but use the stock output: `NmeCmdLine -vd NmeDeviceScreen -vi
   "?video-output-id=1&...&video-window-class=CinemoVideo1_TaskA,CinemoVideo1_TaskB"`. This was tested yesterday
   (21 fps decoded, window created). It eliminates the YUV pipe, RGBA conversion and sprite uploads.
2. A new `nbt_video_route` tool (DSI client like nbt_knob) sends the start sequence above. Source area 0,0,800,300 (our
   AA content), ScreenViewPort 0,0,1280,480 (full screen, factor 1.6 in both directions). It sends the stop sequence
   when finished.
3. If the HMI layer is above the video layer, reverse their order with `WalHmiInterface::setZOrder` or give the AA page
   (holding page 13) transparent content.

## Open questions before the first transmission test

- DSI request 0x2 (`processMessage`) format: `u32 clientId` + MessageType. Transmission type and variant
  (received: tx=0, var=2/0), and sender address (own deviceId 0x206x or the HMI's), are unclear. Derive these from
  HmiMain3D or libimp_sys_most_base_nbt.so.11.
- HMI response to another connection using source 0x16: the multimedia player may treat the source as its own.
  Another unused source ID might be preferable.
- Risk: incorrect messages to VideoApp can disrupt the video chain. A reboot is possible, but no data loss. Obtain
  approval before each transmission test.
