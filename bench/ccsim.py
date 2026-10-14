#!/usr/bin/env python3
"""A busy, simulated Claude Code session, for the tab gauntlet (bench/gauntlet.sh).

Runs until killed. About 60 times a second it does what Claude Code's renderer
does: one synchronized-output frame (CSI ? 2026 h ... l) that rewrites only
the rows that changed. On top of that:
  - streams a reply a few words at a time, and every few seconds dumps a block
    of "tool output" that changes most of the screen at once;
  - spins a braille throbber in the window title (OSC 0);
  - cycles OSC 7501 states: working -> blocked (a permission question) ->
    working -> done -> idle -> working ...;
  - "fullscreen" style draws on the alternate screen like Claude Code 2.1;
    "inline" style draws on the main screen and scrolls finished lines up into
    scrollback, redrawing only its live region at the bottom (cursor up, erase,
    rewrite), like the classic Ink renderer.

  ccsim.py [SEED] [STYLE] [FPS]     STYLE: fullscreen | inline | mixed (default)
"""
import base64, os, random, shutil, signal, sys, time

seed = int(sys.argv[1]) if len(sys.argv) > 1 else os.getpid()
style = sys.argv[2] if len(sys.argv) > 2 else "mixed"
fps = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
rnd = random.Random(seed)
if style == "mixed":
    style = "fullscreen" if seed % 2 == 0 else "inline"
out = sys.stdout.buffer
WORDS = ("the parser now keeps a per row extent so a line feed only touches the cells in use "
         "which matters when output floods past faster than the screen can show it and "
         "every frame redraws from the grid while scrollback holds logical lines").split()
SPIN = "⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏"
STATES = [("working", 6.0), ("blocked", 2.5), ("working", 4.0), ("done", 1.5), ("idle", 1.0)]

def b64(s): return base64.b64encode(s.encode()).decode()
def osc7501(state, extra=""): return ("\x1b]7501;state=%s:app=claude-code%s\x1b\\" % (state, extra)).encode()

def size():
    c, r = shutil.get_terminal_size((100, 30))
    return max(c, 20), max(r, 8)

running = True
def stop(*_):
    global running
    running = False
signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

conv = [""]          # the conversation, as logical lines
prev = {}            # fullscreen: row -> what we last drew there
live_h = 0           # inline: height of the live region last drawn
t0 = time.monotonic()
state_i, state_t = 0, t0
next_dump = t0 + rnd.uniform(2, 6)
frame = 0

if style == "fullscreen":
    out.write(b"\x1b[?1049h\x1b[?25l\x1b[2J")
out.write(osc7501("working", ":msg=" + b64("Reading hardcore.c")))
out.flush()

def stream(now):
    global next_dump
    state = STATES[state_i][0]
    if state == "working":
        for _ in range(rnd.randrange(1, 4)):
            conv[-1] += (" " if conv[-1] else "") + rnd.choice(WORDS)
            if len(conv[-1]) > rnd.randrange(40, 160):
                conv.append("")
        if now >= next_dump:          # a tool result: a block of lines at once
            n = rnd.randrange(20, 80)
            conv.extend("  %4d │ %s" % (i, " ".join(rnd.choice(WORDS) for _ in range(rnd.randrange(3, 14))))
                        for i in range(n))
            conv.append("")
            next_dump = now + rnd.uniform(3, 8)
    del conv[:-2000]

def status_line(cols, now):
    state = STATES[state_i][0]
    spin = SPIN[int(now * 10) % len(SPIN)] if state == "working" else "·"
    text = {"working": "%s Working… (%ds · esc to interrupt)" % (spin, int(now - t0)),
            "blocked": "? Allow Bash: git push origin main   1. Yes  2. No",
            "done": "✓ Done", "idle": "  "}[state]
    return text[:cols]

def frame_fullscreen(cols, rows, now):
    body_h = rows - 4
    lines = []
    for l in conv:                     # wrap the conversation to the width
        while len(l) > cols:
            lines.append(l[:cols]); l = l[cols:]
        lines.append(l)
    view = lines[-body_h:]
    view = [""] * (body_h - len(view)) + view
    want = {i: v for i, v in enumerate(view)}
    want[body_h] = "─" * cols
    want[body_h + 1] = "❯ "
    want[body_h + 2] = "─" * cols
    want[body_h + 3] = status_line(cols, now)
    parts = [b"\x1b[?2026h\x1b[?25l"]
    for r, text in want.items():
        if prev.get(r) != text:
            color = b"\x1b[38;2;136;136;136m" if r in (body_h, body_h + 2) else b""
            parts.append(b"\x1b[%d;1H%s%s\x1b[0m\x1b[K" % (r + 1, color, text.encode()))
            prev[r] = text
    parts.append(b"\x1b[%d;3H\x1b[?25h\x1b[?2026l" % (body_h + 2))
    return b"".join(parts)

def frame_inline(cols, rows, now):
    global live_h
    parts = [b"\x1b[?2026h\x1b[?25l"]
    if live_h > 1:
        parts.append(b"\x1b[%dA" % (live_h - 1))
    parts.append(b"\r\x1b[J")
    while len(conv) > 1:               # finished lines go up into scrollback for good
        parts.append(conv.pop(0).encode()[:cols * 4] + b"\r\n")
    live = [conv[-1][-cols:], "─" * cols, "❯ ", status_line(cols, now)]
    parts.append("\r\n".join(live).encode())
    live_h = len(live)
    parts.append(b"\x1b[?25h\x1b[?2026l")
    return b"".join(parts)

try:
    while running:
        now = time.monotonic()
        cols, rows = size()
        if now - state_t > STATES[state_i][1]:
            state_i = (state_i + 1) % len(STATES)
            state_t = now
            st, _ = STATES[state_i]
            extra = {"working": ":msg=" + b64("Running make test"),
                     "blocked": ":kind=permission:msg=" + b64("approve Bash: git push"),
                     "done": ":msg=" + b64("Pushed 2 commits")}.get(st, "")
            out.write(osc7501(st, extra))
        if frame % 6 == 0:             # the title throbber, ~10 Hz
            out.write(("\x1b]0;%s Claude Code\x07" % SPIN[(frame // 6) % len(SPIN)]).encode())
        stream(now)
        out.write(frame_fullscreen(cols, rows, now) if style == "fullscreen" else frame_inline(cols, rows, now))
        out.flush()
        frame += 1
        time.sleep(max(0.0, 1.0 / fps - (time.monotonic() - now)))
finally:
    out.write(osc7501("clear"))
    if style == "fullscreen":
        out.write(b"\x1b[?25h\x1b[?1049l")
    out.flush()
