#!/usr/bin/env python3
"""Black-box native-backend checks. Only disposable PTYs/sessions are touched.

The test harness is Python; the runtime under test is a bare executable with a
PATH containing neither Python nor Lua and no Resources directory.
"""
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tempfile
import time
import unittest
import uuid

from test_here_dispatch import OwnedTerminal, tty_settings, alive

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/hardcore'


@unittest.skipUnless(EXE.is_file() and os.name == 'posix', 'native POSIX build required')
class NativeHere(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='hc native runtime ', dir='/tmp')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.exe = self.root / 'hardcore'
        shutil.copy2(EXE, self.exe)
        (self.root/'home').mkdir()
        (self.root/'empty-path').mkdir()
        self.env = dict(os.environ, HOME=str(self.root/'home'), PATH=str(self.root/'empty-path'),
                        SHELL='/bin/sh', TERM='xterm-256color', COLORTERM='truecolor', TERM_PROGRAM='owned-truecolor-host')
        for key in ('HARDCORE_CTL', 'HARDCORE_ADOPT', 'HARDCORE_RUNTIME_DIR', 'HARDCORE_TEST_CONFIG',
                    'HARDCORE_HERE_SESSION', 'HARDCORE_HERE_PID', 'HARDCORE_HERE_TAB_PID', 'HC_HERE_MEDIA_FD'):
            self.env.pop(key, None)
        self.name = 'native-test-' + uuid.uuid4().hex[:12]
        self.addCleanup(self.stop)
        self.hosts = []

    def cli(self, *args, timeout=10):
        return subprocess.run([str(self.exe), 'here', *args], env=self.env,
            cwd=self.root, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout)

    def stop(self, name=None):
        name = name or self.name
        state = self.cli('--status', name)
        owned = set()
        if state.returncode == 0:
            state = json.loads(state.stdout)
            owned.add(state['pid'])
            owned.update(tab['pid'] for tab in state['tabs'])
        self.cli('--stop', name)
        # --stop acknowledges the request before PTY shutdown and settings
        # persistence. Wait for the owned session to finish those writes before
        # TemporaryDirectory removes HOME; retrying rmtree would hide the race.
        lock = Path('/tmp') / f'hardcore-here-{os.getuid()}' / (name + '.native.sock.lock')
        try:
            fd = os.open(lock, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
        except FileNotFoundError:
            fd = None
        try:
            deadline = time.monotonic() + 10
            while True:
                released = fd is None
                if fd is not None:
                    try:
                        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        pass
                    else:
                        released = True
                        fcntl.flock(fd, fcntl.LOCK_UN)
                remaining = [pid for pid in owned if alive(pid)]
                if released and not remaining:
                    return
                if time.monotonic() >= deadline:
                    self.fail(f'Owned session {name} did not finish shutdown: '
                              f'live PIDs={remaining}, lock held={not released}')
                time.sleep(.02)
        finally:
            if fd is not None:
                os.close(fd)

    def host(self, *args):
        t = OwnedTerminal([str(self.exe), 'here', *args], self.env, self.root)
        self.hosts.append(t)
        self.addCleanup(t.close)
        return t

    def start(self, script='printf "NATIVE_READY\\n"; exec /bin/cat'):
        t = self.host('--session', self.name, '--shell', '/bin/sh', '--', '/bin/sh', '-c', script)
        t.wait(lambda: b'NATIVE_READY' in t.output, 'native bare executable startup')
        return t

    def status(self):
        r = self.cli('--status', self.name)
        self.assertEqual(r.returncode, 0, r.stdout+r.stderr)
        return json.loads(r.stdout)

    def test_bare_binary_has_no_script_runtime_and_helpers_are_built_in(self):
        r = self.cli('--help')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('Ctrl-B', r.stdout)
        t = self.start()
        t.send(b'bare-runtime-input\n')
        t.wait(lambda: b'bare-runtime-input' in t.output, 'native child echo')
        # Files is another mode of this exact executable; no helper scripts.
        t.send(b'\x02F')
        t.wait(lambda: b'Hardcore files' in t.output, 'built-in Files')
        self.assertFalse((self.root/'Resources').exists())

    def test_detach_reattach_preserves_process_and_unsubmitted_input(self):
        t = self.start()
        before = self.status()
        t.send(b'unsubmitted-draft')
        t.send(b'\x02D')
        self.assertEqual(t.exited(), 0)
        self.assertEqual(tty_settings(t.slave), t.original)
        t2 = self.host('--attach', self.name)
        t2.wait(lambda: b'unsubmitted-draft' in t2.output, 'retained draft screen')
        after = self.status()
        self.assertEqual([x['pid'] for x in before['tabs']], [x['pid'] for x in after['tabs']])
        t2.send(b'-continued\n')
        t2.wait(lambda: b'unsubmitted-draft-continued' in t2.output, 'same kernel command buffer')

    def test_exclusive_attachment_requires_explicit_takeover(self):
        original = self.start()
        other = self.host('--attach', self.name)
        self.assertNotEqual(other.exited(), 0)
        self.assertIn(b'attach', other.output.lower())
        replacement = self.host('--attach', self.name, '--takeover')
        replacement.wait(lambda: b'NATIVE_READY' in replacement.output, 'explicit takeover')
        original.exited()
        self.assertEqual(tty_settings(original.slave), original.original)
        self.assertEqual(len(self.status()['tabs']), 1)

    def test_attachment_reload_keeps_pid_shell_and_pending_input(self):
        t=self.start()
        before=self.status();frontend=t.process.pid
        t.send(b'draft-before-reload')
        t.wait(lambda:b'draft-before-reload' in t.output,'draft displayed')
        t.send(b'\x02R')
        t.wait(lambda:t.output.count(b'\x1b[?1049h')>=2,'same executable reload')
        self.assertEqual(t.process.pid,frontend)
        self.assertIsNone(t.process.poll())
        after=self.status()
        self.assertEqual(before['pid'],after['pid'])
        self.assertEqual([x['pid'] for x in before['tabs']],[x['pid'] for x in after['tabs']])
        t.send(b'-after\n')
        t.wait(lambda:b'draft-before-reload-after' in t.output,'kernel input survives reload')

    def test_resize_alternate_screen_and_parser_replies_share_engine(self):
        # DSR is answered by the C engine to its owned PTY, never the outer host.
        report = self.root/'reply'
        code = r'''import os,tty,select,time,pathlib
import sys
tty.setraw(0)
os.write(1,b'\x1b[?1049h\x1b[2;3H\x1b[6n')
out=b'';end=time.monotonic()+3
while time.monotonic()<end and b'R' not in out:
 if select.select([0],[],[],.1)[0]:out+=os.read(0,1024)
pathlib.Path(sys.argv[1]).write_bytes(out)
os.write(1,b'\x1b[?1049lNATIVE_READY\r\n')
while True:time.sleep(.1)
'''
        # An explicit child program may be Python. Hardcore itself must not look
        # for an interpreter in PATH to create/run that child.
        import sys
        t = self.host('--session', self.name, '--', sys.executable, '-c', code, str(report))
        t.wait(lambda: report.exists(), 'C parser replies')
        self.assertEqual(report.read_bytes(), b'\x1b[2;3R')
        t.wait(lambda: b'NATIVE_READY' in t.output, 'alternate-screen restoration')
        t.size(15,60)
        os.kill(t.process.pid, signal.SIGWINCH)
        deadline = time.monotonic()+4
        while time.monotonic()<deadline:
            t.pump()
            value=self.status()
            if value['tabs'][0]['cols']==60:break
        self.assertEqual(value['tabs'][0]['cols'],60)

    def test_paste_keeps_prefix_bytes_literal_and_delivers_large_payload(self):
        report=self.root/'paste'
        import sys
        code=r'''import os,tty,pathlib,sys
tty.setraw(0)
os.write(1,b'\x1b[?2004hNATIVE_READY\r\n')
b=b''
while b'\x1b[201~' not in b:
 b+=os.read(0,8192)
pathlib.Path(sys.argv[1]).write_bytes(b)
while True:os.read(0,4096)
'''
        t=self.host('--session',self.name,'--',sys.executable,'-c',code,str(report))
        t.wait(lambda:b'NATIVE_READY' in t.output,'paste receiver')
        body=(b'word\x02q\x02D'+ 'café界'.encode())*12000
        payload=b'\x1b[200~'+body+b'\x1b[201~'
        # Concurrently drain the output so host PTY flow control is realistic.
        import threading
        thread=threading.Thread(target=t.send,args=(payload,),daemon=True);thread.start()
        t.wait(lambda:report.exists(),'complete bounded paste',timeout=20)
        thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertEqual(report.read_bytes(),payload)
        self.assertIsNone(t.process.poll(),'pasted shortcuts were executed')

    def test_invalid_names_and_non_tty_fail_before_starting(self):
        for name in ('../escape','a/b','', 'a'*49):
            r=self.cli('--session',name)
            self.assertNotEqual(r.returncode,0,(name,r.stdout,r.stderr))
        r=self.cli('--session',self.name)
        self.assertEqual(r.returncode,2,r.stderr)
        self.assertIn('interactive terminal',r.stderr)
        self.assertNotEqual(self.cli('--status',self.name).returncode,0)

    def test_private_runtime_and_explicit_stop(self):
        t=self.start()
        state=self.status()
        pid=state['tabs'][0]['pid']
        directory=Path('/tmp')/f'hardcore-here-{os.getuid()}'
        self.assertEqual(stat.S_IMODE(directory.stat().st_mode),0o700)
        sockets=list(directory.glob(self.name+'*.sock'))
        self.assertTrue(sockets)
        self.assertTrue(all(stat.S_IMODE(p.stat().st_mode)==0o600 for p in sockets))
        self.stop()
        t.exited()
        self.assertEqual(tty_settings(t.slave),t.original)
        deadline=time.monotonic()+5
        while alive(pid) and time.monotonic()<deadline:time.sleep(.02)
        self.assertFalse(alive(pid),'explicit stop left owned shell alive')

    def test_background_jobs_survive_detach_and_stop_reaps_stubborn_job(self):
        report=self.root/'job-pid'
        # A separate process group deliberately ignores graceful shutdown.
        script=('set -m; /bin/sh -c \'trap "" HUP TERM; while :; do /bin/sleep 1; done\' & '
                'echo $! > "$1"; printf "NATIVE_READY\\n"; wait')
        t=self.host('--session',self.name,'--','/bin/sh','-c',script,'test',str(report))
        t.wait(lambda:report.exists() and b'NATIVE_READY' in t.output,'owned background job')
        job=int(report.read_text())
        t.send(b'\x02D');t.exited()
        self.assertTrue(alive(job),'detach stopped background work')
        self.stop()
        deadline=time.monotonic()+5
        while alive(job) and time.monotonic()<deadline:time.sleep(.02)
        self.assertFalse(alive(job),'owned stubborn background job survived explicit stop')

    def test_malformed_peer_cannot_disrupt_active_session(self):
        import socket,struct
        t=self.start();pid=self.status()['tabs'][0]['pid']
        path=Path('/tmp')/f'hardcore-here-{os.getuid()}'/(self.name+'.native.sock')
        for header in ((0,2,0),(0x48434832,2,0x7fffffff),(0x48434832,999,0)):
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(2);s.connect(str(path));s.sendall(struct.pack('!III',*header))
                self.assertEqual(s.recv(1),b'')
        self.assertEqual(self.status()['tabs'][0]['pid'],pid)
        t.send(b'healthy-after-peer-error\n')
        t.wait(lambda:b'healthy-after-peer-error' in t.output,'session remains responsive')


if __name__=='__main__':unittest.main(verbosity=2)
