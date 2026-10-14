# hardcore vs Windows Terminal

Measured 2026-10-13 on one machine:
- **System:** Windows 11 build 26200, AMD Radeon 780M, 1920×1080 at 60 Hz, 96 DPI.
- **WSL:** Debian on WSL2, bash, with the VM already running.
- **Windows Terminal:** 1.24.12741.0, its Debian profile, a separate window titled `BENCHWT`.
- **hardcore:** built from this repo, with the bundled ConPTY (OpenConsole 1.25.260930003).

Both terminals were measured from outside, with the same tool and the same method.
Rerun everything with the commands at the bottom before you change anything that could
affect speed.

## Speed

| | hardcore | Windows Terminal |
|---|---|---|
| **Keypress → frame on screen**, p50 / p90 / p99 (n=300) | **7.9 / 17.5 / 21.8 ms** | 16.1 / 21.7 / 24.5 ms |
| **New tab → tab on screen**, p50 (n=10) | **6 ms** | 63 ms |
| **New tab → prompt drawn**, p50 (n=10) | **6 ms** | 127 ms |
| **Cold launch → window fully on screen**, median of 5 | **~60 ms** | ~360 ms |
| **Cold launch → prompt**, median of 5 | **~130 ms** | ~360 ms |
| `cat` 15 MB of `seq` (2 M short lines), median of 3 | 0.90 s | **0.50 s** |
| `cat` 32 MB of SGR + truecolor lines, median of 3 | 0.75 s | 0.70 s |
| `cat` 19 MB of box drawing + CJK, median of 3 | 0.43 s | 0.42 s |

After the move to the left tab column and Direct3D 11 (`--top 4 --left 164`):
- keypress → frame: 7.9 / 15.7 / 21.7 ms;
- new tab → prompt p50: 12.7 ms. That's about one 60 Hz frame later than the GL
  build's 6 ms, and still about 10× ahead of Windows Terminal.

How each row was measured:
- **Keypress → frame.** `bench/echoprobe.py` runs inside the terminal with the cursor hidden,
  and each key toggles a block of cells. `termbench` sends the key with `SendInput`. It takes
  the time when the change appeared from the present timestamp of the first frame that shows
  it, read through DXGI Desktop Duplication. On a 60 Hz display most of what's left is
  waiting for the compositor's next vsync.
- **New tab.** The old tab's prompt is first pushed down to row 12. Ctrl+Shift+T is then
  timed to two frames: the first without that prompt, and the first with a prompt on row 0.
  hardcore keeps one pre-warmed shell waiting (wsl.exe, bash and your profile already
  loaded), so a new tab only needs a `cd`. That's why both of its numbers land in the same
  frame.
- **Cold launch.** `termbench launch` starts the program and waits until the window's
  pixels are actually on screen (its #0C0C0C background shows at three sample points), then
  until a prompt is drawn. The Windows Terminal number is for a new window of WT, which was
  already running. hardcore turns off the Windows 11 window-open animation, which costs WT
  roughly 170 ms.
- **Throughput.** `bash bench/throughput.sh NAME` runs inside the terminal and times
  `cat FILE > /dev/tty`.

The plain-`seq` gap isn't in hardcore's own code. With a build that read ConPTY's output and
threw it away unparsed, the run still took 0.84 s. Swapping in WT's own ConPTY build
(1.24.261001001) didn't change it either. What's left is how Windows Terminal hosts
OpenConsole: it launches it with its own flags and uses overlapped named pipes, instead of
going through the public `conpty.dll` entry point. That's where to look next.

### Where a keypress's time goes

`HARDCORE_LAT=1` makes hardcore log, for every key, when the shell's echo came
back and when the frame was handed to the compositor. On a flip-model swap
chain, DXGI also reports the vblank the frame was scanned out on (300 keys,
p50 / p90 / p99):

| | ms |
|---|---|
| key → echo back from bash over WSL + ConPTY | 0.97 / 1.14 / 1.34 |
| key → frame handed to the compositor | 1.4–1.8 / 2.1 / 2.7 |
| key → DWM's composed frame (desktop duplication, the table above) | 7.6 / 17 / 21 |
| key → scanned out on the display (DXGI `SyncQPCTime`) | 16–19 / 30 / 33 |

Everything hardcore itself does fits in about 2 ms. The rest is the
compositor and the 60 Hz display.

The renderer moved from OpenGL to a Direct3D 11 flip-model swap chain with
tearing allowed, the presentation path that lets Windows skip composition
(independent flip or an overlay plane). On this machine Windows still composed
every windowed frame (`CompositionMode` 0, with or without a screen capture
running), so the change measured as a wash: GL 7.3 / 16.4 / 20.0 ms, D3D11
7.4 / 15.6 / 20.2 ms by desktop duplication. Checking whether another GPU or
driver promotes it needs PresentMon run as administrator.

### What changed during the session

| Change | Effect |
|---|---|
| Bundled OpenConsole in place of the inbox ConPTY (`conhost.exe` 10.0.26100) | `seq` cat: 9.66 s → 0.83 s. Color: 3.68 → 0.63 s. Unicode: 1.26 → 0.39 s. New-tab prompt: 96 → 73 ms |
| Row-pointer screen buffer + ASCII fast path | Parser alone, 15 MB of `seq` via `--replay`: 2.26 s → 0.47 s |
| Pre-warmed spare shell | New-tab prompt: 73 ms → 6 ms |
| Window shown before GL init, GL driver loaded on a worker thread, open animation off | Launch → window on screen: ~195 → ~60 ms. Launch → prompt: ~155 → ~130 ms |

### Parser and renderer tuning (second pass, same day)

Measured with tools that never take the keyboard or focus. They launch a private
copy of hardcore behind your windows and drive it only through the WM_COPYDATA test
channel:
- `bench/run-bg.sh`: throughput and input→frame latency.
- `bench/render-bench.sh`: full-screen colored redraws.
- `bench/parse-bench.sh`: the parser alone, on captured ConPTY streams.
- `bench/cpu-flood.sh`: CPU seconds per process.
- `bench/snapdiff.sh`: checks two builds draw identical pixels.

`HARDCORE_PERF=1` makes hardcore log once a second what its UI thread did: bytes
parsed, reads, frames, and the milliseconds spent parsing, building frames and
presenting.

| | before | after |
|---|---|---|
| Parser alone, `seq` stream (2 M lines, 178×55) | 31.5 MB/s | **189 MB/s** |
| Parser alone, SGR + truecolor stream | 172 MB/s | **249 MB/s** |
| Parser alone, box drawing + CJK stream | 101 MB/s | 107 MB/s |
| Full-screen colored redraw, 216×66: CPU to build a frame | 0.52 ms | **0.29 ms** |
| Same: upload + `Present` | 0.39 ms | 0.34 ms |
| Same: frames per second, round trip included | 447 | 471 |
| Frames drawn per second during a `cat` flood | 35–59 | **69–152** |
| hardcore CPU during 3× `cat` of the `seq` file | 2.70 s | **2.30 s** |
| Input → frame handed to the compositor, p50 / p99 (n=121) | 1.33 / 2.28 ms | 1.26 / 1.97 ms |
| `cat` of the `seq` file, wall time | 0.85 s | 0.91 s |

What changed:
- **Per-row extent (`rlen`).** Each screen row records where its default-blank tail
  starts. A line feed used to scan all 178 cells of the row going into scrollback
  and rewrite all 178 cells of the row coming back. That's about 5.6 KB of memory
  traffic for a 7-byte line. Now it touches only the cells in use. Scrollback line
  buffers are also recycled once the history is full, instead of being freed and
  allocated again for every line.
- **Instanced quads.** Every rectangle, glyph or fill is one 20-byte instance: a
  pixel rectangle, an atlas rectangle and a color. The vertex shader expands it, so
  it no longer takes six 20-byte vertices. One `DrawInstanced` draws the frame.
  Positions snap to whole pixels, as before.
- **One pass per row.** Backgrounds, glyphs and underlines are drawn row by row
  instead of in three full-screen passes. Selection is worked out once per row, and
  drawing stops at the row's extent. `bench/snapdiff.sh` confirms the frames are
  pixel-identical to the old renderer, with and without a selection.
- **Frame pacing under floods.** A frame is forced at least every 16 ms by the
  high-resolution timer. Before, the limit was "more than 30 ms" by `GetTickCount`,
  which only moves every 15.6 ms, so frames came 31–47 ms apart.

Why plain `seq` didn't get faster end to end:
- The parser now needs about 0.1 s for the whole stream.
- The window size (700×400 to 2400×1300), ConPTY's flags (inherit cursor, resize
  quirk, win32 input mode) and the output pipe (anonymous at 1 MB, or a named pipe
  at 64 KB or 1 MB) all gave the same 0.87–0.96 s.
- During a flood, the WSL VM ↔ Windows traffic slows every Windows thread. The same
  offline parse runs 3.5× slower while another window is flooding. Keeping WSL busy
  with 8 plain CPU loops slows it only 9%.
- The wall time went up about 6% (0.85 → 0.91 s, repeated 5 times). A build with
  only the parser change already shows it. The likely cause: the UI thread now idles
  between reads, so each 9 KB read costs a wake-up that it used to absorb while
  busy. Pausing the reader after large reads made it much worse (4 s), because
  Windows rounds `Sleep(1)` up to its 15.6 ms timer tick.

## Conformance

| Suite | hardcore | Windows Terminal |
|---|---|---|
| [xterm.js escape-sequence fixtures](../tests/xtermjs): expected screens captured from real xterm at 80×25 | **71 / 71** (+2 of the 5 xterm.js skips) | — (parser only, so not runnable against WT) |
| [esctest2](https://github.com/ThomasDickey/esctest2), `--expected-terminal=xterm --max-vt-level=4`, run inside the terminal | **273 passed**, 251 failed, 43 known xterm bugs | 90 passed, 461 failed, 16 known |

The xterm.js fixtures started at 52 of 71. The fixes were:
- backspace at a pending wrap;
- ESC intermediates and CAN/SUB;
- REP;
- DECRC and which CSIs cancel a pending wrap;
- real tab stops (HTS/TBC/CHT/CBT);
- DECSTBM quirks;
- CNL/CPL margins and VPR;
- SL/SR, DECIC/DECDC and DECALN.

Most of hardcore's remaining esctest failures are:
- **Left/right margins** (DECLRMM/DECSLRM, VT420). Many tests that aren't named for margins
  still set them up.
- **VT420 rectangle operations**: DECCRA, DECFRA, DECERA, DECSERA.
- **Selective erase**: DECSCA, DECSED, DECSEL.
- **Color-palette OSCs**: 4, 104, 10–19 set/reset.
- **Window ops** (XTWINOPS beyond 18t).
- **Reverse wraparound** (mode 45).
- **A handful of DECRQM/DECRQSS reports.**

Windows Terminal fails most of the suite during setup, because it doesn't answer `CSI 18 t`
(report the text area size).

## Reproduce

```bash
make && make test                       # unit + xterm.js fixtures + ConPTY selftest
cp hardcore.exe build/hardcore-run.exe && (build/hardcore-run.exe --cd "$PWD" &)
cmd.exe /c "wt.exe -w bench new-tab -p {DEBIAN-PROFILE-GUID} --title BENCHWT --suppressApplicationTitle -d %CD%"

B=bench/termbench.exe; W="--class CASCADIA_HOSTING_WINDOW_CLASS --title BENCHWT"
$B type --class hardcore 'clear; python3 bench/echoprobe.py\n'; $B latency --class hardcore --n 300 --top 4; $B type --class hardcore q
$B type $W 'clear; python3 bench/echoprobe.py\n';               $B latency $W --n 300 --top 46;               $B type $W q
bench/run-newtab.sh 4 --class hardcore --left 164;  bench/run-newtab.sh 46 $W
bench/run-throughput.sh hardcore --class hardcore;  bench/run-throughput.sh wt $W
bench/run-esctest.sh hardcore --class hardcore;     bench/run-esctest.sh wt $W
$B launch --class hardcore --top 4 "$(wslpath -w build/hardcore-run.exe) --cd C:\\Users\\you"
```

Background runs, which leave your keyboard and focus alone:

```bash
bench/run-bg.sh hardcore.exe 1600,900     # throughput + latency + HARDCORE_PERF log
bench/render-bench.sh hardcore.exe        # full-screen colored redraws, 1920×1080
bench/cpu-flood.sh hardcore.exe           # CPU seconds per process during a seq flood
bench/plain3.sh hardcore.exe X=0          # seq wall time ×3; ENV=VAL pairs go to that copy
bench/parse-bench.sh hardcore.exe         # parser alone on build/vt-*.bin (HARDCORE_VTLOG captures)
bench/snapdiff.sh old.exe hardcore.exe    # same screen in both builds, compared pixel for pixel
```

`--top` is the distance in pixels from the top of the client area to the first text row:
4 for hardcore at 96 DPI (30 before the tabs moved to the left column), 46 for Windows Terminal with no info bar showing.
