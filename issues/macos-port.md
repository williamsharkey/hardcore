# macOS port (done 2026-10-13, branch `macos`)

hardcore was Win32-only: ConPTY, Direct3D 11, WM_* messages, the Windows
clipboard. It now builds natively on macOS 11+ as a universal `hardcore.app`.
The approach is the one dumbterm used: one translation unit, built as
Objective-C, on Cocoa, `forkpty` and OpenGL.

## What changed

| | |
|---|---|
| Split | `hardcore.c` keeps the shared core: parser, scrollback, reflow, tab column, renderer front end, selection, links. The Win32 code is fenced with `#ifdef _WIN32`. The core calls down through a small platform API: `plat_invalidate`, `plat_client_size`, `plat_flash`, `plat_timer`, `plat_set_title`, `plat_close_window`, `tab_write`, `tab_set_size`, `set_clipboard_utf8`, `open_url`. The renderer backend is `r_flush`, `r_upload`, `r_begin` and `r_present`. |
| macOS layer | `hardcore_mac.m`, included at the end of `hardcore.c`: an `NSOpenGLView` with `NSTextInputClient` for keys and input methods, `forkpty` with dispatch sources for output and exit, a serial queue per tab for writes, `NSPasteboard`, drag and drop, menus, and the quit/close confirmation. |
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

## Known gaps

- **One window.** Cmd+N isn't there. On Windows each window is its own process; here every tab lives in the one window.
- **No pre-warmed spare shell.** A new tab starts its shell directly. That's fast for a stock zsh. A heavy oh-my-zsh config makes it noticeably slower than the Windows spare path.
- **Input-method preedit isn't drawn.** Dead keys and IME commits work, and the candidate window sits at the cursor, but the marked (uncommitted) text isn't shown in the grid.
- **OpenGL is deprecated on macOS.** It still runs everywhere, on Apple silicon through Apple's GL-on-Metal layer. A Metal backend would be about 150 lines: the same four `r_*` functions.
- **Not signed or notarized.** It's built locally with an ad-hoc signature, which is fine on the machine that built it. Distributing it needs a Developer ID and notarization.
- **fish is untested here.** fish isn't installed on this Mac.
- **The quit/close confirmation** (when something other than the shell is in the foreground) was exercised only by reading the code: an `NSAlert` can't be driven headlessly.
