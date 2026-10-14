#!/usr/bin/env bash
# bench/resize-bg.sh EXE [TABS] — drag-resize cost with deep scrollback. Opens TABS
# tabs (default 1) in a background copy of hardcore, fills each with ~25,000
# lines of mixed length (many wider than the window), then drags the window
# 1600x900 -> 1000x600 and back in 40+40 steps. Each step is timed from outside:
# SetWindowPos on another process's window returns once WM_SIZE is handled,
# which includes reflowing and drawing the new frame.
cd "$(dirname "$0")/.."
exe=$1; tabs=${2:-1}
B=bench/termbench.exe; T="--class hardcore --title hcbench"
[ -f /tmp/tb-long.txt ] || python3 -c "
import random; random.seed(2)
for i in range(25000): print('\x1b[3%dm%06d\x1b[0m ' % (i % 8, i) + ''.join(random.choice('abcdefghij klmnop') for _ in range(random.randrange(400))))" > /tmp/tb-long.txt
mkdir -p build/hcbench; /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1; sleep 0.3
cp "$exe" build/hcbench/hcbench.exe
TL=/mnt/c/Users/willi/AppData/Local/Temp; rm -f "$TL/hardcore-perf.txt"
(cd /mnt/c && HARDCORE_PERF=1 WSLENV=HARDCORE_PERF:${WSLENV:-} setsid "$OLDPWD/build/hcbench/hcbench.exe" --no-activate --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
for i in $(seq 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
$B resize 1600,900 $T; sleep 3
for i in $(seq 2 $tabs); do $B cmd newtab $T; sleep 0.4; done
sleep 1
for i in $(seq 0 $((tabs - 1))); do $B cmd "tab $i" $T; $B send 'cat /tmp/tb-long.txt\n' $T; sleep 1.5; done
$B cmd "tab 0" $T; sleep 1
$B resizes 1600,900,1000,600 --n 40 $T
sleep 1
/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
tr -d '\r' < "$TL/hardcore-perf.txt" 2>/dev/null | grep '^resize' | awk '{b+=$4; t+=$7; f+=$10; n++; if ($10>mf) mf=$10} END {if (n) printf "inside hardcore, mean per step (n=%d): buffers %.2f  terminal %.2f  frame %.2f ms (frame max %.2f)\n", n, b/n, t/n, f/n, mf}'
