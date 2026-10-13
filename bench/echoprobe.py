#!/usr/bin/env python3
"""Runs inside the terminal under test. Hides the cursor (so a blinking cursor
can't fake a pixel change), then on every key toggles a solid block at a fixed
spot. termbench watches for that block to appear or disappear."""
import os, sys, termios, tty
fd = sys.stdin.fileno()
old = termios.tcgetattr(fd)
tty.setraw(fd)
out = sys.stdout
out.write("\x1b[?25l\x1b[2J\x1b[H")
out.flush()
on = False
try:
    while True:
        ch = os.read(fd, 64)
        if b"q" in ch:
            break
        on = not on
        block = ("\x1b[7m" + " " * 16 + "\x1b[0m") if on else " " * 16
        out.write("".join("\x1b[%d;3H%s" % (r, block) for r in range(3, 7)))
        out.flush()
finally:
    out.write("\x1b[?25h\x1b[2J\x1b[H")
    out.flush()
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
