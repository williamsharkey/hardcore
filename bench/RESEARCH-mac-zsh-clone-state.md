# Late zsh clone: Apple history, exit hooks, and editor state

Research fixture: `python3 tests/probe_mac_zsh_clone_state.py`.
Validated on macOS 26.4 (25E246), stock `/bin/zsh` 5.9
(`arm64-apple-darwin25.0`). This is a shell-preservation experiment, not a
production slurp path or permission to close Terminal windows. It starts only
owned PTYs with private HOME/ZDOTDIR directories. It does not access Terminal.app,
inspect existing processes, change the user's shell configuration, or use a
debugger.

Last individually inspected complete result, including controlling-terminal
proof: `/private/tmp/hc-zsh-clone-state-iika4evv/result.json`, from the user's
unattended run `/private/tmp/hc-slurp-verification-h80gugho`. The companion
version-3 native receiver also passed exact draft, cursor, and PTY geometry
checks; see [the audited results below](#version-3-live-receiver-verification).
The probe creates a fresh private diagnostics directory on every run.

**Current restriction:** independent runs in a restricted execution
environment did not establish a usable controlling terminal. They
must not be reported as successful complete transfers. The working-context
results below establish capabilities in their tested context, not portability
through the current sandbox restrictions.

An independent rerun of the initial fixture failed while waiting for the first
undo widget after closing the source (`/private/tmp/hc-zsh-clone-state-vq97e26z`).
The clone had reached its snapshot and executed its USR1 trap, but it did not
acknowledge the undo key. The initial fixture copied receiver terminal settings
immediately after reading prompt bytes and treated a snapshot written inside
the clone widget as proof the editor was ready for further input. Those are
insufficient synchronization points. The failed run did not record terminal
mode, so the exact cause cannot be established retrospectively.

The revised fixture copies terminal settings only after a source widget has
answered, asserts noncanonical mode on both ends, and requires a read-only
editor ping before source closure, after closure, and after the trap. It may
repeat that ping; it sends each mutating undo/redo/yank action only once. Twenty
consecutive cloned/control editor pairs then passed, with diagnostics under
`/private/tmp/hc-zsh-state-repeat-gvrm46vy`. Five additional complete seven-case
runs also passed; their output is recorded under
`/private/tmp/hc-zsh-state-matrix-repeat-uo_dlkm2`.

However, another independent run still failed at the first receiver ping,
**before** source closure (`/private/tmp/hc-zsh-clone-state-ktjpm59g`). Added
diagnostics then captured a controlling-terminal acquisition warning in
`/private/tmp/hc-zsh-clone-state-g_h8jnzf/zle/ping-timeout-diagnostics.json`.
The source and receiver were both noncanonical, and their session/foreground
process-group IDs looked correct. Those checks and the shell's `monitor` option
were insufficient. An independent native-import fixture subsequently recorded
`operation not permitted: /dev/tty`, establishing an access denial in the
restricted execution context. No alternate path or permission change was tried.

The current probe requires the cloned shell to open and write `/dev/tty`, with
its PID-bound proof actually received on the destination master. It rejects a
nonzero status or acquisition warning before closing the source, and records
the error, terminal modes, output, and completed cases separately. Its file
waits also drain owned PTY output, as a terminal normally would, avoiding a
separate output-backpressure weakness. The earlier repeat counts cannot be
used to override the independent controlling-terminal failure. The latest
working-context run passed the stronger proof. The independent guarded run at
`/private/tmp/hc-zsh-clone-state-urgruw4g` preserved the five completed history
and exit-hook cases, then refused the clone before source closure because
`/dev/tty` returned `operation not permitted`. This round adds no
new claim of job-control usability in that restricted context; the prior
successful Ctrl-C/Ctrl-Z/fg experiments belong to their original context.

## Apple session paths collide after an ordinary clone

The fixture sources the installed `/etc/zshrc_Apple_Terminal` only inside a
disposable shell. The inspected file's SHA-256 is
`763ba2dec426f377221e2e93c40708fd545e73fe3b5c5ba9ce6d93df80205bf5`.
Its implementation initializes session paths once, registers a `zshexit` hook,
writes a session restoration file, and merges per-session history into shared
history. Changing `TERM_SESSION_ID` alone would leave its initialized paths
unchanged.

The unmodified source and clone shared `.session`, `.history`, and `.historynew`
paths. After the source exited and the clone exited, the clone's custom saved
state had overwritten the source's session file. The common pre-clone history
sentinel appeared twice in global history. Both copied exit hooks executed.

A tested mitigation for this specific Apple integration is:

1. At a completed ordinary prompt, call the existing
   `shell_session_save_history` before cloning. This flushes common history and
   updates the shell's incremental-save bookkeeping without calling the custom
   session-state save hooks.
2. In the clone, allocate a new session ID and rebind `SHELL_SESSION_FILE`,
   `SHELL_SESSION_HISTFILE`, `SHELL_SESSION_HISTFILE_NEW`, and `HISTFILE` to the new
   session paths. Keep `SHELL_SESSION_HISTFILE_SHARED` pointing to the shared
   history. Copy the just-flushed old per-session `.history` as the clone's
   baseline. Do not re-source startup files or replace in-memory history.
3. Verify separate source and destination session files and retained history
   before considering any retirement action.

With that sequence, both source-first and destination-first exit orders passed:
all three common/source-only/destination-only sentinels appeared exactly once in
global history; each private history contained the baseline and only its own
post-clone sentinel; each session file retained its correct owner. The fixture
does not test simultaneous history writers, truncation under small SAVEHIST,
custom replacements for Apple's functions, or every combination of history
options. In particular, it deliberately disables incremental/shared-history
options and enables the normal per-session path. Those other modes need their
own protocols and tests.

The zsh manual documents interactive history and incremental `fc -AI` writes;
the Apple-specific behavior above comes from the local implementation and the
owned-shell experiment. [zsh shell builtins](https://zsh.sourceforge.io/Doc/Release/Shell-Builtin-Commands.html)

## Copied exit hooks need an ownership decision

An additional control installed both `TRAPEXIT` and a `zshexit` hook. Normal
`exit` in both shells ran both callbacks twice, once under each PID. Replacing
the source with the external executable `exec /usr/bin/true` ran neither source
callback; later normal destination exit ran both callbacks exactly once.

This is a possible way to retire an already verified, resource-free original
shell without duplicating custom cleanup. It deliberately skips source exit
work, including normal history/session saves and potentially logout cleanup.
Necessary saves must be explicit first. The probe tests the hook semantics
separately from the Apple-history protocol, and does not prove that combining
them is safe for arbitrary shell configurations. No production source retirement
was enabled. `exec` replaces the shell process; it is not an ordinary shell exit.
[zsh shell builtins](https://zsh.sourceforge.io/Doc/Release/Shell-Builtin-Commands.html)

## Editor state survives beyond the visible draft

A custom ZLE widget invoked stock `clone` while an unsent Unicode buffer and a
middle cursor were live. Before and after cloning, the fixture compared exact
buffer, cursor, cut buffer, kill-ring entries, current keymap, mark/region state,
undo markers, yank state, directory stack, shell options, and the installed
USR1 trap definition. Every captured field matched, including the fixture's
active mark/region state. Other selection modes and vi operator modes are not
established by this test.

After closing the original PTY master, the clone's inherited USR1 trap ran under
the clone's PID. Undo removed the last edit, redo restored it, and yank followed
by yank-pop used the inherited cut buffer and older kill-ring entry. Every
captured result in that editor sequence matched a separate, uncloned control.
The unsent buffer was never submitted. The fixture records a stock/control
yank-pop placement quirk with the Unicode cut buffer; it checks equivalence,
not an assertion that that placement is ideal.

These probes use the documented ZLE buffer/cursor, undo, cut-buffer/kill-ring,
and widget interfaces. [zsh line editor](https://zsh.sourceforge.io/Doc/Release/Zsh-Line-Editor.html)

## Unattended probe lifecycle

The state and native receiver probes share bounded PTY reads, output limits,
and cleanup. Constructor failures clean up their partial fixtures; cleanup
closes owned PTYs and signals only unreaped Python children. Clone PIDs observed
in receipts are not sufficient authority to signal a process. The inherited
USR1 trap check now asks the owned shell to signal itself through its fixture
widget. Internal deadlines are 120 seconds for the state probe and 90 seconds
for the native receiver, with separate outer deadlines in the unattended runner.

The initial ten deterministic lifecycle checks passed, including cleanup error
preservation, bounded child escalation, constructor failure, output bounds,
and stalled or oversized native packets. A fresh plain owned PTY also passed
startup and exit. The root agent's known-denied `/dev/tty` and GUI cases were
not repeated. The user's ordinary Mac run subsequently passed the complete
shell-state matrix and native input experiment; its saved evidence is audited
below. `tools/verify_mac_slurp.py --offline` continues to exclude those live
probes rather than treating them as verified.

## Audited user-run native evidence

The native fixture at `/private/tmp/hc-zsh-native-u231f3_d` preserved its shell
state exactly, apart from PID/TTY, including a standalone unflushed-history
entry. Its new controlling terminal worked, and the cloned shell accepted
Hardcore input at the restored middle cursor after the original shell exited.
The companion ZLE matrix matched exact state and uncloned editor behavior.
These results resolve the pending ordinary-environment checks for those
capabilities, without changing the restricted agent environment's results.

The native snapshot also exposed a test weakness: each observation printed a
controlling-terminal diagnostic at the live cursor, overwriting the displayed
draft. The probe only looked for two fragments of the draft in its snapshot,
so its positive screen flag was too broad. The exact shell buffer remained
correct. Full visible text and physical cursor restoration need a stronger
snapshot check and an observer that does not print over the editor. The audit
is retained at `/private/tmp/hc-slurp-evidence-audit-i2djhk0s/report.json`;
original evidence is unchanged.

The revised native probe emits `proofVersion: 2`, uses quiet observations with
immutable generation files, and compares exact history event records and
captured memory. A strict snapshot checker compares the entire visible draft
and cursor before source closure, then again after a single edit at the middle
cursor. Snapshot polling is read-only and bounded; redraw completion is not
inferred from a shell-side ready file. The edit preserves the execution-marker
path. The screen proof concerns terminal cells and cursor, not exact rasterized
pixels. At that point the new live checks remained pending an
ordinary-environment run; the subsequent failure and repair are recorded below.

## Native import must synchronize PTY geometry after commit

The subsequent user run at `/private/tmp/hc-slurp-verification-a7n6tn1p`
exercised the version-2 checker and failed before source-close verification.
Saved shell memory, history, draft, and ZLE cursor matched exactly, but the
native screen cursor was column 104 instead of 31 (zero-based). The complete
draft was visible. A read-only audit is retained at
`/private/tmp/hc-slurp-resize-audit-tdyklnd_/report.json`.

The fixture had set only the destination PTY size; the source still used zsh's
80-column fallback. The receiver also contained a production ordering bug:
layout resized its internal grid while the imported PTY was still staged, so
`tab_set_size` correctly avoided mutating the donor. After GO, commit started
I/O but never delivered that final grid size. Later layout saw matching internal
dimensions and did no further work. Saved redraw cursor movements account for
the observed mismatch when an 80-column redraw is interpreted on 105 columns.

Commit now reapplies the current grid immediately after positive GO and I/O
setup. A headless harness tests the actual production functions against owned
PTYs, including success, multiple groups, and failure paths that must leave
donors unchanged. All eight tests pass; removing the new call makes its
negative control fail. A plain owned-shell experiment also confirmed ordinary
kernel resize notifications update zsh's dimensions while preserving draft and
cursor. These tests require neither clone nor GUI access.

Probe version 3 initializes both owned PTYs equally, observes the initial size
in zsh, and requires shell dimensions, kernel PTY dimensions, native grid, and
foreground process group to agree at both screen verification points. It keeps
the complete visible draft and cursor checks. Its six new geometry regressions
pass with the existing lifecycle suite (34 total). The rebuilt binary and all
offline verification stages pass. Neither an offline pass nor the plain-shell
resize experiment establishes that full handoff; the later user-run evidence
below supplies the live check.

## Version-3 live receiver verification

The user-run suite at `/private/tmp/hc-slurp-verification-h80gugho` passed all
eleven stages in 6.44 seconds. Its native fixture,
`/private/tmp/hc-zsh-native-f6kbbekz`, passed complete draft and cursor checks
before the source test shell exited and after a single native-input edit.
The visible cursor moved from zero-based column 31 to 32 when `X` was inserted
at the preserved middle cursor. Shell, kernel PTY, and native grid all agreed
on 105 by 43 dimensions at both checkpoints, with the expected cloned shell
as foreground process group. Captured shell memory and unflushed history were
unchanged; the draft remained unsent. This confirms the repaired import-size
path for the disposable owned fixture.

An independent read-only audit replayed both saved native snapshots and compared
all saved observation generations, including the exact five-event history with
two unflushed sentinels. It passed with original evidence hashes unchanged:
`/private/tmp/hc-native-v3-audit-64uutm0v/report.json`. Source exit follows the
successful `waitpid` path; no raw wait status was retained, so this evidence
does not establish a particular exit code.

The companion matrix at `/private/tmp/hc-zsh-clone-state-iika4evv` also passed.
A read-only raw-file audit independently confirmed all eleven captured editor
fields before/after clone, 28 undo/redo/yank/yank-pop field comparisons against
the uncloned control, six history sentinel counts across both isolated-path
exit orders, and the inherited trap's destination PID (46 checks total).
Evidence and source hashes are at
`/private/tmp/hc-slurp-state-audit-v5q2c_a8/report.json`.

This is live evidence for a script-owned shell and Hardcore receiver, not a
takeover of an existing Terminal session. The version-3 result explicitly
leaves existing-job transfer, source-screen transfer, and automatic Terminal
closure eligibility false. It does not close the remaining gaps below.

## Remaining limits

Cloning creates a new shell PID. Upstream `zsh/clone` clears the job table and
closes coprocess descriptors in the child. This does not move active Claude,
Codex, other foreground processes, or background jobs into a new session.
[zsh clone implementation](https://github.com/zsh-users/zsh/blob/master/Src/Modules/clone.c)

Previous owned-PTY probes also found old slave descriptors inherited by the
clone and regular-file offsets shared between parent and child. Exit-hook
suppression does not fix those resources. Async prompt jobs, callbacks, sockets,
locks, timers, custom session stores, vi editing/completion/search state, PS2
continuations, and concurrent history modification still require investigation.
Neither probe captures Terminal scrollback or establishes exact Terminal tab
identity. There remains no basis for claiming full-shell transfer or safe
automatic Terminal close.
