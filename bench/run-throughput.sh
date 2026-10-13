#!/usr/bin/env bash
# bench/run-throughput.sh NAME termbench-window-args...  (from WSL, outside the terminal)
name=$1; shift
rm -f /tmp/termbench-$name.txt
"$(dirname "$0")/termbench.exe" type "$@" "bash bench/throughput.sh $name\n" || exit 1
for i in $(seq 1 480); do [ "$(wc -l < /tmp/termbench-$name.txt 2>/dev/null)" = 3 ] && break; sleep 0.25; done
sed "s/^/$name /" /tmp/termbench-$name.txt
