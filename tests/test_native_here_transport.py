#!/usr/bin/env python3
"""Native IPC/PTY stress checks: only explicitly owned sessions and processes."""
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import sys
import threading
import time
import unittest

from test_native_here import NativeHere, EXE
from test_here_dispatch import tty_settings, alive

MAGIC = 0x48434832


def frame(kind, payload=b''):
    return struct.pack('!III', MAGIC, kind, len(payload)) + payload


def receive(sock):
    def exact(size):
        result = b''
        while len(result) < size:
            block = sock.recv(size-len(result))
            if not block:
                raise EOFError('session closed')
            result += block
        return result
    magic, kind, size = struct.unpack('!III', exact(12))
    if magic != MAGIC or size > 8*1024*1024:
        raise ValueError('invalid native frame')
    return kind, exact(size)


@unittest.skipUnless(EXE.is_file() and os.name == 'posix', 'native POSIX build required')
class NativeTransport(unittest.TestCase):
    setUp = NativeHere.setUp
    stop = NativeHere.stop
    cli = NativeHere.cli
    host = NativeHere.host
    start = NativeHere.start
    status = NativeHere.status

    def socket(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(3)
        sock.connect(f'/tmp/hardcore-here-{os.getuid()}/{self.name}.native.sock')
        self.addCleanup(sock.close)
        return sock

    def test_invalid_and_oversized_frames_do_not_affect_attached_session(self):
        host = self.start()
        pid = self.status()['tabs'][0]['pid']
        for data in (struct.pack('!III', 0, 1, 0),
                     struct.pack('!III', MAGIC, 2, 0xffffffff),
                     frame(2, b'unauthorized input')):
            sock = self.socket()
            sock.sendall(data)
            self.assertEqual(sock.recv(1), b'', 'invalid peer was retained')
            sock.close()
            self.assertEqual(self.status()['tabs'][0]['pid'], pid)
        host.send(b'STILL_OWNED\n')
        host.wait(lambda: b'STILL_OWNED' in host.output, 'original controller survives bad peer')

    def test_fragmented_frames_and_reconnect_after_transport_loss(self):
        host = self.start()
        pid = self.status()['tabs'][0]['pid']
        host.send(b'\x02D')
        self.assertEqual(host.exited(), 0)
        sock = self.socket()
        hello = frame(1, struct.pack('!IIII', 2, 100, 24, 0))
        for byte in hello:
            sock.sendall(bytes([byte]))
        self.assertEqual(receive(sock)[0], 101)
        message = frame(2, b'FRAGMENTED_NATIVE_INPUT\n')
        for pos in range(0, len(message), 3):
            sock.sendall(message[pos:pos+3])
        seen = b''
        deadline = time.monotonic()+4
        while b'FRAGMENTED_NATIVE_INPUT' not in seen and time.monotonic()<deadline:
            kind, payload = receive(sock)
            if kind == 102:
                seen += payload
        self.assertIn(b'FRAGMENTED_NATIVE_INPUT', seen)
        sock.close()  # Abrupt frontend loss preserves the session.
        resumed = self.host('--attach', self.name)
        resumed.wait(lambda: b'FRAGMENTED_NATIVE_INPUT' in resumed.output, 'redraw after transport loss')
        self.assertEqual(self.status()['tabs'][0]['pid'], pid)

    def test_explicit_stop_removes_hup_and_term_ignoring_owned_jobs(self):
        report = self.root/'owned-jobs.json'
        code = r'''import os,signal,time,json,pathlib,sys
signal.signal(signal.SIGHUP,signal.SIG_IGN)
signal.signal(signal.SIGTERM,signal.SIG_IGN)
pid=os.fork()
if not pid:
 os.setpgid(0,0)
 while True:time.sleep(.2)
pathlib.Path(sys.argv[1]).write_text(json.dumps([os.getpid(),pid]))
os.write(1,b'NATIVE_READY\r\n')
while True:time.sleep(.2)
'''
        host = self.host('--session', self.name, '--', sys.executable, '-c', code, str(report))
        host.wait(lambda: report.exists(), 'owned stubborn jobs')
        pids = json.loads(report.read_text())
        self.assertTrue(all(alive(pid) for pid in pids))
        self.stop()
        host.exited()
        deadline = time.monotonic()+5
        while any(alive(pid) for pid in pids) and time.monotonic()<deadline:
            time.sleep(.03)
        self.assertFalse(any(alive(pid) for pid in pids), f'owned jobs survived stop: {pids}')
        self.assertEqual(tty_settings(host.slave), host.original)

    def test_large_paste_survives_pty_backpressure(self):
        report = self.root/'input-count'
        length = 5*1024*1024 + 123
        code = r'''import os,tty,time,sys,pathlib
n=int(sys.argv[2]);tty.setraw(0)
os.write(1,b'\x1b[?2004hNATIVE_READY\r\n')
time.sleep(1.5)
read=0
while read<n:
 block=os.read(0,min(32768,n-read))
 if not block:break
 read+=len(block)
pathlib.Path(sys.argv[1]).write_text(str(read))
while True:time.sleep(.1)
'''
        payload = b'\x1b[200~' + b'a'*length + b'\x1b[201~'
        host = self.host('--session', self.name, '--', sys.executable, '-c', code,
                         str(report), str(len(payload)))
        host.wait(lambda: b'NATIVE_READY' in host.output, 'backpressure receiver')
        sender = threading.Thread(target=host.send, args=(payload,), daemon=True)
        sender.start()
        host.wait(lambda: report.exists(), 'large paste under backpressure', timeout=30)
        sender.join(2)
        self.assertFalse(sender.is_alive(), 'sender remained blocked')
        self.assertEqual(int(report.read_text()), len(payload))
        self.assertIsNone(host.process.poll())

    def test_reconnect_discards_old_attachment_prefix(self):
        host = self.start()
        host.send(b'\x02')
        host.wait(lambda: b'Ctrl-B:' in host.output, 'prefix pending')
        os.kill(host.process.pid, signal.SIGTERM)
        host.exited()
        resumed = self.host('--attach', self.name)
        resumed.wait(lambda: b'NATIVE_READY' in resumed.output, 'reattached without pending prefix')
        resumed.send(b'c\n')
        time.sleep(.1)
        self.assertEqual(len(self.status()['tabs']), 1,
                         'first input after reconnect executed the old Ctrl-B prefix')

    def test_suspend_preserves_unsent_input_under_socket_backpressure(self):
        # A protocol fixture controls the receiving pace, allowing this to test
        # the actual native attachment's queue without timing a child shell.
        directory = Path('/tmp')/f'hardcore-here-{os.getuid()}'
        directory.mkdir(mode=0o700, exist_ok=True)
        path = directory/(self.name+'.native.sock')
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path));os.chmod(path,0o600);server.listen(4)
        self.addCleanup(lambda: path.unlink(missing_ok=True))
        self.addCleanup(server.close)
        allow_read = threading.Event();result=[];ready=threading.Event()
        payload = b'SUSPEND_QUEUED_INPUT_'*5000
        def serve():
            conn=None
            try:
                server.settimeout(5)
                while True:
                    conn,_=server.accept();conn.settimeout(5)
                    try:conn.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,2048)
                    except OSError:pass # Darwin fixes UNIX-domain stream buffer sizing
                    try:kind,_=receive(conn)
                    except EOFError:conn.close();continue # startup existence probe
                    if kind!=1:raise AssertionError(kind)
                    break
                conn.sendall(frame(101));ready.set()
                if not allow_read.wait(8):raise TimeoutError('test did not resume receiver')
                data=bytearray()
                while len(data)<len(payload):
                    kind,body=receive(conn)
                    if kind==2:data.extend(body)
                result.append(bytes(data))
                conn.sendall(frame(103,struct.pack('!I',1)))
            except BaseException as exc:result.append(exc)
            finally:
                if conn:conn.close()
        worker=threading.Thread(target=serve,daemon=True);worker.start()
        host=self.host('--attach',self.name)
        host.wait(lambda:bool(result) or (ready.is_set() and b'\x1b[?1049h' in host.output),'native attachment entered')
        self.assertFalse(result, result)
        os.set_blocking(host.master,False)
        sent=0;send_deadline=time.monotonic()+3
        while sent<len(payload) and time.monotonic()<send_deadline:
            try:sent+=os.write(host.master,payload[sent:sent+4096])
            except BlockingIOError:time.sleep(.002)
        self.assertEqual(sent,len(payload),'fixture payload did not fit bounded client queue')
        time.sleep(.15) # all host bytes have entered the native client queue
        os.kill(host.process.pid,signal.SIGTSTP)
        deadline=time.monotonic()+4
        stopped=False
        import subprocess
        while time.monotonic()<deadline:
            state=subprocess.check_output(['/bin/ps','-o','state=','-p',str(host.process.pid)],text=True).strip()
            if 'T' in state:stopped=True;break
            time.sleep(.02)
        self.assertTrue(stopped,'attachment did not suspend')
        self.assertEqual(tty_settings(host.slave),host.original)
        allow_read.set();os.kill(host.process.pid,signal.SIGCONT)
        host.wait(lambda:bool(result),'queued input after resume',timeout=8)
        worker.join(2)
        self.assertEqual(result,[payload])
        self.assertEqual(host.exited(),0)
        self.assertEqual(tty_settings(host.slave),host.original)


del NativeHere  # imported fixture methods, not an extra discovered test suite

if __name__ == '__main__':
    unittest.main(verbosity=2)
