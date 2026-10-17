# hardcore on macOS vs Terminal.app and other terminals

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

## Against Ghostty, kitty, WezTerm, iTerm2 and Terminal.app (2026-10-17)

`bench/compare-mac.py` starts each terminal at 120×36 running `bench/termload.sh`, which times
its phases from inside, while the script samples the terminal process's CPU time and memory at
each phase boundary from outside. Nothing is typed and nothing is read off the screen, so it
needs neither the Accessibility nor the Screen Recording permission (macbench needs both).

A flood counts as done when the terminal answers a cursor-position query (DSR) sent after it. A
terminal answers only once it has processed everything before the query. Timing `cat` alone
measures how far ahead a terminal buffers, not how fast it keeps up.

Same machine as above. Ghostty 1.3.1, kitty 0.49.2, WezTerm 20240203, iTerm2 3.7.6, Terminal 2.15.
Homebrew no longer installs Alacritty: the cask fails Gatekeeper and was disabled on 2026-09-01.

| seconds, best of 3 (lower is better) | plain 18.8 MB | SGR + truecolor 31.7 MB | box drawing + CJK 18.7 MB | 10 × clear + 4,000-line transcript |
|---|---:|---:|---:|---:|
| **hardcore, after** | **0.119** | **0.163** | **0.097** | **0.020** |
| hardcore, before (aa28e5b) | 0.248 | 0.411 | 0.306 | 0.043 |
| Ghostty | 0.217 | 0.469 | 0.177 | 0.039 |
| kitty | 0.413 | 0.279 | 0.182 | 0.036 |
| WezTerm | 0.739 | 0.842 | 0.620 | 0.144 |
| iTerm2 (80×25: it ignores the resize request) | 0.225 | 3.906 | 7.023 | 0.144 |
| Terminal.app | 1.053 | 1.109 | 0.547 | 0.105 |

| CPU % of one core while... | idle | Claude Code–like redraw, alt screen (ccsim, 60 fps) | the same, inline | streamed reply, 300 words/s |
|---|---:|---:|---:|---:|
| hardcore | 0.1 | 10.4 | 10.1 | 13.5 |
| Ghostty | 0.8 | 9.9 | 11.8 | 14.8 |
| kitty | 0.4 | 11.4 | 11.6 | 9.2 |
| WezTerm | 0.4 | 16.9 | 17.1 | 15.0 |
| iTerm2 | 3.7 | 44.5 | 43.1 | 50.0 |
| Terminal.app | 1.1 | 14.2 | 14.3 | 13.0 |

Memory, idle → largest seen: hardcore 30 → 86 MB, Ghostty 79 → 109, kitty 90 → 93, WezTerm
41 → 132, iTerm2 125 → 302, Terminal.app 40 → 238.

At the redraw rates Claude Code produces, every terminal but iTerm2 costs about a tenth of a
core. There is little to win there. The floods are where they differ.

What moved hardcore's floods:

| Change | Effect |
|---|---|
| Pty output read on a queue of each tab's own (`PtyIn` in hardcore_mac.m), parsed on the main queue | The 1 KB reads macOS hands out overlap parsing and painting instead of taking turns with them. Plain 0.248 → 0.12 s; colour 0.41 → 0.16 s. The reader stops at 8 MB ahead of the parser. A synchronous `read_some` (reload, handoff, first prompt) holds it until that main-queue turn is over. |
| Character widths for the BMP in a 16 KB table, filled on first use | Two binary searches per non-ASCII character before. Parser alone on CJK: 161 → 118 ms. |
| A fast path for whole UTF-8 characters mid-row, like the ASCII one | CJK parser 118 → 85 ms (117 → 221 MB/s). It is checked against the byte-at-a-time path on 20,000 random inputs under AddressSanitizer. |
| Resize walks back at most 1,000 rows to a logical line's start | One 2.5 MB line with no newline cost 13–19 ms per resize step (every step rewrapped the whole line). Now 0.3 ms, plus one 24 ms settle after the drag. Alacritty #2567/#2807, WezTerm #714. |
| The window title is set only when it changed | Every output event of the active tab used to set it. |

Keypress latency was not re-measured: this session had no Screen Recording permission for macbench.

### Bugs from other terminals' trackers

Five agents read the Ghostty, Alacritty (and vte), kitty, WezTerm, iTerm2 and Claude Code issue
trackers for reports that apply to hardcore. Each confirmed one is a case in `unit_issues`
(`--unit`):

- The pending wrap was lost on resize. After a line exactly filled the width, a resize put
  the cursor on the last character, so the next one overwrote it (alacritty#3584, wezterm#6669,
  ghostty#12933).
- EL (`CSI K`, `CSI 2K`) left a row's soft-wrap flag set, so the next resize joined rows that
  Ink, Claude Code's renderer, had erased and rewritten (ghostty D#13616, alacritty#8930). Writing
  the last column now also ends the old wrap.
- The cursor saved with `ESC 7` on the main screen wasn't reflowed with its text (wezterm#6669).
- A stray DCS/APC/PM/SOS swallowed all later output until a BEL, for example after `cat` of a
  binary file. ESC now ends the string and starts a new sequence, and CAN/SUB abort strings and
  OSCs (alacritty#6714, wezterm#1844).
- Scrolled back with the 20,000-line ring full, streaming output moved the view (net 0 instead
  of +1 per line).
- Colon sub-parameters were read as `;`, so `4:3` meant underline + italic and `58:2::r:g:b`
  ended in a full reset (vte#22).
- Overlong UTF-8, surrogates and code points past U+10FFFF are U+FFFD now. DEL inside a CSI is
  ignored, and an 8-bit byte voids the CSI.
- Erase, delete and insert no longer leave half a wide character (alacritty#8786).
- A repeated `CSI ? 2026 h` restarted the 200 ms hold, so it could hold painting off forever
  (alacritty#8550).
- OSCs were truncated at 8 KB, so long OSC 52 copies were cut short silently. They grow to
  4 MB now, and longer ones are dropped whole (vte#151).
- Modes Claude Code turned on (mouse, focus, sync, a hidden cursor) end when its exit marker
  arrives, so a killed Claude doesn't leave the shell receiving escape codes for clicks.
- A fuzzer found that a 1-column terminal looped forever in reflow and overran a row in
  `term_put`. The window and `hardcore here` never go below 2 columns, but the core now clamps
  too.

Decided against, for now:
- Grapheme widths (VS16, ZWJ: ⚠️ is 1 cell here and 2 for Ink): this follows the wcwidth
  ConPTY measures with.
- Combining marks: they are dropped, as before.
- The kitty keyboard protocol: Claude Code works without it.
- Pushing the screen into scrollback on ED 2: hardcore matches xterm.

Claude Code (2.1.302) asks the terminal for DECRQM 2026 after XTVERSION, and hardcore answers
both, so it does get synchronized output. An earlier probe of ours that didn't answer DECRQM
wrongly concluded otherwise.

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

The performance tables below retain their original measurement context.
Later dated sections record subsequent implementation and slurp experiments;
they do not imply those older performance measurements were rerun.

## Speed

October 16, 2026 TV-panel preparation microbenchmark: an optimized headless
harness called the production `hardcore_tv_layout.h` preparation path with
both Settings and CAF enabled, including allocation, validation, and secondary
reflow composition. Each fresh layout evaluated 19 candidate callbacks across
the two panels; each panel is bounded to twelve candidates.

| Grid | Runs | Mean | p95 | Maximum |
|---|---:|---:|---:|---:|
| 120 × 50 (6,000 cells) | 300 | 0.176 ms | 0.180 ms | 0.207 ms |
| 200 × 240 (48,000-cell cap) | 200 | 1.432 ms | 1.466 ms | 1.503 ms |

These timings exclude building the primary-menu baseline and drawing the GUI;
they are not end-to-end hover or frame-latency measurements. Native tests
separately verify that rollover changes the Metal-rendered content without
resizing PTYs, changing selected tabs, or moving browser keyboard focus.

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

October 14, 2026 follow-up: a live drag could still stall the UI for a second
because live resize enabled `presentsWithTransaction` and called `nextDrawable`
on the main thread. Rendering now stays on the render thread during the drag,
using the existing latest-frame queue; AppKit never waits for a drawable or GPU
scheduling during ordinary resize painting.

A 60-step outward / 60-step return drag (`macbench drag --n 60 -250,-150`),
with a single shell tab and `HARDCORE_LAT=1`, measured:

| Resize callback | Before | After |
|---|---:|---:|
| Steps handled | 53 | 119 |
| Main-thread median | 0.944 ms | 0.179 ms |
| Main-thread p90 | 1.410 ms | 0.230 ms |
| Main-thread maximum | 1001.696 ms | 0.384 ms |

These are callback timings, not input-to-display latency. Explicit test snapshots
still render synchronously so their image files are available when requested.

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

## Unprepared agent session attachment (2026-10-14)

These experiments start fresh jobs directly in Apple Terminal, without tmux,
`hardcore host`, debugger injection, altered signatures, or a preparatory shell
setting. Credentials/configuration are isolated where possible; existing user
conversations are never selected. They are opt-in live probes, not release gates
or production slurp adapters.

| Agent | Route examined | Result |
|---|---|---|
| Codex 0.162.1 | Ordinary launch with default shared daemon; new `codex resume ID` frontend in Hardcore | Passed: original active Python tool PID survives termination of source frontend; original turn finishes; new prompt gets an answer in Hardcore. |
| Grok 1.0.50 | Ordinary launch with isolated leader socket; second `--resume ID` frontend | Failed: source termination loses active turn; replacement reports “Grok stopped before this turn finished.” Saved history is not a live handoff. |
| Claude Code 2.1.300 | Existing-session `/bg` and `attach`; Ctrl+B for a foreground Bash tool | Partial success: same shell child PID, output, and conversation recall survive closure of the original PTY. Claude's process and session ID change. Successful child completion is incorrectly reported as exit code `-1`. |
| Antigravity 1.3.5 | Enable `/remote-control` in an existing session | Documentation permits this without restarting, but explicitly ties its lifetime to the original CLI. No closeable-source handoff established. |

Codex probe: `python3 tests/probe_mac_agent_attach.py`. It uses a private
`CODEX_HOME`, requests a sleeping Python tool, attaches a new frontend to the
same thread, terminates the old frontend while the tool is still active, and
verifies both completion and a subsequent answer. A screenshot verifies the
actual Hardcore display. The frontend PID and PTY change; the agent's ongoing
execution remains in its existing daemon. `--no-daemon`, graceful source detach,
automatic mapping from a source TTY to its exact thread, approvals in flight,
and arbitrary child trees are not covered. The first attempt had a test parser
error (it expected an obsolete completion event); subsequent corrected runs
passed, including explicit source-frontend termination.

Grok probe: `python3 tests/probe_mac_grok_attach.py`. It intentionally fails when
the active turn does not survive. A supplied leader socket is only isolation;
it does not prove ordinary Grok execution lives in a transferable backend.

Claude probe: `python3 tests/probe_claude_background.py`, also with
`--foreground`. Tested 2.1.300 after the installed CLI updated from 2.1.299.
These two cases use a direct, initially unhosted PTY; they do not automate
Terminal.app. The first starts a Bash background task. The second starts a
foreground Bash task, waits until Claude offers Ctrl+B, and backgrounds that
same running task. Both then invoke `/bg`, wait for the original Claude process
to exit, attach the resulting job in Hardcore, and close the original PTY.
The disposable Python child keeps the same PID, records exactly one launch,
and finishes its timer. Its stdout completion marker survives in Claude's task
output file, and the replacement session answers a conversation-recall prompt.
Screenshots confirm the actual Hardcore display. No original Terminal window
is required to remain open in this probe, and no Terminal close dialog occurs.

This is **cooperative task handoff, not whole-process transfer**. The foreground
test moved child PID 26362 from source Claude PID 26260 into a new background
conversation; source session ID and destination session ID differ. In both
cases Claude's task notification reported `exit code -1` despite normal child
completion. Consequently this route is not ready for silent production slurp.
It needs correct handling of completion status and explicit classification of
movable tasks. Streaming model requests, arbitrary foreground subprocess trees,
approvals, and running subagents are not proven preserved. Installed code uses
checkpoint/resume for some agents and warns that other tasks stop or restart.
The first foreground probe sent Ctrl+B before the action became available and
was stopped; the corrected probe waits for Claude's background-action hint.

Remote Control does not remove the lifetime dependency: Anthropic's
[documentation](https://code.claude.com/docs/en/remote-control) requires the
original local Claude process to remain running. No supported mechanism for
retargeting all of an ordinary Claude process's live terminal descriptors has
been established.

Terminal may show a modal confirmation if asked to close a busy tab. The new
probes terminate only their own disposable source frontend and wait for the tab
to report idle before requesting closure. They do not turn off the user's
warning preference. Production handoff must confirm receiver attachment, obtain
a supported source detach, and close only the idle source tab; force-killing a
frontend is a research stress test, not the intended user workflow.

Primary references: [Claude agent view and `/bg`](https://claude.com/resources/articles/agent-view-in-claude-code),
[Antigravity remote-control lifecycle](https://antigravity.google/docs/remote-control?tab=cli),
and installed CLI help (`codex agents --help`, `codex resume --help`,
`grok leader --help`).

### Claude exit-status follow-up (2026-10-14)

The installed Claude 2.1.300 adopted-shell implementation deliberately returns
`-1` when the detached process disappears: it no longer has the original parent's
wait result. This does not establish that the command failed. A separate macOS
observer can recover the actual status without becoming the parent: register
`EVFILT_PROC` with `NOTE_EXIT | NOTE_EXITSTATUS` while the process is alive.
Apple documents non-parent status observation in the
[macOS 11.0.1 release notes](https://developer.apple.com/documentation/macos-release-notes/macos-big-sur-11_0_1-release-notes).

Added `tools/mac_exit_watch.py` as a research helper, plus
`make mac-exit-watch-test`. All nine deterministic cases passed: exit codes
0, 7, and 42 and SIGTERM, registering both before and after orphaning, plus
rejection of an already-reaped PID. These tests do not require Claude or GUI
automation. Registration errors fail closed. The observer must register before
exit and must watch the correct process: a descendant's status alone does not
establish the result of a shell pipeline or an entire tool task.

The live `tests/probe_claude_background.py --foreground --exit-code 7` test
preserved child PID 28500 through Ctrl+B, `/bg`, attachment inside Hardcore, and
closure of the original PTY. The child ran once, its output survived, and the
replacement Claude answered the conversation-recall question. The independent
kernel observer correctly reported 7 while Claude reported -1. The probe now
records both statuses and supports deliberate success/failure fixtures.
Private diagnostics: `/tmp/hc-claude-bg-opqyvb9g`.

A second live run with `--foreground --exit-code 0` preserved child PID 29124,
one launch, output, and conversation recall, and the kernel correctly returned
0. Its completion check initially timed out because the temporary directory
contained an underscore and the probe guessed Claude's project-path encoding
incorrectly. Fixed the lookup to find only the exact destination session UUID.
Rechecking the retained evidence with this lookup passed: Claude's notification
again reported -1. Diagnostics: `/tmp/hc-claude-bg-_22duwam`. This second run did
not reach the final screenshot step; it is not recorded as a clean end-to-end
probe pass.

This is not yet an exit-status fix inside Claude or a production slurp adapter.
Do not rewrite its transcript or silently reinterpret unknown as success. A
production handoff would need task-to-PID verification, status observation
established before source exit, destination attachment confirmation, and safe
handling of unsupported work. Only after that should an idle source tab close.
These tests used ordinary, unhosted disposable PTYs; they did not automate
Terminal.app or verify its close-confirmation behavior.

Other routes checked: the foreground cross-session inbox's inspected control
dispatcher handles messaging, idle notifications, rename, and artifact replies;
no background/PTY-transfer action was found there. This is version-specific
inspection, not a stable API contract. Remote Control retains the original
process dependency. Anthropic's
[backgrounding documentation](https://code.claude.com/docs/en/agent-view#what-carries-over-when-you-background)
explicitly says backgrounding starts a fresh process from the saved conversation;
eligible work carries over, while some workflow subagents restart. Thus `/bg`
remains a promising cooperative handoff, not evidence of whole-process migration.

### Expanded Claude routes (2026-10-15)

Still testing Claude 2.1.300. Two new probes narrow the options without using
Terminal.app automation or consuming model responses:

* `make mac-pty-lifetime-test`: five checks passed on disposable macOS PTYs.
  With SIGHUP ignored, closing the last master produced EOF on read and EIO on
  write. Keeping an extra slave open did not prevent this, even with CLOCAL set.
  Duplicating the master after the child started preserved bidirectional I/O
  when the original master closed. Reopening `/dev/ptmx` allocated a different
  PTY. The positive case assumes the master owner cooperates; it is not a way
  to obtain Terminal.app's handle. The initial positive-control test lost
  output when its child exited immediately; it now waits for an acknowledgement
  before closing the slave.
* `python3 tests/probe_claude_retroactive_hook.py`: a UserPromptSubmit hook added
  to an isolated project after Claude started ran in that same live session.
  It provided the exact session ID and transcript path. The hook blocked the
  disposable prompt before inference. Its stdin/stdout/stderr were pipes and
  `/dev/tty` failed with ENXIO; this does not give it Claude's terminal handles
  or a way to change its parent's descriptors. Corrected an initial assertion
  that assumed terminal rendering preserved spaces; the subsequent complete
  run passed. Diagnostics: `/tmp/hc-claude-late-hook-js28at5h`.

The hook is a potential retroactive coordination point, not a migration
mechanism. A production adapter would need to scope it to the chosen session,
preserve existing settings, and remove its changes afterward. No user or
repository Claude settings were changed by this probe. Claude's
[hook documentation](https://code.claude.com/docs/en/hooks#hook-input-and-output)
describes command hooks as separate sessions without controlling terminals;
the documented output fields do not provide terminal-descriptor replacement.

Expanded route assessment:

| Route | Finding | Remaining obstacle |
| --- | --- | --- |
| `/bg` and attach | Previously preserved running shell task PIDs and output | New Claude process; unknown task status; unsupported work needs classification |
| Retroactive hook | Live hot reload and precise session metadata verified | Can coordinate; cannot rewire the parent process |
| Retain/transfer original PTY master | Disposable positive control preserves I/O | Terminal.app must export the handle or allow debugger attachment |
| Hold slave / CLOCAL / ignore SIGHUP | Negative tests verified | Does not retain usable I/O after master closure |
| Reopen `/dev/ptmx` | Allocates another terminal | Does not recover the existing master |
| `/desktop` | Official docs describe saving the conversation, opening Desktop, and exiting CLI | Not live-process transfer into Hardcore |
| Remote Control / teleport | Remote Control retains local-process dependency; teleport is cloud-session continuation | No demonstrated local PTY ownership transfer |

The [reptyr author's description](https://blog.nelhage.com/2014/08/new-reptyr-feature-tty-stealing/)
confirms that its whole-terminal mode obtains the master by tracing the terminal
emulator and sending the descriptor with SCM_RIGHTS. That is the right
architecture when the owner cooperates, but does not remove macOS's existing
Terminal.app attachment restriction. Apple's
[PTY multiplexor source](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/tty_ptmx.c)
also explains the new-device allocation observed in the reopen test.
[Desktop documentation](https://code.claude.com/docs/en/desktop#coming-from-the-cli)
does not claim an in-memory transfer. No security settings, binary signatures,
or existing user sessions were changed in this round.

### Foreground handoff and process-tree probe (2026-10-15)

Expanded `tests/probe_claude_background.py` with `--process-tree` and
`--direct-background`, recording both parent and worker launches and requiring
handoff before the fixture finishes. The direct flag deliberately tests an
unsafe route on disposable commands; it is not a recommended user workflow.

* **Direct `/bg` failed and replayed work.** Starting a foreground parent and
  worker, then issuing `/bg` without Ctrl+B, ended original parent PID 37621.
  The replacement session ran the command again: launch files contain parents
  37621 and 37729 and workers 37622 and 37730. Neither original fixture
  completed. Stopped only the disposable background job `846cbbfc`.
  Diagnostics: `/tmp/hc-claude-bg-wwpv1vyh`.
* **Guarded Ctrl+B then `/bg` passed the process-tree probe.** Parent 37818 and
  worker 37819 survived source Claude 37742 exiting and closure of the original
  PTY, each with exactly one launch. The parent waited for the worker and exited
  normally. The kernel observer returned 0, output survived, and the receiver
  inside Hardcore answered the recall question. Screenshot inspected.
  Diagnostics: `/tmp/hc-claude-bg-34mq8wrz`.
* **Unknown status changes Claude's interpretation.** Even in the successful
  process-tree run, the replacement Claude interpreted its reported -1 as
  likely termination/interruption. The observed kernel status was 0. Therefore
  this discrepancy is not merely cosmetic; an adapter needs to deliver an
  accurately attributed status through a supported integration, or obtain an
  upstream fix, before treating the workflow as seamless.

The probe now identifies its exact disposable destination before assertions
about surviving processes so failure cleanup can stop it, and writes
`failure.json` for subsequent failed experiments. This is still an unhosted PTY
test, not Terminal.app GUI coverage or evidence that arbitrary subprocess trees
and pending approvals transfer.

Anthropic documents a similar foreground-command hazard for
[the left-arrow background shortcut](https://code.claude.com/docs/en/agent-view#switch-sessions-without-leaving-the-terminal):
after a waiting period, a foreground command can be stopped and executed again
in the replacement session. This round demonstrated replay for direct `/bg`
as well. A slurp adapter must positively confirm eligible background task state
before invoking session handoff; merely seeing a new session appear is not
evidence of a successful move. Unsent input and pending dialogs also need
explicit handling rather than blindly injecting shortcuts.

### Parallel leads: retroactive readiness and completion context (2026-10-15)

The new `--late-hooks` mode in `tests/probe_claude_background.py` installs hooks
only after an ordinary foreground Claude command has started. It uses a fresh
project with no launch-time hooks, host, or tmux. It never changes the user's
Claude settings. Three live parent/worker tests passed:

| Fixture exit | Original parent / worker | Outcome | Diagnostics |
| --- | --- | --- | --- |
| 7 | 40696 / 40697 | Both PIDs retained; one launch each; Claude explained the independent failure status and did not rerun | `/tmp/hc-claude-bg-c8yy7u20` |
| 0 | 41180 / 41181 | Both PIDs retained; one launch each; Claude explained success and answered `PINEAPPLE HCSTATUS 0` inside Hardcore | `/tmp/hc-claude-bg-w89z9fl8` |
| 0 (stricter repeat) | 42104 / 42105 | Same result; waited for destination Stop with exact final answer, then rechecked both launch counts | `/tmp/hc-claude-bg-522big23` |

The hook adds an explicitly attributed macOS observation as
`UserPromptSubmit.additionalContext` when the adopted-task completion arrives.
It preserves the original notification. Claude still internally reports -1,
but its explanation now distinguishes that unknown result from the independently
observed 0 or 7. The successful run's actual Hardcore screenshot was inspected.
This validates improved model interpretation, not a repair of Claude's internal
task record.

The second run also enforced a positive readiness check before `/bg`:
`PostToolUse` reported `backgroundedByUser: true`, `backgroundTaskId`, and the
exact Bash tool-use ID; `Stop.background_tasks` then listed that same shell task
as running. The hook matched destination session, task ID, tool-use ID, fixture
output, and observed PID before adding context. The first run matched the
destination, fixture output, and PID only. Subsequent probe hardening uses atomic
identity/status publication and avoids delivering the same context twice.
The stricter repeat exercised those changes and waited for the destination
Stop hook's exact `last_assistant_message`, instead of accepting partially
rendered terminal output. Launch counts are rechecked after that final response
to reject delayed replay. Cleanup skips confirmed exited PIDs and checks a
still-running fixture's command before signalling it.

These are deliberately scoped fixtures. Production still needs a reliable
mapping from an arbitrary shell task to its actual process leader and start
identity. A worker's status does not establish pipeline success. Hook load must
be confirmed, the observer registered before handoff, and missing/ambiguous
status must remain unknown. The classic hook input lacks a trusted origin tag,
so text shaped like a task notification is not sufficient authentication on its
own. No silent retry or permission-policy bypass is justified.

Independent investigation found a stronger documented integration surface:
[mods](https://code.claude.com/docs/en/plugins/mods/overview#install-or-update-a-mod)
can load into a running session through `/reload-plugins`. Their `prompt.submit`
event has an engine-stamped `origin.kind` of `task-notification`; a handler can
append context while preserving the original text. This is a better candidate
than parsing arbitrary user text. `session.receive` concerns external delivery,
so it should not be assumed to intercept local Bash completion. See the
[mod API reference](https://code.claude.com/docs/en/plugins/mods/reference).

Mods do not expose a direct PTY transplant: the
[documented API restrictions](https://code.claude.com/docs/en/plugins/mods/api#reach-files-processes-and-the-network)
exclude direct Node/native access, and the process APIs launch subprocesses
rather than replacing the host's descriptors. The installed Terminal scripting
dictionary likewise has no descriptor-export/detach command. Its close-warning
exceptions merely suppress warnings and would not preserve execution.

#### Live mod control probe

`python3 tests/probe_claude_mod_control.py` passed without a model prompt.
The source started as ordinary Claude with no plugin, hook, host, or tmux.
The probe installed its local mod afterward into a disposable project and
reloaded plugins. Source PID 42671 and session
`14c919e9-2e05-4913-b79e-bd4a6311494c` stayed unchanged throughout. It verified:

* An immediate custom command reads exact session identity and prompt state.
* A timer reads an exact unsent draft and cursor position without submitting it.
* The command API can invoke a local command without inference.
* A `command.run` handler for canonical `background` intercepts typed `/bg` and
  blocks handoff, leaving the source alive. Matching the alias `bg` is wrong.

Diagnostics: `/tmp/hc-claude-late-mod-mia3fuxf`. The probe uses a private config
directory; temporarily copied login/account data is removed even on early
setup failure. It does not install into the user's actual plugin configuration.
This validates late installation and control, not a full running-task transfer.

The exploratory runs also found two important scheduling details: awaiting a
second command inside a held command handler is rejected as a potential
deadlock; scheduling from the documented timer succeeded. A command API call
does not recursively invoke its calling mod's handler. Therefore a controller
must check readiness before its own `$.command.run` request rather than rely
on its own background-command guard to catch that request. A local `status`
command can wait for its panel to close, so it is not a suitable unattended
completion check.

### Cooperative adapter implementation (2026-10-15)

The late-load research now has a bundled implementation: `hardcore claude
install` installs a controller/guard plugin and a separate dispatcher into the
current project. `/reload-plugins` loads them into an ordinary running Claude
session. `/hardcore-slurp wait` requests handoff after a verified normal turn
with no background tasks, scheduled work, active agents or draft. It never
submits a model prompt or interrupts a tool to create this boundary.

The native command starts a receiving Hardcore window before arming the move.
Private, expiring rendezvous records bind source UUID, process identity, cwd,
receiver identity and a unique temporary title. The receiver attaches only the
new background job with that exact marker, after an explicit acknowledgement
from its loaded adapter. The destination restores the prior display title
before acknowledging; its live process identity is checked again before attach.
Cancellation, receiver death, changed source identity, stale
requests and ambiguous destinations refuse the handoff. No Terminal scripting
or close-window action is part of this route.

`python3 tests/probe_claude_handoff_guard.py --model-turn` passed against Claude
2.1.300. Plugins were installed after the disposable ordinary PTY session
started. An intentionally unprepared request from the real dispatcher reached
the separate guard with the engine-stamped plugin origin and was refused.
The source PID and UUID and an exact unsent draft survived. One tiny model turn
then emitted main `classic.Stop` with explicitly empty `background_tasks` and
`session_crons`, followed by normal `turn.complete`; the production adapter's
status reported ready. Diagnostics: `/tmp/hc-claude-handoff-guard-lbgvwc3y`.
This probe instruments a disposable plugin copy to issue the unprepared
request; it never forges readiness. Temporary login data was removed afterward.

The two-plugin split addresses the documented self-hook bypass for commands
issued by a mod. The final guard checks readiness again when the queued
background command executes and returns a refusal on handled errors. Driver
calls require controller origin and recheck guard presence. The driver declares
the guard as a plugin dependency, using Claude's supported dependency handling
to prevent ordinary disabling of the guard while the driver is enabled. This
is not an unconditional security boundary against runtime failures or manual
configuration/file changes; the core background command itself accepts
arbitrary arguments.

The conservative route hands off the conversation after work finishes, not
the original Claude process or active arbitrary tool trees. A quiet prompt
immediately after late loading is insufficient to establish readiness. These
limits remain explicit; earlier successful active-task fixtures do not establish
a general production-safe task-to-PID mapping or accurate pipeline status.

#### Complete native handoff

`python3 tests/probe_claude_handoff_guard.py --handoff build/hardcore` passed.
The ordinary source started without plugins, host or tmux. Plugins installed
later, and the production controller prepared and armed the receiver. Source
Claude PID 48660 exited normally; its PTY was closed. Hardcore PID 48858
remained alive, and receiving tab PID 48859 executed `claude attach 7787a1db`.
The destination's distinct session UUID was
`7787a1db-8259-4fd5-bd64-40c6c28cacdb`. A new turn inside Hardcore recalled the
original `HCPROBE_READY` token exactly. The public agent listing retained
`kind: background`; the prior display title `work-5e` was restored. The actual
Hardcore screenshot was inspected. Diagnostics:
`/tmp/hc-claude-handoff-guard-fcp4geup/result.json` and `receiver.png`.

The preceding attempt exposed a lifecycle bug: redirecting the receiving GUI's
stdio did not isolate it from the launching Claude process group, and Claude's
exit also terminated Hardcore. Detached `mac_spawn` now uses
`POSIX_SPAWN_SETSID`. A separate native test verifies that the GUI and waiting
client survive termination of their disposable launcher's group, cancellation
preserves the original source PID, and a dead receiving client prevents arming.
That test uses a fake Claude CLI and no credentials or inference:
`python3 tests/test_claude_handoff_window.py`.

The installer was also exercised with the real Claude CLI in private settings:
install, reinstall, status, version upgrade, and attempted disabling of the
guard while the dependent driver remains enabled. Claude refused that disable.
Unit tests additionally cover state changes during preparation/arming, draft
clearing, duplicate dispatch, cancellation races, exact destination correlation,
expiry, receiver/process identity changes and private record validation.

These end-to-end sessions ran on ordinary disposable PTYs, not Terminal.app's
GUI. They establish no prelaunch host requirement and successful native
attachment, but do not constitute fresh Terminal GUI coverage. Cleanup stopped
only the test background job and its identity-checked receiver; private login
data was removed.

#### Destination readiness race and repeated verification

Automatic wait exposed a second timing bug. `claude agents` could advertise a
destination before the supervisor accepted attachment. An early `claude attach`
entered its stopped-session wake path and reported that no saved transcript
existed, although the destination was still starting and later restored its
title. Captured diagnostics: `/tmp/hc-claude-handoff-guard-tst_2_ta`.
Hardcore now retains attachment errors and recovery instructions on screen.

The receiver now waits for the destination mod to claim its exact request,
finish the local title-restoration command, and explicitly acknowledge readiness.
The helper binds and rechecks the destination's live PID/start identity and cwd.
It does not attach an `armed` or merely `claimed` request, and it never retries
by blindly respawning a session. Reloading plugins after attachment cannot
restore an obsolete title over a user's subsequent rename. The waiting tab
reports the source cwd through encoded OSC 7 so its recovery shell opens there.

Three subsequent end-to-end runs passed on Claude 2.1.300:

| Scenario | Bound destination PID | Diagnostics |
| --- | --- | --- |
| Wait for the next ordinary turn | 53081 | `/tmp/hc-claude-handoff-guard-lcfuu4ox` |
| Wait through a foreground tool | 53451 | `/tmp/hc-claude-handoff-guard-lehc4j65` |
| Repeat ordinary-turn wait | 53934 | `/tmp/hc-claude-handoff-guard-h1yx3xcl` |

Every run observed `prepared → armed → claimed → ready → attaching`, normal
source frontend exit, original PTY closure, and exact conversation recall in a
new destination turn with the same bound destination PID. The foreground fixture
(PID 53394) launched exactly once and completed before receiver preparation;
the launch count was checked again after recall. Screenshots were inspected.
An intervening probe failure was traced to its own simulated paste/Enter timing;
the probe now waits for the exact draft to reach Claude before pressing Enter.

Final focused coverage: 41 plugin checks, 38 broker checks, 15 installer checks
(including actual private Claude CLI install/update/dependency enforcement), and
five native receiver checks. Native tests cover exact readiness routing and cwd
encoding for spaces, semicolons, newlines and Unicode. The app bundle's native
CLI also installed its packaged plugins successfully in a disposable project.
Core terminal unit tests, xterm.js fixtures (71 passed, five skipped), shell
selftest, live native handoff/rollback and tmux attachment passed. The universal
Mac build was warning-free and its app signature verified.

#### External requests, draft preservation, and attachment receipts

The 0.2.0 Claude plugins accept a private, process-bound move request from
`hardcore claude move SESSION_ID`. Installation uses the official project-scoped
CLI when necessary; the loaded mod consumes the request without typing over
the composer. Requests expire after one hour and cancellation also covers a
poll already in flight. A fresh install still requires `/reload-plugins`.

Three further ordinary-PTY to native-Hardcore runs passed on Claude 2.1.300:

| Scenario | Diagnostics |
| --- | --- |
| External queued request with an unsent Unicode draft | `/tmp/hc-claude-handoff-guard-uskechhc` |
| Public `hardcore claude move` with an unsent multiline Unicode draft | `/tmp/hc-claude-handoff-guard-79f3oiox` |
| Automatic wait through a foreground Bash fixture | `/tmp/hc-claude-handoff-guard-ozb27c7l` |

Each reached the new `attached` state only after exact draft readback, equality
of the source text/tool message projection's SHA-256 digest, and a native
receipt for the bound window, attached child PID, PTY and request token. The
receipt marker must be in actual visible terminal cells and a Metal frame must
complete. All three then recalled the source conversation after the original
PTY closed. The foreground job ran exactly once and completed before handoff.
The multiline draft included Unicode and literal shell syntax, remained unsent,
and was captured in `verified-draft.png` before the probe cleared its own draft
and submitted the recall turn. Screenshots were inspected.

Native fake-client tests reject startup without output, wrong request/PID/TTY,
a marker now only in scrollback, and a departed client leaving a stale marker.
The actual attached client first succeeds while its marker is visible, then
fails after 100 further lines move that marker outside the viewport. Existing
receiver lifecycle, cancellation, readiness and cwd tests still pass.
Deterministic coverage: 56 plugin checks, 51 broker checks, 18 installer checks
(including the opt-in official CLI integration), and 17 retirement-gate checks.
Core unit tests, 71 xterm.js fixtures and the shell selftest also pass.

An additional no-model bootstrap probe found that Claude's documented Ctrl+S
stash followed by `/reload-plugins` automatically restores a known Unicode text
draft and its exact middle cursor while preserving the source process. Evidence:
`/tmp/hc-plugin-stash-26pr2pue/result.json`; reproduction:
`python3 tests/probe_claude_bootstrap.py`. This is not enabled as blind Terminal
input: composer focus must be known, an existing stash could be overwritten,
and image attachments were not covered.

Full shell and terminal preservation remains the requirement for retiring the
source. The message API is a limited projection (at most 4096 messages), and
the draft API does not expose all attachments. Terminal's scripting dictionary
exports plain text, not full styled cell state, shell memory or unflushed shell
history. The independent retirement verifier therefore refuses the default
full-shell policy; it also rejects remaining controlling-TTY processes and
detached jobs holding slave descriptors. No automatic close action is enabled.
Terminal.app access was explicitly blocked by the computer-use tool during
this round, so actual Terminal bootstrap and closure were not tested or
attempted through another automation route.

#### Guarded cursor restoration, richer verification, and recovery

The 0.3.0 adapters restore the exact cursor inside a multiline Unicode draft.
The receiving window temporarily holds human input while terminal query replies
continue normally. A request-bound, visible cursor marker and completed Metal
frame authorize one Left-key event; the mod consumes it to restore the saved
cursor through Claude's editor API. Exact text/cursor readback and a separate
rendered receipt precede input release. Wrong identities, expired guards,
departed clients and markers only in scrollback cannot authorize restoration.
Lost replies can be retried without sending another key or refilling a composer
that has already been released.

History verification now hashes the rich Messages API content, including media,
thinking and tool blocks. A live run exposed Claude appending local commands to
the final user message. The comparison now binds the source's exact block/text
boundary before hashing that original prefix; it does not trim arbitrary text
or omit media. This remains normalized current context, capped at 4096 messages,
not proof of all compacted history, system prompts or tool definitions.

Two further ordinary-PTY to native-Hardcore runs passed on Claude 2.1.300:

| Scenario | Bound destination PID | Diagnostics |
| --- | --- | --- |
| Public move with multiline Unicode draft and middle cursor | 68089 | `/tmp/hc-claude-handoff-guard-kpfs44pk` |
| Automatic wait through an active foreground Bash fixture | 69065 | `/tmp/hc-claude-handoff-guard-5s3ict51` |

Both reached `attached` with exact draft/cursor and rich history verification,
a rendered receiver receipt, and released input. After the original PTY closed,
a new destination turn recalled the source conversation. The foreground fixture
ran exactly once and finished before preparation. The middle-cursor draft
remained unsent; its saved screenshot was inspected before the probe cleared
its own draft for the recall turn.

Real composer probes found another preservation gap: collapsed large pastes
and pasted images appear to the draft API as placeholders, without their
payloads. Preparation now refuses those detectable cases. Evidence:
`/tmp/hc-claude-bootstrap-kn3m_7c6/result.json` and
`/tmp/hc-claude-bootstrap-5j36tcl7/result.json`. The independent cursor probe is
`/tmp/hc-claude-bootstrap-9_f_ly0v/result.json`. Hidden stash inventory and edits
after Claude accepts asynchronous background dispatch remain unproven.

The broker also binds the receiving GUI's process identity, checks source
identity across adapter installation, retains failed restoration diagnostics,
and supports read-only `moves`/`inspect` recovery commands. `recover
--export-draft` writes exact text and cursor to a new private JSON file, refuses
existing files and symlinks, and never submits the draft. Historical verification
is shown separately from current client availability. Recovery was exercised
against this round's failed normalization probe, including overwrite refusal.

Final coverage: 71 plugin checks, 69 broker checks, 20 installer cases (one
opt-in skip in the default run; all 20 passed with official CLI integration),
17 retirement-gate checks, and 11 native receiver checks. Core unit tests,
71 xterm.js fixtures (five skipped), and the zsh selftest passed. The universal
Mac build was warning-free. `make install` installed the app and CLI; signature
verification passed. The installed app's own CLI installed and reported both
packaged 0.3.0 plugins in a disposable project with private configuration.

#### Late cloning of an ordinary shell

A separate stock-zsh probe established a new route for preserving an already
running shell's memory without a prelaunch host. Loading `zsh/clone` late and
cloning onto a fresh PTY retained unexported variables, arrays, aliases,
functions, options, cwd, unflushed history, and a Unicode ZLE command buffer
with its middle cursor. The new shell remained interactive after the original
PTY closed; fresh foreground job control worked. Complete evidence and
reproduction are in [the zsh clone research notes](RESEARCH-mac-zsh-clone.md).

Existing jobs did not move, explicit old-slave descriptors broke after closure,
and regular-file descriptors shared offsets with the original shell. This is
an experimental shell-copy route, not a production full-state move. Styled
Terminal state and the remaining shell/job/descriptor gaps still prevent
automatic Terminal closure. These tests used owned PTYs and Hardcore windows;
Terminal.app access remained blocked and no alternate automation was attempted.

#### First user-operated Terminal.app handoff

On 2026-10-15 the user ran the Desktop manual-test launcher in Terminal.app.
The source Claude inherited `/dev/ttys012` directly, with no allocated source
PTY or Hardcore host, and plugins were installed after that process started.
The user reloaded plugins, completed the tiny test turn, entered the unsent
Unicode draft, and moved its cursor left three times. The watcher then queued
the public move command. The saved result is `passed` at
`/Users/wm/Desktop/Hardcore-Terminal-Test-Results/run-hs5wl4eu/result.json`.

The exact draft `HC TERMINAL UNSENT café 🚀 END` and UTF-16 cursor 27 matched in
source and destination. The broker verified current conversation content and
recorded a rendered receipt for request `3406f4ec3e3d441c93cc6f069bff3d0c`, window
78725, receiver 78726 and `/dev/ttys009`; the input guard was released. The
original Claude frontend exited with status 0. At verification, both the
destination and receiving client were live, with no inspection errors.

Terminal and its original parent shell stayed open. Shell memory, jobs and
Terminal scrollback were not transferred. The user performed all Terminal
input; no blocked Terminal automation was used. The initial installation banner
was obscured by Claude's redraw, and the ready notice appeared above the reload
command. The follow-up below removes that dependence on transient notices.

#### Remaining-state probes and persistent manual-test progress

The manual Claude fixture now saves a persistent private `STATUS.txt`, including
the current phase and next action. Its read-only `--status RUN_DIRECTORY` mode
also reports historical results without contacting Claude or changing files.
Reload, observer readiness, reply, draft/cursor and verification have distinct
phases; instructions no longer depend on a banner inside Claude's redraw area.
All 34 manual-fixture tests pass.

New owned-zsh experiments reproduced an Apple session-history collision: an
ordinary clone can overwrite the source's saved session and duplicate shared
history. Flushing pending history before cloning and rebinding every initialized
per-session path avoided that collision in both exit orders. A separate
`exec /usr/bin/true` control suppressed copied source exit hooks, while normal
destination exit ran them once. This skips all source exit work and is not
enabled as a production retirement procedure. See the
[shell-state research](RESEARCH-mac-zsh-clone-state.md) for fixtures and limits.

Editor tests also compare undo/redo, cut buffer, kill ring, cursor, directory
stack, options and traps with an uncloned control. Repeated passes did not
establish reliability: independent runs exposed failures before and after
source closure. Additional diagnostics caught zsh failing to acquire the new
controlling terminal even while `$TTY` and the copied draft looked correct.
Those cases cannot count as usable shell transfers. The native receiver probe
now separately requires a `/dev/tty` write to arrive on the new PTY before it
records clone success. It correctly refused the current environment's failing
case at `/private/tmp/hc-zsh-native-1s3yt13v`. Its earlier native GUI attempt also
timed out with Metal/WindowServer connection errors
(`/private/tmp/hc-zsh-native-5l58u92z`). No native shell-transfer success is
claimed for this round.
The strengthened shell-state probe independently refused the same access
denial before source closure at `/private/tmp/hc-zsh-clone-state-urgruw4g`,
retaining the five completed history/exit-hook cases in its partial results.

The new styled-history archive helper has 18 passing offline integration tests
and 18 native selftest checks, with warning-free `-Werror` compilation. It
requires unique fixture markers and matching plain/RTF text, retains private
originals, and produces bounded, inert text/style runs. Tests cover literal RTF
escape text, missing final newlines, Unicode, colors, control sanitization,
objects, nesting/size limits and overwrite/symlink refusal. The manual fixture
is ready to measure Terminal's actual copy output; no clipboard read or Terminal
automation occurred in these offline tests. See the
[presentation research](RESEARCH-mac-terminal-state.md).

Existing jobs, inherited resources, arbitrary shell hooks and full Terminal
emulator state remain open gaps. Automatic source-window closure stays disabled.

#### User-operated archive mismatch and capture repair

The first real Terminal styled-copy run used an existing window and refused a
plain/RTF mismatch at `/private/tmp/hc-terminal-archive-2j2cy25a`. Its old helper
discarded the rejected copies, so the mismatch's location and cause are unknown;
an existing window alone is not established as the cause.

The helper now pairs plain/RTF data from the same clipboard item, compares
independently located fixture ranges instead of unrelated surrounding text,
and retains private original/decoded copies and a bounded comparison report on
an identified mismatch. Unicode and newline equivalence never authorize an
exact-match pass. A new user-operated `--recapture` mode reuses the saved fixture
with a freshly compiled helper and a new attempt directory. The wrapper also
requires equality with the known expected fixture, independently of agreement
between the two clipboard representations.

All 24 archive integration tests, 13 workflow tests and 22 native selftest checks
pass; compilation uses `-Werror`. No live clipboard read or Terminal automation
was used during this repair. At that point real Terminal style coverage was
pending a successful capture; the next section records its resolution.
See [the archive research](RESEARCH-mac-terminal-state.md)
for the failure evidence and comparison semantics.

#### Fresh Terminal capture: exact text with mapped RTF styles

The fresh-window capture at `/private/tmp/hc-terminal-archive-bnqxoii3` identified
the mismatch precisely: the plain copy preserved `e` plus `U+0301`, while the
RTF already encoded composed `U+00E9`. This was the only fixture text difference;
the plain copy exactly matched the expected fixture. It is an exporter
composition difference, not evidence that the user selected the wrong window.

The decoder now preserves exact plain UTF-16 and maps canonically equivalent
RTF graphemes onto it, refusing changed clusters with ambiguous internal styles.
Unchanged clusters retain internal style boundaries. Raw representation
equality remains false in the report; a separate mapping proof records the
one reconciled grapheme. Newline differences and compatibility-only Unicode
changes remain refusals. The wrapper now recognizes older numeric boolean
diagnostic flags and can decode saved captures entirely offline.

Processing the real saved files succeeded at
`/private/tmp/hc-terminal-archive-bnqxoii3/attempt-k1oln8aa/coverage.json`:
exact fixture text, all 64 history rows, Unicode, foreground/background colors,
bold, italic, underline and colored blanks. The 17/153/85 truecolor sample
resolved to `[0.06667, 0.6, 0.33333, 1]`. The hyperlink label survived but its
target is absent in the exported RTF. Paragraph/ligature attributes and full
terminal cell/cursor/mode state remain outside this archive's proof.

All 28 archive integration tests, 23 workflow tests and 22 native checks pass,
including both composition directions, Hangul, reordered combining marks,
shifted subsequent style offsets, and ambiguity refusals. Native compilation
with `-Werror` and syntax/whitespace checks pass. Existing captured originals
were preserved; no clipboard read, Terminal automation or further manual run
was used for this successful decode. This does not authorize automatic source
window closure.

#### Unattended verification and probe cleanup

`python3 tools/verify_mac_slurp.py` now collects archive, handoff, retirement,
and probe-lifecycle regressions, saved-capture replay, seeded archive robustness,
owned zsh state checks, and a disposable native Hardcore receiver in one run.
It supplies EOF on stdin, uses stage deadlines and private bounded logs, and
updates `report.json` and `SUMMARY.txt` after each stage. Independent checks
continue after failures. Explicit `--offline` and `--no-gui` scopes record their
exclusions; missing prerequisites cannot produce a full-run pass.

The final combined offline run passed at
`/private/tmp/hc-slurp-verification-visye5dd/report.json`: 28 archive, 24 archive
workflow, 34 handoff workflow, 17 retirement, 10 probe-lifecycle, and 18 runner tests;
saved Terminal replay; and 65 archive robustness cases. The replay retains exact
plain text, all 64 fixture history rows, and the separately verified mapping of
the one canonically equivalent grapheme. Raw plain/RTF equality remains false.
The original capture is copied into the new private run before replay, and the
robustness probe verifies its input originals remain unchanged.

The runner's own regression suite is included in the command. Its tests
exercise EOF stdin, bounded stdout/stderr storage, timeout cleanup of an owned
Python process group, missing prerequisites, explicit exclusions, private
snapshots, and continuation after independent failures. Fault injection also
checks collector and cleanup errors, failed log creation, and preservation of
the original user interrupt; these tests use disposable subprocesses,
without Terminal, Claude or GUI access.

The robustness corpus covers both canonical composition directions, reordered
marks, Hangul, supplementary characters, emoji sequences, malformed/object RTF,
size/style limits, and deliberate mutations of the real saved text. Large
canonical fixtures include sparse changes in about 105K UTF-16 units, with
colored text after the changed clusters. Timings are recorded observations,
not machine-dependent performance assertions. Native compilation uses `-Werror`.

The shell/native probes now bound PTY output, GUI output, packet waits, and
cleanup. They close owned descriptors and signal only unreaped direct children;
observed clone/job PIDs are not cleanup authority. Setup failures run cleanup,
and cleanup failures preserve the primary error and partial evidence. Ten
deterministic lifecycle tests and a plain owned-PTY startup/exit check passed.
The known-denied controlling-terminal and native GUI stages were not retried
in this agent environment. At that point their result remained pending the
user's unattended run; an offline pass does not establish either live stage,
fresh Terminal capture, real Claude handoff, or full-state source-window closure.

#### User-run native shell handoff and screen-proof correction

The user's ordinary Terminal run completed all ten selected stages in 5.78
seconds at `/private/tmp/hc-slurp-verification-zm74qu_n/report.json`. The shell
matrix is saved at `/private/tmp/hc-zsh-clone-state-f1z50voo/result.json` and the
native receiver fixture at `/private/tmp/hc-zsh-native-u231f3_d/result.json`.
These are owned disposable PTYs launched by the script, not the user's existing
Terminal shell or an active Claude conversation.

A separate read-only audit at
`/private/tmp/hc-slurp-evidence-audit-i2djhk0s/report.json` compared the saved
artifacts. The native source/clone shell-state captures are byte-for-byte equal
after removing the expected PID/TTY line. The unflushed-history sentinel is an
exact history entry and its history file remains absent. The destination
acquired its controlling terminal, and its PID-bound `/dev/tty` proof reached
the new master. After the source shell exited, native Hardcore input inserted
exactly one character at the preserved middle cursor. All captured ZLE fields
matched, and undo/redo/yank behavior matched the uncloned control. The Apple
history-path mitigation again passed both exit orders.

The audit also found that the old native screen check was too weak. It accepted
the fragments `UNSENT` and `café`, even though the observer's own `/dev/tty`
diagnostic overwrote the rest of the visible draft. The exact shell buffer
survived, but `nativeSnapshotContainsDraft: true` in that historical result
does not prove the complete visible draft or its screen cursor. The saved
snapshot retains the diagnostic text and lacks the full expected edited line.
This is a probe-induced rendering disturbance and an insufficient assertion;
the run cannot count as complete screen-restoration evidence. Original reports
and captured files are retained unchanged.

The revised native probe records `proofVersion: 2`. Its controlling-terminal
proof runs once, outside subsequent quiet editor observations. Observation
files are immutable per generation. Exact history event IDs/values and captured
shell memory must match the source, and the edit occurs inside the quoted
payload so the execution-marker path stays unchanged. It requests one redraw
and polls read-only native snapshots until the complete visible draft, blank
surrounding cells and physical cursor match, both before source closure and
after the edit. The checker accepts only this plain fixture's supported
snapshot format and retains a bounded viewport. Received duplicate PTY
descriptors close even when a preview is rejected. This establishes a terminal
cell/cursor assertion, not an exact GPU-pixel assertion. The strengthened live
check was pending at that point; the following sections record the discovered
resize failure, its repair, and the verified version-3 run.

The strengthened probe has 28 passing offline lifecycle/evidence/snapshot
tests, including the historical diagnostic overwrite, missing suffixes,
scrollback-only matches, wrong cursors, wide-character wrapping, unsupported
styles/modes, immutable observations, exact history, and descriptor cleanup.
The combined offline run at
`/private/tmp/hc-slurp-verification-73vc676n/report.json` passed 149 regression
tests, saved-capture replay, and all 65 archive robustness cases. The actual
historical native snapshot was independently refused by the new checker.
The generated zsh fixture passes syntax checking. No live clone, GUI, Terminal
or `/dev/tty` operation was repeated for this repair.

#### Native import geometry failure and repair

The user's next run, `/private/tmp/hc-slurp-verification-a7n6tn1p`, passed every
stage except `owned-native-receiver`. Its stricter version-2 screen check caught
a real failure before the source-close verification step: the complete draft
was present, but the visible cursor was at zero-based column 104 instead of 31.
The saved fixture is `/private/tmp/hc-zsh-native-c6imuzsf`; a read-only audit at
`/private/tmp/hc-slurp-resize-audit-tdyklnd_/report.json` records original-file
hashes and confirms exact captured shell memory, history, draft, and ZLE cursor.

The fixture initialized only its destination PTY to 110 by 32. The source had
zero kernel dimensions and zsh's 80-by-24 fallback. Independently, the native
import path resized its internal terminal to 105 by 43 before starting I/O.
`tab_set_size` deliberately skipped the staged donor PTY, but commit never
reapplied the final grid. Subsequent layout saw an already matching internal
grid and skipped resizing again. The saved redraw's cursor-up and right-19
sequence fits an 80-column wrap; interpreted on 105 columns, it clamps to the
observed right edge.

Successful import now applies the current terminal dimensions immediately
after positive GO and `tab_start_io`. Staged and rejected transfers still leave
the donor PTY size untouched. `tests/test_mac_import_resize.py` compiles the
actual production commit and resize functions into a headless harness using
owned PTYs. All eight cases pass, including multiple tabs, mixed accepted and
rejected groups, paint/READY/GO failures, and a negative control that removes
the new resize call and must fail the kernel-size assertion. Both `mac-test`
and the unattended runner include this regression.

The native probe now emits `proofVersion: 3`, initializes both owned PTYs to
110 by 32, and observes zsh acknowledging that size before entering the draft.
Before source closure and after the edit, it requires agreement between the
shell's dimensions, kernel PTY size, and native grid, plus the expected foreground
process group. The exact visible draft/cursor assertions remain in place;
observers never assign shell dimensions or manually signal the clone to force
the result. Six new geometry regressions bring the lifecycle suite to 34 tests.

The universal binary rebuilt without warnings; core tests reported zero
failures, and xterm.js fixtures passed 71 with five skipped. The combined
offline run at `/private/tmp/hc-slurp-verification-s1dyqalg/report.json` passed
163 regression tests, saved-capture replay, and all 65 archive robustness cases.
A separate ordinary owned-PTY experiment at
`/private/tmp/hc-zsh-plain-resize-ecb6dqb8/result.json` confirmed that kernel size
changes updated zsh's dimensions without changing its draft or cursor. It did
not clone a shell, access `/dev/tty`, or launch a GUI. The repaired version-3
live clone/native receiver check remained pending an ordinary-environment run;
the known-denied live stages were not retried in the agent sandbox.

#### Verified live native draft, cursor, and geometry

The user's subsequent run at
`/private/tmp/hc-slurp-verification-h80gugho/report.json` passed all eleven
selected stages in 6.44 seconds, including the version-3 native fixture at
`/private/tmp/hc-zsh-native-f6kbbekz/result.json`. The native stage took 0.55
seconds. This supplies the previously pending live receiver evidence for the
import-size repair and stricter screen checks.

Before the source test shell exited, the receiver contained the complete
Unicode draft with visible cursor at row 0, column 31. The shell, kernel PTY,
and native grid all reported 105 columns by 43 rows, and the receiver's
foreground process group matched the cloned shell. After the source exited,
typing `X` in Hardcore inserted it at the preserved middle cursor. The exact
edited draft and visible cursor at column 32 passed, with the same three-way
dimension agreement. Captured shell memory and unflushed history matched the
source; the draft was never submitted. These are terminal cell/cursor proofs,
not a pixel comparison.

An independent read-only audit at
`/private/tmp/hc-native-v3-audit-64uutm0v/report.json` replayed both saved screen
decoders and compared every saved observation generation. The five-event
history, including both unflushed sentinels, and captured shell memory matched
through adoption and editing. Initial source/clone geometry was 110 by 32;
both native checkpoints agreed on 105 by 43. Evidence hashes were unchanged.
Source exit is supported by the successful `waitpid` path and recorded result;
the probe does not retain its raw wait status or claim a normal exit code.

The companion state matrix at `/private/tmp/hc-zsh-clone-state-iika4evv`
again passed both isolated Apple-history exit orders and matched captured ZLE
state and undo/redo/yank behavior against the uncloned control. A read-only
audit at `/private/tmp/hc-slurp-state-audit-v5q2c_a8/report.json` independently
compared the raw artifacts: all 46 checks passed, including eleven captured
fields, 28 editor/control comparisons, six shared-history sentinel counts,
and the inherited trap's destination PID. Original artifacts are unchanged
and their SHA-256 hashes are retained in the audit.

This run used disposable script-owned shells and a native Hardcore window.
It did not move an existing Terminal shell or active Claude process, capture
new Terminal scrollback, or establish preservation of existing jobs and file
descriptors. Full-state migration and automatic Terminal-window closure remain
unproven. No further live run is needed to confirm this particular resize fix.

## Consolidated implementation validation (2026-10-15)

The consolidation combines the eight upstream commits through `67b98e7` with
the local Mac install, slurp, Claude adapter, and preservation research work.
The update command keeps upstream's in-process `exec` reload while the separate
slurp protocol transfers PTY descriptors to another window. Shared listener,
rendering, and reload guards must support both paths. Existing Windows
application coverage and the three-layout/local-reflow UI are retained.

The final combined `make mac-offline-test` passed:

| Check | Result |
| --- | --- |
| Build and packaging | Universal arm64/x86_64 executable built without warnings; signed app bundle and Mac release archive created |
| Terminal core | Zero failures |
| xterm.js fixtures | 71 passed; five skipped |
| Display-only reflow | Eleven groups and 500 seeded randomized cases passed |
| Installer | Nine tests passed |
| Release archive updater | Six tests passed |
| Updater CLI dispatch | Four fake-socket tests passed after the final combined run |
| Claude CLI | 20 checks; one optional check skipped |
| Claude handoff helper | 69 tests passed |
| Terminal retirement verifier | 17 tests passed |
| Claude plugin | 71 Node tests passed |
| Offline slurp verification | 172 regression tests, 65 archive robustness cases, and saved-capture replay passed |

The offline verification report is
`/private/tmp/hc-slurp-verification-gk0vdzo6/report.json`. Nine new native
frame/UTF-8/spare-reaper cases include two negative controls and account for
the increase from the first combined run's 163 regression tests. A separate reflow run
under AddressSanitizer and UndefinedBehaviorSanitizer passed the same eleven
groups and 500 randomized cases. Windows cross-compilation syntax checking
also passed with a private `SLURP_VERSION` stub header; this was neither a
complete Windows executable build nor a Windows runtime test.

The final rebuild includes the native follow-up fixes and their headless frame
regression. The four subsequent fake-socket dispatch tests verify all reload
spellings, the plist fallback, exclusion of another installation, and the
private runtime directory. Those tests are now included in `mac-test` and
`mac-offline-test`. The successful live version-3 receiver result above
predates this consolidation. GUI update,
chrome, native handoff, and Terminal automation tests were not rerun in the
restricted environment, and the earlier performance measurements were not
repeated. The offline results do not certify those live paths in the merged
build.

### Saved Claude receipts after an exec reload

The saved handoff proof binds a receiving window to its PID plus `ps` start
time and arguments. An in-process reload preserves the PID but changes its
arguments to the `--mac-adopt` invocation. A later `hardcore claude inspect`
can therefore report `clientAvailable:false` even while the attached Claude
process remains live and `destinationAvailable` is true. This is a limitation
of retained window-identity evidence, not proof that the conversation exited.
The saved draft remains exportable, and an existing live Claude job remains a
recovery option. No identity check was weakened to turn the old receipt into
a new verification.

Active draft restoration blocks reload. Revalidating an already completed
handoff's window receipt across a later exec reload remains separate work;
the current consolidation does not claim that continuity has been established.

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
bench/compare-mac.py /tmp/termcompare                    # floods, redraw CPU and memory against the other terminals
                                                        #   (HC_COMPARE_APP=other.app to measure another hardcore build)
```

`--left` / `--top` are points from the window's top-left corner to the first text cell
(164 / 4 for hardcore: the 160-point tab column plus padding). `--row` is the height of a
text row in points.

macbench needs the Accessibility and Screen Recording permissions, for whatever app it's
run from. It stops rather than type when the target window isn't in front.
