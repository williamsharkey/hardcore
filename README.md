# hardcore

A terminal for WSL on Windows 11, built for running Claude Code. It grew out
of [dumbterm](https://github.com/williamsharkey/dumbterm) (and was called
wsl-easy for a while). It does one job, running your WSL shell, and it isn't a
jack of all trades. There are no options, themes or settings UI: one font, one
look, one size, one layout.

```
 ─  □  ×  │ wm@w11:/mnt/c/Users/you/github/thing$ claude
▌⣿ thing  │
   tmp    │  ...
 ⍾  notes │
   +      │
          │
```

- **Starts in `/mnt/c/Users/<you>`** (your `%USERPROFILE%`), not in `~` inside the distro.
- **Tabs run down the left edge**, like Chrome's vertical tabs. That column is
  all the chrome there is: window buttons at the top, tabs, then **+**. The
  terminal gets the whole height. Snap layouts still appear when you hover
  over maximize, and the empty part of the column drags the window.
- **+** (or Ctrl+Shift+T) opens a new WSL tab **in the same folder as the
  tab you're in**.
- **A tab's label is its folder's name.** Nothing else: no icons, no
  program titles, no `✳ Claude Code — …` noise.
- **One status cell before the name:**
  - **Throbber.** While Claude Code works, its tab shows a braille drip:
    dots fall, stack up and drain. The signal is Claude's `◐`/`◑` title
    spinner or OSC 9;4 progress, and any program that spins its title or
    reports progress gets it too.
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

## Claude Code

hardcore was checked against what Claude Code 2.1 actually sends and asks
for, recorded inside it:

| Claude Code uses | hardcore |
|---|---|
| 24-bit color for its theme | `COLORTERM=truecolor` is exported. Over WSL nothing else says so (WT relies on `WT_SESSION`), and without it chalk drops to 256 colors |
| OSC 8 links (file paths, docs) | Ctrl+click opens them. `FORCE_HYPERLINK=1` is exported so supports-hyperlinks says yes |
| XTVERSION (`CSI > 0 q`), DA1/DA2, OSC 11 background query | answered: `hardcore(0.3)`. The replies make it back through ConPTY |
| Synchronized output (2026) | frames are held until the update ends (200 ms cap), so redraws don't flicker |
| Focus events, bracketed paste, SGR mouse | yes |
| Title spinner, OSC 9;4 progress | the tab throbber, and the bell mark when it stops |
| Shift+Enter for a newline | sends ESC CR, which Claude reads as newline-don't-submit. No `/terminal-setup` needed |
| Shift+Tab (mode cycle), Esc, Ctrl+letters, Alt+letters | passed through untouched. Ctrl+V stays Claude's own image paste |
| Images | Ctrl+Shift+V or right-click with an image on the clipboard saves it as a PNG (`%LOCALAPPDATA%\hardcore\pasted`) and types its path. Copied or dropped files type their WSL paths |

Claude Code turns on mouse reporting, so a plain drag goes to Claude. Hold
**Shift** to select text with the terminal instead.

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
| Ctrl+Alt+1…9 | Jump to tab (9 = last) |
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
wsl.exe --cd "<folder>" -e bash --rcfile %LOCALAPPDATA%\hardcore\rc.bash -i
```

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
- `bench/termbench.exe` measures any terminal window from outside: keypress → frame
  (DXGI desktop duplication, so the times come from present timestamps), new tab →
  prompt, cold launch, plus screenshot and typing helpers. See
  [bench/RESULTS.md](bench/RESULTS.md).

## What carried over from dumbterm, and what changed

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
