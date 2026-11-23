#!/bin/sh
# Build every program of the installed package inside the QNX 6.5.0 VM.
#
# Normally started by scripts/build_all.py, which prepares the current directory:
#   src/                 this repository's src/ folder
#   reference/           NBT-AAD milestone-7 src/ and include/ with our overlay and patches applied
#   mbedtls-3.6.7/       Mbed TLS release with the legacy-UTCTime patch and the QNX config
#   expected/            optional: binaries of an installer payload to compare against
# Environment: HMI_LIBS = folder with libAppFramework.A.so and libCoreFramework.A.so copied from your own NBT
#              (needed only for the HMI plugin; without it the plugin is skipped).
#              HMI_MODE = stubs to build link-only BMW stubs, or real (default).
# Output: out/ (the programs as they go into installer/payload/rt and plugin/).
set -e
QNX_HOST=/usr/qnx650/host/qnx6/x86
QNX_TARGET=/usr/qnx650/target/qnx6
PATH=$QNX_HOST/usr/bin:$PATH
export QNX_HOST QNX_TARGET PATH

W=`pwd`
OUT=$W/out
A=$W/src/android_auto
HMI_LIBS=${HMI_LIBS:-$HOME/.nbt-build/hmi-libs}
HMI_MODE=${HMI_MODE:-real}
CC="qcc -V4.4.2,gcc_ntox86"
CXX="qcc -V4.4.2,gcc_ntox86_cpp -lang-c++ -O1 -Wall -D_QNX_SOURCE -Wc,-std=gnu++0x"
echo ABI_TOOLCHAIN_INFO_BEGIN
echo "C: $CC"
echo "C++: $CXX"
qcc -V 2>&1 || echo "QCC_VERSION_LIST_UNAVAILABLE"
ntox86-ld --version 2>&1 || echo "LD_VERSION_UNAVAILABLE"
echo ABI_TOOLCHAIN_INFO_END
TLSINC="-I$W/mbedtls-3.6.7/include"
TLSLIBS="$W/mbedtls-3.6.7/library/libmbedtls.a $W/mbedtls-3.6.7/library/libmbedx509.a $W/mbedtls-3.6.7/library/libmbedcrypto.a"
AAINC="$TLSINC -I$W/reference/src -I$W/reference/include"
mkdir -p $OUT

echo "== Mbed TLS 3.6.7 static libraries (a few minutes)"
if ! (cd mbedtls-3.6.7 && make lib CC="$CC" AR=ntox86-ar CFLAGS='-O2 -D_QNX_SOURCE -Wc,-std=gnu99' >$W/mbedtls-build.log 2>&1); then
  tail -20 $W/mbedtls-build.log
  exit 1
fi

echo "== NBT-AAD core: libnbt_aa_core.a"
mkdir -p core
for c in aa_session aa_cryptor audio/audio_acks sdr_builder crash_handler; do
  $CXX $AAINC -c reference/src/$c.cpp -o core/`basename $c`.o
done
# browser audio mode (Android Auto sound over the AA connection, off unless the aa-audio mode file exists)
for c in browser_audio asound_device audio_route mixer resampler drift config stream_rate pcm_ring playout speech_focus; do
  $CXX $AAINC -c reference/src/audio/$c.cpp -o core/audio_$c.o
done
$CC -O1 -I$W/reference/src -c reference/src/audio/speex.c -o core/audio_speex.o   # vendored speexdsp resampler
rm -f libnbt_aa_core.a
ntox86-ar rcs libnbt_aa_core.a core/aa_session.o core/aa_cryptor.o core/audio_acks.o core/sdr_builder.o core/crash_handler.o \
  core/audio_*.o

echo "== nbt_android_auto (USB/AOA + TLS + Android Auto session)"
cd $A
$CC -Wall -Wextra -DNBT_AA_TLS_HANDSHAKE -DNBT_AA_CAPTURE -c nbt_android_auto.c -o $W/nbt_android_auto.o
$CXX $AAINC aa_video_capture.cpp aa_audio.cpp aa_media_out.cpp tls_usb_probe.cpp $W/nbt_android_auto.o $W/core/aa_cryptor.o $W/libnbt_aa_core.a $TLSLIBS \
  -lsocket -lm -lusbdi -o $OUT/nbt_android_auto

echo "== native_service, decode_runner, fifo_http, accessory_window"
$CC -O2 -Wall -Wextra nbt_native_service.c -o $OUT/native_service
$CXX -O2 -Wextra nbt_decode_runner.cpp -o $OUT/decode_runner
$CXX -O0 -Wextra nbt_fifo_http.cpp -lsocket -o $OUT/fifo_http
$CC -Wall -Wextra accessory_window.c -o $OUT/accessory_window

echo "== nbt_knob, nbt_hwvideo, nbt_cluster (MOST/DSI)"
cd $A/knob
$CXX -Iref nbt_knob.cpp ref/dsi_client.cpp ref/most/client.cpp ref/car/ccs_input.cpp -o $OUT/nbt_knob
$CXX -Iref nbt_hwvideo.cpp ref/dsi_client.cpp ref/most/client.cpp -o $OUT/nbt_hwvideo
$CXX -Iref nbt_cluster.cpp ref/dsi_client.cpp ref/most/client.cpp -o $OUT/nbt_cluster -lm
# audioconn replacement for the original NBT (ConnectionMaster FBlock 03 build/remove of connection 0xb8)
$CXX -Iref nbt_audioconn.cpp ref/dsi_client.cpp ref/most/client.cpp -o $OUT/audioconn
$CXX -Iref nbt_key.cpp ref/dsi_client.cpp ref/most/client.cpp -o $OUT/nbt_key

echo "== nbt_a2i_beep (test: beeps into an io-amp input)"
$CC -Wall $A/audio/nbt_a2i_beep.c -o $OUT/nbt_a2i_beep -lm

echo "== HMI plugin libNbtAndroidAuto.so"
cd $W/src/idrive
HMI_LINK_EXTRA=
case "$HMI_MODE" in
  stubs)
    HMI_LIBS=$W/hmi-stubs
    mkdir -p "$HMI_LIBS"
    $CC -shared -fPIC -nostdlib -Wl,-soname,libAppFramework.A.so "$W/toolchain/hmi-stubs/app.c" -o "$HMI_LIBS/libAppFramework.A.so"
    $CC -shared -fPIC -nostdlib -Wl,-soname,libCoreFramework.A.so "$W/toolchain/hmi-stubs/core.c" -o "$HMI_LIBS/libCoreFramework.A.so"
    # The real CoreFramework imports this libgcc helper; linking against it normally pulls the helper into the plugin.
    HMI_LINK_EXTRA=-Wl,-u,__divdi3
    ;;
  real) ;;
  *) echo "Invalid HMI_MODE: $HMI_MODE" >&2; exit 1 ;;
esac
if test -f "$HMI_LIBS/libAppFramework.A.so" -a -f "$HMI_LIBS/libCoreFramework.A.so"; then
  qcc -V4.4.2,gcc_ntox86_cpp-ne -O2 -Wall -Wextra -shared -fPIC -fno-exceptions -fno-rtti -Wl,-z,defs \
    aa_entry_plugin.cpp -o $OUT/libNbtAndroidAuto.so -L"$HMI_LIBS" -lAppFramework.A -lCoreFramework.A $HMI_LINK_EXTRA
  echo "QNX_HMI_LINK=$HMI_MODE"
else
  if test "$HMI_MODE" = stubs; then echo "Missing generated HMI stubs" >&2; exit 1; fi
  echo "SKIPPED: no libAppFramework.A.so / libCoreFramework.A.so in $HMI_LIBS (copy them from your NBT)"
  echo "QNX_HMI_LINK=missing"
fi

echo "== nbt_reset (Restart system on the Android Auto page)"
cd $W/src/idrive/reboot
$CC -O2 -Wall -Wextra nbt_reset.c -lsocket -o $OUT/nbt_reset

cd $W
echo "== built"
ls -l $OUT

if test -d expected; then
  echo "== compare with expected/ (SAME = byte-identical, SAME-STRIPPED = identical without symbol tables)"
  mkdir -p cmp
  for f in `ls $OUT`; do
    if test ! -f expected/$f; then echo "NO-REFERENCE  $f"; continue; fi
    if cmp -s $OUT/$f expected/$f; then echo "SAME          $f"; continue; fi
    cp $OUT/$f cmp/a; cp expected/$f cmp/b; ntox86-strip cmp/a cmp/b
    if cmp -s cmp/a cmp/b; then echo "SAME-STRIPPED $f"; else echo "DIFFERENT     $f"; fi
  done
fi
echo "BUILD_QNX_DONE"
