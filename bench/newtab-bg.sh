#!/usr/bin/env bash
# bench/newtab-bg.sh EXE [N] — new tab → first frame showing it, and → first frame
# with its prompt (ms, timed inside hardcore with HARDCORE_LAT, frames as handed
# to the compositor). Warm: a pre-warmed shell is waiting, as in normal use.
# Cold: a second tab right after the first, before the next spare is ready.
cd "$(dirname "$0")/.."
exe=$1; n=${2:-10}
B=bench/termbench.exe; T="--class hardcore --title hcbench"
TL=/mnt/c/Users/willi/AppData/Local/Temp
mkdir -p build/hcbench; /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1; sleep 0.3
cp "$exe" build/hcbench/hcbench.exe; rm -f "$TL/hardcore-newtab.txt"
(cd /mnt/c && HARDCORE_LAT=1 WSLENV=HARDCORE_LAT:${WSLENV:-} setsid "$OLDPWD/build/hcbench/hcbench.exe" --no-activate \
  --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
for i in $(seq 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
$B resize 1600,900 $T; sleep 3
for i in $(seq $n); do $B cmd newtab $T; sleep 0.6; $B cmd closetab $T; sleep 2; done
warm=$(wc -l < "$TL/hardcore-newtab.txt")
for i in $(seq $n); do $B cmd newtab $T; $B cmd newtab $T; sleep 4; $B cmd closetab $T; $B cmd closetab $T; sleep 0.5; done
/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
tr -d '\r' < "$TL/hardcore-newtab.txt" | python3 -c '
import sys
rows = [list(map(float, l.split())) for l in sys.stdin if l.strip()]
warm = int(sys.argv[1])
def p(v, q): v = sorted(v); return v[min(len(v) - 1, int(q * len(v)))]
for name, rs in (("warm", rows[:warm]), ("cold (2nd of 2)", rows[warm + 1::2])):
    if rs: print("  %-16s n=%-3d tab shown p50 %6.1f ms   prompt drawn p50 %6.1f / p90 %6.1f ms" % (name, len(rs), p([r[0] for r in rs], .5), p([r[1] for r in rs], .5), p([r[1] for r in rs], .9)))' "$warm"
