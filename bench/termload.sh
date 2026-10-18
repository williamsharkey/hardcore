#!/bin/bash
# Runs inside the terminal under test, started by bench/compare-mac.sh:
#   termload.sh NAME OUTDIR
# Each phase writes a "mark" line; compare-mac.sh samples the terminal
# process's CPU time and memory at every mark, so the terminal's own cost of
# each phase is measured from outside without reading anything off the screen.
name=$1; out=${2:-/tmp/termcompare}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"; res=$out/$name.txt; : > "$res"
now() { python3 -c 'import time; print("%.3f" % time.time())'; }
mark() { echo "mark $1 $(now)" >> "$res"; }

printf '\033[8;36;120t'            # XTWINOPS resize, for the terminals without a size flag
sleep 2
echo "size $(stty size </dev/tty)" >> "$res"
[ -f /tmp/tb-plain.txt ] || seq 1 2000000 > /tmp/tb-plain.txt
[ -f /tmp/tb-color.txt ] || awk 'BEGIN{for(i=0;i<300000;i++) printf "\033[3%dm%08d\033[0m \033[1m%s\033[0m \033[38;2;%d;%d;%dmtruecolor segment\033[0m ✓ 日本\n", i%8, i, "INFO request handled in 12ms", i%256, (i*7)%256, (i*13)%256}' > /tmp/tb-color.txt
[ -f /tmp/tb-unicode.txt ] || python3 -c "
import random; random.seed(1)
ch='abcdefghijklmnopqrstuvwxyz ─│┌┐└┘├┤αβγ日本語한국어✓✗●○…→'
print('\n'.join(''.join(random.choice(ch) for _ in range(100)) for _ in range(100000)))" > /tmp/tb-unicode.txt
# a long Claude-style transcript, reprinted whole: clear screen and scrollback, then 4,000 lines
[ -f /tmp/tb-reprint.txt ] || python3 - <<'EOF'
import random
random.seed(7)
w = "the parser keeps a per row extent so a line feed only touches cells in use while scrollback holds logical lines".split()
out = []
for rep in range(10):
    out.append("\x1b[2J\x1b[3J\x1b[H")
    for i in range(4000):
        k = i % 9
        s = " ".join(random.choice(w) for _ in range(random.randrange(4, 30)))
        if k == 0: out.append("\x1b[1m⏺ \x1b[0m" + s + "\n")
        elif k == 1: out.append("  \x1b[2m⎿\x1b[0m  \x1b[38;2;120;200;120m+ " + s + "\x1b[0m\n")
        elif k == 2: out.append("  \x1b[38;2;220;100;100m- " + s + "\x1b[0m\n")
        elif k == 3: out.append("\x1b[48;2;40;40;40m  " + s[:100].ljust(100) + "\x1b[0m\n")
        else: out.append(s + "\n")
open("/tmp/tb-reprint.txt", "w").write("".join(out))
EOF
sleep 3
mark idle_start; sleep 10; mark idle_end
TIMEFORMAT=%R
for f in plain color unicode reprint; do
  mark "cat_${f}_start"
  for r in 1 2 3; do
    # until the terminal has processed it all: write it, then ask where the
    # cursor is (DSR); the answer comes only after everything before it.
    # (Timing cat alone measures how fast a terminal buffers, not parses.)
    t=$(python3 - /tmp/tb-$f.txt <<'EOF2'
import os, sys, termios, time, tty
fd = os.open('/dev/tty', os.O_RDWR)
old = termios.tcgetattr(fd)
tty.setcbreak(fd)                       # keeps output processing (LF -> CR LF) as cat has it
data = memoryview(open(sys.argv[1], 'rb').read())
t0 = time.time()
while data:
    n = os.write(fd, data[:65536]); data = data[n:]
os.write(fd, b'\x1b[6n')
reply = b''
while not reply.endswith(b'R'):
    reply += os.read(fd, 64)
print('%.3f' % (time.time() - t0))
termios.tcsetattr(fd, termios.TCSADRAIN, old)
EOF2
)
    echo "cat $f $r $t" >> "$res"
  done
  mark "cat_${f}_end"
done
printf '\033[2J\033[3J\033[H'
sleep 2
for style in fullscreen inline; do
  python3 "$here/ccsim.py" 1 $style 60 & p=$!
  sleep 2; mark "cc_${style}_start"; sleep 10; mark "cc_${style}_end"
  kill $p; wait $p 2>/dev/null
  printf '\033[?1049l\033[?25h\033[2J\033[H'; sleep 2
done
# streamed reply text: ~300 tokens/s of words, 15 s, like a fast model reply
mark stream_start
python3 - <<'EOF'
import random, sys, time
random.seed(3)
w = "the parser keeps a per row extent so a line feed only touches cells in use while scrollback holds logical lines".split()
end = time.time() + 15; col = 0
while time.time() < end:
    s = random.choice(w) + " "
    if col + len(s) > 110: s = "\r\n" + s; col = 0
    col += len(s.strip("\r\n"))
    sys.stdout.write(s); sys.stdout.flush(); time.sleep(1 / 300)
EOF
mark stream_end
echo done >> "$res"
sleep 1
