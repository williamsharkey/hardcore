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
this bridge cannot safely take over their connection.

## Validation

`make slurp-test` builds the executable and runs three integration checks:

- `tests/test_slurp_linux.py` creates synthetic Linux terminals and checks
  process identity, shell memory, background jobs, input and interrupts, stale
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
