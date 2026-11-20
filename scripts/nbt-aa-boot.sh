#!/bin/sh
# Android Auto on the original NBT: start the service from the PERSISTENT package (installed by car-aa-autostart-install.sh).
# Started by the "Android Auto" HMI plugin (libNbtAndroidAuto.so) when the HMI loads it, and again when the page
# is opened while the service is not running. Also usable by hand: sh /var/opt/hmi/data/rw/NbtAndroidAutoRT/boot.sh
# Hardware video path (VideoApp, car-proven 2026-11-07), iDrive controller (nbt_knob), RAM only at runtime.
# Disable the automatic start: touch /var/opt/hmi/data/rw/NbtAndroidAutoRT/disabled
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
LOG=/fs/ram3/nbt-aa-boot.log
exec >> $LOG 2>&1
echo "=== boot.sh `date`"
if test -e $P/disabled; then echo 'disabled'; exit 0; fi
# Never remove the marker here: native_service serializes stale-lock recovery.
if test -e $R/service.lock; then
  LP=`sed 1q $R/service.lock 2>/dev/null`
  case "$LP" in
    ''|*[!0-9]*|0|1) echo "ambiguous service.lock; refusing to start"; exit 1;;
  esac
  if kill -0 "$LP" 2>/dev/null; then echo "service already running (lock, pid $LP)"; exit 0; fi
  echo "possibly stale service.lock (pid $LP): native_service will check"
fi
mkdir -p $R
chmod 700 $R
for f in native_service nbt_android_auto nbt_knob nbt_hwvideo decode_runner accessory_window fifo_http; do
  cp $P/$f $R/$f && chmod 700 $R/$f
done
# io-nme's global options as they are at boot, saved by decode_runner on its first start (2026-11-12, see
# nbt_decode_runner.cpp): a new boot takes a new copy
rm -f $R/nme-options.saved $R/nme-options.saved.tmp
cp $P/cert.pem $P/key.pem $P/ca.pem $R/ && chmod 600 $R/*.pem
cp /opt/mm/bin/NmeCmdLine /opt/mm/lib/cinemo/cinemo_classes.xml /opt/mm/lib/cinemo/cinemo_links.xml $R/
chmod 700 $R/NmeCmdLine
touch $R/hw.mode
# optional sharper picture: phone sends 1280x720 (touch $P/res720 to enable, rm to go back to 800x480)
rm -f $R/res720
if test -e $P/res720; then touch $R/res720; echo 'res720 on'; fi
# Default: Android Auto sound on main source f6. aa-audio selects the alternative browser route.
# bt-audio selects Bluetooth.
rm -f $R/aa-audio $R/audioconn $R/bt-audio $R/media-jack $R/prompts-off
if test -f $P/audioconn; then cp $P/audioconn $R/audioconn && chmod 700 $R/audioconn; fi
if test -e $P/aa-audio; then cp $P/aa-audio $R/aa-audio; fi
if test -e $P/prompts-off; then touch $R/prompts-off; fi
if test -e $P/bt-audio; then touch $R/bt-audio; fi
echo 'audio: mode selected by native_service'
rm -f $R/wal.args $R/shadow.path
# No gate (user 2026-11-12): the phone is taken at once, the session runs in the background and the picture stays hidden
# (/fs/ram3/nbt-aa-hidden) until the Android Auto page is opened. The old behaviour (phone only after the page was opened
# once): touch $P/gate-on
rm -f $R/gate.mode
if test -e $P/gate-on; then touch $R/gate.mode; echo 'gate on'; fi
DPI=140
if test -f $P/vid.dpi; then DPI=`sed 1q $P/vid.dpi`; fi
# picture mode (car-tested 2026-11-08), one flag file in $P, none = classic 800x300 stretched to the whole display:
#   anamorph   whole 800x480 canvas, phone told pixels are 1.6x wide, stretched to 1280 horizontally only
#   center800  whole 800x480 canvas 1:1 in the middle, black bars left/right
#   split800   whole 800x480 canvas 1:1 at the left edge, iDrive split screen visible on the right
# These modes use the full 800x480 canvas: DPI 150 by default (the 800x300 DPI of 100 would make everything tiny),
# own value in $P/mode.dpi.
# Default without any flag: anamorph ("Breitbild" in the Android Auto page, which writes split800 or anamorph);
# $P/classic restores the old 800x300 picture.
rm -f $R/center800 $R/anamorph $R/split800
MODE=anamorph
for m in split800 center800 anamorph classic; do
  if test -e $P/$m; then MODE=$m; break; fi
done
if test $MODE != classic; then
  touch $R/$MODE; DPI=150                    # 140 -> 150 (user 2026-11-09: everything a bit bigger)
  if test -f $P/mode.dpi; then DPI=`sed 1q $P/mode.dpi`; fi
fi
echo "picture mode $MODE, dpi $DPI"
cd $R
NBT_VID_DPI=$DPI NBT_VID_INSETS=0,180,0,0 LD_LIBRARY_PATH=/opt/mm/lib:/opt/mm/lib/cinemo:/lib:/usr/lib ./native_service </dev/null >/dev/null 2>&1 &
echo "native_service started pid=$!"

# Android Auto turn-by-turn in the instrument cluster (arrow, distance, street, road shield, lanes; nbt_cluster draws only
# while the phone guides and yields to the iDrive navigation). Disable: touch $P/cluster-off
if test -f $P/nbt_cluster -a ! -e $P/cluster-off; then
  # stop a running copy first: a running binary cannot be overwritten (car 2026-11-10: "cp: Resource busy")
  slay nbt_cluster 2>/dev/null
  sleep 1
  cp $P/nbt_cluster $R/nbt_cluster && chmod 700 $R/nbt_cluster
  (cd $R; LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_cluster </dev/null > /fs/ram3/nbt-cluster.log 2>&1 &)
  echo 'cluster guidance started'
fi

# Diagnostic logs on PERSISTENT storage (car 2026-11-07: after an Android Auto drop-out SSH only worked again after a
# restart, and the RAM logs were gone). Written only when the public state changes (connecting, running, draining, ...),
# so the flash sees a few small writes per session. The previous boot's set is kept as logs.prev.
# Rotated once per head-unit BOOT, not per service start (boot.sh also runs on every service restart: that used to
# throw away the logs of the drive that needed the restart). /fs/ram3 is empty after a boot, so a marker there tells
# a new boot from a restart. The last 15 boots are kept in history/boot-<n> (logs.prev = the previous boot, as before);
# scripts/nbt_pull_logs.py on the Mac fetches all of it.
L=$P/logs
H=$P/history
if test -e "$P/extended-log-history" && test ! -e /fs/ram3/nbt-aa-booted; then
  N=`cat $P/boot-count 2>/dev/null`
  case "$N" in ''|*[!0-9]*) N=0 ;; esac
  N=$((N+1)); echo $N > $P/boot-count
  rm -r $L.prev 2>/dev/null
  if test -d $L; then
    mkdir -p $H
    OLD=`cat $L/boot-number 2>/dev/null`
    case "$OLD" in ''|*[!0-9]*) OLD=$((N-1)) ;; esac
    cp -r $L $H/boot-$OLD 2>/dev/null
    mv $L $L.prev
  fi
  n=0
  for d in `ls -t -d $H/boot-* 2>/dev/null`; do n=$((n+1)); if test $n -gt 15; then rm -r $d; fi; done
  # and at most ~20 MB in all (the flash is shared with iDrive): drop the oldest boots until it fits
  while test `du -sk $H 2>/dev/null | sed 's/[^0-9].*//'` -gt 20000 2>/dev/null; do
    o=`ls -t -d $H/boot-* 2>/dev/null | tail -1`; test -n "$o" || break; rm -r $o
  done
  mkdir -p $L
  echo $N > $L/boot-number
  echo $N > /fs/ram3/nbt-aa-booted
elif test ! -e "$P/extended-log-history"; then
  rm -r $L.prev 2>/dev/null
  if test -d $L; then mv $L $L.prev; fi
fi
mkdir -p $L
# optional passive MOST trace of the cluster graphics terminal (FBlock 0x82) right after the start, when the NBT creates
# its drawing instances (touch $P/trace-most; writes $L/most-boot-82.txt, 4 minutes, observe only)
if test -e $P/trace-most -a -x $R/nbt_knob; then
  (cd $R; LD_LIBRARY_PATH=/opt/sys/lib:/lib:/usr/lib ./nbt_knob --trace --filter 82 --seconds 240 --fifo /fs/ram3/nbt-trace-dummy.fifo > $L/most-boot-82.txt 2>&1 &)
  echo 'most boot trace started'
fi
# optional read-only observer for the cluster investigation (touch $P/trace-cluster; writes $L/cluster-trace.txt)
if test -e $P/trace-cluster -a -f $P/cluster-trace.sh; then
  sh $P/cluster-trace.sh 3600 </dev/null >/dev/null 2>&1 &
  echo "cluster trace started pid=$!"
fi
(
  last=''
  while test -e $R/service.lock -o -z "$last"; do
    s=`cat /fs/ram3/nbt-aa-state 2>/dev/null | tr '\n' ' '`
    if test "$s" != "$last"; then
      last="$s"
      echo "`date '+%Y-%m-%d %H:%M:%S'` $s" >> $L/states.txt
      cp /fs/ram3/nbt-aa-plugin.log /fs/ram3/nbt-aa-boot.log $L/ 2>/dev/null
      for k in capture decoder hwvideo; do
        f=`ls -t $R/$k-*.log 2>/dev/null | sed 1q`
        if test -n "$f"; then cp $f $L/$k-last.log; fi
      done
      tail -200 $R/knob.log > $L/knob-tail.log 2>/dev/null
      tail -100 /fs/ram3/nbt-cluster.log > $L/cluster-tail.log 2>/dev/null
      sloginfo 2>/dev/null | grep -e DisplayManagerScreen -e nvideocapture -e 'WindowVisible' -e 'zorder' -e 'NVRM' -e watchdog -e nme -e Nme -e NME -e udio | tail -60 > $L/slog-tail.txt
    fi
    # iDrive (HmiMain3D) restarted without a head-unit boot: the HMI plugin leaves this flag (aa_entry_plugin.cpp)
    if test -e "$P/incident-logging" && test -e /fs/ram3/nbt-aa-hmi-restart; then
      rm -f /fs/ram3/nbt-aa-hmi-restart
      sh $P/incident.sh hmi-restart 0 "iDrive (HMI) restarted while the head unit kept running" </dev/null >/dev/null 2>&1 &
    fi
    sleep 5
  done
  echo "`date '+%Y-%m-%d %H:%M:%S'` service ended" >> $L/states.txt
) </dev/null >/dev/null 2>&1 &
