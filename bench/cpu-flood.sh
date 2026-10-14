#!/usr/bin/env bash
# bench/cpu-flood.sh EXE [FILE] — CPU seconds hardcore, OpenConsole and wsl.exe
# spend while `cat FILE` (default: the plain seq file) runs 3 times in a
# background copy of hardcore, plus the wall time.
cd "$(dirname "$0")/.."
exe=$1; file=${2:-/tmp/tb-plain.txt}
B=bench/termbench.exe; T="--class hardcore --title hcbench"
PS=/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe
cpu() { (cd /mnt/c && $PS -NoProfile -Command "Get-Process hcbench,OpenConsole,wsl -ErrorAction SilentlyContinue | ForEach-Object { '{0} {1} {2:N3}' -f \$_.Name,\$_.Id,\$_.TotalProcessorTime.TotalSeconds }") | tr -d '\r'; }
mkdir -p build/hcbench; /mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1; sleep 0.3
cp "$exe" build/hcbench/hcbench.exe
(cd /mnt/c && setsid "$OLDPWD/build/hcbench/hcbench.exe" --no-activate --cd "$(wslpath -w "$OLDPWD/build/hcbench")" </dev/null >/dev/null 2>&1 &)
for i in $(seq 100); do $B cmd "tab 0" $T >/dev/null 2>&1 && break; sleep 0.1; done
$B resize 1600,900 $T; sleep 2.5
cpu > /tmp/cf0.txt; rm -f /tmp/cf.txt
$B send "TIMEFORMAT=%R; { time (cat $file; cat $file; cat $file); } 2> /tmp/cf.txt\n" $T
for i in $(seq 300); do [ -s /tmp/cf.txt ] && break; sleep 0.2; done
cpu > /tmp/cf1.txt
/mnt/c/Windows/System32/taskkill.exe /F /IM hcbench.exe >/dev/null 2>&1
join <(awk '{print $1"/"$2, $3}' /tmp/cf0.txt | sort) <(awk '{print $1"/"$2, $3}' /tmp/cf1.txt | sort) |
  awk -v wall="$(cat /tmp/cf.txt)" '{split($1,a,"/"); d[a[1]]+=$3-$2} END {printf "wall %.2f s   hardcore %.2f   OpenConsole %.2f   wsl %.2f  (CPU s)\n", wall, d["hcbench"], d["OpenConsole"], d["wsl"]}'
