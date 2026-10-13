# wsl-easy vs Windows Terminal

Measured 2026-10-13 on one machine:
- **System:** Windows 11 build 26200, AMD Radeon 780M, 1920×1080 at 60 Hz, 96 DPI.
- **WSL:** Debian on WSL2, bash, with the VM already running.
- **Windows Terminal:** 1.24.12741.0, its Debian profile, a separate window titled `BENCHWT`.
- **wsl-easy:** built from this repo, with the bundled ConPTY (OpenConsole 1.25.260930003).

Both terminals were measured from outside, with the same tool and the same method.
Rerun everything with the commands at the bottom before you change anything that could
affect speed.

## Speed

| | wsl-easy | Windows Terminal |
|---|---|---|
| **Keypress → frame on screen**, p50 / p90 / p99 (n=300) | **7.9 / 17.5 / 21.8 ms** | 16.1 / 21.7 / 24.5 ms |
| **New tab → tab on screen**, p50 (n=10) | **6 ms** | 63 ms |
| **New tab → prompt drawn**, p50 (n=10) | **6 ms** | 127 ms |
| **Cold launch → window fully on screen**, median of 5 | **~60 ms** | ~360 ms |
| **Cold launch → prompt**, median of 5 | **~130 ms** | ~360 ms |
| `cat` 15 MB of `seq` (2 M short lines), median of 3 | 0.90 s | **0.50 s** |
| `cat` 32 MB of SGR + truecolor lines, median of 3 | 0.75 s | 0.70 s |
| `cat` 19 MB of box drawing + CJK, median of 3 | 0.43 s | 0.42 s |

How each row was measured:
- **Keypress → frame.** `bench/echoprobe.py` runs inside the terminal with the cursor hidden,
  and each key toggles a block of cells. `termbench` sends the key with `SendInput`. It takes
  the time when the change appeared from the present timestamp of the first frame that shows
  it, read through DXGI Desktop Duplication. On a 60 Hz display most of what's left is
  waiting for the compositor's next vsync.
- **New tab.** The old tab's prompt is first pushed down to row 12. Ctrl+Shift+T is then
  timed to two frames: the first without that prompt, and the first with a prompt on row 0.
  wsl-easy keeps one pre-warmed shell waiting (wsl.exe, bash and your profile already
  loaded), so a new tab only needs a `cd`. That's why both of its numbers land in the same
  frame.
- **Cold launch.** `termbench launch` starts the program and waits until the window's
  pixels are actually on screen (its #0C0C0C background shows at three sample points), then
  until a prompt is drawn. The Windows Terminal number is for a new window of WT, which was
  already running. wsl-easy turns off the Windows 11 window-open animation, which costs WT
  roughly 170 ms.
- **Throughput.** `bash bench/throughput.sh NAME` runs inside the terminal and times
  `cat FILE > /dev/tty`.

The plain-`seq` gap isn't in wsl-easy's own code. With a build that read ConPTY's output and
threw it away unparsed, the run still took 0.84 s. Swapping in WT's own ConPTY build
(1.24.261001001) didn't change it either. What's left is how Windows Terminal hosts
OpenConsole: it launches it with its own flags and uses overlapped named pipes, instead of
going through the public `conpty.dll` entry point. That's where to look next.

### What changed during the session

| Change | Effect |
|---|---|
| Bundled OpenConsole in place of the inbox ConPTY (`conhost.exe` 10.0.26100) | `seq` cat: 9.66 s → 0.83 s. Color: 3.68 → 0.63 s. Unicode: 1.26 → 0.39 s. New-tab prompt: 96 → 73 ms |
| Row-pointer screen buffer + ASCII fast path | Parser alone, 15 MB of `seq` via `--replay`: 2.26 s → 0.47 s |
| Pre-warmed spare shell | New-tab prompt: 73 ms → 6 ms |
| Window shown before GL init, GL driver loaded on a worker thread, open animation off | Launch → window on screen: ~195 → ~60 ms. Launch → prompt: ~155 → ~130 ms |

## Conformance

| Suite | wsl-easy | Windows Terminal |
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

Most of wsl-easy's remaining esctest failures are:
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
cp wsl-easy.exe build/wsl-easy-run.exe && (build/wsl-easy-run.exe --cd "$PWD" &)
cmd.exe /c "wt.exe -w bench new-tab -p {DEBIAN-PROFILE-GUID} --title BENCHWT --suppressApplicationTitle -d %CD%"

B=bench/termbench.exe; W="--class CASCADIA_HOSTING_WINDOW_CLASS --title BENCHWT"
$B type --class wsl-easy 'clear; python3 bench/echoprobe.py\n'; $B latency --class wsl-easy --n 300 --top 30; $B type --class wsl-easy q
$B type $W 'clear; python3 bench/echoprobe.py\n';               $B latency $W --n 300 --top 46;               $B type $W q
bench/run-newtab.sh 30 --class wsl-easy;  bench/run-newtab.sh 46 $W
bench/run-throughput.sh wsl-easy --class wsl-easy;  bench/run-throughput.sh wt $W
bench/run-esctest.sh wsl-easy --class wsl-easy;     bench/run-esctest.sh wt $W
$B launch --class wsl-easy --top 30 "$(wslpath -w build/wsl-easy-run.exe) --cd C:\\Users\\you"
```

`--top` is the distance in pixels from the top of the client area to the first text row:
30 for wsl-easy at 96 DPI, 46 for Windows Terminal with no info bar showing.
