# hardcore on macOS vs Terminal.app

Measured 2026-10-13 on one machine:
- **Hardware:** iMac 24" (Apple M3), built-in Retina display at 60 Hz, macOS 15.6.1.
- **Shell:** zsh 5.9 with the owner's dotfiles (a fresh login zsh prints its first prompt in ~20 ms).
- **Terminal.app:** 2.14, the owner's default profile, a separate window titled `BENCHTERM`.
- **hardcore:** built from this repo. "GL" is the first port (NSOpenGLView). "Metal" is
  this version.

Everything is measured from outside with `bench/macbench`, the Mac counterpart of
`termbench`:
- **Input:** real key events through the window server (`CGEventPost` at the HID tap).
- **Output:** ScreenCaptureKit frames of the display, each stamped with the time the window
  server put it on screen.

The internal breakdowns come from hardcore's own logs (`HARDCORE_LAT=1`, `HARDCORE_TRACE=1`).

## After merging main (2026-10-14)

`main` brought:
- per-row extents (`rlen`) in the screen model;
- instanced quads in the renderer (ported to Metal: `hardcore.metal`);
- `--bench`, the parser on its own;
- unit tests for scrollback rows.

Measured against the build just before the merge, side by side:

| | before the merge | after |
|---|---|---|
| `cat` 18.8 MB `seq` / 31.7 MB color / 18.7 MB CJK (best of 3) | 0.52 / 0.36 / 0.29 s | **0.26** / 0.38 / 0.30 s |
| keypress → frame p50 / p90 (n=300) | 22.3 / 34.1 ms | 22.4 / 34.4 ms |
| new tab → prompt p50 | 18 ms | 17.1 ms |
| launch → prompt p50 (same app identity) | 150 ms | 145–151 ms |
| Cmd+Q → gone p50 | 31 ms | 32.7 ms |
| reflow per drag step, 16 full tabs | 0.83–0.86 ms | 0.84–0.88 ms |
| frame, pixel for pixel (a test screen with colours, wide chars, links, a selection) | — | **identical**: 0 of 2.2 million pixels differ |
| esctest2 | 273 passed / 251 failed / 43 known | 273 / 251 / 43 (the same) |

Plain output now goes twice as fast, from the per-row extents. Nothing else
moved. `--unit`, the xterm.js fixtures and the shell selftest all pass, and
the unit tests, replay and resize bench are clean under AddressSanitizer and
UBSan.

The numbers in the rest of this file are from before the merge.

## Speed

| | hardcore (Metal) | hardcore (GL, first port) | Terminal.app |
|---|---|---|---|
| **Keypress → frame on screen**, p50 / p90 / p99 (n=300) | **22.6 / 34.5 / 50.3 ms** | 27.2 / 36.8 / 38.8 ms | 38.8 / 61.0 / 77.9 ms |
| **New tab → prompt drawn**, p50 (n=10) | **18 ms**, tab and prompt in the same frame | 66 ms* | — (new tabs open as windows here) |
| **Cold launch → window on screen**, p50 (n=6) | **~115 ms** | ~151 ms | — (it hosts this session) |
| **Cold launch → prompt drawn**, p50 (n=6) | **~132 ms** | ~168 ms | — |
| **Close (Cmd+Q) → window gone, process exited**, p50 (n=6) | **31 ms** | 55 ms | — |
| `cat` 18.8 MB of `seq` (2 M short lines), best of 2–3 | **0.52 s** | 0.62 s | 1.07 s |
| `cat` 31.7 MB of SGR + truecolor lines | **0.38 s** | 0.45 s | 1.12 s |
| `cat` 18.7 MB of box drawing + CJK | **0.31 s** | 0.36 s | 0.60 s |

\* Measured with window capture, before macbench moved to display capture. The GL build's
display-capture new-tab run caught only 4 of 10 tabs.

hardcore on Windows measured 0.90 / 0.75 / 0.43 s on the same three `cat` files. On the
Mac, hardcore swallows output about twice as fast as Terminal.app.

### Where a keypress's time goes (Metal, `HARDCORE_LAT=1`, 300 keys, p50 / p90 / p99)

| | ms |
|---|---|
| key → `keyDown:` runs in hardcore (window server delivery) | 2.28 / 3.95 / 12.5 |
| key → written to the pty | 2.36 / 7.33 / 12.6 |
| key → the shell's echo arrives | 2.64 / 7.53 / 12.9 |
| key → frame built and committed | 2.92 / 8.95 / 19.5 |
| key → frame on the display (Metal `presentedTime`) | 21.7 / 39.2 / 71.1 |

Everything hardcore itself does fits in about 0.6 ms after the event arrives:
- write the key;
- parse the echo;
- build a full frame: 0.15 ms of CPU for a whole screen of coloured text, 0.4 ms at most;
- hand the frame to the render thread.

The other ~19 ms are the window server and the 60 Hz display. Moving the font into the
shader, or drawing the grid in the GPU, would trim a fraction of a millisecond. This is
measured, so don't spend effort there first.

## What changed, and what it did

| Change | Effect |
|---|---|
| OpenGL (NSOpenGLView) → Metal on a CAMetalLayer, two drawables, no display sync | keypress → frame p50 27.2 → 22.6 ms. Display sync on measured about 4 ms worse |
| Frames built on the main thread, drawn by a render thread | the main thread used to block in `nextDrawable` for up to 15 ms when a frame was already on its way. Now paints take at most 0.14 ms on the main thread |
| Repaints paced to the display refresh | the first Metal version painted after every chunk of a flood, which cut `cat` to a third: 75% of the main thread drew frames nobody could see (sampled). Plain/color/unicode went 0.97 / 1.83 / 1.26 → 0.52 / 0.38 / 0.31 s |
| Keystrokes written straight into a non-blocking pty, queue only when it's full | removed a background-queue hop, about 3 ms at p90 |
| Plain typing skips the text-input system on a plain keyboard layout | keyDown → written p99 5 ms → 0.3 ms. Dead keys and input methods still go through it |
| Pre-warmed spare shell (zsh `precmd` hook / bash `read`), and the tab's first frame held until its prompt is printed | new tab → prompt 66 → 18 ms. The shell answers in 0–6 ms (your prompt hooks run first); the hold puts tab and prompt in one frame instead of an empty tab a refresh earlier |
| Shader compiled at build time (`hardcore.metal` → metallib, embedded) | Metal ready ~175 ms after exec on a cold first run → under 1 ms of shader work |
| First shell forked before AppKit loads; its prompt read straight from the pty; window flushed onto the screen before `finishLaunching` | launch → window ~151 → ~115 ms; launch → prompt ~168 → ~132 ms. AppKit alone costs ~70 ms (38 to start, 32 to finish launching) |
| Cmd+Q handled before the menu; exit without AppKit's teardown | close 55 → 31 ms. Inside hardcore it's 4 ms from the key to `_exit` |

## Resizing with full scrollback

`hardcore --bench-resize OUT` fills terminals with 20,000 lines of scrollback each, plus a
screen, of transcript-like text: mixed lengths, colour, CJK, and one line in seven long
enough to wrap. It then times the reflow through the sizes a drag passes through.
`macbench drag` does the same in the real app: it grabs the window's corner with real mouse
events, and hardcore logs each step's main-thread time with `HARDCORE_LAT=1`.
`HARDCORE_REFLOW_ALL=1` brings back the old behaviour for comparison.

| | before | after |
|---|---|---|
| reflow, 1 tab, per drag step (bench, p50) | 2.2 ms | **0.05 ms** |
| reflow, 16 tabs, per drag step (bench, p50 / max) | ~35 ms (8 tabs measured 16 / 34 ms) | **0.9 / 2.3 ms** |
| live drag, 16 tabs, reflow per step (app, p50 / p90 / max) | 49.5 / 62.9 / 123 ms | **2.5 / 3.2 / 8.0 ms** |
| live drag, 16 tabs: drag moves handled, steps longer than a 60 Hz frame | 46 of 120, 45 of 46 | **118–120 of 120, 0** |
| catching up the rest of the history after the drag | — | 3.3 ms per tab, one tab per tick, the visible one first |

How:
- **Tail-only reflow while resizing.** Each step rewraps only the newest 500 lines of
  scrollback (from a logical line's start) plus the screen, so a step costs the same with
  20,000 lines as with 200.
- **`term_settle` catches up the rest**, 250 ms after the last step. It runs one tab per
  16 ms tick, the visible tab first. Scrolling back, jumping between prompts, and switching to
  a tab all force it first. A height-only resize never needs it.
- **Each scrollback line remembers the width it wrapped at.** Rejoining a line therefore
  doesn't depend on the order the widths came in.

Checked by `--unit` and `--bench-resize`:
- **Tail reflow plus settle gives exactly what full reflow at every step gives**: every line,
  wrap and the cursor, on the same drag.
- **With full scrollback, every line both still have matches.** The old way also lost about
  5,400 of the oldest lines when narrowing pushed it past the 20,000-line limit; the new way
  keeps them.
- **Sixty resizes and back give exactly the text that was never resized.**

That last check found a bug that predates the port, on Windows too. When a wide character
(CJK, emoji) doesn't fit at the end of a row, it moves to the next row and leaves a blank.
Reflow joined that blank back in as a real space, so every resize added a space before such
characters. That blank is now marked (`W_PAD`) and never becomes text. With the old
behaviour put back, both tests fail.

## What Claude Code writes

Recorded from a real Claude Code 2.1.296 session in a pty, with no prompt sent:

| | writes | bytes |
|---|---|---|
| a keystroke in the input box | 1 | 47–57: cursor moves and the changed cells only |
| opening the `/` command menu | 4 (1,024 + 1,024 + 1,024 + 131) | 3,203, within ~2 ms |
| startup | 13–14 | ~2,000 |

- **Claude Code's renderer diffs.** It doesn't send the whole screen, or a character at a
  time.
- **It doesn't ask about synchronized output (mode 2026)**, so it never brackets its
  redraws.
- **A macOS pty hands out at most 1 KB per read.** Anything bigger than an echo therefore
  arrives in pieces.

To keep those pieces from being painted half-done, hardcore reads everything the pty has at
once. When a read comes back full (1 KB), it holds the paint up to 2 ms more for the rest,
never more than 8 ms in all. Replaying the recorded menu redraw 20 times took 48 frames
with the hold and 68 without; the 20 extra frames were half-drawn menus. A keystroke's
echo is never full, so it isn't held.

## Measuring on macOS: what fooled me

- **Capture a region of the display, not the window.** With `SCContentFilter`'s
  desktop-independent *window* filter, about half of hardcore's Metal frames never showed up
  in the capture, though they were on the real screen; full screenshots taken at the same
  moment showed them. Metal's `presentedTime` reported 0 for the same frames. The window
  server can composite a CAMetalLayer without going through the window's own image.
  Capturing the display (cropped to the window) sees every frame. It reads about 5–9 ms
  later than window capture for every terminal, so compare numbers only within one method.
  All the tables above use display capture.
- **One long-lived macbench process.** Starting a new process per keypress (`macbench key`
  in a loop) roughly doubled the measured present-to-display time. The new process
  connecting to the window server disturbs it. `macbench latency` and `newtab` post all
  their keys from one process.
- **Check every frame for both "old content gone" and "new content there".**
  ScreenCaptureKit only delivers a frame when something changed. A detector that waits for
  one condition, then starts looking for the next, misses a tab and its prompt arriving in
  the same frame, and adds a whole refresh.
- **Painting from a run loop observer (just before the thread sleeps)** looked like the
  natural way to coalesce. It measured worse: frames reached the screen late and the capture
  missed them. A coalesced `dispatch_async`, paced to the refresh rate, didn't.
- **Metal's presented handler is too late to pace with.** It arrives ~4 ms after the frame
  was shown, up to 13 ms, and not at all for frames that were replaced. Waiting on it before
  drawing the next frame lost redraws outright.

## Conformance

| Suite | hardcore (macOS) | hardcore (Windows) | Terminal.app |
|---|---|---|---|
| [xterm.js escape-sequence fixtures](../tests/xtermjs) | **71 / 71** | 71 / 71 | — (parser only) |
| [esctest2](https://github.com/ThomasDickey/esctest2), `--expected-terminal=xterm --max-vt-level=4`, run inside the terminal | **273 passed**, 251 failed, 43 known xterm bugs | 273 passed, 251 failed, 43 known | 60 passed, 478 failed, 29 known (298 of its failures are timeouts: it never answers the query) |

macOS and Windows match test for test. The parser, screen and replies are the same code,
and the replies make it back through a macOS pty just as they do through ConPTY. The
failures are the ones listed in [RESULTS.md](RESULTS.md):
- left/right margins;
- VT420 rectangle operations;
- selective erase;
- palette OSCs;
- XTWINOPS;
- reverse wraparound.

## Reproduce

```bash
make && make test                                       # unit + xterm.js fixtures + forkpty selftest
clang -fobjc-arc -O2 -o bench/macbench bench/macbench.m \
  -framework Cocoa -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo
build/hardcore ~/dev/hardcore &                         # (or HARDCORE_LAT=1 / HARDCORE_TRACE=1 ... 2>log)
osascript -e 'tell application "Terminal" to do script "printf \"\\033]0;BENCHTERM\\007\""'

B=bench/macbench; T="--app Terminal --title BENCHTERM"
$B type --app hardcore 'clear; python3 bench/echoprobe.py\n'; $B latency --app hardcore --n 300; $B type --app hardcore q
$B type $T 'clear; python3 bench/echoprobe.py\n';             $B latency $T --n 300;             $B type $T q
$B type --app hardcore 'clear; printf "\n%.0s" {1..12}\n'
$B newtab --app hardcore --n 10 --left 164 --top 4 --row 16 --bg 0C0C0C
$B latency --app hardcore --n 60 cmd+1,cmd+2            # tab switches, after opening a second tab
bench/run-throughput-mac.sh hardcore --app hardcore;     bench/run-throughput-mac.sh terminal $T
bench/run-esctest-mac.sh hardcore --app hardcore;        bench/run-esctest-mac.sh terminal $T
$B key --app hardcore cmd+q
$B launch --app hardcore --n 6 --left 164 --top 4 --row 16 --bg 0C0C0C build/hardcore ~/dev/hardcore
$B quit --app hardcore --n 6 --keys cmd+q build/hardcore ~/dev/hardcore
build/hardcore --bench-resize build/resize.txt          # reflow, 1 and 16 full tabs, plus the correctness checks
$B drag --app hardcore --n 60 -400,-200                 # a live corner drag (16 tabs: cat a 21,000-line file in each first)
python3 bench/latstats.py LOG                           # the HARDCORE_LAT breakdown
```

`--left` / `--top` are points from the window's top-left corner to the first text cell
(164 / 4 for hardcore: the 160-point tab column plus padding). `--row` is the height of a
text row in points.

macbench needs the Accessibility and Screen Recording permissions, for whatever app it's
run from. It stops rather than type when the target window isn't in front.
