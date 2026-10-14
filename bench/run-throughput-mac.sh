#!/usr/bin/env bash
# bench/run-throughput-mac.sh NAME macbench-window-args...  (macOS, outside the terminal)
# The Mac counterpart of run-throughput.sh: types bench/throughput.sh into the
# terminal and waits for its three timings.
name=$1; shift
rm -f /tmp/termbench-$name.txt
"$(dirname "$0")/macbench" type "$@" "cd $(cd "$(dirname "$0")/.." && pwd) && bash bench/throughput.sh $name\n" || exit 1
for i in $(seq 1 480); do [ "$(wc -l 2>/dev/null < /tmp/termbench-$name.txt | tr -d " ")" = 3 ] && break; sleep 0.25; done
sed "s/^/$name /" /tmp/termbench-$name.txt
