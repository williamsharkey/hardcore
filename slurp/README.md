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
