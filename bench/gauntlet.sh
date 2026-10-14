#!/usr/bin/env bash
# bench/gauntlet.sh EXE [TABS] — the visible tab under fire. Opens TABS tabs
# (default 16) in a background copy of hardcore; every tab but the first runs
# bench/ccsim.py (a busy simulated Claude Code: synchronized-output frames at
# ~60 fps, streaming text, output dumps, title throbber, OSC 7501 states).
# Then, in the visible first tab, it measures what you would feel:
#   - keystroke latency (bench/echoprobe.py, input -> frame presented);
#   - full-screen redraw rate (bench/fullscreen.py);
# and prints hardcore's UI-thread split from HARDCORE_PERF.
# Compare TABS=1 (nothing else running) with 16 and 64: the visible tab
# shouldn't get slower.
cd "$(dirname "$0")/.."
exe=$1; tabs=${2:-16}; keys=${KEYS:-150}
B=bench/termbench.exe; T="--class hardcore --title hcbench"
TL=/mnt/c/Users/willi/AppData/Local/Temp
mkdir -p build/hcbench; /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1; sleep 0.3
cp "$exe" build/hcbench/hcbench.exe
rm -f "$TL/hardcore-perf.txt" "$TL/hardcore-lat.txt" /tmp/fullscreen.txt
(cd /mnt/c && HARDCORE_PERF=1 HARDCORE_LAT=1 WSLENV=HARDCORE_PERF:HARDCORE_LAT:${WSLENV:-} setsid "$OLDPWD/build/hcbench/hcbench.exe" \
  --no-activate --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
for i in $(seq 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
$B resize 1600,900 $T; sleep 3
for i in $(seq 2 $tabs); do $B cmd newtab $T; sleep 0.2; done
sleep 2
for i in $(seq 1 $((tabs - 1))); do $B cmd "tab $i" $T; $B send "python3 $PWD/bench/ccsim.py $i\n" $T; done
$B cmd "tab 0" $T; sleep 4
latmark=$(wc -l < "$TL/hardcore-lat.txt" 2>/dev/null || echo 0)
$B send "clear; python3 $PWD/bench/echoprobe.py\n" $T; sleep 1.5
mark=$(wc -l < "$TL/hardcore-perf.txt")
for i in $(seq $keys); do $B send x $T; sleep 0.0$((RANDOM % 5 + 4)); done
$B send q $T; sleep 0.5
$B send "python3 $PWD/bench/fullscreen.py 300\n" $T
for i in $(seq 150); do [ -s /tmp/fullscreen.txt ] && break; sleep 0.2; done
sleep 1.1
drag=$($B resizes 1600,900,1100,650 --n 20 $T)
sleep 1.5
[ -n "${SNAP:-}" ] && { $B cmd "tab $((tabs / 2))" $T; sleep 0.5; $B snap "$(wslpath -w "$SNAP")" $T; sleep 0.8; }
/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
echo "== gauntlet: $tabs tabs ($((tabs - 1)) simulated Claude sessions), $(basename "$exe")"
tr -d '\r\0' < "$TL/hardcore-lat.txt" | tail -n +$((latmark + 2)) | python3 -c '
import sys
rows = [list(map(float, l.split()[:4])) for l in sys.stdin if l.strip()]
def q(v, p): v = sorted(v); return v[min(len(v) - 1, int(p * len(v)))] if v else float("nan")
for k, n in ((0, "key -> echo back"), (2, "key -> frame presented"), (3, "key -> scanned out")):
    v = [r[k] for r in rows if len(r) > k and r[k] >= 0]
    print("  %-24s p50 %6.2f  p90 %6.2f  p99 %6.2f ms  (n=%d)" % (n, q(v, .5), q(v, .9), q(v, .99), len(v)))'
echo "$drag" | sed 's/^/  /'
printf '  %-24s %s\n' "full-screen redraw" "$(cat /tmp/fullscreen.txt 2>/dev/null || echo 'did not finish')"
tr -d '\r' < "$TL/hardcore-perf.txt" | tail -n +$((mark + 1)) | grep -v '^resize' | awk '
  { for (i = 1; i <= NF; i++) { if ($i == "MB/s") mb += $(i-1); if ($i == "fps") fp += $(i-1); if ($i == "parse") pa += $(i+1); if ($i == "build") bu += $(i+1); if ($i == "present") pr += $(i+1) } n++ }
  END { if (n) printf "  UI thread per second:    parse %.0f ms, build %.0f ms, present %.0f ms; %.1f MB/s in, %.0f frames/s (avg of %d s)\n", pa/n, bu/n, pr/n, mb/n, fp/n, n }'
