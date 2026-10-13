# wsl-easy

A tabbed terminal for WSL on Windows 11, grown out of
[dumbterm](https://github.com/williamsharkey/dumbterm). It does one job: run
your WSL shell. It isn't a jack of all trades.

- **Starts in `/mnt/c/Users/<you>`** (your `%USERPROFILE%`), not in `~` inside the distro.
- **Tabs.** Click **+** (or press Ctrl+Shift+T) and the new WSL tab opens in
  **the same folder as the tab you're in**.
- **Tab labels** show the folder name, or the program's own title while one
  is running (for example, Claude Code).
- An inactive tab that produces output gets an amber dot, so you can see when
  Claude is working in another tab.
- One static `.exe` (about 2.3 MB) with the full GNU Unifont BMP built in.
  There's nothing to install and no runtime dependencies.

## Build

From WSL, with `mingw-w64` and `python3` installed:

```bash
make            # → wsl-easy.exe
make selftest   # headless: ConPTY → wsl.exe → bash → cwd tracking → VT parser
make install    # copy to %LOCALAPPDATA%\wsl-easy + Start menu shortcut
```

## Keys

| Keys | Action |
|---|---|
| Ctrl+Shift+T, or click **+**, or double-click the empty tab bar | New tab in the current folder |
| Ctrl+Shift+W, or middle-click a tab, or click its × | Close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl+Alt+1…9 | Jump to tab (9 = last) |
| Ctrl+Shift+C / Ctrl+Shift+V | Copy / paste |
| Right-click | Copy the selection if there is one, otherwise paste (conhost habit) |
| Shift+Insert / Ctrl+Insert | Paste / copy |
| Drag / double-click | Select / select word (hold Shift to select inside mouse-aware apps) |
| Shift+PgUp / Shift+PgDn, mouse wheel | Scrollback |
| Ctrl+= / Ctrl+- / Ctrl+0, Ctrl+wheel | Zoom (integer pixel scale) / reset to DPI default |
| Shift+Enter | Sends ESC CR: a newline in Claude Code instead of submitting |
| Ctrl+Backspace | Delete the previous word (^W) |

F-keys, Ctrl+letters and Alt+letters go to the shell untouched.

## How "same folder" works

Each tab runs:

```
wsl.exe --cd "<folder>" -e bash --rcfile %LOCALAPPDATA%\wsl-easy\rc.bash -i
```

`rc.bash` is rewritten on every launch. It sources your startup files the way
a login shell would (`/etc/profile`, then `~/.bash_profile`, `~/.bash_login`
or `~/.profile`), then adds one function to `PROMPT_COMMAND` that prints
`ESC ] 9 ; 9 ; $PWD ESC \` at each prompt. That's the same OSC that Windows
Terminal uses for "duplicate tab", and ConPTY passes it through. wsl-easy
remembers the path, and **+** hands it to `wsl.exe --cd`. Your dotfiles are
never edited.

If your login shell is zsh or fish, the tab still opens bash.

## Settings

`%LOCALAPPDATA%\wsl-easy\config.ini` is written when the window closes:

```ini
scale=0          # 0 = follow monitor DPI; 1, 2, 3… = fixed pixel scale
dir=             # folder for new windows; empty = %USERPROFILE%
distro=          # WSL distro; empty = default
width=… height=… # last window size
```

The command line overrides it: `wsl-easy.exe [--cd DIR] [--distro NAME] [DIR]`.

## What carried over from dumbterm, and what changed

| | dumbterm (W7) | wsl-easy (W11) |
|---|---|---|
| Child I/O | anonymous pipes + `--require` JS shim faking `isTTY` | ConPTY: a real pty for the WSL shell |
| VT parser | cursor/erase/SGR | adds scroll regions, IL/DL/ICH/DCH/ECH, alt screen (1049), DEC line drawing, DECSC/DECRC, DSR/DA replies, mouse (1000/1002/1003/1006), bracketed paste, focus events, sync output (2026), OSC 0/2/7/9;9/10/11/52, cursor shapes |
| Glyphs | 609-glyph Unifont subset in a header | the full Unifont BMP (57k glyphs), uploaded to a GL atlas the first time each glyph is drawn |
| Wide chars | squeezed into one cell | real 2-cell CJK and emoji widths; 16-px symbols in 1-cell slots are still shrunk the dumbterm way |
| Renderer | immediate-mode GL, amber cursor and scrollbar | same, and it redraws only when something changes (no busy loop) |
| Scrollback | fixed 5000 × 400-cell ring | 20,000 lines, each trimmed to its used width |

Known gaps: emoji outside the BMP render as `�`, because the plane-1 Unifont
file isn't bundled. There's no reflow on resize, and tabs can't be reordered
by dragging.

## Font

GNU Unifont 13.0.06, dual licensed under GPLv2+ with the font embedding
exception or the SIL OFL 1.1. See `font/unifont-13.0.06-license.txt`.
