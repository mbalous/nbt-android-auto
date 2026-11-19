# Android Auto audio without Bluetooth – status 7 November 2026

## Findings

- **A2I beep test** (`nbt_a2i_beep`, sources and scripts in `experiments/audio/`):
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
