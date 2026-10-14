#!/usr/bin/env python3
"""Run inside the terminal: redraws the whole screen N times with colored text
(every cell a glyph, a color change every few cells), like a busy TUI. Each frame
waits for a round trip (DSR) so the terminal draws each one instead of coalescing."""
import os, sys, shutil, termios, tty, time
n = int(sys.argv[1]) if len(sys.argv) > 1 else 300
cols, rows = shutil.get_terminal_size()
fd = sys.stdin.fileno(); old = termios.tcgetattr(fd); tty.setraw(fd)
out = sys.stdout.buffer
out.write(b"\x1b[?1049h\x1b[?25l")
t0 = time.perf_counter()
for f in range(n):
    parts = [b"\x1b[H"]
    for y in range(rows):
        line = []
        for x in range(0, cols, 6):
            c = (x * 7 + y * 3 + f) % 216 + 16
            line.append(b"\x1b[38;5;%dm%s" % (c, (b"abcdef0123456789"[(x + y + f) % 10:][:6]).ljust(6)[: cols - x]))
        parts.append(b"".join(line))
        if y < rows - 1: parts.append(b"\r\n")
    parts.append(b"\x1b[0m\x1b[6n")
    out.write(b"".join(parts)); out.flush()
    r = b""
    while not r.endswith(b"R"): r += os.read(fd, 32)
dt = time.perf_counter() - t0
out.write(b"\x1b[?25h\x1b[?1049l"); out.flush()
termios.tcsetattr(fd, termios.TCSADRAIN, old)
with open("/tmp/fullscreen.txt", "w") as fh: fh.write("%d frames %dx%d in %.2f s: %.0f fps\n" % (n, cols, rows, dt, n / dt))
