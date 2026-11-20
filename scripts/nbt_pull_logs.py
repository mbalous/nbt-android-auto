#!/usr/bin/env python3
"""Pull the Android Auto logs from the NBT and summarise what went wrong.

Run on the Mac after driving, with the ENET cable in and ignition on (SSH must already be open on the NBT):

    python3 scripts/nbt_pull_logs.py                 # pull + summary (asks for the NBT root password once)
    python3 scripts/nbt_pull_logs.py --summary DIR   # summary of an earlier pull again, no car needed

Everything is read-only on the car: one SSH session prints the files, nothing is changed or deleted there.
Fetched: events.log (+ .1), boots.log, logs/, logs.prev/, history/ (last 15 boots), incidents/, snapshots/, the RAM logs
of the current boot (/fs/ram3/nbt-aa-*), and the QNX system log, process list and free flash space right now.
Saved to .local/car-logs/<time>/ (git-ignored: it can contain phone names and car details), with summary.md.
Set NBT_HOST to change the address (default 169.254.199.99), NBT_SSH_KEY to log in with a key instead of the password.
"""
import argparse
import datetime as dt
import os
import re
import secrets
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / ".local" / "car-logs"
P = "/var/opt/hmi/data/rw/NbtAndroidAutoRT"

REMOTE = r"""
T=%(token)s
P=%(pkg)s
emit() { echo "<<<$T BEGIN $1>>>"; cat "$1" 2>/dev/null; echo; echo "<<<$T END>>>"; }
run() { echo "<<<$T BEGIN live/$1>>>"; shift; sh -c "$*" 2>&1; echo; echo "<<<$T END>>>"; }
for f in $P/events.log $P/events.log.1 $P/boots.log $P/boot-count; do test -f $f && emit $f; done
for d in $P/logs $P/logs.prev $P/history $P/incidents $P/snapshots; do
  test -d $d && for f in `find $d -type f 2>/dev/null`; do emit $f; done
done
for f in /fs/ram3/nbt-aa-* /fs/ram3/nbt-cluster.log /fs/ram3/nbt-aa-native/*.log /fs/ram3/nbt-aa-native/status \
         /fs/ram3/nbt-aa-native/audio-route.* /fs/ram3/nbt-aa-native/volume /fs/ram3/nbt-aa-native/nowplaying; do
  test -f $f && emit $f
done
run sloginfo.txt 'sloginfo | tail -3000'
run pidin-info.txt 'pidin info'
run processes.txt 'pidin -f anJ'
run df.txt 'df -k'
run date.txt 'date'
"""

ERROR_PATTERNS = [
    ("USB transfer failed", r"AA_USB_(IN|OUT)_FAILED"),
    ("video stall", r"AA_STALL no video"),
    ("ByeBye not answered", r"AA_BYEBYE no answer"),
    ("live queue drops", r"AA_LIVE_QUEUE dropped=[1-9]"),
    ("A2I open failed", r"A2I_Client_Open\(.*failed"),
    ("mic silent", r"AA_MIC level: peak [0-7] of"),
    ("mic could not start", r"AA_MIC capture could not start|AA_MIC fallback could not start"),
    ("mic switched to fallback", r"AA_MIC .* gives only zeros"),
    ("audio ran dry", r"\[Browser\] .* ran dry"),
    ("audio route refused", r"refused|not granted"),
    ("hold refused by loop guard", r"hold refused by loop guard"),
    ("hold ignored by iDrive", r"verify: no hold state focused"),
    ("HMI restarted", r"HMI restarted"),
    ("hwvideo gave up", r"\[hw\].*(no acknowledgement|giving up)|nbt_hwvideo ended"),
    ("signal / crash", r"died by signal|CRASH"),
    ("NME options changed back", r"NME_OPTIONS .*: [0-9]+ written back"),
]


def ssh_cmd(host):
    cmd = ["ssh", "-o", "HostKeyAlgorithms=+ssh-rsa", "-o", "PubkeyAcceptedAlgorithms=+ssh-rsa", "-o",
           "ConnectTimeout=10", "-o", "ServerAliveInterval=15"]
    key = os.environ.get("NBT_SSH_KEY")
    if key:
        cmd += ["-i", key, "-o", "IdentitiesOnly=yes"]
    return cmd + ["root@" + host, "sh -s"]


def pull(host):
    token = secrets.token_hex(8)
    script = REMOTE % {"token": token, "pkg": P}
    print("Connecting to %s (enter the NBT root password if asked) ..." % host, file=sys.stderr)
    r = subprocess.run(ssh_cmd(host), input=script.encode(), stdout=subprocess.PIPE)
    if r.returncode not in (0, 1) or not r.stdout:
        sys.exit("SSH failed (exit %d). Ignition on, ENET cable in, SSH open on the NBT?" % r.returncode)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    dest = OUT / stamp
    begin = re.compile(rb"<<<" + token.encode() + rb" BEGIN (.+?)>>>\n")
    end = b"\n<<<" + token.encode() + b" END>>>"
    data, pos, n = r.stdout, 0, 0
    while True:
        m = begin.search(data, pos)
        if not m:
            break
        e = data.find(end, m.end())
        if e < 0:
            break
        name = m.group(1).decode("utf-8", "replace").lstrip("/")
        body = data[m.end():e]
        if body.endswith(b"\n"):
            body = body[:-1]   # the echo after cat
        target = dest / ("live" if name.startswith("live/") else "car") / name.replace("live/", "")
        if ".." in Path(name).parts:
            pos = e + len(end)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(body)
        n += 1
        pos = e + len(end)
    if not n:
        sys.exit("Nothing received from the NBT.")
    print("Saved %d files to %s" % (n, rel(dest)), file=sys.stderr)
    return dest


def rel(p):
    try:
        return p.relative_to(ROOT)
    except ValueError:
        return p


def read(p):
    try:
        return p.read_text("utf-8", "replace")
    except OSError:
        return ""


TS = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")


def when(line):
    m = TS.match(line)
    return dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S") if m else None


def summary(dest):
    car = dest / "car" / P.lstrip("/")
    ram = dest / "car" / "fs" / "ram3"
    lines = []
    out = lines.append
    out("# Android Auto log summary - pulled %s\n" % dest.name)

    last_pull = None
    marker = OUT / "last-pull"
    if marker.exists():
        try:
            last_pull = dt.datetime.strptime(marker.read_text().strip(), "%Y-%m-%d %H:%M:%S")
        except ValueError:
            pass
    if last_pull:
        out("Entries newer than the previous pull (%s) are marked **NEW**.\n" % last_pull)

    events = read(car / "events.log.1") + read(car / "events.log")
    ev = [l for l in events.splitlines() if TS.match(l)]

    # sessions
    starts = [l for l in ev if "session running" in l]
    ends = [l for l in ev if "session end:" in l]
    reasons = Counter(re.sub(r" after \d+s.*", "", l.split("session end: ", 1)[1]) for l in ends)
    short = [l for l in ends if re.search(r"after ([0-9]|[1-5][0-9])s", l)]
    out("## Sessions\n")
    out("- %d sessions started, %d ended (%d within a minute)" % (len(starts), len(ends), len(short)))
    for r_, c in reasons.most_common():
        out("  - %dx %s" % (c, r_))

    # incidents
    inc = [l for l in ev if " INCIDENT " in l]
    kinds = Counter(l.split(" INCIDENT ", 1)[1].split()[0] for l in inc)
    out("\n## Incidents (%d)\n" % len(inc))
    for k, c in kinds.most_common():
        out("- %dx %s" % (c, k))
    for l in inc[-25:]:
        t = when(l)
        new = " **NEW**" if last_pull and t and t > last_pull else ""
        out("  - `%s`%s" % (l.strip()[:200], new))

    # crashes / freezes straight from events.log
    hard = [l for l in ev if " CRASH " in l or " FREEZE " in l]
    if hard:
        out("\n## Crashes and freezes\n")
        for l in hard[-20:]:
            out("- `%s`" % l.strip()[:200])

    # boots: a boot right after activity while a session ran = suspected reboot while driving
    out("\n## Head-unit boots\n")
    boots = [l for l in read(car / "boots.log").splitlines() if TS.match(l)]
    out("- %d boots recorded" % len(boots))
    hist = car / "history"
    for d in sorted(hist.glob("boot-*"), key=lambda p: int(re.sub(r"\D", "", p.name) or 0)):
        st = [l for l in read(d / "states.txt").splitlines() if TS.match(l)]
        if not st:
            continue
        last = st[-1]
        nb = int(re.sub(r"\D", "", d.name) or 0) + 1
        nxt = next((when(b) for b in boots if b.endswith(" boot %d" % nb)), None)
        gap = (nxt - when(last)).total_seconds() if nxt and when(last) else None
        flag = ""
        if gap is not None and gap < 180 and "running" in last:
            flag = "  **suspected reboot while Android Auto ran** (next boot %ds later)" % gap
        out("- %s: last state `%s`%s" % (d.name, last[20:120], flag))

    # error patterns across every text file
    out("\n## Error patterns (all pulled logs)\n")
    counts, examples = Counter(), defaultdict(list)
    for f in dest.rglob("*"):
        if not f.is_file() or f.stat().st_size > 20_000_000:
            continue
        for l in read(f).splitlines():
            for label, pat in ERROR_PATTERNS:
                if re.search(pat, l):
                    counts[label] += 1
                    if len(examples[label]) < 2:
                        examples[label].append("%s: %s" % (f.relative_to(dest), l.strip()[:160]))
    if not counts:
        out("- none")
    for label, c in counts.most_common():
        out("- %dx %s" % (c, label))
        for e in examples[label]:
            out("  - `%s`" % e)

    # current state
    out("\n## Right now\n")
    out("- state: `%s`" % read(ram / "nbt-aa-state").strip().replace("\n", " ")[:200])
    df = read(dest / "live" / "df.txt")
    m = re.search(r"\n(\S*\s+\d+\s+\d+\s+(\d+)\s+(\d+)%\s+/var/opt[^\n]*)", df) or re.search(r"\n([^\n]*hmi[^\n]*)", df)
    if m:
        out("- flash (package folder): `%s`" % m.group(1).strip()[:120])

    text = "\n".join(lines) + "\n"
    (dest / "summary.md").write_text(text)
    return text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--summary", type=Path, help="summarise an earlier pull folder again (no car needed)")
    args = ap.parse_args()
    if args.summary:
        print(summary(args.summary.resolve()))
        return
    dest = pull(os.environ.get("NBT_HOST", "169.254.199.99"))
    print(summary(dest))
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "last-pull").write_text(dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    print("Summary: %s" % rel(dest / "summary.md"))


if __name__ == "__main__":
    main()
