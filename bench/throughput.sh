#!/usr/bin/env bash
# Run inside the terminal under test: bash bench/throughput.sh NAME
# Times how long the terminal takes to swallow large outputs; results go to
# /tmp/termbench-NAME.txt so nothing has to be read back off the screen.
name=${1:-term}
out=/tmp/termbench-$name.txt
[ -f /tmp/tb-plain.txt ] || seq 1 2000000 > /tmp/tb-plain.txt
[ -f /tmp/tb-color.txt ] || awk 'BEGIN{for(i=0;i<300000;i++) printf "\033[3%dm%08d\033[0m \033[1m%s\033[0m \033[38;2;%d;%d;%dmtruecolor segment\033[0m ✓ 日本\n", i%8, i, "INFO request handled in 12ms", i%256, (i*7)%256, (i*13)%256}' > /tmp/tb-color.txt
[ -f /tmp/tb-unicode.txt ] || python3 -c "
import random; random.seed(1)
ch='abcdefghijklmnopqrstuvwxyz ─│┌┐└┘├┤αβγ日本語한국어✓✗●○…→'
print('\n'.join(''.join(random.choice(ch) for _ in range(100)) for _ in range(100000)))" > /tmp/tb-unicode.txt
TIMEFORMAT=%R
: > $out
for f in plain color unicode; do
  s=$(wc -c < /tmp/tb-$f.txt | tr -d " ")   # GNU and BSD alike
  t=$( { time cat /tmp/tb-$f.txt; } 2>&1 >/dev/tty )
  echo "$f $s $t" >> $out
done
clear
column -t $out
