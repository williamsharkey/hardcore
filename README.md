# hardcore

A terminal for WSL on Windows 11, built for running Claude Code. It also runs
natively on macOS (see [macOS](#macos)). It does one
job, running your WSL shell, and it isn't a jack of all trades. There are no options, themes or settings UI: one font, one
look, one size, one layout.

```
▌⣿ thing            │ wm@w11:~/github/thing$ claude          ░ ─  □  × ░
  Running make test │ 
 ? api              │ > ...
  approve Bash:     │ 
  git push origin   │ 
 ⍾ notes            │ 
  +                 │
```

- **Starts in `/mnt/c/Users/<you>`** (your `%USERPROFILE%`), not in `~` inside the distro.
- **Tabs run down the left edge**, like Chrome's vertical tabs: tabs, then
  **+**. Min/max/close float over the terminal's top-right corner on smoked
  glass. Text runs underneath and only the mouse belongs to them, so no row
  is lost to a title bar. Snap layouts appear when you hover over maximize,
  and the empty part of the tab column drags the window.
- **+** (or Ctrl+Shift+T) opens a new WSL tab **in the same folder as the
  tab you're in**.
- **A tab's label is its folder's name.** Nothing else: no icons, no
  program titles, no `✳ Claude Code — …` noise.
- **One status cell before the name**, plus a dim line under it saying what
  Claude is doing, asking or just did (see [Claude Code](#claude-code)):
  - **Amber `?`.** Claude is blocked on you: a permission, a question, a
    login. Its question is shown under the name, two lines if needed.
  - **Red `!`.** The turn failed (API error, overloaded, ...).
  - **Throbber.** Claude is working: a braille drip, where dots fall, stack
    up and drain. Programs that spin their title or send OSC 9;4 progress get
    it too.
  - **Bell mark.** An amber bell appears when a background tab finishes (its
    throbber stops) or rings (BEL, or an OSC 9 / 99 / 777 notification). If
    hardcore isn't the focused window, its taskbar button flashes too. The
    mark clears when you look at the tab.
  - **Gray dot.** Plain output in a background tab.
- **Fast where you feel it.**
  - A new tab's prompt is there within a frame or so (6–13 ms, against 127 ms
    in Windows Terminal), because a pre-warmed shell is waiting.
  - Keypress to screen is about 8 ms p50 (WT: 16 ms).
  - A cold launch puts a usable window up in about 60 ms.

  Full numbers and the method are in [bench/RESULTS.md](bench/RESULTS.md).
- **One font.** GNU Unifont 18 at an integer scale picked from the monitor's
  DPI: 115k glyphs, every emoji, and hand-drawn Powerline symbols.
- **One static `.exe`** (about 6 MB) with the font and Windows Terminal's own
  ConPTY built in. Nothing to install, no runtime dependencies. It draws with
  Direct3D 11 on a flip-model swap chain.

## Install

Download `hardcore.exe` from
[Releases](https://github.com/williamsharkey/hardcore/releases) and run it.
It's one file with nothing to install; put it wherever you like and pin it to
the taskbar. The exe isn't code-signed yet, so the first run shows Windows'
"Windows protected your PC" screen: **More info → Run anyway**.

Or build it yourself (see [Build](#build)); `make install` adds Start menu
and desktop shortcuts.

## Claude Code

hardcore was checked against what Claude Code 2.1 actually sends and asks
for, recorded inside it.

**Program status (OSC 7501).** Claude Code asks the terminal `OSC 7501 ; ?`.
A terminal that answers gets told, as things change:
- the session's state (`working`, `blocked` with a kind such as permission
  or question, `done`, `idle`, `error`);
- the message that goes with it (`approve Bash: git push`, the turn's
  summary);
- its background tasks.

hardcore answers, and that's what drives the tab column:
- the `?`, `!` and throbber marks;
- the dim detail line under each name;
- **Ctrl+Shift+J**, which jumps to the next tab that needs you (blocked
  first, then failed, then finished);
- a flashing taskbar button when Claude stops to ask while you're in
  another window.

The title-spinner guess is only a fallback, for older versions.

**Turns and prompts (OSC 133).** Each shell prompt is marked, and so is
each Claude turn when Claude marks it. **Ctrl+Shift+↑ / ↓** scrolls back
and forth between them.

| Claude Code uses | hardcore |
|---|---|
| 24-bit color for its theme | `COLORTERM=truecolor` is exported. Over WSL nothing else says so (WT relies on `WT_SESSION`), and without it chalk drops to 256 colors |
| OSC 8 links (file paths, docs) | Ctrl+click opens them. `FORCE_HYPERLINK=1` is exported so supports-hyperlinks says yes |
| XTVERSION (`CSI > 0 q`), DA1/DA2, OSC 11 background query, cell size (`CSI 16 t`), DECRQM 2026 | answered: `hardcore(0.3)`. The replies make it back through ConPTY |
| Synchronized output (2026) | frames are held until the update ends (200 ms cap), so redraws don't flicker |
| Focus events, bracketed paste, SGR mouse | yes |
| Title spinner, OSC 9;4 progress | the tab throbber, and the bell mark when it stops |
| Shift+Enter for a newline | sends ESC CR, which Claude reads as newline-don't-submit. No `/terminal-setup` needed |
| Shift+Tab (mode cycle), Esc, Ctrl+letters, Alt+letters | passed through untouched. Ctrl+V stays Claude's own image paste |
| Images | Ctrl+Shift+V or right-click with an image on the clipboard saves it as a PNG (`%LOCALAPPDATA%\hardcore\pasted`) and types its path. Copied or dropped files type their WSL paths |

Claude Code turns on mouse reporting, so a plain drag goes to Claude. Hold
**Shift** to select text with the terminal instead.

## What it needs

- **Windows 11.** Windows 10 1903 or later should work (ConPTY is bundled),
  but it's untested and the corners are square. x64; ARM64 Windows runs it
  under emulation.
- **WSL with any distro.** WSL 1 and WSL 2 both work, and so does a custom
  drive mount root (`[automount] root=` in wsl.conf).
  - With no distro installed, the first tab is a welcome screen: **I** runs
    `wsl --install` (Windows asks for admin), **P** switches to PowerShell.
  - A distro without bash (Alpine) runs its own default shell. Tabs then
    can't follow `cd`.
  - If the shell dies at startup, its error stays on screen with **Enter**
    to retry, instead of the window vanishing.
- **No WSL at all?** `shell=powershell` in config.ini, or **P** on the
  welcome screen, gives the same terminal with PowerShell 7 (or Windows
  PowerShell) in every tab. New tabs still open in the current folder, and
  Claude Code for Windows gets the same integration.

See [PRODUCT.md](PRODUCT.md) for what sharing it more widely would take.

## macOS

The same terminal runs natively on a Mac: same parser, tab column, Unifont
and Claude Code integration, on Cocoa, `forkpty` and OpenGL instead of
Win32, ConPTY and Direct3D. It's one universal (arm64 + x86_64) app.

```bash
make            # → hardcore.app (needs the Xcode command line tools and python3)
make install    # → ~/Applications/hardcore.app
make test       # unit tests, xterm.js fixtures, headless forkpty + shell selftest
```

What's different on the Mac:
- **Your login shell, not bash.**
  - zsh tabs get two hooks (prompt marks and the folder) through a
    `ZDOTDIR` that hands straight back to your own `.zshenv`, `.zprofile`,
    `.zshrc` and `.zlogin`.
  - bash gets `rc.bash` as under WSL.
  - fish gets an `--init-command`.
  - Anything else runs plain, and new tabs ask the kernel where its shell is.
  - Your dotfiles are never edited.
- **Starts in your home folder.** Override it with `dir=` in
  `~/Library/Application Support/hardcore/config.ini`, or with
  `hardcore.app/Contents/MacOS/hardcore DIR`. A folder dropped on the Dock
  icon opens a tab there.
- **The window's own traffic lights** sit over the tab column. The empty
  part of the column still drags the window, and double-clicking it zooms.
- **Mac keys:**

  | Keys | Action |
  |---|---|
  | Cmd+T, Cmd+W | New tab in the current folder, close tab |
  | Cmd+Shift+] / [, Ctrl+Tab | Next / previous tab |
  | Cmd+1…9 | Jump to tab (9 = last) |
  | Cmd+J | Jump to the tab that needs you |
  | Cmd+↑ / ↓ | Previous / next prompt or Claude turn |
  | Cmd+C, Cmd+V | Copy, paste (an image pastes as a saved PNG's path; Claude Code's own Ctrl+V image paste still works) |
  | Cmd+click | Open a link |
  | Left Option | Meta: Option+B/F/D/Backspace, Option+←/→ by word, Option+Enter for a newline in Claude Code |
  | Right Option | Types é, ü, @, { and the rest, as usual |
  | Cmd+←/→, Cmd+Backspace | Start / end of line, delete to start of line |
  | Shift or Option + drag | Select text inside mouse-aware programs |

- **Closing asks only when something is running.** A tab (or the window)
  with claude, vim or a build in the foreground asks before it closes. A
  tab sitting at a prompt just closes.
- Pasted images go to `~/Library/Application Support/hardcore/pasted`.

`HARDCORE_CTL=/path/fifo` turns on a test channel. It's the Mac side of
`termbench`: `type`, `key`, `cmd`, `snap` and `winid` lines drive a window
that doesn't have focus (see the comment in `hardcore_mac.m`).

## Build

From WSL, with `mingw-w64` and `python3` installed:

```bash
make            # → hardcore.exe
make test       # unit tests, xterm.js conformance fixtures, headless ConPTY selftest
make install    # copy to %LOCALAPPDATA%\hardcore + Start menu and desktop shortcuts
```

## Keys

| Keys | Action |
|---|---|
| Ctrl+Shift+T, or click **+** | New tab in the current folder |
| Double-click the empty part of the tab column | Maximize / restore (it's the title bar) |
| Ctrl+Shift+W, or middle-click a tab, or click its × | Close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+Alt+1…9 | Jump to tab (9 = last). AltGr+digit still types `{ [ ] }` on layouts that need it |
| Ctrl+Shift+J | Jump to the next tab that needs you: Claude blocked, failed, or finished |
| Ctrl+Shift+↑ / ↓ | Previous / next prompt or Claude turn in the scrollback |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste. A clipboard image pastes as the path of a saved PNG; copied files paste as their paths |
| Right-click | Copy the selection if there is one, otherwise paste (conhost habit) |
| Shift+Insert / Ctrl+Insert | Paste / copy |
| Drag / double-click | Select / select word (hold Shift to select inside mouse-aware apps) |
| Shift+PgUp / Shift+PgDn, mouse wheel | Scrollback |
| Ctrl+click | Open a link: an OSC 8 hyperlink (dotted underline) or a URL in the text. `file:///mnt/c/…` opens in Explorer |
| Drop files from Explorer | Types their WSL paths, quoted if needed: `'/mnt/c/My Stuff/a.png'`, `\\wsl.localhost\Debian\home\x` → `/home/x` |
| Shift+Enter | Sends ESC CR: a newline in Claude Code instead of submitting |
| Ctrl+Backspace | Delete the previous word (^W) |

F-keys, Ctrl+letters and Alt+letters go to the shell untouched.

## How "same folder" works

Each tab runs:

```
wsl.exe --cd "<folder>" -e bash -c 'exec bash --rcfile "$HARDCORE_RC" -i'
```

`HARDCORE_RC` is `%LOCALAPPDATA%\hardcore\rc.bash`, handed over through
`WSLENV` so WSL translates the path for whatever mount root you use.

`rc.bash` is rewritten on every launch. It sources your startup files the way
a login shell would (`/etc/profile`, then `~/.bash_profile`, `~/.bash_login`
or `~/.profile`), then adds one function to `PROMPT_COMMAND` that prints
`ESC ] 9 ; 9 ; $PWD ESC \` at each prompt. That's the same OSC that Windows
Terminal uses for "duplicate tab", and ConPTY passes it through. hardcore
remembers the path. Your dotfiles are never edited.

A new tab normally doesn't wait for `wsl.exe` at all. Each window keeps one
spare shell that has already started and loaded your profile. The spare
then blocks on a silent `read`. **+** writes the folder to it, it `cd`s there,
and the prompt appears in the same frame as the tab. A fresh spare starts a
second later. If the spare isn't ready yet, the tab starts with
`wsl.exe --cd "<folder>"` as above.

If your login shell is zsh or fish, the tab still opens bash.

## Settings

`%LOCALAPPDATA%\hardcore\config.ini` is written when the window closes:

```ini
dir=             # folder for new windows; empty = %USERPROFILE%
distro=          # WSL distro; empty = default
shell=           # powershell = PowerShell tabs instead of WSL
width=… height=… # last window size
```

The command line overrides it: `hardcore.exe [--cd DIR] [--distro NAME] [DIR]`.
`--inbox-conpty` uses Windows' built-in ConPTY instead of the bundled one, for
comparison.

## Testing

- `tests/run_xtermjs.py` replays xterm.js's escape-sequence fixtures through the
  parser (`hardcore.exe --replay`) and diffs against screens captured from real
  xterm. All 71 pass.
- `bench/run-esctest.sh` runs [esctest2](https://github.com/ThomasDickey/esctest2)
  inside a live window: 273 passing, against 90 for Windows Terminal.
- `bench/termbench.exe send|cmd|snap` drives a hardcore window over
  WM_COPYDATA. It types, opens tabs and snapshots hardcore's own back buffer,
  all without focus or synthetic input. Start the window with
  `--no-activate` and tests never take your keyboard. `bench/claude-sim.sh`
  plays Claude Code's OSC 7501 states.
- `bench/termbench.exe` measures any terminal window from outside: keypress → frame
  (DXGI desktop duplication, so the times come from present timestamps), new tab →
  prompt, cold launch, plus screenshot and typing helpers. See
  [bench/RESULTS.md](bench/RESULTS.md).

## Where it came from

hardcore grew out of dumbterm, an earlier terminal for Windows 7.

### What carried over, and what changed

| | dumbterm (W7) | hardcore (W11) |
|---|---|---|
| Child I/O | anonymous pipes + `--require` JS shim faking `isTTY` | ConPTY: a real pty for the WSL shell, using Windows Terminal's OpenConsole (bundled, MIT) rather than the slow inbox conhost |
| VT parser | cursor/erase/SGR | adds scroll regions, IL/DL/ICH/DCH/ECH, SL/SR, DECIC/DECDC, tab stops, alt screen (1049), DEC line drawing, DECSC/DECRC, RIS/DECSTR, LNM, DSR/DA/DECRQM/DECRQCRA replies, mouse (1000/1002/1003/1006), bracketed paste, focus events, sync output (2026), OSC 0/2/7/8/9;9/10/11/52, cursor shapes |
| Glyphs | 609-glyph Unifont subset in a header | Unifont 18.0.01, planes 0–3 (115k glyphs, all 1,219 emoji-presentation characters), uploaded to a GPU atlas the first time each glyph is drawn |
| Wide chars | squeezed into one cell | real 2-cell CJK and emoji widths; 16-px symbols in 1-cell slots are still shrunk the dumbterm way |
| Renderer | immediate-mode GL, amber cursor and scrollbar | the same immediate-mode drawing on Direct3D 11 (flip-model swap chain, one Draw per frame); it redraws only when something changes |
| Scrollback | fixed 5000 × 400-cell ring | 20,000 lines, each trimmed to its used width. Text rewraps when the window is resized |

Known gaps:
- No left/right margins (DECSLRM) or VT420 rectangle operations.
- No find-in-scrollback.
- Tabs can't be reordered by dragging.
- Huge floods of very short lines drain about 1.8× slower than in Windows
  Terminal. The time is inside ConPTY's hosting, not hardcore's parser
  (see RESULTS.md).

## Font

GNU Unifont 18.0.01 (`font/unifont-18.0.01.hex.gz`): planes 0–3, without
the private-use area. It's dual licensed under GPLv2+ with the font embedding
exception or the SIL OFL 1.1; see `font/unifont-license.txt`.

On top of it, `font/hardcore-extra.hex` holds a few hand-drawn glyphs in the
same style. They come from `tools/draw_extra.py`, whose glyphs are ASCII pictures
you can edit:
- the tab bell;
- the Powerline prompt symbols ` `.

Unifont's private-use area is CSUR (Klingon and others), so it's left out, and
these fill the slots that prompts actually use.

Character widths come from Unicode 17 (`font/ucd`, generated by
`tools/gen_width.py`). They use the classic wcwidth rule, which ConPTY also
measures with.

## License

MIT, see [LICENSE](LICENSE). The embedded Unifont (GPLv2+ with the font
embedding exception, or OFL 1.1) and ConPTY (MIT) keep their own licenses.
