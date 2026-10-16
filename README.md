# hardcore

Experimental browser tabs are available on **macOS 11+**. Press **⇧⌘B** to
open the built-in welcome page, **⌘L** to enter an address, and **⇧⌘M** to switch
between Hardcore and native rendering. **⌘[ / ⌘]** navigate back/forward and
**⌘R** reloads the page. Browser tabs appear below their owning terminal tab and
use the page title and domain as their label. Labels fit the actual grid cells,
including the smaller space beside pagination: repeated site branding is removed,
familiar domains abbreviate when needed, and long titles retain beginning/end text.
Missing titles fall back to a readable URL path and host; query strings and
credentials stay out of labels. Titles update with the live page and survive
app reloads. Hover the top window edge
for grid-aligned back/forward buttons, address input, Go, close, new browser tab,
and the Mono/Native switch. The narrow-window layout uses additional grid rows.

Hardcore mode is the default: a live system WebKit page projects semantic text,
headings, links, lists, code, tables and basic controls into the current bitmap
font, theme and character grid. Blocks wrap around either menu dock and regain
full width outside it. Links have a faint one-device-pixel hairline; hovering removes it, brightens
the link and changes the cursor to a pointing hand. Click links/buttons/controls;
inputs open a grid editor, selects use a grid picker, and text areas support
multiple lines (Control-Return applies).
Tab/Shift-Tab selects visible controls and Return activates them. Wheel, arrows,
Page Up/Down, Home and End scroll; Shift/Option-drag selects grid text. Native
mode displays the real page across the full canvas, with a full-opacity menu
that comes forward on hover. Switching modes retains the same live document. Right-click opens a grid
context menu for copying link locations, opening child browser tabs, copying
selection, navigation, and inspecting elements. **Inspect element** splits off
a lower grid pane with DOM attributes/child nodes, computed styles, and text;
click child nodes or the parent button to navigate the tree. This is a compact
DOM inspector, not a JavaScript console or a full browser developer-tools suite.

Inline images snap to whole character rectangles, up to 24 columns by 8 rows.
Smaller originals stay near their natural device-pixel size, rounded to the nearest
whole-cell rectangle. Image orientation and intrinsic dimensions survive font
changes and window resizing.
Text in the same block wraps beside them. Thumbnails are decoded at bounded
size, nearest-neighbor sampled, ordered-dithered into the theme's 16 terminal
colors plus its foreground/background, and cached between frames. Downloads
are asynchronous, capped at 2 MB per image, with at most 32 cached sources per
page. Embedded base64 PNG/JPEG images work too. Image requests use an isolated
HTTP session; authenticated/blocked assets and unsupported image formats show
a placeholder. Native mode uses WebKit's own complete image presentation.

From a terminal or an agent running inside Hardcore:

```sh
hardcore browser open https://example.com
hardcore browser list
hardcore browser mode TAB_ID native
hardcore browser close TAB_ID
```

Commands return JSON. New terminal shells inherit `HARDCORE_WINDOW_PID` and
`HARDCORE_TAB_ID` so browser tabs attach beneath the invoking terminal. Use
`--window PID --parent TAB_ID` explicitly from another shell; if older shells
have no IDs, a single matching window is selected and its active tab supplies
the parent. `hardcore browser --help` lists commands. After building locally,
reload the existing window with `./hardcore.app/Contents/MacOS/hardcore --reload`
before trying these commands in that window.

This first pass uses its own WebKit cookies/profile, separate from Safari and
Chrome. It does not inherit their signed-in sessions or password-manager
extensions. Successful logins in Hardcore use its persistent WebKit store;
switching Mono/Native keeps that same session. Some providers, including Google,
restrict login in embedded browsers. Native mode is still the embedded WebKit
page, so it does not bypass that restriction. Use the regular browser for sites
that require its existing login. Grid mode covers the main document's first 100,000 characters and 2,000
blocks; canvas, embedded frames, rich editors, combining marks and advanced
controls need native mode. Browser URLs, mode and parent IDs survive an app
reload, but pages reload and their in-memory JavaScript/form state is lost;
terminal sessions keep the existing live-reload behavior. Windows WebView2,
extensions (including Claude's Chrome extension), and an agent DOM/MCP API are
not implemented. Browser layout, projection, command validation and reload
metadata have headless tests; native rendering/input still need desktop review.

Cross-window transfers (`slurp --pid` and `reload --exe`) currently leave a
source window containing browser tabs open and report that it cannot transfer.
Use `hardcore reload` for an in-place update of that window; browser tabs and
terminal sessions are preserved together.

Website-specific helpers live in `browser/adapters/*.js`. Each registers with
`globalThis.hardcoreSiteAdapters.push({matches(host,path), root(document),
skip(element), project(document, context)})`; specific matches precede the generic fallback. The optional
`project` helper emits semantic blocks through `context.emit`, with live runs from
`context.inline` and stable DOM identities from `context.id`. Tables supply an
array of run arrays in `columns`, a shared `table` ID, optional `header` and
`numeric` column bitmask. `gap`, `hang` and `rule` control document rhythm;
widths and wrapping remain native grid decisions. Helpers preserve
the live document. `tools/embed_browser.py` compiles these scripts and the welcome
page into the executable, so users need no runtime script files or Node/Python.

Wikipedia articles use a dedicated document adapter: a quiet masthead, title,
lead paragraphs, compact facts, linked contents, and section headings with dim
rules. Infobox values stay together; tables have shared column widths, numeric
alignment, and one rule under their headers. Narrow tables turn into labeled
records. Grouped headers and row spans retain their meaning, nested tables
flatten into text, and captions, reference links and accessible equation text
stay with the article. Site navigation/edit widgets and duplicate math images
are omitted from the projection. Contents and citation links jump within the
grid document. Lists keep hanging indents while article paragraphs wrap flush.
All styles use the active theme and font; native rendering retains the original
page. Unsupported complex spatial diagrams retain their text but may need native
mode for their original visual relationships.

`make browser-test` exercises six explicitly synthetic Wikipedia structural
fixtures at four widths, on both docks and without a dock, and generates a
readable review at `build/wikipedia-preview/index.html`. They cover biography,
geography, mathematics, population data, prose history and complex spans; they
are not live-page screenshots. `python3 tools/capture_wikipedia.py --representative`
can capture original article HTML and provenance when network access is available.

Drag the single **↕** handle in the header to position the menu. It snaps
to the nearer left or right edge and follows the pointer vertically, keeping
the terminal's full width. The menu uses full opacity wherever content fits
around it. Unreflowable terminal text cuts a continuous notch into the menu,
including the spaces between its words, with a one-device-pixel outline.
Remaining menu words keep their normal colors and opacity. They can shift into
spare space or shorten redundant gaps, retaining at least one space between
words. Incomplete controls are not clickable. Hovering
anywhere in its bounds brings the entire menu forward. Tabs occupy one row each
without horizontal separators; their close button appears only on hover.
On macOS, hovering a tab immediately previews it in the main canvas. Clicking
selects it; leaving the tab list or window, typing, scrolling, losing focus,
minimizing, or resizing restores the selected tab. The menu stays fixed and
fully visible during preview. Hovering neither changes the input destination
nor clears unread activity. A small **·** means output arrived in the background;
the large **●** marks the selected tab. A spinner or **!** can replace these
markers while a program is busy or needs attention. Native browser tabs preview
their cached text projection without navigating or moving keyboard focus.

The macOS header uses two rows of controls in the selected bitmap font:

| Control | Action |
|---|---|
| **↕ move** | Drag the tab menu |
| **⚙** | Toggle Settings |
| **caf / CAF** | Toggle display and system sleep prevention; uppercase and amber when active |
| **+term** | New terminal in the current folder |
| **+www** | Open a new website tab |
| **files** | Open the current folder in the bundled file browser |

The move handle, settings, and CAF occupy the first row; new-tab actions occupy
the second. Gaps between controls are inert. Drag the normal window title band
to move the app window; its pointer stays an arrow.

Isolated right-aligned status lines can slide into blank space on their own
row to expose the menu or a TV, preserving colors, links, and spacing. They
prefer a two-character gutter and use a flush fit when needed. Cursor rows,
code, and aligned tables remain fixed.

Settings and CAF appear in separate small panels called TVs. They prefer a
two-row gap below the tab menu, can join it when space is tight, and consider
nearby positions and the opposite edge. The tab menu has priority. Placement
checks at most twelve positions per TV and keeps existing placements stable;
content-driven moves use a 0.3-second fade out and 0.3-second fade in. Settings
and CAF panels show a ten-second inactivity countdown, paused while hovered,
then fade away. Using a panel restarts its countdown. Closing the CAF panel
does not stop sleep prevention: click **CAF** in the header to stop it.
If even a compact panel cannot fit, it waits for more room. Exposed corners
are rounded, and docked backgrounds and hairlines reach the actual window edge,
including partial character columns.

CAF runs a window-owned `caffeinate -di` assertion. Its panel shows the flags,
elapsed time and power source. It survives a live app update and stops when its
owning window exits; it does not stop separately launched caffeinate processes.
The file browser supports navigation, filtering, sorting, previews, Quick Look,
opening files, new folders, rename, copy, move and Trash; press **?** for keys.
It confirms changes and refuses overwrites. Cross-volume move and Trash are
currently refused while preserving the source; copy can cross volumes.
On macOS, hover a tab and click **□**, two character columns left of **×**, to
pin a **Cutie TV**: a small live view of that same session. Up to four can stay
open alongside Settings and CAF. They use the current font and theme, preserve
terminal colors, wrap text into their compact viewport, and collapse repeated
empty rows. The source process, terminal dimensions, scroll position, selected
tab and unread indicator stay unchanged when a TV opens. Scroll inside a TV to
browse its bounded recent history independently. Automatic Cuties prefer a vertical
stack below the tab menu, in tab-list order even when opened in a different order.
When that stack cannot fit, they use nearby space or the other edge.

Drag **↕** in a TV header (left of **↗** and **−**) to position it on the grid.
The bottom-right grip has a diagonal resize cursor; drag it to change the TV
viewport. A manually positioned TV stays put while room permits. Placement and
resizing avoid the main menu and other TVs. TVs can range from 18×4 to 64×64
cells, limited by available space, without resizing the source terminal or
sending its program a resize event. **Escape**, focus loss, minimize, or window
resize ends a drag at its last valid position.

Click inside a terminal TV to type into that session; its footer turns amber
and says **Typing · Esc returns**. **Escape** or a click elsewhere returns input
to the main tab. **Option–Escape** sends Escape to the TV's program. Paste and
arrow keys work, and mouse-aware programs receive clicks mapped back to current
source cells. Focus also clears on window exit, focus loss, minimize, resize or
tab selection. **↗** expands the source tab; **−** minimizes only the TV.
An open TV has a persistent **□** beside its tab. Hover that row to turn **□**
into **−**, beside the tab’s separate **×** close button. Click **−** to return
the miniature to the tab menu while its session keeps running. A selected or
hover-previewed tab gives its TV a double hairline border. Cuties do
not expire after ten seconds. Their placement stays fixed during interaction;
if the window cannot fit a panel, it waits for room.

Browser Cuties show a compact text projection with clickable links and buttons;
images appear as small markers. Detailed form controls expand their source tab
for editing. Terminal Cuties preserve the application's output rather than
inventing abbreviated controls, so dense full-screen apps may need scrolling
or **↗** for extended work.

Settings options use highlights without horizontal row borders;
the right dock extends offscreen with no outside border. Lists use all available
height and paginate only when needed. Harmless styles on blank cells, such as
dim text after Codex’s “(^t to expand)”, do not obscure the menu.

Both docks use one font-sized grid from the drawable origin (0, 0),
with no padding or reserved scrollbar strip. Only incomplete cells at the
right and bottom remain background.

Local reflow is always enabled and its debug markers are always hidden. It
rearranges recognizable prose around the menu for display only, using spare
space in the paragraph's existing rows, then at most three following blank rows.
If that cannot fit, it can also borrow up to three immediately preceding blank
rows. It prefers a two-character gap beside the menu, falling back to a flush
fit before moving a block upward. Rows owned by other panels, active input,
or earlier reflowed content cannot be borrowed. Prose words stay whole and
unrelated text stays on its original row. A left dock shifts only the rows beside the menu; other rows retain the
full width. Foreground colors, bold/dim/italic
text, ordinary Unicode punctuation, and prose bullets retain their styling and
hanging indentation. Codex “• Ran …” summaries may wrap at existing word spaces
when local space permits. Numbered source/diff rows, file paths, and tool-output
trees also wrap as separate literal records. Numbers and diff signs stay on
their own first row; continuations align with that record's code or output.
Compact colored diff gutters, tool-tree search expressions, opened URLs, and
saved-file records are recognized too. Paths can break after `/`, and argument
lists after unquoted commas or semicolons.
Original glyphs, colors, diff backgrounds, and link identities are preserved.
A local record block moves its output and details footer together, including
Codex's `+ 1 line (⌃t to expand)` and `+ N lines (⌃t to expand)` footers, so blank
rows after the footer are available for continuations. When only
some records fit, the others retain their complete text beneath the menu.
Short Started, Completed, and similar status lines can shift intact beside a
left dock. A record may borrow blank space beyond one such status after its
details footer, without cascading through later sections. Recognized process
tables wrap only the COMMAND column. Proven painted diff continuations stay
joined exactly, including physical midword splits. The empty left wing of the
centered Back-to-bottom control is usable while that control stays fixed.

Uniform shaded message/input panels with a recognized prompt reflow within
their own rows and padding. Their cursor, selection, links, and colors follow
the moved cells. Uncertain panels, ordinary tables, fenced code, and unrelated
input rows retain their original layout; no content is discarded to make room.
Mouse reporting does not disable prose reflow; reported mouse
coordinates map back to the original cells. Ambiguous physical continuations
retain their original layout rather than acquiring guessed word boundaries.

If a notch remains, the menu waits for the screen to settle before looking for a
better position. It prefers a clear position on the same side, then less reflow
and more padding, without shrinking the menu or hiding options. A substantial
improvement fades the menu out for 0.3 seconds, changes the layout once at zero
opacity, and fades it in for 0.3 seconds. Typing, selecting, hovering, dragging,
and ongoing output defer the move; a cooldown prevents repeated repositioning.
When no better fit exists, terminal text keeps priority and the menu stays put.
The original
terminal data, scrollback, and PTY dimensions are unchanged; selection maps
displayed characters back to their source cells.

A terminal for WSL on Windows 11, built for running Claude Code. It also runs
natively on macOS (see [macOS](#macos)). Font, theme, and layout settings are
shared by every window.

```text
┌⠿⠿ hardcore ⠿⠿───────────┐  wm@w11:~/github/thing$ claude
│● thing:claude          ×│
│! api:codex             ×│  > ...
│· notes                 ×│
│Settings +              +│
└─────────────────────────┘
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
- **Tabs dock on either edge**: tabs, then
  **Settings** and **+**. The tab column is 28 cells wide and sits on the terminal's own text
  grid, so each of its rows lines up with a terminal row. Drag the header's
  single ↕ handle to move the menu between edges or up and down. Each tab is a status cell, then the
  folder's name. The active tab is marked by a one-pixel line in the theme's cursor color down its
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
- **One status cell before the name** keeps each tab on a single row
  (see [Claude Code](#claude-code)):
  - **Amber `!`.** The program needs attention, including a permission or
    question from Claude, or a background tab that finished or rang a bell.
  - **Red `!`.** The turn failed (API error, overloaded, ...).
  - **Throbber.** The program is working: a braille drip, where dots fall,
    stack up and drain. Title spinners and OSC 9;4 progress drive it too.
  - **Gray dot.** Plain output in a background tab.
  - **Amber dot and border.** The active tab.
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
  Powerline symbols. Or one of ten alternative bitmap fonts (see
  [Settings](#settings)), with Unifont filling in whatever they lack. On a
  Retina or 200% screen, box-drawing lines are 1 device pixel thin.
- **One static `.exe`** (about 8 MB) with the fonts, Windows Terminal's own
  ConPTY and the WSL attachment helper built in. Nothing to install, no
  runtime dependencies to download. It draws with Direct3D 11 on a flip-model
  swap chain.

## Run inside another terminal

`hardcore here` opens terminal tabs inside your current terminal instead of
creating a GPU window. It works with Python 3.10+ on macOS and Linux, including
WSL. The host supplies the font and character rendering; Hardcore keeps separate
PTYs, screen state, colors, and bounded scrollback for each tab.

```sh
hardcore here
hardcore here --cwd ~/project
hardcore here -- python3 my_program.py
```

Press **Ctrl-B**, release it, then **c** for a new shell, **n/p** to switch tabs,
**1–9** to select, or **?** for help. **x** closes a tab and **q** quits, both
with confirmation. **u/d** scroll history; **]** returns to live output.
**Ctrl-B Ctrl-B** sends a literal Ctrl-B to the program. Tabs are also clickable.
Resizing updates the child PTYs; exiting restores the host terminal.

The Mac app includes this frontend. For a standalone Linux/WSL installation,
run `make install-here` and put `~/.local/bin` on your PATH. This needs no graphics
libraries or cross compiler. If a native Hardcore command already exists as a
symlink, the launcher preserves it for commands other than `here`.

This is an initial terminal frontend: it supports shell tabs and common ANSI,
256-color, truecolor, alternate-screen, mouse, and bracketed-paste operations.
Bitmap-font selection, browser tabs, Cutie TVs, GUI session transfers, and full
terminal-protocol parity remain features of the native app. It does not attach
the host's existing shell or detach its child sessions when it exits.

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

The release download and ConPTY handoff described here apply to Windows.
For Mac live updates and Terminal slurp, see [macOS](#macos).

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

### Slurp: attach running sessions

**Windows and WSL:** the following attachment paths apply to Windows builds.
For Hardcore windows, Terminal.app, tmux, and Claude handoff on a Mac, see
[Slurp on macOS](#slurp-on-macos).

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
- **Classic PowerShell, cmd and Git Bash consoles:** attaches to the original console
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
  attachment path above is separate and does not have this restriction. Git
  Bash inside mintty uses an MSYS PTY and is also skipped; Git Bash in a classic
  Windows console is supported.

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

The [application test matrix](slurp/README.md#application-matrix) covers real
Windows Terminal WSL sessions, including editors and AI CLIs, and native shells.
It distinguishes process/input checks from presentation state and active model
requests that have not yet been tested.


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
- the attention, error and throbber marks on each tab;
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

While running, the Dock icon is a small terminal showing the active
tab's folder and foreground program, such as `wm` / `codex`. The lower line
shows its activity or attention state, with a count when there are multiple
tabs. Switching tabs or foreground programs updates the summary, including
while the window is minimized. Each window runs in its own process and
updates its application's Dock tile.
Terminal bridge helpers run in the background and do not add Dock icons of
their own; their tabs belong to the receiving Hardcore window.

The tile uses the window's theme colors and the selected bitmap font,
including Hairline and both Courier sizes. Theme previews appear in the Dock
too. Glyphs come from the same font tables as the terminal and are fitted to
the tile; their physical size depends on your Dock size. Fresh installs start
with **Gruvbox Dark (`g`)**, and the installed app icon uses its cream and dark
gray colors. Existing saved theme choices are preserved.

**Window → Show Active Tab in Dock Icon** toggles the summary and saves the
choice across windows; `dock_summary=0` in the Mac `config.ini` restores the
standard icon (`1` enables the summary, the default). The tile uses folder and
program metadata, not terminal contents or command arguments. Long labels
are shortened to fit. Updates are checked at most four times per second and
the Dock only redraws when the summary, font, or theme changes; no Dock
animation runs for every terminal output chunk.

### Build and install on macOS

```bash
make            # → hardcore.app (needs the Xcode command line tools and python3;
                #   with full Xcode the shader is compiled ahead of time)
make install    # → ~/Applications/hardcore.app + ~/.local/bin/hardcore
make test       # unit, xterm.js, PTY import-size regression, and shell selftest
```

After installing, run `hardcore` (or `hardcore DIR` to open a folder).
The command is a symlink to the installed app's executable. If `~/.local/bin`
isn't on your PATH, the installer prints the command to enable it in your
current shell and instructions to make it persistent; shell dotfiles are never
edited automatically. With the default install location, enable the command in
the current zsh/bash shell with `export PATH="$HOME/.local/bin:$PATH"`; add that
line to your shell startup file to keep it available in future shells. You can
also launch with `open "$HOME/Applications/hardcore.app"`.
Override the destinations with `make install APPDIR=/path/to/apps BINDIR=/path/to/bin`.
An existing unrelated `hardcore` command at the destination is never overwritten.

### Live updates on macOS

On a Mac, `hardcore update` downloads the latest release's
`hardcore-macos.zip`, verifies GitHub's SHA-256 digest and the app's code
signature, and replaces the app bundle. Each window running that installation
then reloads in place by executing the new binary in the same Hardcore process.
The shell and foreground programs keep their PIDs and PTYs; they are never
restarted. The window, active tab, both terminal screens,
scrollback, scroll position, and partial terminal escape sequences are carried
over to the new executable. The Cocoa window may briefly disappear while it
is rebuilt; jobs continue throughout.

New macOS tabs export the app's executable directory on `PATH` and reassert
it after shell startup. CLI programs launched in these tabs inherit it; no
per-agent alias, plugin, or shell function is needed. In Claude Code, Codex,
Grok, and Antigravity CLI (AGY), use `!hardcore --version` to check command
access, then `!hardcore update`. The `!` is the CLI's shell-mode shortcut;
at a regular shell prompt, omit it.

New tabs advertise `TERM=xterm-256color` and `COLORTERM=truecolor`. They clear
inherited `NO_COLOR`, `FORCE_COLOR`, `CLICOLOR`, and `CLICOLOR_FORCE` so an
automation tool's plain-output settings do not disable an interactive TUI's
colors. Shell startup files can still set an intentional color preference.
Live updates preserve existing processes and their environment: a program
already started with color disabled needs to be restarted with those overrides
unset (resume its saved session where supported).

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

`hardcore reload`, `hardcore --reload`, and `hardcore update --local` reload
windows from the executable already installed without downloading a release.
`hardcore --version` reports the release and build. A bare development binary
can use `update --from /path/to/new/binary`; release downloads require an app
bundle. Font, theme, layout, and shared-grid settings work in either case.

The replacement is checked for protocol compatibility and Metal startup before
installation. A pending paste postpones a window's reload. If `exec` fails,
that window keeps running. The previous installation is kept beside the app
as `hardcore.app.previous`; an adoption or Metal initialization failure can
fall back to that executable. The updater reports any window that did not
acknowledge a successful reload.

**Bootstrap:** older Mac builds have no live-update listener. Install this
support and restart those windows once; subsequent supported updates can
preserve running sessions. The automatic download path also needs a release
containing `hardcore-macos.zip`. `make release-mac` (also `make mac-release`)
prepares that universal app archive; building it does not publish a release. `make mac-update-test` runs
GUI/PTY integration tests using disposable installations on a logged-in Mac.

### Slurp on macOS

Run `hardcore slurp` to discover sessions and use the available transfer route.
It transfers native Hardcore tabs, attaches portable hosts and eligible tmux
sessions, and prepares a cooperative handoff for supported Claude sessions
already running in Terminal. For an eligible Claude source, it installs the
project adapter if needed and queues a request for that exact source session.
A first-time adapter still needs one
`/reload-plugins` in the existing Claude session, then a safely completed turn.
The command reports this pending state; queuing a request is not a completed move.

| Source | Current behavior | Can the source close? |
| --- | --- | --- |
| Hardcore window | Transfers its live PTYs and terminal state. | Donor closes after verified handoff. |
| Ordinary Terminal.app tab | Reports that direct transfer is unavailable; `--bridge` explicitly opens a plain-text view. | Keep Terminal open. |
| Claude already running in Terminal | Installs the adapter if needed and queues a cooperative conversation handoff; see below. | Keep the original shell and Terminal open. |
| `hardcore host` or tmux | Attaches another client to the existing tmux session. | The original client can detach after attachment. |
| Plain iTerm2 or other terminal | Direct takeover is not implemented. | No automatic closure. |

```sh
hardcore slurp --list                 # preview sources
hardcore slurp --terminal /dev/ttys003 # select one Terminal.app source
hardcore slurp --pid 12345            # transfer one Hardcore window
hardcore slurp                        # discover and prepare eligible transfers
hardcore slurp --tmux session-name    # attach a live tmux session explicitly
hardcore slurp --bridge --terminal /dev/ttys003 # explicit plain-text bridge
```

**Plain-text bridges require `--bridge`.** Default slurp leaves unsupported
sources alone and explains what is unavailable. In a bridge, the original tab
and programs remain running; keep Terminal open. Hardcore displays the tab's
live plain text and forwards keyboard input to that specific tab. Closing the
Hardcore tab detaches the bridge without terminating the original job. The
bridge requires macOS Automation permission to read Terminal and Accessibility
permission to forward input. If denied, the command explains the permission
needed. It does not restart the session.

Terminal's scripting interface does not expose cell colours or cursor
position, so these are not preserved in the bridge. It polls screen text,
resizes the source grid to match, and selects the source tab when forwarding
input. This is less faithful than a native Hardcore PTY transfer, especially
for full-screen applications. Avoid typing into the source tab concurrently.
Bridge tabs show `folder:program [bridge]`, using the source process's working
directory and foreground program instead of the receiving helper's home
directory. tmux tabs likewise follow their active pane's directory and program.
An idle shell clears the previous foreground-program label.

**Hardcore-to-Hardcore slurp transfers live PTYs** to a receiving window,
including scrollback, both screens, cursor, modes, prompt marks and partial
escape/UTF-8 sequences. The donor windows close only after the new
window is ready. Windows without handoff support are not eligible.

**tmux sessions stay alive in their tmux server.** Default slurp attaches detached
ordinary tmux sessions and portable `hardcore host` sessions, including hosts
with a connected Terminal client. `--tmux NAME` can also attach an already attached session;
other clients remain connected. `--tmux-socket PATH` selects a nondefault
server. This also works for tmux sessions used in Terminal or iTerm2. Direct
attachment to plain iTerm2 sessions is not implemented yet.

### Portable sessions on macOS

For portable Claude Code, Codex, Grok, or Antigravity sessions, start the
conversation in Terminal with `hardcore host` (requires `tmux` on PATH):

```sh
hardcore host claude                 # also: codex, grok, agy
# Later, from another terminal:
hardcore slurp --list
hardcore slurp --terminal /dev/ttys003
```

`host` runs the command in a private tmux server. Slurp recognizes that
Terminal tab and attaches its live session in Hardcore, preserving the agent,
child processes, PTY, screen and keyboard modes. After attachment, the original
Terminal tab can close; the session also survives Hardcore reloads. This does
not use debugger access or modify signed executables. `hardcore host` without
a command starts a shell. These sessions appear as portable hosts in the slurp
list; a detached host can be attached by its unique session name with
`hardcore slurp --tmux hardcore-…`. Closing a client detaches it; exit the job
inside the session to end it.

`host` must be used **before starting the conversation**. It cannot retrofit
an already-running plain Terminal job. The installed signed Claude Code,
Codex, Grok and Antigravity binaries tested on this Mac deny debugger attach.

### Claude conversation handoff on macOS

Use the same `hardcore slurp` command for an existing Claude conversation in
Terminal; the adapter can be installed after Claude starts:

```sh
hardcore slurp
# Or select one existing Terminal tab:
hardcore slurp --terminal /dev/ttys003
```

Slurp discovers the project's live Claude session, installs both adapter
plugins for that project if needed, and queues a request bound to the exact
session and process identity. If this running session has not loaded the
adapter, run `/reload-plugins` once in Claude. The queued request then reaches
the adapter without a second slurp command or a `/hardcore-slurp` command.
Installation alone does not activate plugins inside an already-running Claude
session. Hardcore does not type a reload command over an unsent draft.

The adapter waits to observe a normally completed turn with no background work,
scheduled tasks or active agents. It prepares a receiving Hardcore window,
checks readiness again, and asks Claude
to background the conversation. The destination restores its title and
acknowledges readiness before Hardcore attaches that exact session.
`/hardcore-slurp status` explains any wait; `/hardcore-slurp cancel`
cancels it. A late-loaded idle conversation needs its next ordinary turn before
readiness can be established; the adapter never sends a model prompt itself.

For diagnosis or selecting a source by Claude's session ID, the lower-level
commands remain available:

```sh
claude agents --json --all --cwd "$PWD" # find the interactive source sessionId
hardcore claude move SESSION_ID         # optional: --project /path/to/project
hardcore claude status                  # inspect this project's adapter installation
```

`move` uses the same project installation, exact-source verification and
one-hour request queue as generic slurp. `/hardcore-slurp wait` also requests
a handoff from within Claude. All routes use the same readiness checks.
Text drafts transfer without being submitted, including multiline
Unicode text and a cursor in the middle. The receiver temporarily holds input
while restoring and checking the exact text and cursor. It also checks a SHA-256
digest of Claude's available rich conversation blocks, including image,
document, tool and thinking blocks, then emits a unique receipt over its
attached PTY. Hardcore verifies the matching tab, PID and TTY and completes
a rendered frame before recording attachment verification and releasing input.
The guard has a deadline and releases on restoration failure. Collapsed paste
and image placeholders refuse before moving because their hidden payloads
cannot be captured through the draft API.

Saved handoffs remain inspectable after a failed attachment:

```sh
hardcore claude moves
hardcore claude inspect REQUEST_ID
hardcore claude recover REQUEST_ID --export-draft saved-draft.json
```

The listing reports saved status; `inspect` separately checks whether the
original source, destination, receiving client and window still exist.
`recover` creates a private JSON file containing the saved draft and cursor,
refusing existing files. These commands do not submit text or restart a job.

This experimental adapter requires the **running source Claude process** to be
version 2.1.300 or newer. Updating the installed `claude` command does not upgrade
an existing process. Slurp reports a known older source version and leaves that
session running without installing or queueing its adapter.
The adapter installs two project-scoped plugins using the official CLI; both must remain enabled during
handoff. It uses Claude's shared plugin cache but does not enable the plugins
in other projects. Managed settings that suppress user-installed hooks can
prevent readiness. See [adapter details](plugins/hardcore-slurp/README.md).

The Claude route preserves the conversation by creating and attaching a new
Claude session. It does not preserve the original Claude PID or transplant its
PTY. Active work finishes before handoff; the adapter does not kill and replay
commands or guess their exit status. It leaves the original Terminal window
open, so it does not trigger Terminal's terminate-processes dialog. If the
receiving client fails after backgrounding, `claude agents` lists the background
session for recovery with `claude attach ID`.

This is not yet automatic Terminal retirement. Claude's message API provides
at most 4096 messages from normalized current context, excluding older compacted
history and system configuration. The draft API does not expose all attachments
or hidden stashes. Terminal's plain-text scrollback,
styled screen/cursor state, shell variables, aliases, jobs and unflushed shell
history are not transferred by this route. A verified Claude attachment is
therefore insufficient to close the source window. The separate retirement
verifier rejects closure under its default full-shell requirement and rejects
remaining source TTY users, including detached jobs holding the slave open.

### Verify macOS slurp

From the repository, a single command runs without input prompts:

```sh
python3 tools/verify_mac_slurp.py
```

For a build and the full deterministic Mac test set without live shell-clone
or GUI probes, run `make mac-offline-test`. It also prepares the signed app
and release archive; it does not install or publish them.

`verify_mac_slurp.py` runs archive and handoff regression tests, Unicode/RTF robustness cases,
replays the latest valid saved Terminal fixture when available, and exercises
shell cloning and a disposable Hardcore receiver window. Each live probe owns
its test shells and PTYs. The native probe uses `build/hardcore`; build it first
with `make` if the report identifies a missing executable.

The native check compares the complete visible Unicode draft and cursor before
the source test shell exits, then verifies an edit at the preserved cursor in
Hardcore. It also requires the shell, kernel PTY, and Hardcore grid to agree on
dimensions, and compares captured shell memory and unflushed history exactly.
The companion shell tests cover editor state, undo/redo, and isolated Apple
session-history paths. These checks passed in the
[audited live run](bench/RESULTS-mac.md#verified-live-native-draft-cursor-and-geometry).

Results go into a new private temporary directory with `report.json`,
`SUMMARY.txt`, and individual logs. Failures, timeouts and skipped checks are
reported separately. `--offline` selects the tests and saved-file replay that
do not launch the shell/GUI probes; `--capture CAPTURE_DIRECTORY` selects a
particular saved fixture. Replaying a prior Terminal copy is historical
evidence, not a new capture of Terminal's current state. Full-state migration
and automatic source-window closure remain outside this verification's proof.

### Remaining Mac transfer research

A separate owned-PTY experiment found that stock macOS `zsh/clone` can copy a
running shell's memory and ZLE draft onto a new PTY after launch. Existing jobs
remain in the original shell, and extra file descriptors need special handling,
so this is not enabled as automatic Terminal slurp. See the
[shell-preservation research](bench/RESEARCH-mac-zsh-clone.md).
Further tests cover Apple session-history collisions, exit hooks and editor
undo/redo in the [shell-state follow-up](bench/RESEARCH-mac-zsh-clone-state.md).
The owned fixtures now also verify a working controlling terminal and native
receiver. They do not account for every existing job, descriptor, shell plugin,
or Terminal presentation state, so full-shell takeover remains experimental.

There is a separate, promising route for **ordinary Codex sessions using its
default shared daemon**: a new frontend can attach to the same live thread.
An opt-in research probe (`python3 tests/probe_mac_agent_attach.py`) has kept
an active tool running after terminating the original Terminal frontend, then
accepted another prompt in Hardcore. This replaces the frontend, not the
original PTY or shell. It is not yet wired into `hardcore slurp`; safe discovery
of the exact source thread and a graceful detach handshake are still needed.
It does not cover `codex --no-daemon`. See the
[unprepared-session results](bench/RESULTS-mac.md#unprepared-agent-session-attachment-2026-10-14)
for the other agents and limitations.

Additional manual Terminal fixtures, hosted-agent checks, and the debugger-based
experimental takeover suite are documented in
[Mac slurp validation](slurp/README.md#macos-validation-and-research).

### Mac behavior and keys

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
- **The window's traffic lights** rest as subtle neutral circles beneath terminal
  text. They lighten dark backgrounds and darken light ones, including colored
  highlights and text selections, without changing glyph pixels. Middle gray
  retains contrast too. Hovering the native title area reveals the real controls and smokes the
  full-width band; drag there to move the window or double-click to zoom. An
  upward text-selection drag keeps the controls and band hidden and continues
  scrolling. The terminal still starts at the drawable origin. The empty
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
`make slurp-app-test` is an opt-in desktop test: it opens disposable application
windows, transfers them, checks input and state, and closes its fixtures. It
requires Windows Terminal and uses whichever tested applications are installed.
See the [test coverage and remaining work](slurp/README.md#application-matrix).
`make font-settings-test` is an opt-in Windows desktop check that clicks every
font size, verifies its saved setting and native cell dimensions, captures D3D
screenshots, and walks the grouped menu in a short window. It uses a disposable
process with private settings and shell history.

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

On macOS, click **⚙** in the tab-menu header; on Windows, click **Settings**
beneath the tabs. The compact **Font / Theme**
strip selects the list below it; a light underline marks the active section.
Tabs and settings use pages when they cannot
fit. Back/forward buttons and the page count share the first actual item row;
the label between them still selects that item. Fonts, themes, and tabs keep
independent page positions. Menu lists do not scroll.

- **font**: sizes are grouped beside their family name. Click a size to apply
  it; the selected size has an amber background. The menu shows width × height
  when the family fits, or just heights in compact rows. Long families continue
  on the next row, and repeat their name at a page boundary. Every size remains
  selectable when the menu is paginated. Sizes reflect device pixels, including
  the current display scale for the three legacy scaled cuts.

  | Family | Cells, in size order | Saved preference keys |
  |---|---|---|
  | GNU Unifont 18 (default) | 8×16, scaled | `U` |
  | Tamzen | 8×15 scaled; 8×16; 10×20 | `Z`, `z`, `x` |
  | Fixed (X11) | 7×13 scaled; 9×18; 10×20 | `F`, `f`, `g` |
  | Terminus | 8×16; 10×18; 10×20; 11×22; 12×24; 14×28; 16×32 | `a`, `b`, `c`, `d`, `t`, `M`, `T` |
  | Sony (X11) | 12×24 | `Y` |
  | ETL (GNU intlfonts) | 12×24 | `E` |
  | Courier (X11) | 15×24; 20×29 | `C`, `R` |
  | Hairline (Terminus-derived) | 12×26 | `H` |
  | UW ttyp0 | 9×17; 9×18 | `p`, `q` |

  "Scaled" fonts are multiplied by the display scale like Unifont is: each
  pixel is 2×2 on a Retina screen. The 1:1 ones were drawn for the screen's
  own pixels. Sony and ETL only cover Latin-1, so the rest comes from
  Terminus 24, then Unifont.
  All ten smaller comparison cuts are included at 1:1 device pixels, including
  the heavier Fixed 10×20. Existing saved font keys keep their meanings. See
  [small font sources and rendering](font/small-fonts.md) for exact metrics,
  native box coverage, fallback behavior, and licenses.
  Courier's native bitmap strokes stay one device pixel wide in straight
  letter stems and bars, even on Retina displays. Curves, serifs, and joins
  can be wider. Its missing or overhanging glyphs use Unifont fitted to the
  selected cell; the 20×29 font also supports 40-pixel-wide Unicode fallback.
  Hairline is the saved Terminus-derived design with lowercase x-height +2,
  capital/digit height +3, and a 26-pixel cell. It preserves one-pixel strokes
  and the original `f` hook, and fills the taller cell with continuous box and
  block graphics. Select **H · Hairline · 12×26** in the font menu, or use
  `font=H` in `config.ini`. Other source letters and punctuation retain their
  original contours; only ASCII letters and digits have enlarged bodies.
- **theme**: a row per family, with the dark scheme's letter and, for the
  families that have one, the light one's: `h` hardcore (Campbell with an
  amber cursor), `d` Dracula, `m` Monokai, `n` Nord, `g`/`G`
  Gruvbox (**`g` Gruvbox Dark is the default**), `s`/`S` Solarized,
  `o`/`O` One Dark/Light, `c`/`C` Catppuccin
  Mocha/Latte, `k`/`K` Tokyo Night/Day, `r`/`R` Rosé Pine/Dawn. Pointing at
  the row tries the dark one; at the light letter, the light one.

Pointing at a scheme previews it on the whole window. Fonts apply on click,
keeping the terminal and tab controls on the same grid at all times.
Clicking picks it for every hardcore window at once. Drag the header's **↕**
handle to choose the dock's side and vertical position. It starts at the chosen
row, clamped to fit the window. After output settles, persistent text intrusions
can trigger a gentle move to a substantially clearer location, preferably on
the same side. Complex text keeps priority in outlined menu cutouts until hover.

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
theme=g          # a color scheme's letter; default is Gruvbox Dark
tab_underlay=1   # 1=right underlay, 2=left underlay; legacy 0 becomes left
dock_row=0       # desired underlay top row (clamped to fit)
```

The command line overrides it: `hardcore.exe [--cd DIR] [--distro NAME] [DIR]`.
`--inbox-conpty` uses Windows' built-in ConPTY instead of the bundled one, for
comparison.

## Testing

- `make reflow-test` exercises local word wrapping, both dock directions,
  source-cell mapping, paragraph boundaries, protected terminal content, menu
  word packing, horizontal status shifts, shaded panels, literal continuation chains, secondary-TV
  composition, bounded placement, stale hit maps, and the placement/fade state machines.
- `make chrome-test` (macOS desktop) checks native menu layout, pagination,
  both underlays, two-row header actions, title-band cursors, whole-menu hover,
  reflow, opaque cutouts, and timed atomic
  relocation using isolated terminal windows. Complex fixtures also verify
  process columns, shaded input, and the actual mapped cursor pixels. TV tests
  cover separate controls, corner/edge pixels, countdown, and CAF placement;
  hover-preview tests use real owned PTYs and local browser pages to verify
  source/unread preservation, no resize or input leaks, and focus-loss cleanup.
  Cutie tests use independent real PTYs to check live colors, keyboard and
  mapped mouse input, source lifetime, four-panel capacity and persistence.
  `tests/test_cutie_project.py`, `tests/test_cutie_cache.py` and
  `tests/test_cutie_browser.py` cover source mapping, Unicode, stale frames and
  browser projection without changing the authoritative source.
- `tests/test_mac_caf.py` checks assertion ownership, independent windows,
  exec updates, process exit, and bounded power queries. `tests/test_files.py`
  checks file operations and drives the curses browser through an owned PTY.

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
- Hairline 12×26 (`font/hairline/`, SIL OFL 1.1), a modified Terminus 24
  with the selected +2 lowercase / +3 capitals design and adapted terminal
  graphics. It uses a distinct family name, retains the original attribution,
  and embeds the complete OFL notice. See its [design and regeneration notes](font/hairline/README.md).
- Tamzen 8×15 by Suraj N. Kurapati, from Tamsyn by Scott Fial (`font/tamzen/`,
  free to use, copy, modify and distribute).
- misc-fixed 7×13 from X.Org (`font/misc-fixed/`, public domain).
- Sony 12×24 from X.Org's font-sony-misc (`font/sony/`, Sony's permission
  notice).
- ETL 24 Latin-1 from GNU intlfonts 1.4.2 (`font/etl/`, Electrotechnical
  Laboratory, freely usable).
- X11 Courier regular from X.Org's Adobe 75dpi and 100dpi fonts
  (`font/courier/`, Adobe/DEC permission notice). These are native 15×24 and
  20×29 line cells. The latter BDF's nominal `PIXEL_SIZE` is 34; its ascent
  and descent total 29. Native pixels are retained; glyphs that would be
  clipped by the cell are omitted so the normal Unicode fallback can draw
  the complete character.

The separate [Hairline Lab](experiments/font-lab/README.md) compares these
bitmaps and experiments with taller Terminus-derived ASCII contours. Its
per-glyph expansion controls do not change the installed fonts.

The [Small Font Lab](experiments/small-font-lab/README.md) compares 11 original
bitmap faces at native device size, focusing on 17–20 px cells for lower-DPI
displays. It includes smaller Terminus, UW ttyp0, X11 Fixed, and Tamzen faces,
terminal/code samples, box-join tests, and a pixel inspector that distinguishes
native glyphs, generated boxes, and Unicode fallback. Build its self-contained
preview with `python3 experiments/small-font-lab/build.py`.

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
