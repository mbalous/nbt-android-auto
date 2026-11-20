# Open toolchain vs QNX 6.5 SDP VM

## Result

The open toolchain is useful for CI and development without the proprietary SDK, but it is **not yet a validated
replacement for the VM build or a car-ready release toolchain**. Both builds compiled the shared application sources
in the comparison below. That establishes build coverage, not equivalent ABI, runtime behavior or vehicle safety.
Keep the SDP VM build as the reference for releases while the open build's gaps are investigated.

## Evidence from 2026-11-20

- The SDP build used `scripts/build_all.py --windows-openssh` against the non-root QNX 6.5 VM, with the two real HMI
  libraries present. It built and fetched all 13 programs in `build_all.py`'s `PROGRAMS`, including the plugin.
- The [open CI build](https://github.com/mbalous/nbt-android-auto/actions/runs/44807802193/job/136536261717) ran
  `python3 scripts/build_open.py` without `--reuse`; it reported `BUILD_OPEN_DONE` and built 10 programs.
  Its cached toolchain was reused; this does not test a fresh toolchain setup.
- The VM build's source was the merged worktree subsequently committed as
  `2bae38cec590036dc7a4cafd19524c20ed71b62c`. CI checked out the PR merge commit
  `a2781d4a39789bbb8451b54adac7f395e685f076`; both commits have tree
  `8337f0054ab6934d8938f0c71129eb56270f9a09`. Both use NBT-AAD
  `41975f269b065dbdbc77af9f568387cd22b9ed27` and the checked Mbed TLS 3.6.7 inputs.
- The open job did not retain its binaries as artifacts. The sizes below come from its build log; the SDP sizes and
  ELF details were inspected from the locally fetched VM binaries. No open binary ELF, symbol, byte-for-byte, VM
  execution or car comparison was performed. Local Docker had no reachable daemon, so no additional open build was
  run locally. Neither build was deployed to a car for this comparison.

### Output coverage and sizes

File sizes are unstripped bytes from these particular builds, not performance or compatibility measurements.

| Output | Open | SDP VM |
|---|---:|---:|
| `nbt_android_auto` | 1,230,231 | 1,089,854 |
| `native_service` | 36,550 | 37,924 |
| `decode_runner` | 145,992 | 12,888 |
| `fifo_http` | 143,476 | 16,749 |
| `nbt_knob` | 243,825 | 84,399 |
| `nbt_hwvideo` | 219,381 | 54,794 |
| `nbt_cluster` | 261,251 | 107,094 |
| `audioconn` | 215,823 | 50,751 |
| `nbt_reset` | 9,215 | 9,697 |
| `libNbtAndroidAuto.so` | 29,862 | 34,176 |
| `accessory_window` | Not built | 13,133 |
| `nbt_key` (test tool) | Not built | 49,333 |
| `nbt_a2i_beep` (test tool) | Not built | 10,168 |

`accessory_window` is required in the installer payload. It uses QNX resource-manager APIs to keep the stock USB stack
from claiming the phone; the open sysroot lacks its `sys/dispatch.h` and `sys/iofunc.h` headers. This is a sysroot
gap, not an inherent GCC limitation: adding compatible declarations and link symbols would also require checking
structure layouts and behavior against the SDK. `build_open.py --reuse <payload>` can copy an existing binary, but a
reused binary is not an open build and must match the intended release. The other two omitted programs are test tools,
not payload entries. The open build generates plugin resources; packaging also requires the PEM inputs described in
[DEVELOPMENT.md](../docs/DEVELOPMENT.md).

## Important differences

| Area | Open cross-build | QNX SDP VM |
|---|---|---|
| Compiler | GNU GCC/binutils targeting `i486-pc-nto-qnx6.5.0`; GCC 4.4.2 with a project patch | QNX `qcc` using the SDP 6.5 GCC 4.4.2 x86 variants |
| System interfaces | Project-written partial headers, startup code and `popen`/`pclose`; generated link-only library stubs | SDP headers, startup objects and libraries |
| C++ executables | GNU libstdc++ linked statically; larger outputs for many C++ programs | QNX Dinkum C++ runtime dynamically linked |
| HMI plugin | Links against project-generated symbol stubs, with `-z defs` | Links against the actual AppFramework and CoreFramework libraries, with `-z defs` |
| Source paths | `NBT_OPEN_TOOLCHAIN` changes the decoder's `devctlv` lookup and the plugin's `spawn` declarations | Uses SDP `devctl`/`neutrino` and `spawn` headers |

The stubs are link inputs, not replacements for the real libraries at runtime. Successful linking against them proves
only that the stub contains the requested names, not that those names, signatures, layouts, startup behavior, symbol
versions or C++ interactions match the target. For reference, the SDP `nbt_android_auto` inspected here is ELF32
little-endian i386 with interpreter `/usr/lib/ldqnx.so.2` and needs `libsocket.so.3`, `libm.so.2`, `libusbdi.so.2`,
`libcpp.so.4` and `libc.so.3`. Its plugin needs `libAppFramework.A.so`, `libCoreFramework.A.so`, `libcpp-ne.so.4`
and `libc.so.3`. These are **SDP observations**, not verified properties of the open binaries.

## Work needed before considering replacement

1. Rebuild both variants from the same clean revision and pinned inputs; retain the open and SDP artifacts and logs.
   Record compiler versions, selected programs, any reused files and hashes. Follow [README.md](README.md) and
   [DEVELOPMENT.md](../docs/DEVELOPMENT.md); do not publish proprietary HMI libraries, credentials or private logs.
2. Compare each common ELF using `file`, `readelf -h -l -d -V` and `readelf --dyn-syms`, including interpreter,
   `DT_NEEDED`, symbol versions/imports/exports and plugin dependencies. Investigate differences rather than assuming
   matching names imply matching ABI. Compare the hand-written headers, struct layouts, constants, startup, spawn,
   `devctlv` and C++ runtime boundaries against SDP, especially for USB, HMI and DSI.
3. Close the `accessory_window` gap, or explicitly maintain and validate an SDP-built component with traceable
   provenance. Decide separately whether the omitted test tools need open builds.
4. With appropriate target libraries and isolated, non-destructive test fixtures, run dedicated ABI/runtime probes and
   existing protocol tests on the QNX VM, comparing behavior with SDP. Do not start vehicle services or install anything
   on the car as part of this step. VM success alone does not establish NBT compatibility.
5. Before any release or vehicle deployment, obtain separate authorization, prepare a known-working rollback and run
   a parked vehicle validation of startup, USB/session, HMI plugin and controls, video, audio and repeated reconnects.
   Stop on abnormal behavior. Retain the SDP path until these checks and review pass.

The historical comparisons in [README.md](README.md#how-it-differs-from-an-sdp-build) concern earlier sources and
cannot substitute for these checks on the current code.
