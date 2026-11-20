# Android Auto audio without Bluetooth – status 7 November 2026

The session executable name in this historical report is normalized to its current name.
The 15 November implementation below describes the pre-merge browser audio mode, now selected with `aa-audio`.
The default is Valinet's `f6` main source. Wired stereo worked in a limited parked retest of the combined
implementation; switching sources and the optional browser audio route were not validated.

## Findings

- **A2I beep test** (`nbt_a2i_beep`, source in `src/android_auto/audio/`, scripts in `experiments/audio/`):
  - Data was written to `input_stereo_1` through `_5`, but nothing was audible.
  - Opening `input_stereo_0` hung because it was in use (BT/media).
  - An input is audible only when its associated source is active in audio management.
- **Cinemo `NmeCmdLine`** does not play WAV files (0 samples, including with `-n`).
- **NBT audio architecture:**
  - The amplifier / ConnectionMaster is external: FBlock **03** at address **0x100E**.
  - The HMI builds connections there (`CPxyConnectionMaster::BuildHighSyncConnection`, `RemoveHighSyncConnection`).
  - Replies to the HMI use fkt **053** op=D (ResultAck `00 02`, `00 03`, …) and status fkt **C03** (connection table,
    e.g. `00 00 00 01 00`, `00 00 00 be 00`).
  - The request itself is not visible in the capture because it goes to an external device.
  - Evidence: `build/audio-trace-20261107/atrace.log` (source changes using MODE: FBlock 15/02 fkt 513) and
    `build/video-trace-20261107/trace.log`.
- **Browser audio as in the reference (NBT Evo):**
  - `/opt/sys/bin/dev-libasound -x input_stereo_3` (Connectivity.cfg), the driver `/opt/mm/lib/libasound/libasound.so`
    and the `CONN_BROWSER_AUDIO` package are present.
  - `audioconn` is missing on this NBT.
  - HMI strings: `CPxyBrowser::CSourceActivity`, `EBrowserAudioSourceType`, `getConnIDForAudioSource`.
- **Telephony** uses the TCB (`devu-cdc_tcm-io_amp`, `play_dev=/input_mono_3`). **Bluetooth A2DP** (Media → Bluetooth)
  works in principle, but unreliably with the OnePlus.

## Plan

1. **Offline:**
   - Inspect the `CPxyConnectionMaster::CBuildHighSyncConnection` serializer in HmiMain3D: function ID (probably 053)
     and payload (handle, connection ID, sink).
   - Identify the browser source's connection ID (reference: 0xb8; the `EAudioConnectionIDs` table in HmiMain3D).
2. **Android Auto:**
   - Remove channel 9 (media sink) from `NBT_SDR_DROP`; retain channel 8 (guidance).
   - Put PCM from `setAudioFrameCallback` into a ring buffer.
3. **Output:**
   - Send PCM through `libasound.so` (`snd_pcm_open_preferred`, 16-bit stereo, 44.1 kHz according to the reference;
     reference resampler 48 → 44.1 kHz) to `input_stereo_3`.
4. **Route:**
   - On the first sound, send `BuildHighSyncConnection(browser ID)` to 0x100E; send `Remove` after silence. Use the
     nbt_knob/nbt_hwvideo send function (`Client::send`).
5. **Test in the car** with the libasound beep test and a built connection, then with Android Auto music. Obtain approval
   before each transmission test.

## Offline HmiMain3D analysis (15 November 2026, nothing transmitted)

Source: `HmiMain3D` from the developer's NBT firmware (not in the repository). Addresses below are VAs in that file.

- **ConnectionMaster proxy (FBlock 0x03)**, constructor `0x8b838fc`, function registration `0x8b8361c`:
  `0x000, 0x001, 0x002, 0x053 (+0x6cc, CBuildHighSyncConnection), 0x054 (+0x764, Remove), 0x402, 0xC03 (table),
  0xC05, 0xC20`.
- **BuildHighSyncConnection request** (`0x8b832e6`): FBlock 0x03, fkt **0x053**, OpType **0x06 StartResultAck**,
  **big-endian** payload `u16 SenderHandle, u16 ConnectionID` → 4 bytes, e.g. `00 01 00 B8`.
  **RemoveHighSyncConnection** (`0x8b83258`): fkt **0x054**, same format. Reply: fkt 0x053/0x054 OpType 0x0D
  ResultAck `u16 SenderHandle` (matching `00 02`, `00 03` in the 7 November capture); errors use OpType 0x0F/0x09.
  The stream writer `0x89b043e` confirms big-endian order (high byte first).
- The HMI calls Build with fixed IDs (`0x36`, `0x1A0`, `0x1AC`, `0x08/0x09/0x0A` per sink), and through
  `CHmiAudioSink` with an ID from the table; SenderHandle is sometimes `0xB1xx`, a counter, or `0xFFFF`.
- **EAudioSource → ConnectionID table** at VA `0x9879224`, 0x88 rows of `{src, conn_zone0, conn_zone1, conn_zone2,
  category}`. `CHmiAudioSink::getConnIDForAudioSource` (`0x899c068`) returns `conn[1 + sinkIdx]`.
  Row `src 0x53 → 0xB8 / 0x149 / 0x14A`, `0x54 → 0xB9`, `0x55 → 0xBA` (category 0xB). The same ID 0xB8 is
  `eSRC_BROWSER → BrowserFront` on NBT Evo (NBT-AAD `audioconn -b 0xb8`). **0xB8 is therefore very likely the browser
  front connection on the original HU_NBT as well** (confirmed in the car, below).
- Online Entertainment Download (`oeapp`) and Streaming (`wrapp`) use `0x50/0x51/0x52` per zone (connections `0x182`,
  `0xC5`, `0xC6`), not 0x53.
- **Browser proxy** `CPxyBrowser`: FBlock **0xBE**; functions `CSourceActivity 0x103`, `CBrowserAudioRequired 0x20E`,
  `CBrowserAudioRequired2 0xD76`. A `be` entry appeared in the C03 status on 7 November. Offline analysis cannot show
  which process serves FBlock 0xBE (the extract contains only `boot/bin`).
- Read the FBlock 03 instance and MOST address at runtime from `kAttrRegistry`; do not hard-code them.

Connection `0xB8` was confirmed in the car (below); whether the HMI shows a source change or only the amplifier switches
was not investigated.

## Implementation and results (2026-11-15)

Android Auto media, navigation voice and system sound play over the USB connection on the original HU_NBT, without Bluetooth.

How it works
- **Output device.** The original NBT has no `libasound` / `dev-libasound` (it runs `dev-ioamp-router`, whose clients use the
  A2I library). `src/android_auto/audio_a2i/asound_device.*` replaces NBT-AAD's `AsoundDevice` (copied over it by
  `scripts/build_all.py`): the browser-audio mixer writes through `/opt/sys/lib/liba2i_client.so` into the A2I endpoint
  `input_stereo_3` (48 kHz stereo, 4096-byte packages x2, fallback 1024 x2, legacy cookie 0x05555555).
- **Route.** `audioconn` (`src/android_auto/knob/nbt_audioconn.cpp`, installed as `audioconn`) replaces NBT Evo's stock
  `audioconn`: `-b 0xb8` / `-r 0xb8` send Build / Remove to the ConnectionMaster (FBlock 0x03, address and instance from
  the MOST registry; Fkt 0x053 / 0x054, StartResultAck, big-endian `{u16 handle, u16 connId}`), `-l` reads the connection
  table. Only 0xb8..0xba are accepted. Frame builder: `ref/most/connmaster.hpp`, test `knob/tests/test_connmaster.cpp`.
- **Policy** (`native_service`, evaluated before every session): Android Auto sound with connection 0xb8, except
  `bt-audio`, `audio-route.failed`, `audio-output.failed`, a pending route cleanup, or a missing `audioconn` /
  `liba2i_client.so`, which means Bluetooth as before. Failures are kept in RAM until reboot. Before sending a build,
  `audioconn` records its connection in `audio-route.built`; `native_service` retries removal after a session and while
  idle until removal is confirmed, because an unanswered build may still interrupt the radio.
  `aa-audio` (optional) holds another connection id; `0` plays unrouted.

Verified on a car (BMW F07 2014, original HU_NBT)
- `audioconn -b 0xb8` -> ConnectionMaster 03/00 (0x100E) **granted**; `-r 0xb8` **granted**.
- `experiments/audio/car-route-beep-test.sh`: with 0xb8 built, 3 beeps were heard on `input_stereo_3`; inputs 1, 2, 4 and 5
  were silent.
- Android Auto music through the A2I output plays (reported by the car owner as clean).
- CPU (`hogs`, Android Auto + music + scrolling, two cores): `nbt_android_auto` incl. mixer about 7 %, `dev-ioamp-router`
  about 5 %; the video decoder (`io-nme`) about 34 %.

Not verified / open
- Other cars, NBT software levels and phones; long drives.
- Navigation prompts ducking the music, and how fast the radio comes back after the route is removed.
- Calls and the microphone still use Bluetooth (channels 2 and 4 stay dropped); no microphone uplink yet.
- `knob/tests/test_connmaster.cpp` was compile-checked with the QNX GCC only, not executed on a host.

Rollback: `touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/bt-audio`, then restart the Android Auto service. If the radio stays
muted: `/var/opt/hmi/data/rw/NbtAndroidAutoRT/audioconn -r 0xb8`. Logs: `capture-*.log` (`[AudioRoute]`, `[Browser]`),
`audioconn-cleanup.log`, `audio-route.*` and `audio-output.failed` in `/fs/ram3/nbt-aa-native`.
## Update 2026-11-18 (fork, car-tested, English)

The audio path works on the original NBT (F30), tested with `nbt_key` and `nbt_a2i_beep`:

1. `nbt_key cm 053 6 00 18 00 b8`: ConnectionMaster builds connection `0xb8`.
2. `nbt_a2i_beep input_stereo_3 2`: the beeps are audible (A2I client, 48 kHz 16 bit stereo).
3. `nbt_key cm 054 6 00 19 00 b8`: the connection is removed, the main source plays again.

**ConnectionMaster methods** (FBlock 03 at 100E; HmiMain3D `CPxyConnectionMaster` method table, `0x8b818f1`):

| fkt | Method | Data |
| --- | --- | --- |
| `053` | BuildHighSyncConnection | handle (2 bytes), connection (2 bytes); 5 bytes = error 05 (length) |
| `054` | RemoveHighSyncConnection | handle (2 bytes), connection (2 bytes) |
| `C03` | HighSyncConnectionTable (status) | `00 00`, then per connection: id (2 bytes), state (1 byte) |
| `C05` | HighSyncConnectionControl | not used |

State in `C03`: `00` = playing, `04` = muted by an interrupting connection. Example while `b8` is built over AUX:
`00 00 00 bb 04 00 b8 00`.

**Connection IDs**: HmiMain3D has a table of 136 rows {EAudioSource, three connection IDs, category} at `0x9877344`
(read by `CHmiAudioSink::getConnIDForAudioSource`). Category `01` holds the main sources (`01` radio, `bb` AUX, `c7`
Bluetooth audio, ...), `b8` is category `0b`.

**`b8` (browser, NBT-AAD: `eSRC_BROWSER`, "interrupting entertainment")** is not a main source: while it is built the
amplifier mutes the main source, but iDrive and the source status still report the main source, so the steering wheel
keys still act on it (car 2026-11-18: rocker changed the selected source's tracks / stations). Good for navigation
prompts; for music a real main source (category `01`) with a writable input is needed.

NBT-AAD's browser audio mode (`src/audio/browser_audio.*`, mixer, resampler, libasound shim) is built for exactly this
connection; it builds / removes it through BMW's `audioconn` tool, which this NBT does not have. Here `053` / `054` over
MOST take its place.
