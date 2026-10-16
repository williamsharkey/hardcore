#!/usr/bin/env python3
"""Probe stock zsh's late, cooperative shell cloning on owned macOS PTYs.

No Terminal.app access, debugger, injected module, user config change, or
prelaunch host. A cloned shell has a NEW PID; zsh deliberately clears its job
table. This is a shell-memory preservation lead, not arbitrary job migration.
"""
import errno
import json
import os
from pathlib import Path
import pty
import re
import select
import shlex
import signal
import subprocess
import sys
import tempfile
import termios
import time


class Shell:
    def __init__(self, root, zle=False):
        self.root = root
        self.buffers = {}
        self.new_master = self.new_slave = -1
        self.clone_pid = None
        self.job_pid = None
        self.reaped = False
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.environ.update(HOME=str(root), ZDOTDIR=str(root), TERM='xterm-256color')
            os.execv('/bin/zsh', ['zsh', '-f'])
        self.expect(self.master, rb'[%#] ')
        self.command(self.master, "PS1='HC> '; PS2='MORE> '; unsetopt " +
            ('' if zle else 'ZLE ') + "BANG_HIST INC_APPEND_HISTORY SHARE_HISTORY; print -r -- HC_INIT_DONE")
        self.expect(self.master, b'\r?\nHC_INIT_DONE\r?\n')
        self.expect(self.master, b'HC> ')

    def command(self, fd, text):
        os.write(fd, text.encode() + b'\r')

    def expect(self, fd, pattern, timeout=10):
        pattern = re.compile(pattern)
        data = self.buffers.pop(fd, b'')
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            found = pattern.search(data)
            if found:
                self.buffers[fd] = data[found.end():]
                return data[:found.end()], found
            if select.select([fd], [], [], .1)[0]:
                try:
                    part = os.read(fd, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not part:
                    break
                data += part
        (self.root / 'failure-output.bin').write_bytes(data)
        if self.master >= 0 and self.master != fd:
            original = self.buffers.pop(self.master, b'')
            while select.select([self.master], [], [], .1)[0]:
                try:
                    part = os.read(self.master, 65536)
                except OSError:
                    break
                if not part:
                    break
                original += part
            (self.root / 'source-failure-output.bin').write_bytes(original)
            print('Source diagnostics:', original[-2000:], file=sys.stderr)
        raise AssertionError(f'PTY did not produce {pattern.pattern!r}: {data[-1500:]!r}')

    def clone(self):
        self.new_master, self.new_slave = pty.openpty()
        tty = os.ttyname(self.new_slave)
        self.command(self.master, 'zmodload zsh/clone')
        self.expect(self.master, b'HC> ')
        self.command(self.master, 'clone ' + shlex.quote(tty))
        self.expect(self.master, b'HC> ')
        self.command(self.master, 'print -r -- "HC_CLONE_PID=$$ HC_CLONE_TTY=$TTY HC_FORK=$!"')
        _, parent = self.expect(self.master, rb'HC_CLONE_PID=([0-9]+) HC_CLONE_TTY=(/dev/ttys[0-9]+) HC_FORK=([0-9]+)')
        self.clone_pid = int(parent[3])
        status = subprocess.run(['ps', '-p', str(self.clone_pid), '-o', 'pid=,ppid=,tty=,stat=,args='],
                                capture_output=True, text=True).stdout
        (self.root / 'clone-process.txt').write_text(status)
        self.command(self.new_master, 'print -r -- "HC_CLONE_PID=$$ HC_CLONE_TTY=$TTY"')
        output, match = self.expect(self.new_master, rb'HC_CLONE_PID=([0-9]+) HC_CLONE_TTY=(/dev/ttys[0-9]+)')
        self.clone_pid = int(match[1])
        assert self.clone_pid != self.pid
        assert match[2].decode() == tty
        assert os.getsid(self.clone_pid) == self.clone_pid
        os.close(self.new_slave)
        self.new_slave = -1
        self.expect(self.new_master, b'HC> ')
        return tty

    def cleanup(self):
        # Only PIDs recorded directly from these private fixtures are touched.
        for pid in (self.job_pid, self.clone_pid, None if self.reaped else self.pid):
            if pid:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for fd in (self.master, self.new_master, self.new_slave):
            if fd >= 0:
                try:
                    os.close(fd)
                except OSError:
                    pass
        if not self.reaped:
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass


def until(check, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        answer = check()
        if answer:
            return answer
        time.sleep(.02)
    raise AssertionError('fixture did not reach the expected state')


def close_source(shell):
    os.close(shell.master)
    shell.master = -1
    until(lambda: os.waitpid(shell.pid, os.WNOHANG)[0])
    shell.reaped = True


def job_control(shell):
    shell.expect(shell.new_master, b'HC> ')
    shell.command(shell.new_master, 'sleep 180')
    shell.job_pid = until(lambda: (group if (group := os.tcgetpgrp(shell.new_master))
                                  != shell.clone_pid else None))
    os.write(shell.new_master, b'\x03')
    shell.expect(shell.new_master, b'HC> ')
    shell.command(shell.new_master, 'print -r -- "HC_INTERRUPT_STATUS=$?"')
    shell.expect(shell.new_master, b'HC_INTERRUPT_STATUS=130\r?\n')
    shell.job_pid = None
    shell.expect(shell.new_master, b'HC> ')
    shell.command(shell.new_master, 'sleep 180')
    shell.job_pid = until(lambda: (group if (group := os.tcgetpgrp(shell.new_master))
                                  != shell.clone_pid else None))
    os.write(shell.new_master, b'\x1a')
    shell.expect(shell.new_master, rb'(suspended|stopped)')
    shell.expect(shell.new_master, b'HC> ')
    shell.command(shell.new_master, 'fg')
    until(lambda: os.tcgetpgrp(shell.new_master) == shell.job_pid)
    os.write(shell.new_master, b'\x03')
    shell.expect(shell.new_master, b'HC> ')
    shell.command(shell.new_master, 'print -r -- "HC_FOREGROUND_STATUS=$?"')
    shell.expect(shell.new_master, b'HC_FOREGROUND_STATUS=130\r?\n')
    shell.job_pid = None
    return {'interruptDelivered': True, 'suspendAndForegroundDelivered': True}


def idle_memory(root):
    root.mkdir()
    work = root / 'working directory café'
    work.mkdir()
    shell = Shell(root)
    try:
        history = root / 'unflushed.history'
        shell.command(shell.master,
            "HC_LOCAL='in-memory λ'; typeset -a HC_ARRAY=(alpha 'β gamma'); "
            "typeset -A HC_MAP=(left 'value café' right 'kept'); "
            "function hc_function(){ print -r -- \"FUNCTION:$HC_LOCAL:${HC_ARRAY[2]}:${HC_MAP[left]}\"; }; "
            "alias hc_alias='hc_function'; setopt EXTENDED_GLOB; HISTSIZE=500; SAVEHIST=500; "
            'HISTFILE=' + shlex.quote(str(history)) + '; cd ' + shlex.quote(str(work)) +
            "; print -s -- 'HC_MEMORY_HISTORY_SENTINEL'; print -r -- HC_SETUP_DONE")
        shell.expect(shell.master, b'\r?\nHC_SETUP_DONE\r?\n')
        shell.expect(shell.master, b'HC> ')
        assert not history.exists(), 'history was already flushed before the clone'
        tty = shell.clone()
        # A real terminal teardown, not merely stopping input to the source.
        close_source(shell)
        shell.command(shell.new_master,
            'print -r -- "HC_LIVE_PID=$$ HC_LIVE_TTY=$TTY"; typeset -p HC_LOCAL HC_ARRAY HC_MAP; '
            'whence -v hc_alias hc_function; print -r -- "HC_OPTION:$options[extendedglob]"; '
            'pwd; hc_alias; fc -ln 1; print -r -- HC_STATE_DONE')
        output, _ = shell.expect(shell.new_master, b'\r?\nHC_STATE_DONE\r?\n')
        text = output.decode(errors='replace')
        for wanted in ('in-memory λ', 'β gamma', 'value café',
                       'hc_alias is an alias for hc_function', 'hc_function is a shell function',
                       'HC_OPTION:on', str(work), 'FUNCTION:in-memory λ:β gamma:value café',
                       'HC_MEMORY_HISTORY_SENTINEL', f'HC_LIVE_PID={shell.clone_pid} HC_LIVE_TTY={tty}'):
            assert wanted in text, wanted
        (root / 'clone-output.txt').write_text(text)
        shell.expect(shell.new_master, b'HC> ')
        shell.command(shell.new_master, 'sleep 0.1 & wait $!; print -r -- "HC_NEW_JOB_STATUS=$?"')
        shell.expect(shell.new_master, b'HC_NEW_JOB_STATUS=0\r?\n')
        signals = job_control(shell)
        return {'sourcePid': shell.pid, 'clonePid': shell.clone_pid, 'newTty': tty,
                'sourceMasterClosed': True, 'sourceShellExited': True,
                'variablesArraysFunctionsAliasesOptionsCwdHistoryPreserved': True,
                'newJobControlWorks': True, **signals}
    finally:
        shell.cleanup()


def active_job(root):
    root.mkdir()
    shell = Shell(root)
    try:
        shell.command(shell.master, 'sleep 180 & print -r -- "HC_JOB_PID=$!"')
        _, match = shell.expect(shell.master, rb'HC_JOB_PID=([0-9]+)\r?\n')
        shell.job_pid = int(match[1])
        shell.expect(shell.master, b'HC> ')
        source_tty = subprocess.run(['ps', '-p', str(shell.job_pid), '-o', 'tty='],
                                    capture_output=True, text=True, check=True).stdout.strip()
        new_tty = shell.clone()
        clone_jobs = root / 'clone-jobs.txt'
        source_jobs = root / 'source-jobs.txt'
        shell.command(shell.new_master, 'jobs -l > ' + shlex.quote(str(clone_jobs)) +
                      '; print -r -- HC_CLONE_JOBS_DONE')
        shell.expect(shell.new_master, b'\r?\nHC_CLONE_JOBS_DONE\r?\n')
        shell.command(shell.master, 'jobs -l > ' + shlex.quote(str(source_jobs)) +
                      '; print -r -- HC_SOURCE_JOBS_DONE')
        shell.expect(shell.master, b'\r?\nHC_SOURCE_JOBS_DONE\r?\n')
        assert clone_jobs.read_text() == '', 'clone unexpectedly kept a usable job table'
        assert str(shell.job_pid) in source_jobs.read_text(), 'source no longer owned its background job'
        after_tty = subprocess.run(['ps', '-p', str(shell.job_pid), '-o', 'tty='],
                                   capture_output=True, text=True, check=True).stdout.strip()
        assert after_tty == source_tty and '/dev/' + after_tty != new_tty
        return {'sourcePid': shell.pid, 'clonePid': shell.clone_pid, 'backgroundJobPid': shell.job_pid,
                'jobRemainsOnSourceTty': source_tty, 'newTty': new_tty,
                'cloneJobTableEmpty': True, 'sourceStillOwnsJob': True,
                'sourceMayClose': False}
    finally:
        shell.cleanup()


def zle_buffer(root):
    root.mkdir()
    shell = Shell(root, zle=True)
    try:
        shell.new_master, shell.new_slave = pty.openpty()
        tty = os.ttyname(shell.new_slave)
        # The receiver's initial termios must match the live line editor's
        # mode, since clone is invoked while ZLE is already active.
        termios.tcsetattr(shell.new_slave, termios.TCSANOW, termios.tcgetattr(shell.master))
        setup = root / 'widget.zsh'
        setup.write_text('HC_PROBE_DIR=' + shlex.quote(str(root)) + '\nHC_PROBE_TTY=' + shlex.quote(tty) + '\n' + r'''
zmodload zsh/clone
function hc_probe_observe() {
  print -rn -- "$BUFFER" > "$HC_PROBE_DIR/draft-$$"
  print -r -- "$CURSOR" > "$HC_PROBE_DIR/cursor-$$"
}
function hc_probe_clone() {
  local kept_buffer=$BUFFER kept_cursor=$CURSOR
  clone "$HC_PROBE_TTY"
  BUFFER=$kept_buffer CURSOR=$kept_cursor
  hc_probe_observe
  zle -R
}
zle -N hc_probe_clone
zle -N hc_probe_observe
bindkey '^X^X' hc_probe_clone
bindkey '^X^V' hc_probe_observe
print -r -- HC_WIDGET_READY
''')
        shell.command(shell.master, 'source ' + shlex.quote(str(setup)))
        shell.expect(shell.master, b'\r?\nHC_WIDGET_READY\r?\n')
        shell.expect(shell.master, b'HC> ')
        should_not_execute = root / 'MUST_NOT_EXECUTE'
        draft = "printf '%s' 'draft café 🦊'; touch " + shlex.quote(str(should_not_execute))
        back = 9
        os.write(shell.master, draft.encode() + b'\x1b[D' * back + b'\x18\x18')
        files = until(lambda: (paths if len(paths := list(root.glob('draft-*'))) == 2 else None))
        pids = [int(path.name.split('-')[1]) for path in files]
        assert shell.pid in pids
        shell.clone_pid = next(pid for pid in pids if pid != shell.pid)
        until(lambda: (root / f'cursor-{shell.clone_pid}').exists())
        for pid in pids:
            assert (root / f'draft-{pid}').read_text() == draft
            assert int((root / f'cursor-{pid}').read_text()) == len(draft) - back
        os.close(shell.new_slave)
        shell.new_slave = -1
        close_source(shell)
        # Modify the still-unsent line in the destination's existing editor;
        # capture with a second widget, never by submitting it as shell input.
        expected = draft[:len(draft)-back] + 'X' + draft[len(draft)-back:]
        os.write(shell.new_master, b'X\x18\x16')
        until(lambda: (root / f'draft-{shell.clone_pid}').read_text() == expected)
        assert int((root / f'cursor-{shell.clone_pid}').read_text()) == len(draft) - back + 1
        assert not should_not_execute.exists()
        return {'sourcePid': shell.pid, 'clonePid': shell.clone_pid, 'newTty': tty,
                'unicodeDraftAndMiddleCursorPreserved': True, 'sourceMasterClosed': True,
                'editAtRestoredCursorWorks': True, 'draftNotExecuted': True,
                'termiosCopiedBeforeClone': True}
    finally:
        shell.cleanup()


def extra_fd(root):
    root.mkdir()
    shell = Shell(root)
    try:
        shared = root / 'shared-offset.txt'
        shared.write_text('ABCD')
        shell.command(shell.master, 'exec {HC_EXTRA}>"$TTY"; exec {HC_GENERIC}>/dev/tty; '
                      'exec {HC_FILE}<' + shlex.quote(str(shared)) + '; '
                      'print -r -- "HC_EXTRA_FD=$HC_EXTRA HC_GENERIC_FD=$HC_GENERIC"')
        _, match = shell.expect(shell.master, rb'HC_EXTRA_FD=([0-9]+) HC_GENERIC_FD=([0-9]+)\r?\n')
        fd, generic = int(match[1]), int(match[2])
        shell.expect(shell.master, b'HC> ')
        tty = shell.clone()
        before = subprocess.run(['/usr/sbin/lsof', '-nP', '-a', '-p', str(shell.clone_pid), '-d', f'{fd},{generic}', '-F', 'pfn'],
                                capture_output=True, text=True)
        (root / 'extra-fd-before-close.txt').write_text(before.stdout + before.stderr)
        shell.command(shell.new_master, 'read -k1 -u $HC_FILE HC_CHAR; print -r -- "HC_CHILD_FILE_CHAR=$HC_CHAR"')
        shell.expect(shell.new_master, rb'HC_CHILD_FILE_CHAR=A\r?\n')
        shell.expect(shell.new_master, b'HC> ')
        shell.command(shell.master, 'read -k1 -u $HC_FILE HC_CHAR; print -r -- "HC_PARENT_FILE_CHAR=$HC_CHAR"')
        shell.expect(shell.master, rb'HC_PARENT_FILE_CHAR=B\r?\n')
        close_source(shell)
        shell.command(shell.new_master, 'print -u $HC_EXTRA -- HC_EXTRA_WRITE; print -r -- "HC_EXTRA_STATUS=$?"')
        output, status = shell.expect(shell.new_master, rb'HC_EXTRA_STATUS=([0-9]+)\r?\n')
        (root / 'extra-fd-after-close.txt').write_bytes(output)
        assert int(status[1]) != 0, 'old terminal fd unexpectedly survived master closure'
        shell.expect(shell.new_master, b'HC> ')
        shell.command(shell.new_master, 'print -u $HC_GENERIC -- HC_GENERIC_WRITE; print -r -- "HC_GENERIC_STATUS=$?"')
        generic_output, _ = shell.expect(shell.new_master, rb'HC_GENERIC_STATUS=0\r?\n')
        assert b'\r\nHC_GENERIC_WRITE\r\n' in generic_output
        return {'sourcePid': shell.pid, 'clonePid': shell.clone_pid, 'newTty': tty,
                'extraFd': fd, 'extraFdLsof': before.stdout,
                'extraFdWriteStatusAfterOriginalMasterClose': int(status[1]),
                'genericDevTtyFollowsNewControllingTty': True,
                'regularFileOffsetSharedWithSource': True,
                'extraTerminalDescriptorsNeedExplicitHandling': True}
    finally:
        shell.cleanup()


def main():
    if sys.platform != 'darwin':
        raise SystemExit('This probe uses macOS stock /bin/zsh and owned PTYs.')
    root = Path(tempfile.mkdtemp(prefix='hc-zsh-clone-', dir='/tmp')).resolve()
    print('Private diagnostics:', root, flush=True)
    results = {'idleMemory': idle_memory(root / 'idle'), 'activeJob': active_job(root / 'active'),
               'zleBuffer': zle_buffer(root / 'zle'), 'extraTerminalFd': extra_fd(root / 'fd')}
    (root / 'result.json').write_text(json.dumps(results, indent=2))
    print(json.dumps(results, indent=2), flush=True)


if __name__ == '__main__':
    main()
