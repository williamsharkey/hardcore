#!/usr/bin/env bash
# bench/run-newtab.sh TOP termbench-window-args...  (from WSL, outside the terminal)
# TOP = pixels from the client-area top to the first text row.
top=$1; shift
B="$(dirname "$0")/termbench.exe"
"$B" type "$@" 'clear; printf "\n%.0s" {1..12}\n' || exit 1
sleep 0.8
"$B" newtab "$@" --n 10 --top "$top"
