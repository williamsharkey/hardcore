#!/usr/bin/env bash
# bench/plain3.sh EXE [ENV=VAL ...] — `time cat` of the plain seq file, 3 runs, in a
# fresh background copy of hardcore driven over the test channel. Prints seconds.
cd "$(dirname "$0")/.."
exe=$1; shift
B=bench/termbench.exe; T="--class hardcore --title hcbench"
mkdir -p build/hcbench; /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1; sleep 0.3
cp "$exe" build/hcbench/hcbench.exe
names=$(for kv in "$@"; do printf '%s:' "${kv%%=*}"; done)
(cd /mnt/c && env "$@" WSLENV="${names}${WSLENV:-}" setsid "$OLDPWD/build/hcbench/hcbench.exe" --no-activate \
  --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
for i in $(seq 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
$B resize ${SIZE:-1600,900} $T; sleep 2.5
rm -f /tmp/plain3.txt
$B send 'TIMEFORMAT=%R; for i in 1 2 3; do { time cat /tmp/tb-plain.txt; } 2>>/tmp/plain3.txt; done\n' $T
for i in $(seq 200); do [ "$(wc -l < /tmp/plain3.txt 2>/dev/null)" = 3 ] && break; sleep 0.2; done
/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
echo "$* : $(tr '\n' ' ' < /tmp/plain3.txt)"
