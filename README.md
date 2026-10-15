# hardcore

The **Tab underlay** checkbox in Settings enables a full-width terminal with
right-side tabs beneath the content. Tabs show over whitespace; text obscuring
a tab leaves its initial visible, with the throbber overlaid while busy. Hover
a tab to bring it forward. Unchecked (the default), tabs occupy a fixed left
sidebar. The choice is saved and shared by all windows.

Both layouts use one font-sized grid from the drawable origin (0, 0), with no
padding or reserved scrollbar strip. Only incomplete cells at the right and
bottom remain background.

A terminal for WSL on Windows 11, built for running Claude Code. It also runs
natively on macOS (see [macOS](#macos)). It does one
job, running your WSL shell, and it isn't a jack of all trades. The only
settings are a font, a color scheme, and the tab underlay checkbox, shared
by every window.

```
⡪⢕⡪⢕⡪⢕⡪⢕⡪⢕⡪⢕⡪⢕⡪⢕⡪⢕ │ wm@w11:~/github/thing$ claude          ░ ─  □  × ░
▏⣿thing           × │
  Running make test │ > ...
 ?api               │
  approve Bash: git │
  push origin main  │
 ⍾notes             │
 s                + │
```

- **Close it, open it, and your tabs come back.** Closing the window (or
  Windows shutting down or restarting) saves every tab: its shell (bash,
  PowerShell or cmd), folder and text, and the Claude Code session running in it. The next start rebuilds
  them. Each tab's text is replayed under a dim `── restored ──` line, and
  Claude comes back with `claude --resume`, with the flags you started it with.
  Exiting the last tab yourself means you're done: the next window starts
  fresh. See [Sessions](#sessions).
- **`hardcore` in any shell opens a window right there.** It works in WSL
  (Claude Code's `!` too), PowerShell and cmd. See [Update](#update).
- **Otherwise it opens where you left off.** Launched from the Start menu,
  desktop or taskbar with no saved tabs, a new window starts in the folder the
  last window closed in. The very first time it's `/mnt/c/Users/<you>` (your
  `%USERPROFILE%`), not `~` inside the distro.
- **Tabs run down the left edge**, like Chrome's vertical tabs: tabs, then
  **s** (settings) and **+**. The tab column is 20 cells wide and sits on the terminal's own text
  grid, so each of its rows lines up with a terminal row. Row 0 is a grip of
  dim braille dots: drag the window there. Each tab is a status cell, then the
  folder's name. The active tab is marked by a one-pixel amber line down its
  left edge, drawn over the status cell rather than taking a column. Min/max/close float over the terminal's top-right corner on smoked
  glass. Text runs underneath and only the mouse belongs to them, so no row
  is lost to a title bar. The whole top line works as a title bar: point at
  it and it smokes over and the cursor turns into the move arrows. Drag it to
  move the window, or double-click it to maximize. When the mouse isn't there,
  the text shows clearly. Snap layouts appear when you hover over maximize,
  and the empty part of the tab column drags the window too.
- **Warns before the disk fills.** WSL's disk is a `.vhdx` on your Windows
  drive. When that drive runs out, Linux programs die with "Bus error", and
  then the whole WSL VM goes down, taking every tab with it. Under 20 GB free,
  the bottom of the tab column shows how much is left (amber), and under 5 GB
  it turns red.
- **+** (or Ctrl+Shift+T) opens a new WSL tab **in the same folder as the
  tab you're in**.
- **A tab's label is `folder:program`**, for example `hardcore:codex`.
  At a shell prompt it is just the folder. On macOS, the foreground process
  is checked even when it produces no output; common JavaScript launchers
  show the app name instead of `node`. WSL uses the shell's command reports.
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
  - Drag-resizing with full scrollback costs about 4–5 ms a step, with 1 tab
    or 16. A step rewraps only the newest scrollback and the tab you see.
    Everything else catches up once you let go, and narrowing the window never
    drops history.
  - The tab you're looking at keeps its speed with 63 busy Claude sessions in
    other tabs. Keypress to frame is about 1.3 ms either way.

  Full numbers and the method are in [bench/RESULTS.md](bench/RESULTS.md).
- **Bitmap fonts on the pixel grid.** GNU Unifont 18 at an integer scale
  picked from the monitor's DPI: 115k glyphs, every emoji, and hand-drawn
  Powerline symbols. Or one of seven hand-drawn bitmap fonts (see
  [Settings](#settings)), with Unifont filling in whatever they lack. On a
  Retina or 200% screen, box-drawing lines are 1 device pixel thin.
- **One static `.exe`** (about 8 MB) with the fonts, Windows Terminal's own
  ConPTY and the WSL attachment helper built in. Nothing to install, no
  runtime dependencies to download. It draws with Direct3D 11 on a flip-model
  swap chain.

## Install

In PowerShell:

```powershell
irm https://raw.githubusercontent.com/williamsharkey/hardcore/main/install.ps1 | iex
```

Or from a WSL shell:

```sh
curl -fsSL https://raw.githubusercontent.com/williamsharkey/hardcore/main/install.sh | sh
```

Either one:
- downloads the latest release's `hardcore.exe` and checks it against the
  SHA-256 GitHub publishes for it;
- puts it in `%LOCALAPPDATA%\hardcore`;
- adds Start menu and desktop shortcuts;
- starts it.

hardcore is that one exe. Its font, Windows Terminal's ConPTY and everything
else it needs are built in, and there's nothing else to install. A file
downloaded this way doesn't get the "downloaded from the internet" mark a
browser adds, so there's no "Windows protected your PC" screen.

You can also download `hardcore.exe` from
[Releases](https://github.com/williamsharkey/hardcore/releases) and run it from
anywhere. It isn't code-signed yet, so the first run of a browser download
shows that screen: **More info → Run anyway**. Or build it yourself (see
[Build](#build)); `make install` adds the shortcuts.

### Update

From any shell:

```
hardcore               # a new hardcore window in this folder
hardcore DIR           # ...or in DIR
hardcore update        # install the latest release, like `claude update`
hardcore slurp         # gather live WSL and classic Windows console sessions
hardcore slurp --list  # preview eligible sessions without attaching
hardcore --version     # which release and commit this is
```

`hardcore` works in any WSL shell, Claude Code's `!` included, and in PowerShell
and cmd:
- **WSL:** hardcore's first tab adds a symlink, `~/.local/bin/hardcore`,
  pointing at the installed exe. It isn't made if you already have something
  by that name.
- **PowerShell and cmd outside hardcore:** the installer adds hardcore's
  folder to your user PATH.
- **hardcore's own PowerShell and cmd tabs** have it on their PATH already.

Started from a shell, hardcore opens in that shell's folder and hands the
prompt straight back. WSL would otherwise wait for the window to close.
Started from the Start menu, the desktop or the taskbar, it restores your last
session.

`hardcore update` asks GitHub for the latest release and compares it with this
build's version. If it's newer, it downloads `hardcore.exe`, checks it against
the SHA-256 GitHub publishes for it, and swaps it in. A running exe can be
renamed, so nothing has to close for that.

**Then every open window reloads in place.** The programs in your tabs keep
running through it: Claude, builds and servers don't notice. Each window hands
itself to the new exe:
- **Each tab's ConPTY** (OpenConsole and its pipes) stays alive. Its handles
  pass to the new process, which goes on reading the same pipes, so no output
  is lost.
- **What's on screen** passes over as the escape sequences that rebuild it:
  scrollback, both screens, the cursor, the scroll region, colors, links and
  modes such as mouse reporting and bracketed paste. The new build's internals
  can differ from the old one's. Programs like Claude Code keep a model of the
  screen and send only changes, so it has to come back exactly, cell for cell,
  and `--unit` checks that.
- **The new window** opens exactly where the old one is, with the same tab
  active and each tab scrolled where it was. It draws, and only then does the
  old process end, without closing the pseudoconsoles. So the switch is one
  frame.
- **If anything goes wrong** before that, the old window carries on unchanged
  and `hardcore update` says to close and reopen it.

The first update from a release without this (0.4.x or older) can't reload in
place, because that window doesn't know how. Close and reopen it once: your
tabs come back through [sessions](#sessions). Copies left behind by earlier
updates are deleted on the next update. `--force` reinstalls the latest
release even if this one isn't older.

### macOS live updates

On a Mac, `hardcore update` downloads the latest release's
`hardcore-macos.zip`, verifies GitHub's SHA-256 digest and the app's code
signature, and replaces the app bundle. Each window running that installation
then reloads in place. The shell and foreground programs keep their PIDs and
PTYs; they are never restarted. The window, active tab, both terminal screens,
scrollback, scroll position, and partial terminal escape sequences are carried
over to the new executable. The Cocoa window may briefly disappear while it
is rebuilt; jobs continue throughout.

New macOS tabs export the app's executable directory on `PATH` and reassert
it after shell startup. CLI programs launched in these tabs inherit it; no
per-agent alias, plugin, or shell function is needed. In Claude Code, Codex,
Grok, and Antigravity CLI (AGY), use `!hardcore --version` to check command
access, then `!hardcore update`. The `!` is the CLI's shell-mode shortcut;
at a regular shell prompt, omit it.

`make install` also creates `~/.local/bin/hardcore`, pointing at the installed
app, so shells outside Hardcore can find it when that directory is on their
`PATH`. Merely copying the app does not install that external launcher.
Existing app bundles are kept as backups, and supported running windows
reload after installation. For an old session or a tool that resets `PATH`,
use `~/Applications/hardcore.app/Contents/MacOS/hardcore update` (adjust the
path for another install location). A remote shell or container needs its own
installation; a CLI sandbox must allow the update's network and app-file access.

The macOS live-update implementation is currently a development build feature;
the public shell installer still directs Mac users to build from source.
`hardcore update` follows published GitHub releases, not unpublished branch
commits.

For a locally built version, without waiting for a release:

```sh
make hardcore.app
~/Applications/hardcore.app/Contents/MacOS/hardcore update --from "$PWD/hardcore.app"
```

`hardcore --reload` reloads windows from the executable already installed.
`hardcore --version` reports the release and build. A bare development binary
can use `update --from /path/to/new/binary`; release downloads require an app
bundle. The checkbox, font, theme and shared grid work in either case.

The replacement is checked for protocol compatibility and Metal startup before
installation. A pending paste postpones a window's reload. If `exec` fails,
that window keeps running. The previous installation is kept beside the app
as `hardcore.app.previous`; an adoption or Metal initialization failure can
fall back to that executable. The updater reports any window that did not
acknowledge a successful reload.

**Bootstrap:** older Mac builds have no live-update listener. Install this
support and restart those windows once; subsequent supported updates can
preserve running sessions. The automatic download path also needs a release
containing `hardcore-macos.zip`. `make release-mac` prepares that universal app
archive; building it does not publish a release. `make mac-update-test` runs
GUI/PTY integration tests using disposable installations on a logged-in Mac.

### Slurp: attach running sessions

`hardcore slurp` discovers sessions and adds them to an existing Hardcore
window, or opens a new window. A **slurp** tab reports candidates and skipped
hosts; individual attachment errors remain visible in their tabs. It does not
restart the programs or replay their commands. To select just one session:

```text
hardcore slurp --windows 1234
hardcore slurp --wsl 5678 --distro Debian
```

Add `--list` to either command for read-only discovery. Use the Linux session
leader PID from the listing for `--wsl`, and a Windows console client PID for
`--windows`.

- **WSL:** takes over the existing Linux PTY, including its shell, foreground
  program and background jobs. PIDs, in-memory command history, variables,
  working directory, terminal settings and job control remain intact. This
  works with WSL sessions originally displayed by Windows Terminal as well as
  classic console windows. Discovery visits already-running distributions,
  selects the distro's default user's direct WSL terminals, and excludes
  Hardcore's own sessions, SSH and tmux sessions. Once the tab says **attached
  live WSL session**, the old terminal connection is no longer needed. Shared
  terminal windows are never automatically closed.
- **Classic PowerShell and cmd consoles:** attaches to the original console
  and displays its live screen buffer in a Hardcore tab. The original console
  is hidden only after the screen has been captured and Hardcore has drawn.
  Closing that tab, closing Hardcore, or Hardcore exiting unexpectedly reveals
  the original console again; it does not kill the shell. This preserves
  process memory, shell history, partially edited command lines, cursor, colors
  and available scrollback. QuickEdit selection is captured where the column
  widths match, then cleared in conhost so its paused input/output can resume.
- **Native PowerShell/cmd tabs inside Windows Terminal, IDE terminals and other
  pseudoconsole hosts:** skipped with a report. Those hosts do not expose the
  handoff needed to transfer their native console connection safely. The WSL
  attachment path above is separate and does not have this restriction.

WSL attachment uses the relevant MIT-licensed **reptyr** source routines, built
as part of Hardcore; there is no reptyr installation or download. The static
Linux helper is carried in `hardcore.exe` and extracted into Hardcore's local
application directory when needed. It runs through WSL as root to access WSL's
root-owned relay, checks the target user and process creation time, and leaves
the system's ptrace policy unchanged. It uses whole-terminal takeover rather
than injecting Ctrl-Z, `bg`, `fg` or restart commands. See
[the source and attribution](slurp/README.md).
`hardcore slurp --license` prints the included reptyr license.

Some presentation state cannot transfer. A Linux PTY has no saved screen,
scrollback, selection, hyperlinks or terminal-emulator modes to read; full-screen
programs receive a redraw signal, but may still need their own redraw shortcut.
The original terminal retains its old scrollback. Native console capture is
limited to 20,000 rows / four million cells, the console API's 16-color cell
representation and the currently active screen buffer. It polls at about 10 Hz
and retains the source console's dimensions; Hardcore reflows its display, so
full-screen layouts may differ at another width. Application mouse reporting
and rectangular selections are not translated. Native attached tabs detach
instead of becoming saved/restarted sessions, and must be detached before an
in-place update. WSL attachment currently requires x86-64 Linux and an executable
Windows drive mount inside WSL.


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

## Sessions

Session restoration starts new processes after reopening Hardcore.
[Slurp](#slurp-attach-running-sessions) attaches processes that are already
running. Imported native consoles are excluded from session restoration and
return to their original windows when detached. Attaching a WSL shell does not
install the shell hooks described below into that running shell.

hardcore's `rc.bash` wraps `claude` in a small shell function. When Claude Code
starts, the function tells hardcore which session it is
(`OSC 7700 ; claude=ID`), and again when it exits:
- **A new session:** the wrapper picks the id and passes it as `--session-id`.
- **`--resume ID`:** that id.
- **`--continue`:** the newest transcript in that folder.
- **The picker (`-r` with no id):** unknown, so the tab comes back with
  `claude --continue`.

Subcommands (`claude mcp …`, `claude doctor`), `-p` and `--version` pass
straight through. So does a one-word prompt like `claude hello`, which looks
like a subcommand. Your own `claude` alias or function, if you have one, takes
precedence, and then sessions aren't tracked.

The wrapper also remembers the flags you started Claude with
(`--dangerously-skip-permissions`, `--model opus`, …), but not the session flags
or the first prompt, so that prompt isn't sent again.

On close, hardcore writes `%LOCALAPPDATA%\hardcore\session\`: `session.txt`
(each tab's shell, folder, Claude session and flags) plus each tab's last 3,000
rows of text, and the window's position, size and maximized state, the active
tab and each tab's scroll position. On the next start the window opens where
it was, and each tab is rebuilt:
- **Its text is replayed** with colors and wraps, and rewraps to the new width.
- **The shell starts below it.** ConPTY's inherit-cursor mode makes it start at
  the cursor, not the top. The `── restored ──` line says what comes next:
  Claude resuming, or the command waiting behind ↑.
- **A Claude tab runs `claude <flags> --resume <id>`**, which is also put in the
  shell's history. A session closed before its first message has no transcript
  yet, so it gets a fresh `claude` with the same flags instead.

On restore, an id must be letters, digits and dashes, and flags may contain
only characters plain flags use, because the command goes through `eval`.

Other programs, such as vim or a running build, aren't run again for you.
A command can be one you shouldn't rerun, like a push, a deploy or an `rm`.
Instead, the shell reports each command line as you start it (bash's `PS0`,
`OSC 7700 ; cmd=`), and the restored tab puts the one that was running into its
history and says so:

```
── restored · ↑ reruns: vim -O hardcore.c hardcore_mac.m ──
wm@w11:~/github/hardcore$ █
```

One up-arrow brings it back exactly as you typed it, flags and quotes
included, and Enter runs it. It's added at the first prompt, so it's the
newest entry even though bash loads its history file after hardcore's
startup file. A folder on the command line
(`hardcore.exe DIR`) opens there instead of restoring. With several windows
open, the last one closed is the one restored. Sessions are Windows-only for
now.

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
and Claude Code integration, on Cocoa, `forkpty` and Metal instead of
Win32, ConPTY and Direct3D. It's one universal (arm64 + x86_64) app.

```bash
make            # → hardcore.app (needs the Xcode command line tools and python3;
                #   with full Xcode the shader is compiled ahead of time)
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
- **Opens where you left off.** It opens in the folder the last window closed
  in (`last=`), and the very first time in your home folder. Override it with
  `dir=` in `~/Library/Application Support/hardcore/config.ini`, or with
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

From x86-64 WSL, with `mingw-w64`, a native Linux C compiler with static libc
development files, and `python3` installed:

```bash
make            # → hardcore.exe
make test       # unit tests, xterm.js fixtures, ConPTY and live-attachment tests
make install    # copy to %LOCALAPPDATA%\hardcore + Start menu and desktop shortcuts
```

The Windows build also compiles the WSL attachment code from source using
`LINUX_CC` (default `gcc`). Cross-builds need an x86-64 Linux compiler for that
step. No prebuilt reptyr executable is checked in or downloaded. `make slurp-test`
tests synthetic Linux and Windows consoles, including actual WSL takeover and
closing its original ConPTY; it never attaches to your existing sessions.

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

Point at the gray **s** under the tabs and it reads **settings**; click it.
The tab column then shows two box-drawn tabs, **font** and **theme**, and
under them the name of what you've picked, or of what you're pointing at.

- **font**: a row per font, its letter, name and cell size in device pixels
  (the 2×-scaled ones are listed at their doubled size):

  | Letter | Font | Cell |
  |---|---|---|
  | `U` | GNU Unifont 18 (the default) | 8×16, scaled |
  | `Z` | Tamzen 8×15 | 8×15, scaled |
  | `F` | misc-fixed 7×13 (X11's "fixed") | 7×13, scaled |
  | `t` | Terminus 24 | 12×24, 1:1 |
  | `Y` | Sony 12×24 (X11) | 12×24, 1:1 |
  | `E` | ETL 24 (GNU intlfonts) | 12×24, 1:1 |
  | `M` | Terminus 28 | 14×28, 1:1 |
  | `T` | Terminus 32 | 16×32, 1:1 |

  "Scaled" fonts are multiplied by the display scale like Unifont is: each
  pixel is 2×2 on a Retina screen. The 1:1 ones were drawn for the screen's
  own pixels. Sony and ETL only cover Latin-1, so the rest comes from
  Terminus 24, then Unifont.
- **theme**: a row per family, with the dark scheme's letter and, for the
  families that have one, the light one's: `h` hardcore (Campbell with an
  amber cursor, the default), `d` Dracula, `m` Monokai, `n` Nord, `g`/`G`
  Gruvbox, `s`/`S` Solarized, `o`/`O` One Dark/Light, `c`/`C` Catppuccin
  Mocha/Latte, `k`/`K` Tokyo Night/Day, `r`/`R` Rosé Pine/Dawn. Pointing at
  the row tries the dark one; at the light letter, the light one.

Pointing at a scheme previews it on the whole window. Fonts apply on click,
keeping the terminal and tab controls on the same grid at all times.
Clicking picks it for every hardcore window at once. The **Tab underlay**
checkbox below the list switches between the fixed left sidebar and the
right-side underlay; it is saved as `tab_underlay=0` or `tab_underlay=1`.

`%LOCALAPPDATA%\hardcore\config.ini` (on a Mac,
`~/Library/Application Support/hardcore/config.ini`) holds them, along with
what's written when the window closes:

```ini
dir=             # folder for new windows; empty = %USERPROFILE%
last=            # the folder the last window closed in; wins over dir= (empty it to use dir=)
distro=          # WSL distro; empty = default
shell=           # powershell = PowerShell tabs instead of WSL
width=… height=… # last window size
font=U           # a font's letter
theme=h          # a color scheme's letter
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

The other fonts are BDFs in `font/`, packed by `tools/gen_fonts.py` into
`build/fonts.bin` with only their own glyphs:
- Terminus 4.49.1 by Dimitar Toshkov Zhekov (`font/terminus/`, SIL OFL 1.1).
  Its 28 and 32 sizes draw box lines 2 pixels wide, so their box drawing is
  redrawn from the Unicode names with 1-pixel lines on Terminus's own center
  lines.
- Tamzen 8×15 by Suraj N. Kurapati, from Tamsyn by Scott Fial (`font/tamzen/`,
  free to use, copy, modify and distribute).
- misc-fixed 7×13 from X.Org (`font/misc-fixed/`, public domain).
- Sony 12×24 from X.Org's font-sony-misc (`font/sony/`, Sony's permission
  notice).
- ETL 24 Latin-1 from GNU intlfonts 1.4.2 (`font/etl/`, Electrotechnical
  Laboratory, freely usable).

At scale 2, the scaled fonts' box drawing (U+2500–257F) is redrawn with
1-pixel lines as it's drawn: each lit pixel of the glyph becomes a point,
joined to its neighbors, so the font's own corners and double lines stay.

The color schemes come from
[iTerm2-Color-Schemes](https://github.com/mbadolato/iTerm2-Color-Schemes)
(MIT).

Character widths come from Unicode 17 (`font/ucd`, generated by
`tools/gen_width.py`). They use the classic wcwidth rule, which ConPTY also
measures with.

## License

MIT, see [LICENSE](LICENSE). The embedded Unifont (GPLv2+ with the font
embedding exception, or OFL 1.1), the other fonts (see [Font](#font)),
ConPTY (MIT), and the adapted [reptyr source](slurp/reptyr/COPYING) (MIT) keep
their own licenses. See [attachment source and attribution](slurp/README.md)
for the reptyr revision and local changes.
