# Everyday workflow (Linux and WSL)

These commands originate from Valinet's fork. See [the test checklist](../CAR-TEST-aa-dev.md) for the limited
parked results and remaining validation.

The commands used to build, install and debug this fork from a Linux or WSL laptop on the ENET cable. Run them in your
Linux/WSL shell in the repository folder.

## SSH shortcut

Verify the head unit's SSH host key through a trusted independent channel before connecting; keep host-key checking
enabled. Define this helper function once per shell session; every car command below starts with `car`:

```sh
car() {
  ssh -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedAlgorithms=+ssh-rsa -oMACs=+hmac-sha1 \
      -oPreferredAuthentications=password,keyboard-interactive -oPubkeyAuthentication=no \
      -oIdentitiesOnly=yes -oStrictHostKeyChecking=yes root@169.254.199.99 "$@"
}
```

On distros whose OpenSSL blocks SHA-1 signatures (such as Fedora WSL), prefix `OPENSSL_ENABLE_SHA1_SIGNATURES=1` to
the `ssh` command. On WSL, ensure the ENET link-local network (`169.254.199.99`) is reachable from Linux (for example
via WSL mirrored networking).

The [serial SSH guide](SSH-ACCESS-SERIAL.md#enabling-ssh) documents the default root password. To enable SSH, see
[README.md](../README.md#enable-ssh) or the serial guide.

The password may have been changed. Treat it as sensitive: do not put it in commands, process arguments, logs, new files
or responses. For interactive access, use the SSH prompt. For noninteractive access, use a protected pipe or file
descriptor (for example, `sshpass -d`) to supply the correct password without printing it. Do not repeatedly retry failed
authentication.

The head unit's shell has no `head`: use `sed 3q` instead (`tail` exists). Its `grep` has no `-A` / `-B`.

## Apply a patch, build, install

1. Apply a patch (made with `git format-patch`):
```sh
git am ~/Downloads/<patch-file>.patch
```

2. Build in the QNX 6.5.0 VM (required for supported vehicle binaries; see [QNX VM](DEVELOPMENT.md#qnx-vm)):
```sh
python3 scripts/build_all.py --host qnx-build
```
On distros that require the process-scoped SHA-1 override:
```sh
OPENSSL_ENABLE_SHA1_SIGNATURES=1 python3 scripts/build_all.py --host qnx-build
```

Developers without the BMW libraries can pass `--hmi-stubs` to build the plugin in the same VM. This mode is not
yet qualified for vehicle deployment; it still needs the real BMW libraries on the head unit at runtime.

3. Package from the exact QNX VM build directory printed above (preserves PEMs already in `installer/payload/rt`):
```sh
python3 installer/make_payload.py --built build/qnx-build-<time>-<id>
```
Pass `--pem-dir .local/pem` on the first run before `installer/payload/rt` has the PEM files.

4. Install:
```sh
python3 installer/nbt_aa_installer.py --cli install --known-hosts <verified-known-hosts> --restart
```
For a separately authorized test of an SDP stub-linked payload, the CLI additionally requires
`--allow-qnx-hmi-stubs` (the GUI asks for confirmation).
Files already on the head unit with the same content are not written again (`same` in the output). To write
everything anyway, add `--force`. `--restart` runs `restart.sh` after verification so runtime-only updates
(`native_service`, `nbt_android_auto`, etc.) take effect immediately without rebooting iDrive; if
`libNbtAndroidAuto.so` changed, still reboot iDrive. Verify the head unit's host key independently before adding it
to the known-hosts file. Without `--known-hosts`, the installer accepts an unknown key. See the
[installer safety notes](../installer/README.md#safer-car-testing) before deploying; preflight writes test files and
a failed installation may be partial.

5. Reboot the head unit: Multimedia → Android Auto → **Restart system**, or from the laptop:
```sh
python3 scripts/nbt_reset.py
```

Undo the last patch before it is pushed: `git reset --hard HEAD~1`. Undo a pushed commit: `git revert <commit>`.

## Logs on the car

The logs live in RAM (`/fs/ram3`): a reboot clears them, so read them before rebooting.

| What | Command |
| --- | --- |
| Service state and session number | `car "cat /fs/ram3/nbt-aa-state"` |
| HMI plugin (page focus, show requests, parking assistant) | `car "tail -30 /fs/ram3/nbt-aa-plugin.log"` |
| Controller, steering wheel, audio source | `car "tail -30 /fs/ram3/nbt-aa-native/knob.log"` |
| Session logs, newest first | `car "ls -lt /fs/ram3/nbt-aa-native/capture-*.log"` |
| How sessions ended | `car "grep -h -e AA_USB -e 'No data' -e 'closed socket' /fs/ram3/nbt-aa-native/capture-*.log"` |
| System log | `car "sloginfo \| tail -40"` |

## CPU load

CPU use per adaptive partition (QNX APS) over the last 0.1 s, 1 s and 10 s:

```sh
car "aps show -v"
```

It groups processes, it does not name them: the HMI and everything this project starts (spawned from the HMI plugin) run
in `interaction`, system services such as `io-nme` (the decoder) most likely in `System`. Compare the **Total** of the
10 s column, taken in the same situation (same screen, video running, phone connected for a minute), for example with the
hardware decoder on and off.

## MOST trace

60 s of all MOST messages the head unit receives, then copied to the laptop:

```sh
car "cd /fs/ram3/nbt-aa-native && LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_knob --trace --seconds 60 --fifo /fs/ram3/nbt-trace-dummy.fifo > /fs/ram3/trace.txt 2>&1"
car "cat /fs/ram3/trace.txt" > trace.txt
```

## Hardware decoder

Off by default (software decoding). Applies from the next session: replug the phone or use **Restart Android Auto**.

| What | Command |
| --- | --- |
| Hardware decoder on | `car "touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/hwdecode"` |
| Hardware decoder off | `car "rm -f /var/opt/hmi/data/rw/NbtAndroidAutoRT/hwdecode"` |
| Which one runs | `car "grep -h 'decoder:' /fs/ram3/nbt-aa-native/decoder-*.log"` |

## Android Auto sound

By default the phone's sound comes over USB into the car (source `f6`). The former `media-jack` mode is no longer
supported; factory AUX is unchanged. Create `aa-audio` in the package directory to select the browser route instead.

| What | Command |
| --- | --- |
| Browser route | `car "touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/aa-audio"` |
| f6 (default) | `car "rm -f /var/opt/hmi/data/rw/NbtAndroidAutoRT/aa-audio"` |
| What the session does with the sound | `car "grep -h AA_MEDIA_OUT /fs/ram3/nbt-aa-native/capture-*.log"` |

Applies after **Restart Android Auto** (the service reads the file when it starts).

Software volume is applied to media on both routes. `f6` may also be attenuated by the car, producing double
attenuation; create `aa-volume-off` in the package directory to disable it. This integration is not car-tested.

Navigation prompts while another source plays (radio, ...) cut in over connection `b8`. To turn that off (prompts only
on Android Auto's own source): `car "touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/prompts-off"`. What happened:
`car "grep -e Prompt -e 'build handle' -e 'remove handle' /fs/ram3/nbt-aa-native/knob.log"` and
`car "grep -h -e prompt -e b8 /fs/ram3/nbt-aa-native/capture-*.log"`.

## Test tool nbt_key

Copy it to the car after a build (it is not part of the installer payload):

```sh
car "cat > /fs/ram3/nbt_key && chmod 755 /fs/ram3/nbt_key" < build/qnx-build-<time>-<id>/programs/nbt_key
```

Read the current audio source:

```sh
car "LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib /fs/ram3/nbt_key source 5"
```

Beep test (`nbt_a2i_beep`, also only in the build folder): K short beeps into one io-amp input, audible only while
the amplifier has a connection to that input:

```sh
car "cat > /fs/ram3/nbt_a2i_beep && chmod 755 /fs/ram3/nbt_a2i_beep" < build/qnx-build-<time>-<id>/programs/nbt_a2i_beep
car "LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib /fs/ram3/nbt_a2i_beep input_stereo_3 2"
```

Other modes: `--watch [s]` (print controller messages), `back [s]` (BACK after a tilt right),
`cm <fkt> <op> <bytes>` (send one ConnectionMaster message to 100E): `cm 053 6 00 10 00 bb` switches to AUX,
`cm 053 6 00 11 00 b8` builds the browser connection (main source muted, `input_stereo_3` audible), `cm 054 6 00 12 00 b8`
removes it again. The second pair of bytes is a request number of your choice; change it each time.
