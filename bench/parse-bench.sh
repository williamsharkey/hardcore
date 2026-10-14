#!/usr/bin/env bash
# bench/parse-bench.sh [EXE] — parser alone on the captured streams in build/vt-*.bin
# (capture them with HARDCORE_VTLOG=1 while cat'ing bench/throughput.sh's files).
cd "$(dirname "$0")/.."
exe=$(realpath "${1:-hardcore.exe}")
for f in plain color unicode; do
  [ -f build/vt-$f.bin ] || continue
  "$exe" --bench "$(wslpath -w build/bench-out.txt)" 178 55 "$(wslpath -w build/vt-$f.bin)"
  printf "%-8s %s\n" $f "$(tr -d "\r" < build/bench-out.txt | tail -1)"
done
