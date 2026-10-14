#!/usr/bin/env bash
# bench/render-bench.sh EXE [WxH] — full-screen colored redraws (bench/fullscreen.py)
# in a background copy of hardcore; prints the frame rate and hardcore's own
# per-frame build / present times from HARDCORE_PERF.
cd "$(dirname "$0")/.."
exe=$1; size=${2:-1920,1080}
B=bench/termbench.exe; T="--class hardcore --title hcbench"
TL=/mnt/c/Users/willi/AppData/Local/Temp
mkdir -p build/hcbench; /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1; sleep 0.3
cp "$exe" build/hcbench/hcbench.exe; rm -f "$TL/hardcore-perf.txt" /tmp/fullscreen.txt
(cd /mnt/c && HARDCORE_PERF=1 WSLENV=HARDCORE_PERF:${WSLENV:-} setsid "$OLDPWD/build/hcbench/hcbench.exe" --no-activate \
  --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
for i in $(seq 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
$B resize "$size" $T; sleep 2.5
$B send "python3 $PWD/bench/fullscreen.py 600\n" $T
for i in $(seq 300); do [ -s /tmp/fullscreen.txt ] && break; sleep 0.2; done
sleep 1.1
/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
cat /tmp/fullscreen.txt
tr -d '\r' < "$TL/hardcore-perf.txt" | awk '$5+0 > 50 || $7+0 > 50' | sed 's/^/  /'
