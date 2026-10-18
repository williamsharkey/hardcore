# Other terminals: their bugs, our tests, their features

An ongoing list, added to each round. Hardcore reads other projects' issue
trackers and test suites (Ghostty, kitty, WezTerm, Alacritty, foot, iTerm2,
Windows Terminal, VTE, xterm.js, libvterm, esctest, tmux, zellij), keeps what
applies to its design as a fix and a test, and notes their features with a
recommendation.

Each fix below has a test named after its source: the `unit_issues` cases in
`hardcore.c` (`hardcore --unit`), `tests/test_grid_layout.c`,
`tests/test_grid_stress_mac.py` and `tests/test_input_mac.py`.

## Round 2 — 2026-10-18: the grid, input, and esctest/libvterm

### Grid panes (tmux, zellij, WezTerm, kitty, Ghostty, iTerm2, Windows Terminal)

| Bug | Source | Fix |
|---|---|---|
| A pane that isn't focused never repainted when its program printed | wezterm#6869 | output in any grid pane asks for a frame; panes on screen aren't marked unread |
| Switching tabs or panes sent no focus in/out (DECSET 1004), so vim and Claude Code in the pane left still thought they had focus | zellij#5438, tmux#3506 | `activate()` sends CSI O to the tab left and CSI I to the one chosen, by tab ID so a closed tab's successor still hears CSI I |
| Right and middle clicks and drops went to the focused pane, not the one under the pointer | ghostty#2263 | they pick the pane under them first, as a left click does |
| Any-motion reports (1003) went to the focused pane while the pointer was over another | wezterm#8258 | not reported while the pointer is over another pane |
| An invisible scrollbar strip at every pane's right edge ate clicks and jumped scrollback | iTerm2#11265 | no strip in the grid (none is drawn) |
| Clicking a divider without moving it moved it a cell | ghostty#3020 | the drag follows the pointer from where it grabbed |
| Closing a pane revealed a hidden one | — | the next active pane is a visible one |
| Scrolling over a pane took focus (and trackpad momentum crossing a divider moved it) | iTerm2#11009, wezterm#3398 | the pane under the pointer scrolls; focus stays |
| A dragged divider was lost when the window narrowed for a moment | wezterm#5011 | each axis remembers its last dragged layout for when its count returns |
| A short window left panes only a title row, with a stale pty size | wezterm#4878, tmux#1736 | narrower panes (6 columns or more) before any loses its last line of text |
| Pane titles were cut 21 columns short and showed no bell | ghostty#13301 | room by which buttons show; `3!` marks a pane that wants you, as the tab menu does |

Checked and not affected: mouse reports in panes away from the origin are
pane-relative (iTerm2#5253; now a test); selection stays in its pane;
no gaps or overlaps, and divider ratios don't drift (kitty#10522, kitty#9830);
wide chars at a pane edge; one cursor (ghostty#1500).

Policy kept: the click that picks a pane also reaches its program, as tmux does
(WezTerm can swallow it, wezterm#4414). Tested so it changes only on purpose.

Left as is: every visible pane follows each live-resize step (WinTerminal#20562
stalls with 4+ panes; here, with output flooding all 12 panes, 40 divider
steps are answered in about 0.06 s).

### Input and the pty

| Bug | Source | Fix |
|---|---|---|
| Mid-composition, Return, arrows, Backspace and Esc went to the shell instead of the input method; composed text was never drawn | ghostty#4634 | keys go to the input method while text is marked; it's drawn underlined at the cursor |
| Ctrl+letter did nothing on Russian, Greek and other non-Latin layouts | claude-code#19704, #62628 | the key's ANSI position gives the letter |
| Paste stripped only ESC: ^C, ^D, ^Z, DEL and C1 still reached the shell | oss-security 2023/10/20/2, VTE pastify | paste sends text, tabs and line ends only; a NULL string from AppKit can't crash it |
| F13–F20 (a PC keyboard's PrtSc, ScrLk, Pause) sent private-use characters | Zed fix | xterm's CSI 25~ … 34~; other function keys send nothing |
| Mouse motion was reported every pixel | hterm, xterm | once per cell |
| Shells inherited the app's descriptors (the test control FIFO, Metal's cache files) | kitty, foot | the child closes everything but its pty before exec |
| Copy ended lines in CR LF on macOS (^M in vim) and put a space before a wide char that wrapped | WinTerminal PR #1072 | `\n` on macOS; the W_PAD gap is left out |
| Reading a shell's last output at exit had no bound | — | 50 ms or 8 MB (on macOS the tty is revoked as the shell exits, so this is defence) |

### Escape sequences (esctest, libvterm, vttest, security reports)

| Bug | Source | Fix |
|---|---|---|
| Cmd/Ctrl+click opened any link target: a `.command`, `.app` or `.exe`, or a protocol handler | MS10-007, Electerm CVE-2026-43941 | web and mail open; a file that would run is shown in Finder/Explorer; other schemes ask |
| Any program (an ssh host too) could read the screen back with DECRQCRA | Konsole MR 518, VTE#24 | answered only in tests, as Konsole, VTE and Windows Terminal ship it |
| Program output could add Claude flags a Windows session restore would run (`--dangerously-skip-permissions`) | — | a restore keeps only flags that pick a model or narrow what Claude may do |
| VPA in origin mode ignored the bottom margin | esctest vpa.py | clamped as CUP is |
| DECALN kept a pending wrap and rows' wrap flags | esctest decaln.py | cleared |
| No G1 / SO / SI (vt100, screen, linux terminfo's smacs=^N) | vttest | G1 designation and shifts, saved with the cursor and carried by live reload |
| DEC graphics `_` wasn't a blank | xterm | blank |
| RIS left the alternate screen's old contents | esctest ris.py | emptied |
| DECIC/DECDC/SL/SR split wide chars | — | both halves go |
| REP could write a million cells from 11 bytes | — | at most a screenful |
| C1 controls sent as UTF-8 drew a cell | xterm, VTE | no glyph |
| Live reload of a wide char in a pending wrap erased it | — | the snapshot reprints it from its first cell |

## Round 1 — 2026-10-17: speed, and parser bugs

Measured against Ghostty, kitty, WezTerm, iTerm2 and Terminal.app (see
`bench/RESULTS-mac.md`). The pty is now read off the main queue. Parser fixes
came from Ghostty, Alacritty, WezTerm, kitty and vte: UTF-8 overlongs,
surrogates and values past U+10FFFF; CSI colon sub-parameters and SGR colon
colors; CAN/SUB and ESC inside OSC and DCS; wide-char erase, DCH and ICH; wrap
flags after erase; the cursor and DECSC through reflow; a pending wrap through
resize; repeated BSU; OSC up to 4 MB; the scrolled-back view with a full ring;
1-column resizes; Claude Code's modes when it's killed.

## Still open

| What | Source | Why not yet |
|---|---|---|
| Combining marks and ZWJ sequences are dropped: decomposed accents and NFD Hangul (common in macOS file names) lose characters on screen and in copies | libvterm 10state_putglyph | cells have no room for marks; needs a side table |
| Emoji width: VS16 and ZWJ sequences are 1 or 4 cells here, 2 in Ink (Claude Code), Ghostty, kitty and WezTerm | mitchellh "grapheme clusters" | widths match ConPTY's on Windows; macOS could follow Unicode's emoji widths (or mode 2027) |
| Link IDs are reused after 4096 links, so an old link in scrollback can open a newer target | OSC 8 spec | needs reference counts or a generation per link |
| Program output can still set `claude=`/`claude-args=` (OSC 7700) | — | authenticate rc.bash's OSCs with a per-tab secret it keeps unexported |
| Left Option is always Meta, so `@ [ ] { } | ~` need right Option on German, French and Nordic layouts | Ghostty df97c19a | a setting, or Meta by default only on US/ABC layouts |
| On Windows a big paste writes on the UI thread | WinTerminal | a write queue like macOS's |
| Shift+Enter is ESC CR for Claude Code, which leaves insert mode in vim | claude-code#35692 | the kitty keyboard protocol would end the trade-off |
| `test_tv_mac` (controls) and `test_cutie_mac` (browser, title-drag) fail on main as of 2026-10-18; `test_chrome_mac`'s reload check sometimes finds Settings closed after an in-place reload (on main too: 1 run in 2) | — | predate this round; the reload reopens Settings with a fresh 10 s idle timer, so the cause is still to find |

## Features in other terminals

Recommendation: **adopt**, **variant** (adopt in Hardcore's own shape), or
**skip**, judged by speed, plainness (a font and a color scheme) and Claude
Code. (?) marks a claim to check against that project's current docs.

### Grid and panes

| Feature | Who | Hardcore | Recommendation |
|---|---|---|---|
| Move between panes by keyboard | tmux, kitty, Ghostty, WezTerm, WT | mouse only | **adopt**: Opt/Alt+arrows to the neighbouring pane |
| Zoom one pane without resizing the others | tmux `-Z`, WezTerm, WT | Solo resizes every tab | **variant**: zoom draws the pane full size and resizes only it |
| Swap panes | tmux, WezTerm, WT | no | **variant**: drag a title onto another pane |
| Even out sizes | tmux, kitty, Tilix | no | **adopt**: double-click a divider |
| Pane picker by number | WezTerm PaneSelect | **has it**: Cmd+1–9, and titles carry the numbers | keep |
| Dim the panes that aren't focused | iTerm2, WezTerm, Ghostty | the title is amber | **variant**: a hollow cursor in those panes |
| Broadcast input to every pane | iTerm2, Terminator, tmux | no | **skip**: dangerous next to several Claude sessions |
| Layout presets (main + stack) | tmux, kitty | one automatic grid | **skip** for now; the automatic grid is the point |

### Text and graphics

| Feature | Who | Hardcore | Recommendation |
|---|---|---|---|
| Kitty graphics protocol | kitty, Ghostty, WezTerm (partial), Konsole | APC swallowed | **variant**: PNG transmit and cell placement, drawn with the browser's dithered cell pipeline; Claude Code already asks for it |
| Sixel | WezTerm, iTerm2, foot, WT 1.22+, Contour | no | **skip**: overlaps kitty graphics |
| iTerm2 inline images (OSC 1337) | iTerm2, WezTerm | no | **variant** later, on the kitty decoder |
| Ligatures | most GPU terminals | no | **skip**: bitmap fonts are the product |
| Grapheme clustering (mode 2027) | Contour, Ghostty, foot, WezTerm, kitty | wcwidth | **variant**: keep widths, store marks (see Still open) |

### Shell integration and output

| Feature | Who | Hardcore | Recommendation |
|---|---|---|---|
| OSC 133 marks B/C/D and exit status | Ghostty, kitty, WezTerm, iTerm2, foot, WT | A only (jump to prompt) | **adopt**: unlocks the next two |
| Command-finished notification | Ghostty 1.3, kitty, iTerm2, Warp | `!` on background tabs | **adopt**: from 133;D, when unfocused |
| Copy the last command's output | kitty, WezTerm, Warp blocks | no | **variant**: from 133;C..D, for pasting into Claude |
| Search in scrollback | Ghostty 1.3+, WezTerm, iTerm2, Alacritty, foot, WT | `hardcore here` only | **adopt**: port the here-mode matcher; long transcripts need it |
| Quick-select hints (paths, `file:line`, URLs, hashes) | kitty, WezTerm, Alacritty, foot | no | **adopt**: Claude prints mostly these |
| Open `file:line` in the editor | iTerm2, kitty, WezTerm | `file://` links open | **variant**, with hints |
| Desktop notifications (OSC 9, 777, 99) | iTerm2, kitty, Ghostty, WezTerm, foot | parsed; tab mark and Dock bounce only | **adopt**: a real notification when unfocused, clicking selects the tab |

### Keyboard, sessions, control

| Feature | Who | Hardcore | Recommendation |
|---|---|---|---|
| Kitty keyboard protocol | kitty, Ghostty, WezTerm, Alacritty, foot, WT 1.25 | no | **adopt** flags 1–2: Claude Code, Codex and neovim use it; Shift+Enter's ESC CR could go |
| Secure keyboard entry | Terminal.app, iTerm2, Ghostty | no | **adopt**: a few lines, automatic at password prompts |
| Option as Alt per side / per layout | Ghostty, iTerm2, kitty | left Option is Meta | **adopt** (see Still open) |
| Global hotkey | iTerm2, Ghostty, kitty, WT | no | **variant**: raise Hardcore on the tab that needs you |
| Session restore on macOS | iTerm2, Terminal.app, kitty, WT | Windows only | **adopt**: port the Windows design |
| Remote control API | kitty `@`, `wezterm cli`, `alacritty msg` | `hardcore browser` only | **adopt**: `hardcore tab list/new/send/text` in JSON, for agents |
| Automatic light/dark | Ghostty, kitty 0.38+, WezTerm, iTerm2 | paired themes | **adopt**: the twins make it a few lines |
| Config file watched for hand edits | Ghostty, kitty, Alacritty | settings broadcast to windows | **variant**: reuse the broadcast |
| Background opacity, blur, images | most | no | **skip**: legibility and the speed budget |
| Built-in AI | Warp, iTerm2, Wave | hosts Claude Code | **skip**: improve the hand-off instead |
| tmux control mode | iTerm2 | slurp and host | **skip**: slurp covers it more simply |

Already ahead of the field: live reload that keeps every process, adopting
sessions from other terminals (slurp), Claude Code's state in the tab column,
Chrome-backed browser tabs.

### Top of the list

1. Search in scrollback (desktop)
2. Desktop notifications, and full OSC 133 for command-finished alerts and copying the last output
3. Kitty keyboard protocol, flags 1–2
4. Keyboard pane navigation and zoom in the grid
5. Combining marks stored in cells; emoji widths on macOS
6. Automatic light/dark
7. Quick-select hints with open-in-editor
8. `hardcore tab` remote control
9. Session restore on macOS
10. Kitty graphics subset
