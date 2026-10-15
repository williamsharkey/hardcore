#!/usr/bin/env python3
"""Live GUI integration tests; use isolated HOME, runtime sockets and shell jobs.
Runs only on macOS with a logged-in WindowServer. Never targets user windows.
"""
import array
import os
from pathlib import Path
import plistlib
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

EXE = str(Path(sys.argv[1] if len(sys.argv) > 1 else 'build/hardcore').resolve())


def wait_for(fn, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = fn()
        if result:
            return result
        time.sleep(.05)
    raise AssertionError('timed out waiting for condition')


def send(sock, obj):
    data = plistlib.dumps(obj, fmt=plistlib.FMT_BINARY)
    sock.sendall(struct.pack('!I', len(data)) + data)


def exact(sock, n):
    result = b''
    while len(result) < n:
        data = sock.recv(n-len(result))
        if not data:
            raise EOFError('peer closed')
        result += data
    return result


def recv(sock):
    return plistlib.loads(exact(sock, struct.unpack('!I', exact(sock, 4))[0]))


def request(path, op):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(20)
        sock.connect(str(path))
        send(sock, op)
        return recv(sock)


with tempfile.TemporaryDirectory(prefix='hc-test-', dir='/tmp') as temp:
    root = Path(temp)
    home = root/'home'
    (home/'Library/Application Support').mkdir(parents=True)
    runtime = root/'run'
    ctl = root/'ctl'
    env = dict(os.environ, HOME=str(home), HARDCORE_RUNTIME_DIR=str(runtime),
               HARDCORE_CTL=str(ctl), HARDCORE_NO_ACTIVATE='1', HARDCORE_SHELL='/bin/bash')
    jobs = []
    def windows():
        found = []
        for path in runtime.glob('*.sock'):
            try:
                info = request(path, {'op':'list'})
                found.append((path, info))
            except (OSError, EOFError):
                pass
        return found
    def command(text):
        fd = os.open(ctl, os.O_WRONLY | os.O_NONBLOCK)
        try: os.write(fd, ('type '+text+'\\n\n').encode())
        finally: os.close(fd)
    def run(*args, ok=True):
        r = subprocess.run([EXE, *args], env=env, capture_output=True, text=True, timeout=25)
        if ok: assert r.returncode == 0, (args, r.stdout, r.stderr)
        return r
    log = open(root/'app.log', 'w')
    original = subprocess.Popen([EXE], env=env, stdout=log, stderr=log)
    try:
        first = wait_for(lambda: windows())
        pid = first[0][1]['tabs'][0]['pid']; jobs.append(pid)
        command(f'echo $$ > {root}/shell.pid')
        wait_for(lambda: (root/'shell.pid').exists())
        assert int((root/'shell.pid').read_text()) == pid
        # Failed replacement must not close/read the live PTY.
        r = run('reload', '--exe', '/usr/bin/false', ok=False)
        assert r.returncode != 0, (r.stdout, r.stderr, windows())
        assert wait_for(windows)[0][1]['tabs'][0]['pid'] == pid, original.poll()
        command(f'echo rollback-ok > {root}/rollback')
        wait_for(lambda: (root/'rollback').exists())
        # Preserve multiple tabs and the active tab, not just one PTY.
        with ctl.open('w') as control: control.write('cmd newtab\ncmd tab 0\n')
        wait_for(lambda: len(windows()[0][1]['tabs'])==2)
        expected_pids={tab['pid'] for tab in windows()[0][1]['tabs']}
        jobs.extend(expected_pids)
        # A foreground process survives with its PID, PTY and in-memory state.
        worker = root/'worker.py'
        worker.write_text('''import os, sys, time, termios
from pathlib import Path
attr=termios.tcgetattr(0); attr[3] &= ~termios.ECHO; termios.tcsetattr(0,termios.TCSANOW,attr)
Path(sys.argv[1]).write_text(str(os.getpid()))
print("BEFORE-HANDOFF", flush=True)
os.write(1,bytes([0xe2]))
first=True
while True:
    line = sys.stdin.readline()
    if not line: break
    Path(sys.argv[2]).write_text(str(os.getpid()) + ":" + line)
    os.write(1,bytes([0x82,0xac]) if first else b"m")
    first=False
    print("AFTER-HANDOFF:"+line, flush=True)
    os.write(1,bytes([27])+b"[31")
''')
        command(f'{sys.executable} {worker} {root}/worker.pid {root}/answer')
        wait_for(lambda: (root/'worker.pid').exists())
        worker_pid = int((root/'worker.pid').read_text()); jobs.append(worker_pid)
        old_window_pid = windows()[0][1]['pid']
        print(run('update', '--local').stdout.strip(), flush=True)
        # Reload execs the new binary in the existing GUI process. The native
        # command waits for its receiving listener before reporting success.
        new = wait_for(lambda: next((w for w in windows() if w[1]['pid'] == old_window_pid), None))
        assert original.pid == old_window_pid and original.poll() is None
        assert new[1]['tabs'][0]['pid'] == pid
        assert {tab['pid'] for tab in new[1]['tabs']} == expected_pids
        os.kill(worker_pid, 0)
        command('still-alive')
        wait_for(lambda: (root/'answer').exists())
        assert (root/'answer').read_text() == f'{worker_pid}:still-alive\n'
        # Take + disconnect before READY rolls back, including received FDs.
        with socket.socket(socket.AF_UNIX) as sock:
            sock.settimeout(15); sock.connect(str(new[0])); send(sock, {'op':'take'})
            state = recv(sock); assert state['tabs']
            assert state['tabs'][0]['st'] == 3 and state['tabs'][0]['params'][0] == 31
            assert '€'.encode() in state['tabs'][0]['snapshot']
            for _ in state['tabs']:
                _, anc, _, _ = sock.recvmsg(1, socket.CMSG_SPACE(4))
                for level, typ, data in anc:
                    if typ == socket.SCM_RIGHTS:
                        fds=array.array('i');fds.frombytes(data)
                        for fd in fds: os.close(fd)
        command('after-aborted-take')
        wait_for(lambda: 'after-aborted-take' in (root/'answer').read_text())
        # Slurp moves the same live job into another window.
        print(run('slurp', '--pid', str(new[1]['pid'])).stdout.strip(), flush=True)
        slurped = wait_for(lambda: next((w for w in windows() if w[1]['pid'] != new[1]['pid']), None))
        assert slurped[1]['tabs'][0]['pid'] == pid
        command('after-slurp')
        wait_for(lambda: 'after-slurp' in (root/'answer').read_text())
        assert (root/'answer').read_text().startswith(str(worker_pid)+':')
        # Existing tmux sessions attach without replacing their shell/job.
        tmux_socket = str(root/'tmux.sock')
        def tmux(*args):
            return subprocess.run(['tmux','-S',tmux_socket,*args], env=env, capture_output=True, text=True, check=True).stdout.strip()
        try:
            tmux('new-session','-d','-s','handoff-test','/bin/bash --noprofile --norc')
            pane_pid = int(tmux('display-message','-p','-t','handoff-test','#{pane_pid}'))
            jobs.append(pane_pid)
            listing=run('slurp','--list','--tmux','handoff-test','--tmux-socket',tmux_socket)
            assert 'handoff-test' in listing.stdout
            run('slurp','--tmux','handoff-test','--tmux-socket',tmux_socket)
            wait_for(lambda: int(tmux('display-message','-p','-t','handoff-test','#{session_attached}')) > 0)
            assert int(tmux('display-message','-p','-t','handoff-test','#{pane_pid}')) == pane_pid
        finally:
            subprocess.run(['tmux','-S',tmux_socket,'kill-server'],capture_output=True)
        print('PASS: tmux attachment preserves its original shell PID', flush=True)
        print('PASS: live update retains GUI PID, failed replacement rollback, aborted transfer rollback, slurp, same shell/job PIDs and continued input/output', flush=True)
    finally:
        for _, info in windows():
            for tab in info['tabs']: jobs.append(tab['pid'])
            try: os.kill(info['pid'], signal.SIGTERM)
            except ProcessLookupError: pass
        original.poll()
        if original.poll() is None: original.terminate()
        original.wait(timeout=5)
        for pid in set(jobs):
            try: os.kill(pid, signal.SIGHUP)
            except ProcessLookupError: pass
        log.close()
        Path("build/handoff-test.log").write_text((root/"app.log").read_text())
