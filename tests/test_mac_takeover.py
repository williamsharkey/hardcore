#!/usr/bin/env python3
"""Experimental Terminal -> Hardcore takeover tests, never arbitrary user PIDs.

Requires macOS GUI, Terminal Automation, and LLDB attach permission. All jobs
and windows are created by this test. This is not the production slurp backend.
Source zsh uses NO_HUP. Migration requires a debuggable, single-process job
that is not a session leader. Debugger call failures after setsid cannot yet be
rolled back. Open-file-description sharing is not preserved. Ordinary kqueue
read/write registrations use a private ABI; other event mechanisms are not
handled. Do not use this machinery on user jobs.
"""
import argparse
import array
import ctypes
import fcntl
import json
import os
from pathlib import Path
import plistlib
import pty
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

REPO = Path(__file__).resolve().parent.parent
EXE = str(REPO/'build/hardcore')


def apple(source):
    return subprocess.run(['osascript', '-e', source], check=True,
                          capture_output=True, text=True, timeout=15).stdout.strip()


def wait(fn, description='condition', timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError('timed out: ' + description)


def send(sock, value):
    data = plistlib.dumps(value, fmt=plistlib.FMT_BINARY)
    sock.sendall(struct.pack('!I', len(data)) + data)


def exact(sock, count):
    data = b''
    while len(data) < count:
        part = sock.recv(count-len(data))
        if not part:
            raise EOFError('receiver closed')
        data += part
    return data


def receive(sock):
    return plistlib.loads(exact(sock, struct.unpack('!I', exact(sock, 4))[0]))


def children(pid):
    result = subprocess.run(['pgrep', '-P', str(pid)], capture_output=True, text=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(result.stderr)
    return [int(value) for value in result.stdout.split()]


def eligible(pid):
    if os.getsid(pid) == pid:
        raise ValueError('session leader')
    if children(pid):
        raise ValueError('job has child processes')


def tty_fds(pid, device):
    # Read-only discovery, restricted to this test's own job. A product backend
    # must repeat discovery while all target threads are stopped.
    result = subprocess.run(['/usr/sbin/lsof', '-a', '-p', str(pid), '-Ffn'],
                            capture_output=True, text=True, timeout=10, check=True)
    found = []
    fd = None
    for line in result.stdout.splitlines():
        if line.startswith('f'):
            fd = int(line[1:]) if line[1:].isdigit() else None
        if fd is not None and line in ('n'+device, 'n/dev/tty'):
            found.append(fd)
    assert set((0, 1, 2)).issubset(found), result.stdout
    return found


def tty_watchers(pid, descriptors):
    # Experimental private ABI, from Apple's XNU proc_info_private.h and
    # event_private.h. Only ordinary fd-backed read/write kqueues are replayed.
    # https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/proc_info_private.h
    class Event(ctypes.Structure):
        _fields_ = [('ident', ctypes.c_uint64), ('filter', ctypes.c_int16),
                    ('flags', ctypes.c_uint16), ('qos', ctypes.c_int32),
                    ('udata', ctypes.c_uint64), ('fflags', ctypes.c_uint32),
                    ('xflags', ctypes.c_uint32), ('data', ctypes.c_int64),
                    ('ext', ctypes.c_uint64*4)]

    class Info(ctypes.Structure):
        _fields_ = [('event', Event), ('sdata', ctypes.c_uint64),
                    ('status', ctypes.c_int32), ('sfflags', ctypes.c_int32),
                    ('reserved', ctypes.c_uint64*2)]

    assert ctypes.sizeof(Info) == 104
    lib = ctypes.CDLL('/usr/lib/libproc.dylib', use_errno=True)
    lib.proc_pidinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint64,
                                ctypes.c_void_p, ctypes.c_int]
    lib.proc_pidfdinfo.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_void_p, ctypes.c_int]
    fds = ctypes.create_string_buffer(65536)
    size = lib.proc_pidinfo(pid, 1, 0, fds, len(fds))
    assert 0 < size < len(fds) and size % 8 == 0
    result = []
    for fd, kind in struct.iter_unpack('iI', fds.raw[:size]):
        if kind != 5:  # PROX_FDTYPE_KQUEUE
            continue
        entries = (Info*1024)()
        count = lib.proc_pidfdinfo(pid, fd, 9, entries, ctypes.sizeof(entries))
        assert 0 <= count < len(entries), ('kqueue introspection', ctypes.get_errno())
        for entry in entries[:count]:
            ev = entry.event
            if ev.ident not in descriptors or ev.filter not in (-1, -2):
                continue
            assert not ev.xflags and not ev.qos and not any(ev.ext), 'advanced kqueue unsupported'
            # KN_DISABLED is bit 8 in XNU; retain disabled subscriptions.
            flags = 1 | (ev.flags & (0x10 | 0x20 | 0x80)) | (8 if entry.status & 8 else 0)
            result.append((fd, ev.ident, ev.filter, flags, entry.sfflags, entry.sdata, ev.udata))
    return result


class Session:
    def __init__(self, root):
        self.root = root
        self.pid = self.window = self.app = None
        self.child_pids = []
        self.master, self.slave = pty.openpty()
        self.sender, self.receiver = socket.socketpair()
        self.sender.settimeout(20)
        self.control = root/'ctl'
        home = root/'home'
        (home/'Library/Application Support').mkdir(parents=True)
        self.env = dict(os.environ, HOME=str(home), HARDCORE_RUNTIME_DIR=str(root/'runtime'),
                        HARDCORE_CTL=str(self.control), HARDCORE_NO_ACTIVATE='1')

    def start(self, command, nohup=True):
        command = ('setopt NO_HUP; ' if nohup else '') + shlex.join(command)
        self.device = apple('tell application "Terminal"\nset t to do script ' + json.dumps(command) +
                            '\nset custom title of t to ' + json.dumps(self.root.name) +
                            '\nreturn tty of t\nend tell')
        self.window = int(apple('tell application "Terminal"\nrepeat with w in windows\n'
                               'repeat with t in tabs of w\nif tty of t is ' +
                               json.dumps(self.device) + ' then return id of w\nend repeat\nend repeat\nend tell'))
        wait(lambda: (self.root/'ready').exists() and len((self.root/'ready').read_text().split()) == 2,
             'job ready')
        self.pid, self.parent_group = map(int, (self.root/'ready').read_text().split())

    def stage(self, exe=EXE):
        # The receiver must paint successfully BEFORE the job is altered.
        with (self.root/'app.log').open('w') as log:
            self.app = subprocess.Popen([exe, '--adopt-fd', str(self.receiver.fileno())], env=self.env,
                                        pass_fds=(self.receiver.fileno(),), stdout=log, stderr=log)
        self.receiver.close()
        send(self.sender, {'protocol': 1, 'active': 0, 'tabs': [{
            'pid': self.pid, 'cols': 80, 'rows': 24, 'snapshot': b'', 'osc': b'',
            'params': [0]*32, 'marks': [], 'dir': str(self.root), 'st': 0,
        }]})
        self.sender.sendmsg([b'F'], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [self.master]))])
        assert receive(self.sender)['ready']

    def move(self):
        eligible(self.pid)
        descriptors = tty_fds(self.pid, self.device)
        watchers = tty_watchers(self.pid, descriptors)
        restore_watchers = ''
        for queue, ident, filt, flags, fflags, data, udata in watchers:
            restore_watchers += f'''if(ok) {{
                struct HCEvent {{ unsigned long ident; short filter; unsigned short flags;
                    unsigned int fflags; long data; void *udata; }} ev =
                    {{{ident},{filt},{flags},{fflags},{data},(void*){udata}ULL}};
                if(((int(*)(int,void*,int,void*,int,void*))kevent)({queue},&ev,1,0,0,0)<0) ok=0;
            }}'''
        fd = os.open(self.device, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
        try:
            termios.tcsetattr(self.slave, termios.TCSANOW, termios.tcgetattr(fd))
            fcntl.ioctl(self.slave, termios.TIOCSWINSZ, fcntl.ioctl(fd, termios.TIOCGWINSZ, b'\0'*8))
        finally:
            os.close(fd)
        # Pre-open replacements and capture flags before the irreversible setsid.
        # Each original FD gets its access mode, status flags and FD_CLOEXEC back.
        # This deliberately does not claim to preserve shared file descriptions.
        fds = ','.join(map(str, descriptors))
        path = json.dumps(os.ttyname(self.slave))
        n = len(descriptors)
        expression = f'''expression -- ({{
            int old[{n}]={{{fds}}}, fresh[{n}], flags[{n}], status[{n}]; int ok=1;
            for(int i=0;i<{n};i++) {{
                flags[i]=((int(*)(int,int,...))fcntl)(old[i],1);
                status[i]=((int(*)(int,int,...))fcntl)(old[i],3);
                fresh[i]=((int(*)(const char*,int,...))open)({path},(status[i]&3)|{os.O_NOCTTY});
                if(fresh[i]<0 || flags[i]<0 || status[i]<0) ok=0;
            }}
            if(ok) ok=((int)setpgid(0,{self.parent_group})==0 && (int)setsid()>0);
            if(ok) ok=(((int(*)(int,unsigned long,...))ioctl)(fresh[0],{termios.TIOCSCTTY}UL,0)==0);
            if(ok) for(int i=0;i<{n};i++) {{
                if((int)dup2(fresh[i],old[i])<0) ok=0;
                if(((int(*)(int,int,...))fcntl)(old[i],2,flags[i])<0 ||
                   ((int(*)(int,int,...))fcntl)(old[i],4,status[i])<0) ok=0;
            }}
            for(int i=0;i<{n};i++) if(fresh[i]>=0) (int)close(fresh[i]);
            {restore_watchers}
            ok;
        }})'''
        expression = ' '.join(expression.splitlines())
        result = subprocess.run(['lldb', '--batch', '-p', str(self.pid), '-o', expression,
                                 '-o', 'process detach'], capture_output=True, text=True, timeout=25)
        (self.root/'debugger.log').write_text(result.stdout + result.stderr)
        assert result.returncode == 0 and '= 1' in result.stdout, result.stdout + result.stderr
        send(self.sender, {'go': True})
        for name in ('master', 'slave'):
            os.close(getattr(self, name)); setattr(self, name, -1)
        wait(self.control.exists, 'Hardcore controls')
        print(f'  moved PID {self.pid}, tty descriptors {descriptors}, kqueue watchers {len(watchers)}', flush=True)

    def close_terminal(self, require_idle=False):
        # Closing a busy tab raises Terminal's modal termination dialog. Never
        # suppress that preference or click through it. Agent-attachment probes
        # require an idle shell after exiting their disposable source frontend.
        # Debugger migration tests retain their historical close behavior: a
        # migrated child may still be in the original shell's job table.
        # Keep the window id on failure so cleanup can retry after killing only
        # this fixture's processes.
        if require_idle:
            wait(lambda: apple(f'tell application "Terminal" to return busy of selected tab of window id {self.window}') == 'false',
                 'test Terminal tab becomes idle before close', timeout=10)
        apple(f'tell application "Terminal" to close window id {self.window}')
        self.window = None

    def ctl(self, command):
        fd = os.open(self.control, os.O_WRONLY | os.O_NONBLOCK)
        try:
            os.write(fd, (command+'\n').encode())
        finally:
            os.close(fd)

    def output(self):
        path = self.root/'output'
        return path.read_text(errors='replace') if path.exists() else ''

    def reload(self):
        result = subprocess.run([EXE, 'update', '--local'], env=self.env, capture_output=True,
                                text=True, timeout=25)
        assert result.returncode == 0, result.stdout + result.stderr
        wait(self.control.exists)

    def cleanup(self):
        self.sender.close(); self.receiver.close()
        for pid in self.child_pids + ([self.pid] if self.pid else []):
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        # Includes replacement windows created by the reload test, all in our
        # private runtime directory. Never enumerate or signal user windows.
        for endpoint in (self.root/'runtime').glob('*.sock'):
            try:
                os.kill(int(endpoint.stem), signal.SIGTERM)
            except ProcessLookupError:
                pass
        if self.app:
            try:
                self.app.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.app.kill(); self.app.wait()
        if self.window:
            self.close_terminal(require_idle=True)
        for fd in (self.master, self.slave):
            if fd >= 0:
                os.close(fd)


def worker(session, raw=False, failed_receiver=False):
    root = session.root
    binary = root/'worker'
    subprocess.run(['clang', '-Wall', '-Wextra', '-g', str(REPO/'tests/fixtures/mac_takeover_worker.c'),
                    '-o', str(binary)], check=True)
    session.start([str(binary), str(root/'ready'), str(root/'output'), str(int(raw))])
    if failed_receiver:
        original_sid = os.getsid(session.pid)
        try:
            session.stage('/usr/bin/false')
        except (OSError, EOFError):
            pass
        else:
            raise AssertionError('invalid receiver accepted')
        assert os.getsid(session.pid) == original_sid
        apple('tell application "Terminal" to do script "RECEIVER-FAILED" in selected tab of window id ' + str(session.window))
        wait(lambda: 'RECEIVER-FAILED' in session.output(), 'source still responds')
        return
    session.stage(); session.move(); session.close_terminal()
    session.ctl('type AFTER-CLOSE-λ\\n')
    wait(lambda: 'AFTER-CLOSE-λ' in session.output(), 'same job responds after Terminal closes')
    assert f'pid={session.pid} sid={session.pid} fg={session.pid} flags=1' in session.output()
    session.ctl('cmd resize 1000,700')
    wait(lambda: 'WINCH ' in session.output(), 'resize signal')
    session.ctl('type \x03')
    if raw:
        wait(lambda: '\x03' in session.output(), 'raw Ctrl-C byte')
        assert 'SIGINT' not in session.output()
    else:
        wait(lambda: 'SIGINT' in session.output(), 'Ctrl-C signal')
    session.reload()
    session.ctl('type AFTER-RELOAD\\n')
    wait(lambda: 'AFTER-RELOAD' in session.output(), 'migrated job survives Hardcore reload')
    os.kill(session.pid, 0)


def python_repl(session):
    root = session.root
    script = root/'startup.py'
    script.write_text('''import os
from pathlib import Path
secret = "kept-in-memory"
output = open(''' + repr(str(root/'output')) + ''', 'w', buffering=1)
def check(tag):
    t = os.open('/dev/tty', os.O_RDWR)
    output.write(f'{tag} {secret} pid={os.getpid()} sid={os.getsid(0)} fg={os.tcgetpgrp(t)}\\n')
    os.write(t, b'CTTY-OK\\n')
    os.close(t)
Path(''' + repr(str(root/'ready')) + ''').write_text(f'{os.getpid()} {os.getpgid(os.getppid())}')
''')
    session.start([sys.executable, '-i', str(script)])
    time.sleep(.3)  # enter the interpreter's real blocking input path
    session.stage(); session.move(); session.close_terminal()
    session.ctl("type check('AFTER-CLOSE')\\n")
    wait(lambda: 'AFTER-CLOSE kept-in-memory' in session.output(), 'Python REPL state')
    assert f'pid={session.pid} sid={session.pid} fg={session.pid}' in session.output()
    session.ctl('type \x03')
    session.ctl("type check('AFTER-INTERRUPT')\\n")
    wait(lambda: 'AFTER-INTERRUPT' in session.output(), 'Python REPL after Ctrl-C')
    session.reload()
    session.ctl("type check('AFTER-RELOAD')\\n")
    wait(lambda: 'AFTER-RELOAD' in session.output(), 'Python REPL after reload')


def reject_children(session):
    root = session.root
    script = root/'tree.py'
    script.write_text('''import os, subprocess, time
from pathlib import Path
child = subprocess.Popen(['/bin/sleep', '120'])
Path(''' + repr(str(root/'child')) + ''').write_text(str(child.pid))
Path(''' + repr(str(root/'ready')) + ''').write_text(f'{os.getpid()} {os.getpgid(os.getppid())}')
while True: time.sleep(.1)
''')
    session.start([sys.executable, str(script)])
    session.child_pids.append(int((root/'child').read_text()))
    sid = os.getsid(session.pid)
    try:
        eligible(session.pid)
    except ValueError as e:
        assert str(e) == 'job has child processes'
    else:
        raise AssertionError('unsafe tree accepted')
    assert os.getsid(session.pid) == sid
    for pid in [session.pid] + session.child_pids:
        os.kill(pid, 0)


def node_repl(session):
    node = shutil.which('node')
    if not node:
        raise RuntimeError('Node is required for the node takeover case')
    root = session.root
    script = root/'startup.js'
    script.write_text('''const fs = require('node:fs');
const cp = require('node:child_process');
global.secret = 'kept-in-memory';
global.check = (tag) => {
  const tty = fs.openSync('/dev/tty', 'r+');
  fs.writeSync(tty, 'CTTY-OK\\n'); fs.closeSync(tty);
  fs.appendFileSync(''' + json.dumps(str(root/'output')) + ''',
    `${tag} ${global.secret} pid=${process.pid}\\n`);
};
const group = cp.execFileSync('/bin/ps', ['-o', 'pgid=', '-p', String(process.ppid)], {encoding:'utf8'}).trim();
fs.writeFileSync(''' + json.dumps(str(root/'ready')) + ''', `${process.pid} ${group}`);
''')
    session.start([node, '-i', '-r', str(script)])
    time.sleep(.3)
    session.stage(); session.move(); session.close_terminal()
    session.ctl("type check('AFTER-CLOSE')\\n")
    wait(lambda: 'AFTER-CLOSE kept-in-memory' in session.output(), 'Node REPL state')
    assert f'pid={session.pid}' in session.output()
    assert os.getsid(session.pid) == session.pid
    session.reload()
    session.ctl("type check('AFTER-RELOAD')\\n")
    wait(lambda: 'AFTER-RELOAD' in session.output(), 'Node REPL after reload')


def protected_shell(session):
    root = session.root
    command = ('print -r -- "$$ $(/bin/ps -o pgid= -p $PPID)" > ' + shlex.quote(str(root/'ready')) +
               '; while read line; do print -r -- "$line" >> ' + shlex.quote(str(root/'output')) + '; done')
    session.start(['/bin/zsh', '-f', '-c', command])
    sid = os.getsid(session.pid)
    result = subprocess.run(['lldb', '--batch', '-p', str(session.pid), '-o', 'process detach'],
                            capture_output=True, text=True, timeout=20)
    (root/'debugger.log').write_text(result.stdout + result.stderr)
    assert result.returncode != 0 and 'attach failed' in result.stderr + result.stdout, result.stdout + result.stderr
    assert os.getsid(session.pid) == sid
    apple('tell application "Terminal" to do script "PROTECTED-STILL-ALIVE" in selected tab of window id ' + str(session.window))
    wait(lambda: 'PROTECTED-STILL-ALIVE' in session.output(), 'protected source still usable')


def session_leader(session):
    root = session.root
    script = root/'leader.py'
    script.write_text('''import os
from pathlib import Path
os.setpgid(0, os.getpgid(os.getppid()))
os.setsid()
Path(''' + repr(str(root/'ready')) + ''').write_text(f'{os.getpid()} {os.getpgid(os.getppid())}')
while True:
    line = input()
    with open(''' + repr(str(root/'output')) + ''', 'a') as f: f.write(line + '\\n')
''')
    session.start([sys.executable, str(script)])
    assert os.getsid(session.pid) == session.pid
    try:
        eligible(session.pid)
    except ValueError as e:
        assert str(e) == 'session leader'
    else:
        raise AssertionError('session leader accepted')
    apple('tell application "Terminal" to do script "LEADER-STILL-ALIVE" in selected tab of window id ' + str(session.window))
    wait(lambda: 'LEADER-STILL-ALIVE' in session.output(), 'session leader still usable')


def curses_app(session):
    root = session.root
    script = root/'screen.py'
    script.write_text('''import curses, os
from pathlib import Path
output = open(''' + repr(str(root/'output')) + ''', 'w', buffering=1)
def main(screen):
    curses.raw()
    screen.keypad(True)
    secret = 'SCREEN-STATE-PRESERVED'
    curses.start_color()
    curses.init_pair(1, curses.COLOR_GREEN, curses.COLOR_BLACK)
    def draw():
        screen.erase()
        screen.addstr(0, 0, secret, curses.color_pair(1))
        screen.refresh()
    draw()
    Path(''' + repr(str(root/'ready')) + ''').write_text(f'{os.getpid()} {os.getpgid(os.getppid())}')
    while True:
        key = screen.get_wch()
        if key == curses.KEY_RESIZE:
            screen.clearok(True)
            output.write(f'RESIZE {screen.getmaxyx()}\\n')
        else:
            output.write(f'{secret} pid={os.getpid()} key={key!r}\\n')
        draw()
curses.wrapper(main)
''')
    session.start([sys.executable, str(script)])
    session.stage(); session.move(); session.close_terminal()
    session.ctl('cmd resize 1000,700')
    wait(lambda: 'RESIZE ' in session.output(), 'curses resize')
    # Curses keypad mode expects SS3. This checks byte transport; recovering
    # Terminal's emulator modes for Hardcore's physical-key mapping is separate.
    session.ctl('type λ\x1bOA')
    wait(lambda: "key='λ'" in session.output() and 'key=259' in session.output(), 'curses Unicode and Up key')
    assert f'SCREEN-STATE-PRESERVED pid={session.pid}' in session.output()
    session.reload()
    session.ctl('type R')
    wait(lambda: "key='R'" in session.output(), 'curses after reload')
    screenshot = REPO/'build/mac-takeover-curses.bmp'
    screenshot.unlink(missing_ok=True)
    session.ctl('snap ' + str(screenshot))
    wait(lambda: screenshot.exists() and screenshot.stat().st_size > 1000, 'curses screenshot')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    cases = {'canonical': lambda s: worker(s), 'raw': lambda s: worker(s, raw=True),
             'python': python_repl, 'node': node_repl, 'children': reject_children,
             'protected-shell': protected_shell, 'session-leader': session_leader,
             'curses': curses_app,
             'receiver-failure': lambda s: worker(s, failed_receiver=True)}
    parser.add_argument('--case', choices=cases, action='append')
    parser.add_argument('--repeat', type=int, default=1)
    args = parser.parse_args()
    assert args.repeat > 0
    for repetition in range(args.repeat):
        for name in args.case or cases:
            if name == 'node' and not shutil.which('node'):
                print('SKIP node: Node is not installed', flush=True)
                continue
            print(f'RUN {name} ({repetition+1}/{args.repeat})', flush=True)
            with tempfile.TemporaryDirectory(prefix='hc-takeover-', dir='/tmp') as directory:
                session = Session(Path(directory))
                try:
                    cases[name](session)
                    print(f'PASS {name}', flush=True)
                except BaseException:
                    for filename in ('output', 'app.log', 'debugger.log'):
                        path = session.root/filename
                        if path.exists():
                            print(filename + ':\n' + path.read_text(errors='replace'), flush=True)
                    raise
                finally:
                    session.cleanup()


if __name__ == '__main__':
    main()
