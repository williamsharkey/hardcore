#!/usr/bin/env bash
# bench/run-esctest-mac.sh NAME macbench-window-args...   (macOS, outside the terminal)
# The Mac counterpart of run-esctest.sh: types an esctest2 run into the
# terminal and prints its summary. esctest talks to the terminal through its
# own tty, so it must run inside it.
name=$1; shift
[ -d /tmp/esctest2 ] || git clone -q --depth 1 https://github.com/ThomasDickey/esctest2.git /tmp/esctest2
rm -f /tmp/esc-$name.done /tmp/esc-$name.log
"$(dirname "$0")/macbench" type "$@" "clear; python3 /tmp/esctest2/esctest/esctest.py --expected-terminal=xterm --max-vt-level=4 --logfile=/tmp/esc-$name.log --timeout 0.3 --no-print-logs; clear; echo done > /tmp/esc-$name.done\n" || exit 1
for i in $(seq 1 900); do [ -f /tmp/esc-$name.done ] && break; sleep 1; done
grep -E "^\*\*\* [0-9]+ tests passed" /tmp/esc-$name.log || tail -5 /tmp/esc-$name.log
