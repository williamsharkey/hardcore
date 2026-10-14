# macOS port (done 2026-10-13, branch `macos`)

hardcore was Win32-only: ConPTY, Direct3D 11, WM_* messages, the Windows
clipboard. It now builds natively on macOS 11+ as a universal `hardcore.app`.
The approach is the one dumbterm used: one translation unit, built as
Objective-C, on Cocoa and `forkpty`. It started with OpenGL, and now draws
with Metal (see "Speed" below).

## What changed

| | |
|---|---|
| Split | `hardcore.c` keeps the shared core: parser, scrollback, reflow, tab column, renderer front end, selection, links. The Win32 code is fenced with `#ifdef _WIN32`. The core calls down through a small platform API: `plat_invalidate`, `plat_client_size`, `plat_flash`, `plat_timer`, `plat_set_title`, `plat_close_window`, `tab_write`, `tab_set_size`, `set_clipboard_utf8`, `open_url`. The renderer backend is `r_flush`, `r_upload`, `r_begin` and `r_present`. |
| macOS layer | `hardcore_mac.m`, included at the end of `hardcore.c`: a CAMetalLayer-backed view with `NSTextInputClient` for keys and input methods, a render thread, `forkpty` with dispatch sources for output and exit, non-blocking pty writes, `NSPasteboard`, drag and drop, menus, and the quit/close confirmation. The shader is `hardcore.metal`. |
| Font blob | The `.incbin` asm now emits Mach-O (`.const_data`, `_` symbol prefix) as well as PE. |
| Shell hooks | zsh: a `ZDOTDIR` whose `.zshenv` restores yours and adds `precmd`/`chpwd` hooks. bash: `rc.bash`. fish: `--init-command`. Other shells: `proc_pidinfo` cwd for new tabs. |
| Tests | `make test` on the Mac runs `--unit`, the xterm.js fixtures (`tests/run_xtermjs.py` now finds `build/hardcore` and skips `wslpath`), and a live `--selftest` through your login shell. |
| Windows | Still cross-compiles from a Mac with brew's mingw-w64 (`make hardcore.exe`). The only behavior change is `mouse_report` taking a modifier mask instead of `WPARAM`; `mouse_mods()` maps it. |

## Verified on this Mac (macOS 15.6.1, M-series, Retina)

- `make test`:
  - `--unit` 0 failed;
  - xterm.js fixtures 71 passed, 0 failed, 5 skipped, same as on Windows;
  - `--selftest` passes with zsh and bash, and `cwd` follows `cd /tmp`.
- Driven through `HARDCORE_CTL`, with screenshots by `screencapture -l`:
  - **Rendering:** truecolor, 256 colors, bold, underline, OSC 8 dotted links, CJK and emoji widths, braille, box drawing; crisp at 2× on Retina.
  - **Tabs:** traffic lights over the tab column; new tab in the same folder; the label follows `cd`.
  - **Status column:** `bench/claude-sim.sh` `blocked` shows `?` and the question; `working` shows the throbber and the detail line.
  - **Claude Code 2.1.296:** runs: the trust dialog, the logo and the prompt render correctly.
  - **Keys** (synthetic `NSEvent`s through `keyDown:`, checked with `cat -v` under `stty raw`):
    - letters, Ctrl+letters, Ctrl+Space/[/;
    - left Option = Meta, right Option composes;
    - Option+←/→ = ESC b / ESC f;
    - Shift/Ctrl arrows `ESC[1;2A` / `ESC[1;5D`, Shift+Tab, F1/F12, forward Delete;
    - Shift+Enter = ESC CR;
    - Ctrl/Option/Cmd+Backspace = ^W / ESC DEL / ^U, Cmd+←/→ = ^A / ^E;
    - Cmd+T / Cmd+W / Cmd+Shift+].
  - **Resize:** rewraps scrollback, and `tput cols` follows.
  - **Throughput:** 1,000,000 lines (`seq`) in about 0.8 s, with the window responsive.

## Bugs found and fixed during the port

- `--replay` segfaulted: a terminal with no shell has `fd == 0` (calloc) and no write queue, and parser replies (DA, DSR) went to `dispatch_async(NULL)`. The tab write and size calls now check for the queue.
- Starting hardcore from inside Claude Code leaked that session's markers (`CLAUDE_CODE_CHILD_SESSION`, `CLAUDECODE`, `CLAUDE_CODE_SESSION_ID`, ...) into every tab, so claude there said "Transcript saving is off — inherited CLAUDE_CODE_CHILD_SESSION marker". `child_env()` now drops them, along with other terminals' session variables and hardcore's own test variables.
- `bench/claude-sim.sh` used GNU-only `base64 -w0`; it now uses `base64 | tr -d '\n'`.
- Menu shortcuts didn't fire when the window wasn't key. The menu items now target the view directly.

## Speed (2026-10-13, see bench/RESULTS-mac.md)

`bench/macbench` is the Mac counterpart of `termbench`. It posts real key events and reads
ScreenCaptureKit frames stamped with their display time. Measured first, then optimized:

| | first port (GL) | now (Metal) | Terminal.app |
|---|---|---|---|
| keypress → screen, p50 / p90 | 27.2 / 36.8 ms | **22.6 / 34.5 ms** | 38.8 / 61.0 ms |
| new tab → prompt | 66 ms | **18 ms** | — |
| launch → prompt | ~168 ms | **~132 ms** | — |
| Cmd+Q → gone | 55 ms | **31 ms** | — |
| cat 18.8 MB `seq` / 31.7 MB color / 18.7 MB CJK | 0.62 / 0.45 / 0.36 s | **0.52 / 0.38 / 0.31 s** | 1.07 / 1.12 / 0.60 s |

The changes:
- Metal, with a render thread that does the waiting for drawables.
- Repaints coalesced and paced to the display refresh.
- Direct non-blocking pty writes.
- Typing that skips the text-input system on a plain keyboard layout.
- A pre-warmed spare shell, with the new tab's first frame held for its prompt.
- The shader compiled at build time.
- The first shell forked before AppKit, with its prompt read before the window shows.
- Cmd+Q without AppKit's teardown.

Conformance didn't move. esctest2 gives 273 passed / 251 failed / 43 known xterm bugs,
the same as on Windows. The xterm.js fixtures pass 71 / 71.

## Known gaps

- **One window.** Cmd+N isn't there. On Windows each window is its own process; here every tab lives in the one window.
- **Input-method preedit isn't drawn.** Dead keys and IME commits work, and the candidate window sits at the cursor, but the marked (uncommitted) text isn't shown in the grid.
- **Fish keeps no spare shell.** zsh and bash do; fish tabs start their shell directly.
- **After a full-atlas reset** (more than 16,384 distinct glyphs seen), the one frame being built at that moment can show stale glyphs until the next one.
- **Not signed or notarized.** It's built locally with an ad-hoc signature, which is fine on the machine that built it. Distributing it needs a Developer ID and notarization.
- **fish is untested here.** fish isn't installed on this Mac.
- **The quit/close confirmation** (when something other than the shell is in the foreground) was exercised only by reading the code: an `NSAlert` can't be driven headlessly.
