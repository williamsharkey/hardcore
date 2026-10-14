#!/usr/bin/env bash
# bench/run-bg.sh [EXE] [WxH] — throughput + internal latency, without taking
# your keyboard or focus. Launches a private copy of hardcore behind your
# windows with HARDCORE_PERF and HARDCORE_LAT on, drives it only through the
# WM_COPYDATA test channel, prints the numbers and closes it again.
#
#   throughput: bench/throughput.sh's three files, timed by `time cat`
#   perf:       hardcore's own per-second log while they ran (MB/s, fps, ms)
#   latency:    input → echo back, → frame built, → handed to the compositor (ms)
set -u
cd "$(dirname "$0")/.."
exe=${1:-hardcore.exe}
size=${2:-1600,900}
B=bench/termbench.exe
T="--class hardcore --title hcbench"
tmpw=$(cmd.exe /c echo %TEMP% 2>/dev/null | tr -d '\r')
tmpl=$(wslpath "$tmpw")
mkdir -p build/hcbench
cp "$exe" build/hcbench/hcbench.exe
rm -f "$tmpl/hardcore-perf.txt" "$tmpl/hardcore-lat.txt" /tmp/termbench-hcbench.txt

/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
(cd /mnt/c && HARDCORE_PERF=1 HARDCORE_LAT=1 WSLENV=HARDCORE_PERF:HARDCORE_LAT${WSLENV:+:$WSLENV} \
    setsid "$OLDPWD/build/hcbench/hcbench.exe" --no-activate --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
for i in $(seq 1 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
$B resize "$size" $T
sleep 2.5                                   # the shell's prompt

# latency: the echo probe flips a block on each input
$B send "clear; python3 $PWD/bench/echoprobe.py\n" $T
sleep 1.5
for i in $(seq 1 ${NLAT:-120}); do $B send x $T; sleep 0.0$((RANDOM % 5 + 4)); done
$B send q $T
sleep 0.5
lat=$(tr -d '\r' < "$tmpl/hardcore-lat.txt" 2>/dev/null)

# throughput
: > "$tmpl/hardcore-perf.txt.mark"
$B send "bash $PWD/bench/throughput.sh hcbench\n" $T
for i in $(seq 1 480); do [ "$(wc -l < /tmp/termbench-hcbench.txt 2>/dev/null)" = 3 ] && break; sleep 0.25; done
sleep 1.2
/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1

echo "== throughput ($size)"
awk '{printf "  %-8s %6.1f MB  %5.2f s  %6.1f MB/s\n", $1, $2/1e6, $3, $2/1e6/$3}' /tmp/termbench-hcbench.txt
echo "== perf log (busy seconds)"
tr -d '\r' < "$tmpl/hardcore-perf.txt" | awk '$3+0 > 1 || $5+0 > 100' | sed 's/^/  /'
echo "== latency, ms (n=$(echo "$lat" | grep -c .)): p50 / p90 / p99"
echo "$lat" | python3 -c '
import sys
rows=[list(map(float,l.split()[:3])) for l in sys.stdin if l.strip()]
names=["input -> echo back","input -> frame start","input -> presented"]
for k,n in enumerate(names):
    v=sorted(r[k] for r in rows)
    if not v: continue
    q=lambda p: v[min(len(v)-1,int(p*len(v)))]
    print("  %-22s %5.2f / %5.2f / %5.2f" % (n,q(.5),q(.9),q(.99)))'
