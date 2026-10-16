# Late zsh shell preservation on macOS

Research date: 2026-10-15. Reproduction:

```sh
python3 tests/probe_mac_zsh_clone.py
```

This probe uses fresh, owned PTYs and stock `/bin/zsh` 5.9. It does not access
Terminal.app, load a custom binary module, attach a debugger, change security
settings, or arrange a host before starting the source shell. The fixture uses
an isolated home, disables interactive history expansion for its control
commands, and cleans up only its own processes.

## Newly demonstrated capability

The stock `zsh/clone` module can be loaded after a shell starts. Its `clone`
builtin forks the running shell onto a receiving PTY. The original shell is
not replaced; the receiving shell has a new PID and session. This is a genuine
copy of the running shell's memory, rather than reconstruction from `.zshrc`
or its history file. The mechanism is documented by the
[zsh module manual](https://zsh.sourceforge.io/Doc/Release/Zsh-Modules.html)
and implemented in the upstream
[clone module](https://github.com/zsh-users/zsh/blob/master/Src/Modules/clone.c).

Verified on owned fixtures:

| Case | Result |
| --- | --- |
| Idle shell | Unexported scalar, indexed array, associative array, function, alias, shell option, cwd, and unflushed in-memory command history survived in the cloned shell. The original history file did not exist before cloning. |
| Original terminal closure | Closed the original PTY's last master and observed the source shell exit. The receiving shell remained interactive on its new PTY. |
| Fresh job control | A new child completed successfully; Ctrl+C interrupted a new foreground job with status 130; Ctrl+Z suspended another job, `fg` resumed it, and Ctrl+C interrupted it. |
| Live ZLE draft | A widget captured `BUFFER` and `CURSOR`, called `clone`, and restored both. The Unicode draft and middle cursor matched in both processes. After the source master closed, inserting a character at the restored destination cursor worked. The draft was never submitted. The receiving slave's initial termios was copied from the live source line editor before cloning. |
| Existing background job | The cloned shell's job table was empty. The original shell still owned the original sleep process on its original TTY. This case cannot retire the source. |
| Extra explicit slave FD | An inherited FD opened on the original `/dev/ttysNNN` still pointed there after cloning. After original-master closure it failed to write with status 1. Such descriptors require handling or refusal. |
| Extra generic `/dev/tty` FD | An inherited FD opened on `/dev/tty` followed the clone's new controlling TTY and continued to write successfully. It is different from an FD opened on an explicit slave path. |
| Extra regular-file FD | The clone read `A`, then the source read `B` from the same initially `ABCD` file. Their file offset is shared. The source cannot safely continue independent work during verification. |

The complete passing run is retained at
`/private/tmp/hc-zsh-clone-5vr6m3vh/result.json`. Its idle case moved from PID
66386 to 66389; the ZLE case moved from PID 66417 to 66418. An earlier run also
passed the memory, jobs, ZLE and explicit/generic terminal-FD cases at
`/private/tmp/hc-zsh-clone-_1hgw86s/result.json`.

## What this does not establish

The original running children do not move. The clone implementation clears
its job table and closes its coprocess descriptors. A shell with active or
stopped jobs, coprocesses, or extra source-terminal references requires a
different treatment. Forked regular-file/socket state also needs explicit
review; inherited descriptors are not independent copies of underlying files.

Apple's `/etc/zshrc_Apple_Terminal` adds another integration risk. It derives
session-history paths from `TERM_SESSION_ID` once, then retains those paths and
exit hooks in shell memory. A clone inherits them, so both copies could write
or remove the same session/history files. Changing only `TERM_SESSION_ID` after
cloning would not update the already-bound paths. The controlled `zsh -f`
fixtures do not exercise these ordinary login-shell integrations.

An empty `jobs` listing is also insufficient: disowned children, asynchronous
prompt helpers, coprocesses, sockets and descriptor callbacks need accounting.
Freezing keyboard input alone would not stop both copies' timers, prompt hooks
or shared-file side effects during verification.

The probe does not transfer terminal scrollback, styled cells, alternate
screen contents, title state, selection, or an active application such as
Claude. It does not prove that every ZLE widget, keymap, undo stack, plugin,
trap, directory-stack configuration, or shell version survives correctly.
Nothing here authorizes automatic Terminal closure or makes the existing
full-shell retirement gate pass. Actual Terminal automation remains untested
because access was denied; no alternate route was used to interact with it.

A credible next implementation would clone an idle parent shell only after
the application handoff completes, require that no source jobs or unsupported
descriptors remain, verify the new shell and command buffer, and keep the
source until all other preservation checks succeed. Preserving the same shell
PID is a separate, unproven objective.

## Fileport route assessment

Apple's [fileport manual](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/man/man2/fileport_makeport.2)
describes conversion of an FD already open in the caller into a Mach port and
conversion of a received port into a new FD. Its sharing semantics match
descriptor duplication. It does not provide acquisition of a foreign
process's FD by PID; the master owner must cooperate and send a descriptor or
port. Consequently fileports alone do not obtain Terminal.app's PTY master.
