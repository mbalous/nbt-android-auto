#!/bin/sh
# Incident snapshot (installed as incident.sh in the package folder). Called when something goes wrong, so the logs can
# be pulled from the Mac later (scripts/nbt_pull_logs.py) without anybody being connected at the time:
#   sh incident.sh <kind> [session] [detail ...]
# kinds: crash (a program died by a signal), session-end, no-picture, replug, service-recovered, hmi-restart, user-restart
# Writes incidents/<date>-<kind>.txt (state, flags, the tails of the RAM logs, our processes, the QNX system log) and
# one INCIDENT line in events.log. Bounded: the newest 40 incidents are kept (each a few tens of KB). Read-only for
# everything else: it never stops or changes a process.
P=/var/opt/hmi/data/rw/NbtAndroidAutoRT
R=/fs/ram3/nbt-aa-native
if test ! -e "$P/incident-logging"; then
  if test "$1" = session-summary; then
    case "$2" in "$R"/session-summary-??????) rm -f "$2" ;; esac
  fi
  exit 0
fi
if test "$1" = session-summary; then
  S=$2
  case "$S" in "$R"/session-summary-??????) ;; *) echo "invalid session summary source" >&2; exit 1 ;; esac
  D=$P/session-summaries
  mkdir -p "$D" || { echo "cannot create session summary directory" >&2; rm -f "$S"; exit 1; }
  F=$D/`date +%Y%m%d-%H%M%S`-`basename "$S"`.txt
  if test -e "$F"; then echo "session summary already exists: $F" >&2; rm -f "$S"; exit 1; fi
  if ! dd if="$S" of="$F.tmp" bs=4096 count=1 2>/dev/null; then
    echo "cannot write session summary: $F" >&2; rm -f "$F.tmp" "$S"; exit 1
  fi
  if ! mv "$F.tmp" "$F"; then
    echo "cannot publish session summary: $F" >&2; rm -f "$F.tmp" "$S"; exit 1
  fi
  rm -f "$S" || echo "cannot remove session summary staging file: $S" >&2
  n=0
  for f in `ls -t $D/*.txt 2>/dev/null`; do n=$((n+1)); if test $n -gt 15; then rm -f "$f" || echo "cannot remove old summary: $f" >&2; fi; done
  exit 0
fi
# sed, not tr: QNX tr takes 'a-z' literally (car 2026-11-20: "freeze" became "z", "session-end" became "-")
K=`echo "${1:-unknown}" | sed 's/[^a-z0-9-]//g' | cut -c1-32`
test -n "$K" || K=unknown
S=${2:-0}
if test $# -ge 2; then shift 2; else shift $#; fi
D=$P/incidents
mkdir -p $D || exit 1
NOW=`date '+%Y-%m-%d %H:%M:%S'`
F=$D/`date +%Y%m%d-%H%M%S`-$K-$$.txt
{
  echo "=== incident $K  $NOW  session $S"
  echo "detail: $*"
  echo '--- uptime'; pidin info 2>/dev/null | head -3
  echo '--- aa state'; cat /fs/ram3/nbt-aa-state 2>/dev/null
  echo '--- flags'; ls -l /fs/ram3/nbt-aa-* $R/audio-route.* 2>/dev/null
  echo '--- plugin log'; tail -120 /fs/ram3/nbt-aa-plugin.log 2>/dev/null
  echo '--- boot log'; tail -30 /fs/ram3/nbt-aa-boot.log 2>/dev/null
  for f in `ls -t $R/capture-*.log 2>/dev/null | sed 2q`; do echo "--- $f"; tail -150 $f; done
  for f in `ls -t $R/decoder-*.log 2>/dev/null | sed 2q` `ls -t $R/fifo-http-*.log 2>/dev/null | sed 2q` `ls -t $R/hwvideo-*.log 2>/dev/null | sed 1q`; do
    echo "--- $f"; tail -60 $f
  done
  echo '--- knob'; tail -60 $R/knob.log 2>/dev/null
  echo '--- cluster'; tail -30 /fs/ram3/nbt-cluster.log 2>/dev/null
  echo '--- processes'; pidin -f anJ 2>/dev/null | grep -i -e nbt -e oneplus -e native -e decode -e fifo -e audioconn \
    -e accessory -e HmiMain -e nme -e VideoApp -e ioamp
  echo '--- system log'; sloginfo 2>/dev/null | tail -300
} 2>&1 | dd of="$F.tmp" bs=1024 count=256 2>/dev/null || exit 1
mv "$F.tmp" "$F" || exit 1
if test -f "$P/events.log" && test "`wc -c < "$P/events.log"`" -gt 131072; then
  mv "$P/events.log" "$P/events.log.1"
fi
echo "$NOW INCIDENT $K session=$S $* -> incidents/`basename $F`" >> $P/events.log
n=0
for f in `ls -t $D/*.txt 2>/dev/null`; do n=$((n+1)); if test $n -gt 40; then rm -f $f; fi; done
exit 0
