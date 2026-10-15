#!/usr/bin/env python3
"""Owned-PTY clone research: Apple history collisions and deeper ZLE state.

No Terminal automation, process inspection, debugger, or user-home mutation.
Apple's shell integration is sourced only inside disposable shell fixtures.
"""
import errno
from contextlib import contextmanager
import importlib.util
import json
import os
from pathlib import Path
import pty
import re
import select
import shlex
import signal
import sys
import tempfile
import termios
import time
import uuid


spec = importlib.util.spec_from_file_location('clone_probe',
    Path(__file__).with_name('probe_mac_zsh_clone.py'))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)

MAX_OUTPUT = 2 * 1024 * 1024


@contextmanager
def deadline(seconds):
    def expired(signum, frame):
        raise TimeoutError('owned probe deadline or termination signal reached')
    previous_alarm = signal.signal(signal.SIGALRM, expired)
    previous_term = signal.signal(signal.SIGTERM, expired)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_alarm)
        signal.signal(signal.SIGTERM, previous_term)


def reap_direct_child(pid, grace=1):
    """An unreaped direct child keeps its PID reserved; observed PIDs do not."""
    def poll():
        try:
            return bool(os.waitpid(pid, os.WNOHANG)[0])
        except ChildProcessError:
            return True  # No ownership: never send a signal to this PID.
    for action in (None, signal.SIGTERM, signal.SIGKILL):
        if poll():
            return True
        if action is not None:
            try:
                os.kill(pid, action)
            except ProcessLookupError:
                pass
        limit = time.monotonic() + grace
        while time.monotonic() < limit:
            if poll():
                return True
            time.sleep(.01)
    return poll()


class Shell(base.Shell):
    def __init__(self, root, zle=False):
        self.output_counts = {}
        self.cleanup_errors = []
        self.pid = None
        self.master = self.new_master = self.new_slave = -1
        self.reaped = False
        self.root = root
        os.chmod(root, 0o700)
        try:
            super().__init__(root, zle=zle)
        except BaseException:
            self.cleanup()
            raise

    def cleanup(self):
        # The clone is not Python's child. Close our owned PTYs instead of
        # signalling a PID merely observed in a fixture receipt.
        for name in ('master', 'new_master', 'new_slave'):
            fd = getattr(self, name, -1)
            setattr(self, name, -1)
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError as error:
                    if error.errno != errno.EBADF:
                        self.cleanup_errors.append(f'close {name}: {error}')
        if self.pid and not self.reaped:
            try:
                self.reaped = reap_direct_child(self.pid)
                if not self.reaped:
                    self.cleanup_errors.append('direct source child did not exit within cleanup deadline')
            except OSError as error:
                self.cleanup_errors.append(f'reap source: {error}')
        if self.cleanup_errors:
            try:
                (self.root / 'cleanup-errors.json').write_text(json.dumps(self.cleanup_errors))
            except OSError:
                pass
            if sys.exc_info()[0] is None:
                raise RuntimeError('owned fixture cleanup incomplete: ' + '; '.join(self.cleanup_errors))

    def pump(self):
        for name, fd in (('source', self.master), ('receiver', self.new_master)):
            if fd < 0:
                continue
            for _ in range(16):
                if not select.select([fd], [], [], 0)[0]:
                    break
                try:
                    data = os.read(fd, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    break
                if not data:
                    break
                count = self.output_counts.get(name, 0) + len(data)
                self.output_counts[name] = count
                if count > MAX_OUTPUT:
                    raise AssertionError(f'{name} exceeded bounded fixture output limit')
                self.buffers[fd] = self.buffers.get(fd, b'') + data
                with (self.root / f'{name}-received.bin').open('ab') as stream:
                    stream.write(data)

    def expect(self, fd, pattern, timeout=10):
        pattern = re.compile(pattern)
        limit = time.monotonic() + timeout
        while time.monotonic() < limit:
            data = self.buffers.get(fd, b'')
            found = pattern.search(data)
            if found:
                self.buffers[fd] = data[found.end():]
                return data[:found.end()], found
            self.pump()
            time.sleep(.005)
        data = self.buffers.get(fd, b'')
        (self.root / 'failure-output.bin').write_bytes(data[-MAX_OUTPUT:])
        raise AssertionError(f'PTY did not produce {pattern.pattern!r}: {data[-1500:]!r}')

    def until(self, check, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump()
            answer = check()
            if answer:
                return answer
            time.sleep(.01)
        raise AssertionError('owned fixture did not acknowledge the expected state')

    def clone(self):
        self.new_master, self.new_slave = pty.openpty()
        tty = os.ttyname(self.new_slave)
        self.command(self.master, 'zmodload zsh/clone')
        self.expect(self.master, b'HC> ')
        self.command(self.master, 'clone ' + shlex.quote(tty))
        self.expect(self.master, b'HC> ')
        self.command(self.master, 'print -r -- "HC_CHILD=$!"')
        _, child = self.expect(self.master, rb'HC_CHILD=([0-9]+)\r?\n')
        self.clone_pid = int(child[1])
        self.expect(self.master, b'HC> ')
        self.command(self.new_master, 'print -r -- "HC_PID=$$ HC_TTY=$TTY"')
        _, destination = self.expect(self.new_master, rb'HC_PID=([0-9]+) HC_TTY=(/dev/ttys[0-9]+)')
        assert int(destination[1]) == self.clone_pid != self.pid
        assert destination[2].decode() == tty
        os.close(self.new_slave)
        self.new_slave = -1
        self.expect(self.new_master, b'HC> ')
        return tty

    def script(self, fd, name, text):
        path = self.root / (name + '.zsh')
        path.write_text(text + '\nprintf "\\nHC_SCRIPT_DONE_' + name + '\\n"\n')
        self.command(fd, 'source ' + shlex.quote(str(path)))
        self.expect(fd, b'\r?\nHC_SCRIPT_DONE_' + name.encode() + b'\r?\n')
        self.expect(fd, b'HC> ')

    def exit_clean(self, fd, command='exit'):
        self.command(fd, command)
        data = self.buffers.pop(fd, b'')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if select.select([fd], [], [], .05)[0]:
                try:
                    part = os.read(fd, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    break
                if not part:
                    break
                data += part
                if len(data) > MAX_OUTPUT:
                    raise AssertionError('exit output exceeded bounded fixture limit')
        else:
            raise AssertionError('fixture shell did not exit cleanly')
        (self.root / ('exit-source.txt' if fd == self.master else 'exit-destination.txt')).write_bytes(data)
        os.close(fd)
        if fd == self.master:
            self.master = -1
            status = self.until(lambda: (r if (r := os.waitpid(self.pid, os.WNOHANG))[0] else None), timeout=5)
            self.reaped = True
            assert os.WIFEXITED(status[1]) and os.WEXITSTATUS(status[1]) == 0, status
        else:
            self.new_master = -1
            self.clone_pid = None


def history_case(root, mitigate, destination_first=False):
    root.mkdir()
    shell = Shell(root)
    sid = 'hc-probe-' + uuid.uuid4().hex
    new_sid = 'hc-clone-' + uuid.uuid4().hex
    session_dir = root / '.zsh_sessions'
    session_dir.mkdir(mode=0o700)
    # Avoid the integration's expiry sweep; all session files are our own.
    (session_dir / '_expiration_check_timestamp').touch()
    shared = root / 'shared.history'
    try:
        shell.script(shell.master, 'setup', f'''
HISTFILE={shlex.quote(str(shared))}
HISTSIZE=2000 SAVEHIST=2000
unsetopt APPEND_HISTORY INC_APPEND_HISTORY INC_APPEND_HISTORY_TIME SHARE_HISTORY
TERM_SESSION_ID={shlex.quote(sid)}
SHELL_SESSION_DID_INIT=0 SHELL_SESSIONS_DISABLE=0 INSIDE_EMACS=1
source /etc/zshrc_Apple_Terminal
HC_OWNER=source
function shell_session_save_user_state() {{
  print -r -- "HC_SAVED_OWNER=$HC_OWNER" >> "$1"
  print -r -- "$$:$HC_OWNER" >> {shlex.quote(str(root / 'save-hook.log'))}
}}
function hc_exit_probe() {{ print -r -- "$$:$HC_OWNER" >> {shlex.quote(str(root / 'exit-hook.log'))}; }}
add-zsh-hook zshexit hc_exit_probe
print -s -- HC_BASELINE_SENTINEL
''')
        if mitigate:
            # Mark the common pre-clone history as saved before fork. This is
            # the normal Apple save function, not a reconstruction of history.
            shell.script(shell.master, 'flush', 'shell_session_save_history')
        shell.clone()
        destination_pid = shell.clone_pid
        if mitigate:
            # Rebind every initialized path, not TERM_SESSION_ID alone. Do not
            # re-source startup files or reset the inherited history in memory.
            shell.script(shell.new_master, 'rebind', f'''
HC_OLD_SESSION_HISTORY=$SHELL_SESSION_HISTFILE
TERM_SESSION_ID={shlex.quote(new_sid)}
SHELL_SESSION_FILE="$SHELL_SESSION_DIR/$TERM_SESSION_ID.session"
SHELL_SESSION_HISTFILE="$SHELL_SESSION_DIR/$TERM_SESSION_ID.history"
SHELL_SESSION_HISTFILE_NEW="$SHELL_SESSION_DIR/$TERM_SESSION_ID.historynew"
(umask 077; /bin/cp "$HC_OLD_SESSION_HISTORY" "$SHELL_SESSION_HISTFILE")
HISTFILE=$SHELL_SESSION_HISTFILE_NEW
HC_OWNER=destination
''')
        else:
            shell.script(shell.new_master, 'samepaths', 'HC_OWNER=destination')
        shell.script(shell.master, 'sourceonly', 'print -s -- HC_SOURCE_ONLY_SENTINEL')
        shell.script(shell.new_master, 'destinationonly', 'print -s -- HC_DESTINATION_ONLY_SENTINEL')
        if destination_first:
            shell.exit_clean(shell.new_master)
            shell.exit_clean(shell.master)
        else:
            shell.exit_clean(shell.master)
            original_state_after_source = (session_dir / (sid + '.session')).read_text()
            assert 'HC_SAVED_OWNER=source' in original_state_after_source
            shell.exit_clean(shell.new_master)
        source_state = (session_dir / (sid + '.session')).read_text()
        global_lines = shared.read_text().splitlines()
        counts = {token: global_lines.count(token) for token in
                  ('HC_BASELINE_SENTINEL', 'HC_SOURCE_ONLY_SENTINEL', 'HC_DESTINATION_ONLY_SENTINEL')}
        exits = (root / 'exit-hook.log').read_text().splitlines()
        assert set(exits) == {f'{shell.pid}:source', f'{destination_pid}:destination'}
        if mitigate:
            target_state = (session_dir / (new_sid + '.session')).read_text()
            assert 'HC_SAVED_OWNER=source' in source_state
            assert 'HC_SAVED_OWNER=destination' in target_state
            assert counts == {token: 1 for token in counts}, counts
            old_lines = (session_dir / (sid + '.history')).read_text().splitlines()
            new_lines = (session_dir / (new_sid + '.history')).read_text().splitlines()
            assert 'HC_BASELINE_SENTINEL' in old_lines and 'HC_BASELINE_SENTINEL' in new_lines
            assert 'HC_SOURCE_ONLY_SENTINEL' in old_lines and 'HC_SOURCE_ONLY_SENTINEL' not in new_lines
            assert 'HC_DESTINATION_ONLY_SENTINEL' in new_lines and 'HC_DESTINATION_ONLY_SENTINEL' not in old_lines
        else:
            assert 'HC_SAVED_OWNER=destination' in source_state, 'expected the cloned exit hook to overwrite shared session state'
        return {'mitigated': mitigate, 'destinationExitedFirst': destination_first,
                'sourcePid': shell.pid, 'clonePid': destination_pid,
                'originalSessionOverwritten': not mitigate, 'globalHistorySentinelCounts': counts,
                'bothCopiedExitHooksExecuted': True, 'separateSessionHistoryFiles': mitigate}
    finally:
        shell.cleanup()


def retirement_hook_case(root, replace_source):
    root.mkdir()
    shell = Shell(root)
    try:
        log = root / 'hooks.log'
        shell.script(shell.master, 'hook_setup', f'''
HC_OWNER=source
HC_HOOK_LOG={shlex.quote(str(log))}
autoload -Uz add-zsh-hook
function hc_exit_probe() {{ print -r -- "zshexit:$$:$HC_OWNER" >> "$HC_HOOK_LOG"; }}
function TRAPEXIT() {{ print -r -- "TRAPEXIT:$$:$HC_OWNER" >> "$HC_HOOK_LOG"; }}
add-zsh-hook zshexit hc_exit_probe
''')
        assert not log.exists(), 'fixture exit hooks ran during setup'
        shell.clone()
        destination_pid = shell.clone_pid
        shell.script(shell.new_master, 'hook_destination', 'HC_OWNER=destination')
        assert not log.exists(), 'fixture exit hooks ran before retirement'
        # This is an owned-shell control experiment only. A real source could
        # still own jobs/descriptors, and exec intentionally skips exit work.
        shell.exit_clean(shell.master, 'exec /usr/bin/true' if replace_source else 'exit')
        source_lines = [] if not log.exists() else log.read_text().splitlines()
        expected_source = {f'{hook}:{shell.pid}:source' for hook in ('zshexit', 'TRAPEXIT')}
        assert set(source_lines) == (set() if replace_source else expected_source), source_lines
        shell.exit_clean(shell.new_master)
        lines = log.read_text().splitlines()
        expected_destination = {f'{hook}:{destination_pid}:destination' for hook in ('zshexit', 'TRAPEXIT')}
        assert set(lines) == expected_destination | (set() if replace_source else expected_source), lines
        assert len(lines) == len(set(lines)), lines
        return {'sourcePid': shell.pid, 'clonePid': destination_pid,
                'sourceCommand': 'exec /usr/bin/true' if replace_source else 'exit',
                'sourceExitHooksSuppressed': replace_source,
                'destinationExitHooksExecutedOnce': True, 'observedHooks': lines}
    finally:
        shell.cleanup()


def zle_state(root, do_clone=True):
    root.mkdir()
    for part in ('a', 'b', 'c'):
        (root / part).mkdir()
    shell = Shell(root, zle=True)
    def diagnostics(label):
        result = {'label': label, 'environment': {key: os.environ.get(key)
                  for key in ('TERM', 'LANG', 'LC_ALL', 'LC_CTYPE', 'COLUMNS', 'LINES', 'TERM_PROGRAM')}}
        for name, fd, pid in (('source', shell.master, shell.pid),
                              ('receiver', shell.new_master, shell.clone_pid)):
            values = {}
            if fd >= 0:
                for key, read in (('termios', lambda: repr(termios.tcgetattr(fd))),
                                  ('foregroundPgrp', lambda: os.tcgetpgrp(fd))):
                    try:
                        values[key] = read()
                    except OSError as error:
                        values[key] = repr(error)
                output = shell.buffers.pop(fd, b'')
                for _ in range(16):
                    if not select.select([fd], [], [], 0)[0] or len(output) >= MAX_OUTPUT:
                        break
                    try:
                        block = os.read(fd, 65536)
                    except OSError:
                        break
                    if not block:
                        break
                    output = (output + block)[-MAX_OUTPUT:]
                (root / f'{label}-{name}-output.bin').write_bytes(output)
                values['outputTail'] = repr(output[-2000:])
            if pid:
                for key, read in (('sessionId', lambda: os.getsid(pid)),
                                  ('processGroup', lambda: os.getpgid(pid))):
                    try:
                        values[key] = read()
                    except OSError as error:
                        values[key] = repr(error)
            result[name] = values
        (root / f'{label}-diagnostics.json').write_text(json.dumps(result, indent=2))
        return result
    try:
        shell.new_master, shell.new_slave = pty.openpty()
        tty = os.ttyname(shell.new_slave)
        setup = f'''
HC_PROBE_ROOT={shlex.quote(str(root))}
HC_PROBE_TTY={shlex.quote(tty)}
cd {shlex.quote(str(root / 'a'))}
pushd -q {shlex.quote(str(root / 'b'))}
pushd -q {shlex.quote(str(root / 'c'))}
setopt EXTENDED_GLOB
unsetopt NOMATCH
function TRAPUSR1() {{ print -r -- "$$" >> "$HC_PROBE_ROOT/trap.log"; return 0; }}
zmodload zsh/clone
''' + r'''
function hc_capture() {
  local destination="$HC_PROBE_ROOT/$HC_PHASE-$$"
  print -rn -- "$BUFFER" > "$destination.buffer"
  print -r -- "$CURSOR" > "$destination.cursor"
  print -rn -- "$CUTBUFFER" > "$destination.cut"
  printf '%s\0' "${killring[@]}" > "$destination.ring"
  print -r -- "$KEYMAP" > "$destination.keymap"
  print -r -- "$MARK:$REGION_ACTIVE" > "$destination.region"
  print -r -- "$UNDO_CHANGE_NO:$UNDO_LIMIT_NO" > "$destination.undo"
  print -r -- "$YANK_ACTIVE:$YANK_START:$YANK_END" > "$destination.yank"
  printf '%s\0' "$PWD" "${dirstack[@]}" > "$destination.dirs"
  setopt > "$destination.options"
  functions TRAPUSR1 > "$destination.trap"
  print -r -- done > "$destination.ready"
}
function hc_seed() {
  BUFFER='alpha λ'; CURSOR=$#BUFFER
  CUTBUFFER='cut café'; killring=('ring first' 'ring second')
  MARK=2; REGION_ACTIVE=1
  print -rn -- "$BUFFER" > "$HC_PROBE_ROOT/seed-direct"
}
function hc_add() { BUFFER+=' Ω'; CURSOR=4; print -rn -- "$BUFFER" > "$HC_PROBE_ROOT/add-direct"; }
function hc_before() { HC_PHASE=before; hc_capture; }
function hc_clone() {
  local saved_buffer=$BUFFER saved_cursor=$CURSOR
  clone "$HC_PROBE_TTY"
  BUFFER=$saved_buffer CURSOR=$saved_cursor
  { print -r -- "HC_CTTY_PROOF_$$" > /dev/tty } 2> "$HC_PROBE_ROOT/ctty-$$.error"
  print -r -- "$?" > "$HC_PROBE_ROOT/ctty-$$.status"
  print -r -- "$TTY" > "$HC_PROBE_ROOT/ctty-$$.tty"
  HC_PHASE=cloned; hc_capture
  zle -R
}
function hc_undo() { zle undo; HC_PHASE=undo; hc_capture; zle -R; }
function hc_redo() { zle redo; HC_PHASE=redo; hc_capture; zle -R; }
function hc_yank() { zle yank; zle -f yank; HC_PHASE=yank; hc_capture; zle -R; }
function hc_pop() { zle yank-pop; HC_PHASE=pop; hc_capture; zle -R; }
function hc_signal() { builtin kill -USR1 $$; }
function hc_ping() {
  (( HC_PING_SEQ++ ))
  HC_PHASE=ping$HC_PING_SEQ; hc_capture
  print -r -- "$HC_PING_SEQ" > "$HC_PROBE_ROOT/ping-$$.sequence"
}
for widget in hc_seed hc_add hc_before hc_clone hc_undo hc_redo hc_yank hc_pop hc_ping hc_signal; do zle -N "$widget"; done
bindkey -N hardcore-probe emacs
bindkey -A hardcore-probe main
bindkey '^X^B' hc_seed
bindkey '^X^A' hc_add
bindkey '^X^V' hc_before
bindkey '^X^X' hc_clone
bindkey '^X^U' hc_undo
bindkey '^X^R' hc_redo
bindkey '^X^Y' hc_yank
bindkey '^X^P' hc_pop
bindkey '^X^G' hc_ping
bindkey '^X^T' hc_signal
'''
        shell.script(shell.master, 'zle_setup', setup)
        os.write(shell.master, b'\x18\x02')
        shell.until(lambda: (root / 'seed-direct').exists())
        os.write(shell.master, b'\x18\x01')
        shell.until(lambda: (root / 'add-direct').exists())
        os.write(shell.master, b'\x18\x16')
        shell.until(lambda: (root / f'before-{shell.pid}.ready').exists())

        def capture(phase, pid):
            shell.until(lambda: (root / f'{phase}-{pid}.ready').exists())
            return {field: (root / f'{phase}-{pid}.{field}').read_bytes()
                    for field in ('buffer', 'cursor', 'cut', 'ring', 'keymap', 'region',
                                  'undo', 'yank', 'dirs', 'options', 'trap')}

        before = capture('before', shell.pid)
        assert before['buffer'].decode() == 'alpha λ Ω'
        # Prompt bytes can arrive before ZLE switches the original terminal to
        # editor mode. Only snapshot termios after an actual widget replied.
        editor_mode = termios.tcgetattr(shell.master)
        assert not editor_mode[3] & termios.ICANON, 'source editor is still canonical'
        termios.tcsetattr(shell.new_slave, termios.TCSANOW, editor_mode)
        (root / 'source-editor-mode.txt').write_text(repr(editor_mode))

        def ping_editor(fd, pid):
            sequence = root / f'ping-{pid}.sequence'
            def read_sequence():
                try:
                    return int(sequence.read_text())
                except (FileNotFoundError, ValueError):
                    return None
            initial = shell.until(read_sequence) if sequence.exists() else 0
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                # Read-only editor ping is safe to repeat. Never retry the
                # mutating undo/redo/yank operation itself.
                os.write(fd, b'\x18\x07')
                deadline_ping = time.monotonic() + .05
                while time.monotonic() < deadline_ping:
                    shell.pump()
                    try:
                        current = int(sequence.read_text())
                    except (FileNotFoundError, ValueError):
                        current = initial
                    if current > initial:
                        snapshot = capture(f'ping{current}', pid)
                        assert snapshot['buffer'] == before['buffer']
                        assert snapshot['cursor'] == before['cursor']
                        return
                    time.sleep(.005)
            state = diagnostics('ping-timeout')
            raise AssertionError('owned receiver did not acknowledge editor ping: ' + json.dumps(state))

        comparisons = {}
        if do_clone:
            os.write(shell.master, b'\x18\x18')
            paths = shell.until(lambda: (found if len(found := list(root.glob('cloned-*.ready'))) == 2 else None))
            shell.clone_pid = next(int(path.stem.split('-')[1]) for path in paths
                                   if int(path.stem.split('-')[1]) != shell.pid)
            cloned = capture('cloned', shell.clone_pid)
            comparisons = {key: cloned[key] == value for key, value in before.items()}
            assert all(comparisons.values()), comparisons
            ctty_status = (root / f'ctty-{shell.clone_pid}.status').read_text().strip()
            ctty_error = (root / f'ctty-{shell.clone_pid}.error').read_text()
            assert ctty_status == '0', 'clone controlling terminal unavailable: ' + ctty_error
            assert (root / f'ctty-{shell.clone_pid}.tty').read_text().strip() == tty
            proof_output, _ = shell.expect(shell.new_master, f'HC_CTTY_PROOF_{shell.clone_pid}\r?\n'.encode())
            assert os.getsid(shell.clone_pid) == shell.clone_pid
            assert os.tcgetpgrp(shell.new_master) == shell.clone_pid
            assert b'could not make' not in proof_output, 'clone warned about controlling-terminal acquisition'
            os.close(shell.new_slave)
            shell.new_slave = -1
            ping_editor(shell.new_master, shell.clone_pid)
            base.close_source(shell)
        active_pid = shell.clone_pid if do_clone else shell.pid
        active_fd = shell.new_master if do_clone else shell.master
        assert not termios.tcgetattr(active_fd)[3] & termios.ICANON, 'receiver editor is canonical'
        ping_editor(active_fd, active_pid)
        # Let the owned shell signal itself; never signal an observed clone PID.
        os.write(active_fd, b'\x18\x14')
        shell.until(lambda: (root / 'trap.log').exists())
        assert (root / 'trap.log').read_text().splitlines() == [str(active_pid)]
        ping_editor(active_fd, active_pid)
        os.write(active_fd, b'\x18\x15')
        undone = capture('undo', active_pid)
        assert undone['buffer'].decode() == 'alpha λ', undone['buffer']
        os.write(active_fd, b'\x18\x12')
        redone = capture('redo', active_pid)
        assert redone['buffer'] == before['buffer'], redone['buffer']
        os.write(active_fd, b'\x18\x19')
        yanked = capture('yank', active_pid)
        original = before['buffer'].decode()
        cursor = int(before['cursor'])
        assert yanked['buffer'].decode() == original[:cursor] + before['cut'].decode() + original[cursor:]
        os.write(active_fd, b'\x18\x10')
        popped = capture('pop', active_pid)
        assert before['ring'].split(b'\0')[0] in popped['buffer']
        return {'sourcePid': shell.pid, 'clonePid': shell.clone_pid,
                'exactCapturedFields': comparisons, 'sourceMasterClosed': do_clone,
                'controllingTerminalProofOnReceiver': do_clone,
                'undoRedoPreserved': True,
                'editorSequence': {phase: {key: value.decode() for key, value in snapshot.items()
                                          if key in ('buffer', 'cursor', 'cut', 'ring', 'region', 'undo', 'yank')}
                                   for phase, snapshot in (('undo', undone), ('redo', redone),
                                                           ('yank', yanked), ('yankPop', popped))},
                'inheritedUsr1TrapRuns': True, 'bufferNeverSubmitted': True}
    except BaseException:
        try:
            diagnostics('failure')
        except (OSError, ValueError):
            pass
        raise
    finally:
        shell.cleanup()


def run_cases(root):
    result = {}
    cases = [('unmitigated', lambda: history_case(root / 'shared-paths', False)),
             ('isolatedPaths', lambda: history_case(root / 'split-paths', True)),
             ('isolatedPathsReverseExit', lambda: history_case(root / 'split-paths-reverse', True, True)),
             ('normalExitHooks', lambda: retirement_hook_case(root / 'normal-exit', False)),
             ('execExitHooks', lambda: retirement_hook_case(root / 'exec-exit', True)),
             ('zleState', lambda: zle_state(root / 'zle')),
             ('zleControl', lambda: zle_state(root / 'zle-control', False))]
    for name, run in cases:
        try:
            result[name] = run()
        except BaseException as error:
            try:
                (root / 'failure.json').write_text(json.dumps({'case': name,
                    'type': type(error).__name__, 'reason': str(error)[:12000],
                    'completedCases': list(result)}, indent=2))
            except OSError:
                pass
            raise
        (root / 'result.json').write_text(json.dumps(result, indent=2))
    assert result['zleState']['editorSequence'] == result['zleControl']['editorSequence'], 'cloned editor differs from control'
    result['zleState']['behaviorMatchesUnclonedControl'] = True
    (root / 'result.json').write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2), flush=True)


def main():
    if sys.platform != 'darwin':
        raise SystemExit('This research probe uses stock macOS zsh and Apple shell integration.')
    os.umask(0o077)
    root = Path(tempfile.mkdtemp(prefix='hc-zsh-clone-state-', dir='/private/tmp'))
    print('Private diagnostics:', root, flush=True)
    try:
        with deadline(120):
            run_cases(root)
    except BaseException as error:
        if not (root / 'failure.json').exists():
            try:
                (root / 'failure.json').write_text(json.dumps({'case': 'overall-verification',
                    'type': type(error).__name__, 'reason': str(error)[:12000]}, indent=2))
            except OSError:
                pass
        raise


if __name__ == '__main__':
    main()
