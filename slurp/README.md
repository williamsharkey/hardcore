# Live terminal attachment

The Windows entry point and console bridge are in
[`../hardcore_slurp.h`](../hardcore_slurp.h). Both attachment paths reuse
Hardcore's tabs, terminal model, scrollback and renderer. WSL uses its existing
ConPTY transport to run a Linux relay; native consoles supply screen-buffer
snapshots through a separate bridge. User-facing commands and limits are in
the [main README](../README.md#slurp-attach-running-sessions).

## Linux source and takeover

`linux.c` is Hardcore's WSL session discovery and terminal relay. It links only
the tty-stealing part of reptyr, compiled from the source in `reptyr/`. There is
no downloaded executable, reptyr command-line frontend, runtime compiler, or
package-manager dependency. The build produces a static x86-64 Linux helper
and includes it in the Windows executable. Windows and Linux have different
executable formats and syscall interfaces, so the Linux portion runs inside WSL.

The reference is Nelson Elhage's MIT-licensed
[reptyr](https://github.com/nelhage/reptyr/tree/f4589b3f7bdda8bcc56453bf0748b40225e045da),
commit `f4589b3f7bdda8bcc56453bf0748b40225e045da`. See `reptyr/COPYING`.
Only Linux/x86-64 ptrace support and whole-PTY takeover are retained. Local
changes handle parentheses in `/proc/PID/stat`, extended PTY device numbers,
descriptor cleanup, native-ABI validation and descriptor replacement errors.

The takeover duplicates the PTY master into Hardcore's relay using SCM_RIGHTS,
then replaces the original relay's master descriptors with `/dev/null`. The
shell, foreground process group, children, terminal settings, current directory,
environment and in-memory history stay in the original processes. The session
leader ignores SIGHUP, as in reptyr's `-T` mode. No shell commands, Ctrl-Z/bg/fg
sequences or system-wide ptrace policy changes are needed. Terminal-emulator
scrollback and selections are not stored in a Linux PTY and cannot be recovered
by this mechanism.

Discovery visits running distributions as their default user and selects direct
WSL relay sessions. Attachment checks the target UID and process start time,
locks the target's proc entry against duplicate attachment, and runs as root
inside the distro to access its root-owned relay. The helper is extracted to
`%LOCALAPPDATA%\hardcore\slurp-<hash>.linux`; its cached bytes are checked against
the embedded build before use. It needs an executable Windows mount inside WSL.
The helper is built from source during the Windows build, not fetched or stored
as a prebuilt executable in this repository.

## Classic Windows consoles

A helper process attaches to the existing console and streams its screen
buffer, palette, cursor, title, viewport and selection to Hardcore. The source
window is hidden only after Hardcore receives and renders a snapshot. Input
goes to the original console through Win32 console input records. Captured
QuickEdit selections are cleared at the source so paused input/output resumes.

Closing the tab detaches and reveals the source. An owner-process watchdog also
reveals it if Hardcore crashes. Native attachments are excluded from session
save and must be detached before a live reload. The console's dimensions stay
unchanged; Hardcore reflows the captured cells for its own display. Snapshot
size is bounded to 20,000 rows and four million cells. Native pseudoconsole
hosts, including Windows Terminal's PowerShell/cmd tabs, are skipped because
this bridge cannot safely take over their connection. Automatic discovery also
includes `bash.exe` for Git Bash in a classic console. Git Bash inside mintty
uses an MSYS PTY and cannot use this console bridge.

## Validation

`make slurp-test` builds the executable and runs three integration checks:

- `tests/test_slurp_linux.py` creates synthetic Linux terminals and checks
  process identity, shell memory, background jobs (including stop/resume), input and interrupts, stale
  identities, duplicate attachment, and closure of the old master.
- The native console selftest checks screen state, scrollback, cursor and
  colors, delayed hiding, input, detach, reattachment, and restoration after
  a synthetic owner crash.
- The WSL selftest takes over an actual WSL relay, closes its original ConPTY,
  and checks that the same shell and its in-memory state remain usable.

These tests create and clean up their own sessions. They never attach to user
terminals or change system-wide ptrace policy. `make test` includes these checks
alongside the existing terminal and ConPTY tests. Running them requires WSL
with Windows executable interop and a working default distribution.

## Application matrix

`make slurp-app-test` is a separate, opt-in desktop integration suite. It opens
fresh Windows Terminal windows running WSL applications and attaches them with
the production source-built helper, ConPTY transport and terminal parser. The
test driver is a separate test build, not an alternate implementation of slurp.
Its destination is headless: it checks terminal text/state rather than rendered
pixels or graphical tab interactions.
It closes each original fixture window and checks process creation times,
terminal modes, input and resize behavior. Native fixtures run in classic
conhost windows and verify that detaching reveals the original console.

```sh
make slurp-app-test
# Or select a subset after building build/slurp-driver.exe:
python3 tests/test_slurp_apps.py --apps shell nano vim
python3 tests/test_slurp_apps.py --apps claude grok codex agy
python3 tests/test_slurp_apps.py --apps cmd powershell pwsh git-bash wt-powershell mintty
```

Reports, captured terminal text and fixture files are under `build/slurp-apps/`.
Missing applications are reported as skipped. The tests use existing CLI
installations and credentials; they do not install applications or submit model
prompts. agy's workspace-trust prompt is accepted only for the new disposable
fixture directory. Tests never attach to existing user sessions, write Hardcore
settings, or send global keyboard input.

Validated on 2026-10-14 with Windows Terminal 1.24.12741.0 and Debian WSL:

| Source/application | Checks |
|---|---|
| Windows Terminal / bash | Same shell and background job, variables, history, terminal modes, resize, Ctrl-C after source closure |
| Windows Terminal / nano 8.4 | Same process and terminal modes; cursor position, editing and saving after transfer |
| Windows Terminal / Vim 9.1 minimal build | Unsaved buffer and cursor position retained; edit/save after transfer |
| Windows Terminal / Claude Code 2.1.300 | Live CLI survives source closure; resize and unsent draft input |
| Windows Terminal / Grok 1.0.52 | Live CLI survives source closure; resize and unsent draft input |
| Windows Terminal / Codex CLI 0.162.1 | Live CLI survives source closure; resize and unsent draft input |
| Windows Terminal / agy | Live CLI survives source closure; workspace-trust UI and unsent draft input |
| Classic cmd, Windows PowerShell 5.1, PowerShell 7.6.7 | In-memory variable and input preserved; source console restored on detach |
| Classic Git Bash 5.3.9 | In-memory variable and input preserved; source console restored on detach |
| Native PowerShell inside Windows Terminal | Unsupported connection is rejected; original window remains visible |
| Git Bash inside mintty | Unsupported MSYS PTY is rejected; original window remains visible |

The suite also exercises the discovery pipe closing before the child process
exits. This caught a race where a successful probe could be terminated during
normal teardown. Wrapped screen-text assertions caught an incorrect conversion
between screen wrap flags and scrollback wrap widths.

Remaining work includes transfer during active AI generation/tool execution,
pre-existing multiline CLI drafts, application-specific mouse/paste modes,
Windows-hosted editors, and testing WSL sessions in hosts such as WezTerm and
Alacritty. The matrix verifies idle AI interfaces, not network request continuity
or every UI mode. WSL's old terminal-emulator scrollback and selections still
cannot be recovered from the PTY. Native Windows Terminal and MSYS/mintty session
handoff would require a different implementation or cooperation from the host;
they are not supported by the classic-console bridge.

## macOS validation and research

Mac user-facing commands and the supported source types are in the
[macOS guide](../README.md#slurp-on-macos). Live Hardcore-to-Hardcore PTY
transfer, the Terminal.app bridge, tmux attachment, and the Claude adapter use
different protocols and preserve different state. The research probes below
do not make arbitrary existing Terminal shells eligible for automatic closure.
Run these commands from the repository root.

On macOS, `hardcore slurp` discovers sources and chooses the available transfer
route. For supported ordinary Terminal Claude sessions it installs the project
adapter if needed and queues an exact-source handoff; first-time running sessions still
need `/reload-plugins` in Claude and a subsequent safe completed turn. Unsupported
sources stay in place. Plain-text Terminal views require explicit `--bridge`
and show `folder:program [bridge]`; they keep depending on the original tab.
The running Claude process must be version 2.1.300 or newer; upgrading the
installed CLI leaves existing processes on their old version. Known older
sources are reported and retained without adapter installation or queuing.
Neither a bridge nor a verified Claude conversation transfers the parent shell
and Terminal history or authorizes closing that window.

### Noninteractive verification

```sh
python3 tools/verify_mac_slurp.py
python3 tools/verify_mac_slurp.py --offline
```

`make mac-offline-test` additionally builds the universal app and release
archive, runs the core/xterm.js/reflow, installer/updater, and Claude adapter
suites, then runs the offline verification command above. It does not install
the app or publish a release.

The default `verify_mac_slurp.py` run checks archive decoding and saved captures, shell-state cloning,
and a disposable native Hardcore receiver. It sends no model requests and does
not operate existing Terminal sessions or read the clipboard. `--offline` skips
the live shell and GUI probes; some headless regressions still allocate owned
PTYs. Each run writes private stage logs, `report.json`, and `SUMMARY.txt`.

All eleven selected stages passed in the audited 2026-10-15 version-3 run.
The native fixture verified exact visible Unicode draft, middle cursor, one
subsequent edit, captured shell memory/history, and shell/PTY/grid dimensions.
See the [recorded evidence and its limits](../bench/RESULTS-mac.md#verified-live-native-draft-cursor-and-geometry).

### Claude and manual Terminal fixtures

Run `make claude-adapter-test` for deterministic adapter checks and
`make claude-window-test` for isolated native receiver tests. An opt-in live
probe exercises a fresh ordinary Claude session, late plugin loading, a
disposable foreground command and conversation recall inside Hardcore:

```sh
python3 tests/probe_claude_handoff_guard.py --handoff build/hardcore \
  --wait-next-turn --foreground-work
```

It sends small real model prompts; it does not control your existing Terminal
sessions.

For a user-operated test on Terminal.app's actual TTY, run this in a fresh
Terminal window after `make install`:

```sh
python3 tests/manual_mac_terminal_handoff.py
```

It starts a disposable Claude conversation directly on Terminal's descriptors,
installs the adapter afterward, and guides you through plugin reload, one tiny
prompt, and an unsent Unicode draft with a middle cursor. It never types into
Terminal. Private results, persistent `STATUS.txt` progress, and cleanup
instructions are saved under `~/Desktop/Hardcore-Terminal-Test-Results/`.
The phase and next action can also be read from another shell without contacting
Claude or changing the run:

```sh
python3 tests/manual_mac_terminal_handoff.py --status RUN_DIRECTORY
```

A verified destination stays open
for inspection; the original Terminal and parent shell are retained. The run's
temporary login copy is removed by the printed cleanup command after its
disposable processes have stopped.

A separate user-operated test measures Terminal's styled scrollback export:

```sh
python3 tools/manual_mac_terminal_archive.py
```

Run it in a fresh Terminal window. It prints a known fixture and guides ordinary
Command-A/Command-C, then captures only after you press Return. The private
archive includes the original RTF/text and reports text, Unicode and style
coverage. It does not transfer a shell or close the window. The
[presentation research](../bench/RESEARCH-mac-terminal-state.md) describes the
remaining cursor, wrap and emulator-state gaps. Offline checks are available
with `python3 tests/test_mac_terminal_archive.py`.

If capture refuses a text mismatch, it retains private comparison diagnostics
and the original copies. It compares the uniquely marked fixture independently
from surrounding Terminal history; differences inside the fixture remain
visible in the comparison report. Canonical Unicode composition differences
can be reconciled by keeping the exact plain text and mapping the RTF styles
onto corresponding graphemes. Ambiguous style mappings and other text changes
remain failures. To process an already saved capture entirely offline:

```sh
python3 tools/manual_mac_terminal_archive.py --decode-capture CAPTURE_DIRECTORY
```

This uses the current helper and saves a new attempt without reading the
clipboard. To copy an existing fixture again and keep previous attempts intact:

```sh
python3 tools/manual_mac_terminal_archive.py --recapture RUN_DIRECTORY
```

Follow the copy instructions and press Return to capture. This does not start
another Claude session or reprint the fixture.

### Desktop integration and experimental takeover

`make mac-integration-test` exercises live reload, rollback, partial terminal
sequences, native slurp, tmux attachment, the Terminal bridge, and verified
release installation. It requires a logged-in Mac desktop, Terminal Automation permission, and
Accessibility permission for the Terminal bridge; it creates isolated test sessions and leaves existing sessions alone.

The experimental **real Terminal job takeover** tests are included in that
suite and can also be run with `make mac-takeover-test` (three repetitions),
or `make mac-takeover-test TAKEOVER_REPEAT=1`. They require Xcode's LLDB and
debugger attach permission in addition to Terminal Automation. Node REPL
coverage is skipped if Node is not installed. A focused run is available as
`python3 tests/test_mac_takeover.py --case python --repeat 3`.

These tests launch their own Terminal tabs and Hardcore windows. They move a
live job to a new controlling PTY, close the source Terminal tab, check that
the PID and in-memory state survive, and reload the receiving Hardcore window.
Coverage includes canonical/raw input, extra terminal descriptors and flags,
Unicode, Ctrl-C, resize signals, Python and Node REPLs, and a curses application.
They also verify refusal of child-process trees and session leaders, protected
shell attach failure, and an unusable receiver leaving the source functional.

This backend remains **test-only**. `hardcore slurp --terminal TTY` selects a
source for the supported host or eligible Claude route, leaving unsupported jobs alone.
`hardcore slurp --bridge --terminal TTY` explicitly selects the plain-text view
described in the [Mac guide](../README.md#slurp-on-macos).
The tests configure their source zsh with `NO_HUP`;
arbitrary source jobs still need safe disowning. The experimental Node fix
restores ordinary kqueue terminal registrations using a private macOS ABI.
Descriptor discovery must be synchronized with stopped threads before this
can become a product backend. Shared descriptor relationships, other event
mechanisms, rollback after `setsid`, and initial screen/emulator-mode recovery
remain unresolved. The curses test redraws after resize and sends the known
application-mode arrow sequence; it does not establish physical-key mode
recovery. No existing user session is targeted by these tests.

`make mac-host-test` tests a hosted process tree without model requests and is
part of the integration suite. Optional installed-agent checks are separate:

```sh
python3 tests/test_mac_host.py --agent claude --agent codex --agent grok --agent agy --check-debugger
# Uses existing logins and sends small model requests in new conversations:
python3 tests/test_mac_host.py --agent codex --live --in-flight
```

The live check verifies remembered conversation state after closing Terminal
and reloading Hardcore; `--in-flight` also attaches while a response is pending.
It uses disposable workspaces, never resumes existing conversations, and asks
the agents not to use tools. Active tool execution is covered by the local
process-tree fixture, not by agent model requests.
