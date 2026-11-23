#!/bin/sh
# Build the head-unit programs with the open QNX 6.5.0 x86 toolchain (toolchain/README.md), no QNX SDP or VM.
# Same steps and flags as scripts/build-qnx.sh; started by scripts/build_open.py inside the toolchain container,
# in a folder prepared the same way (src/, reference/, mbedtls-3.6.7/).
# Differences to the SDP build: gcc/g++ instead of qcc, the C++ runtime (GNU libstdc++ 4.4.2) is linked
# statically instead of Dinkum libcpp.so.4, and popen() comes from libqnxopen.a.
# Programs: $PROGRAMS (default: supported programs and the stub-linked HMI plugin).
set -e
T=i486-pc-nto-qnx6.5.0
W=`pwd`
OUT=$W/out
A=$W/src/android_auto
PROGRAMS=${PROGRAMS:-"nbt_android_auto native_service decode_runner fifo_http accessory_window nbt_knob nbt_hwvideo nbt_cluster audioconn nbt_key nbt_a2i_beep nbt_reset libNbtAndroidAuto.so"}
HMI_MODE=${HMI_MODE:-stubs}
CC="$T-gcc"
CXX="$T-g++ -O1 -Wall -D_QNX_SOURCE -std=gnu++0x"
LIBS="-lqnxopen"
AR=$T-ar
echo ABI_TOOLCHAIN_INFO_BEGIN
$T-gcc --version
$T-ld --version
echo ABI_TOOLCHAIN_INFO_END
TLSINC="-I$W/mbedtls-3.6.7/include"
TLSLIBS="$W/mbedtls-3.6.7/library/libmbedtls.a $W/mbedtls-3.6.7/library/libmbedx509.a $W/mbedtls-3.6.7/library/libmbedcrypto.a"
AAINC="$TLSINC -I$W/reference/src -I$W/reference/include"
mkdir -p $OUT
want() { case " $PROGRAMS " in *" $1 "*) return 0;; esac; return 1; }

if want nbt_android_auto; then
  echo "== Mbed TLS 3.6.7 static libraries"
  if ! (cd mbedtls-3.6.7 && make lib CC="$CC" AR=$AR CFLAGS='-O2 -D_QNX_SOURCE -std=gnu99' >$W/mbedtls-build.log 2>&1); then
    tail -20 $W/mbedtls-build.log; exit 1
  fi
  echo "== NBT-AAD core: libnbt_aa_core.a"
  mkdir -p core
  for c in aa_session aa_cryptor audio/audio_acks sdr_builder crash_handler; do
    $CXX $AAINC -c reference/src/$c.cpp -o core/`basename $c`.o
  done
  for c in browser_audio asound_device audio_route mixer resampler drift config stream_rate pcm_ring playout speech_focus; do
    $CXX $AAINC -c reference/src/audio/$c.cpp -o core/audio_$c.o
  done
  $CC -O1 -I$W/reference/src -c reference/src/audio/speex.c -o core/audio_speex.o
  rm -f libnbt_aa_core.a
  $AR rcs libnbt_aa_core.a core/aa_session.o core/aa_cryptor.o core/audio_acks.o core/sdr_builder.o core/crash_handler.o \
    core/audio_*.o
  echo "== nbt_android_auto"
  cd $A
  $CC -Wall -Wextra -DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE -c nbt_android_auto.c -o $W/nbt_android_auto.o
  $CXX $AAINC aa_video_capture.cpp aa_audio.cpp aa_media_out.cpp tls_usb_probe.cpp \
    $W/nbt_android_auto.o $W/core/aa_cryptor.o $W/libnbt_aa_core.a \
    $TLSLIBS -lsocket -lm -lusbdi $LIBS -o $OUT/nbt_android_auto
  cd $W
fi
cd $A
if want native_service; then echo "== native_service"; $CC -O2 -Wall -Wextra nbt_native_service.c $LIBS -o $OUT/native_service; fi
if want decode_runner; then echo "== decode_runner"; $CXX -O2 -Wextra -DNBT_OPEN_TOOLCHAIN nbt_decode_runner.cpp $LIBS -o $OUT/decode_runner; fi
if want fifo_http; then echo "== fifo_http"; $CXX -O0 -Wextra nbt_fifo_http.cpp -lsocket $LIBS -o $OUT/fifo_http; fi
if want accessory_window; then echo "== accessory_window"; $CC -Wall -Wextra accessory_window.c $LIBS -o $OUT/accessory_window; fi
if want nbt_a2i_beep; then echo "== nbt_a2i_beep"; $CC -Wall audio/nbt_a2i_beep.c -lm $LIBS -o $OUT/nbt_a2i_beep; fi
cd $A/knob
if want nbt_knob; then echo "== nbt_knob"; $CXX -Iref nbt_knob.cpp ref/dsi_client.cpp ref/most/client.cpp ref/car/ccs_input.cpp $LIBS -o $OUT/nbt_knob; fi
if want nbt_hwvideo; then echo "== nbt_hwvideo"; $CXX -Iref nbt_hwvideo.cpp ref/dsi_client.cpp ref/most/client.cpp $LIBS -o $OUT/nbt_hwvideo; fi
if want nbt_cluster; then echo "== nbt_cluster"; $CXX -Iref nbt_cluster.cpp ref/dsi_client.cpp ref/most/client.cpp -lm $LIBS -o $OUT/nbt_cluster; fi
if want audioconn; then echo "== audioconn"; $CXX -Iref nbt_audioconn.cpp ref/dsi_client.cpp ref/most/client.cpp $LIBS -o $OUT/audioconn; fi
if want nbt_key; then echo "== nbt_key"; $CXX -Iref nbt_key.cpp ref/dsi_client.cpp ref/most/client.cpp $LIBS -o $OUT/nbt_key; fi
cd $W
if want nbt_reset; then echo "== nbt_reset"; $CC -O2 -Wall -Wextra src/idrive/reboot/nbt_reset.c -lsocket $LIBS -o $OUT/nbt_reset; fi
# The open C++ runtime link always uses a link-only libcpp-ne stub, even with real BMW frameworks.
if want libNbtAndroidAuto.so; then
  echo "== HMI plugin libNbtAndroidAuto.so ($HMI_MODE)"
  S=$W/hmi-stubs
  mkdir -p "$S"
  case "$HMI_MODE" in
    stubs)
      HMI_FRAMEWORK_LIBS=$S
      $CC -shared -fPIC -nostdlib -Wl,-soname,libAppFramework.A.so "$W/toolchain/hmi-stubs/app.c" -o "$S/libAppFramework.A.so"
      $CC -shared -fPIC -nostdlib -Wl,-soname,libCoreFramework.A.so "$W/toolchain/hmi-stubs/core.c" -o "$S/libCoreFramework.A.so"
      ;;
    real)
      HMI_FRAMEWORK_LIBS=${HMI_LIBS:?HMI_LIBS is required in real mode}
      for name in libAppFramework.A.so libCoreFramework.A.so; do
        test -s "$HMI_FRAMEWORK_LIBS/$name" || { echo "Missing real HMI library: $name" >&2; exit 1; }
      done
      ;;
    *) echo "Invalid HMI_MODE: $HMI_MODE" >&2; exit 1 ;;
  esac
  $CC -shared -fPIC -nostdlib -Wl,-soname,libcpp-ne.so.4 "$W/toolchain/hmi-stubs/cpp.c" -o "$S/libcpp-ne.so.4"
  cd $W/src/idrive
  $T-g++ -O2 -Wall -Wextra -D_QNX_SOURCE -DNBT_OPEN_TOOLCHAIN -shared -fPIC -fno-exceptions -fno-rtti -nodefaultlibs -Wl,-z,defs \
    aa_entry_plugin.cpp -o $OUT/libNbtAndroidAuto.so -L"$HMI_FRAMEWORK_LIBS" -lAppFramework.A -lCoreFramework.A "$S/libcpp-ne.so.4" -lc -lgcc
  cd $W
fi
echo "== built"
ls -l $OUT
echo BUILD_OPEN_DONE
