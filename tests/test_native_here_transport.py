#!/usr/bin/env python3
"""Native IPC/PTY stress checks: only explicitly owned sessions and processes."""
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
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
    _native_setup = NativeHere.setUp
    def setUp(self):
        self._native_setup()
        # Never let tests target a real outer session if the test runner itself
        # is invoked from hardcore here. Only test-owned context is supplied.
        for key in ('HARDCORE_HERE_SESSION','HARDCORE_HERE_PID','HARDCORE_HERE_TAB_PID','HC_HERE_MEDIA_FD'):
            self.env.pop(key,None)
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
                     frame(2, b'unauthorized input'),
                     frame(8, struct.pack('!I', 1))):
            sock = self.socket()
            sock.sendall(data)
            self.assertEqual(sock.recv(1), b'', 'invalid peer was retained')
            sock.close()
            self.assertEqual(self.status()['tabs'][0]['pid'], pid)
        host.send(b'STILL_OWNED\n')
        host.wait(lambda: b'STILL_OWNED' in host.output, 'original controller survives bad peer')

    def test_host_color_capability_updates_on_reattach(self):
        # An inherited truecolor variable must not override Apple Terminal's
        # actual 256-color capability. Reattaching to another host renegotiates.
        self.env['TERM_PROGRAM'] = 'Apple_Terminal'
        self.env['COLORTERM'] = 'truecolor'
        host = self.start("printf '\\033[38;2;12;123;245;48;2;45;67;89mNATIVE_READY\\033[0m\\n'; exec /bin/cat")
        self.assertTrue(self.status()['color256'])
        self.assertIn(b'38;5;', host.output)
        self.assertNotIn(b'38;2;', host.output)
        self.assertNotIn(b'48;2;', host.output)
        host.send(b'\x02D');self.assertEqual(host.exited(), 0)
        self.env['TERM_PROGRAM'] = 'iTerm.app'
        resumed = self.host('--attach', self.name)
        resumed.wait(lambda: b'NATIVE_READY' in resumed.output, 'host palette renegotiation')
        self.assertFalse(self.status()['color256'])
        self.assertIn(b'38;2;', resumed.output)

    def media_fixture(self):
        # This is explicitly a test child, never a real camera. The production
        # app itself still runs from a copied bare executable with an empty PATH.
        script = self.root/'controlled-media'
        events = self.root/'media-events'
        script.write_text('#!'+sys.executable+'\n'+r'''import os,select,signal,sys,time
signal.signal(signal.SIGHUP,signal.SIG_IGN)
signal.signal(signal.SIGTERM,signal.SIG_IGN)
fd=int(os.environ['HC_HERE_MEDIA_FD'])
with open(sys.argv[3],'ab',buffering=0) as out:
 out.write(b'S')
 os.write(1,b'NATIVE_READY\r\n')
 while True:
  if select.select([fd],[],[],.1)[0]:
   data=os.read(fd,4096)
   if not data:out.write(b'E');break
   out.write(data)
''')
        script.chmod(0o700)
        host = self.host('--session', self.name, '--', str(script), '--here-media', '--camera', str(events))
        host.wait(lambda: events.exists() and b'1' in events.read_bytes(), 'fake media visibility')
        return host, events

    def test_media_lifetime_follows_tab_visibility_and_attachment(self):
        self.env['HC_HERE_MEDIA_FD'] = '99999'  # Never leak this stale parent value.
        host, events = self.media_fixture()
        tab = self.status()['tabs'][0]
        self.assertTrue(tab['media'] and tab['camera'] and tab['media_visible'])
        host.send(b'\x02c')
        host.wait(lambda: events.read_bytes().endswith(b'0'), 'hidden media paused')
        self.assertFalse(self.status()['tabs'][0]['media_visible'])
        host.send(b'printf "MEDIA_ENV:%s\\n" "${HC_HERE_MEDIA_FD-unset}"\n')
        host.wait(lambda: b'MEDIA_ENV:unset' in host.output, 'ordinary shell environment scrubbed')
        host.send(b'\x02p')
        host.wait(lambda: events.read_bytes().endswith(b'1'), 'selected media resumed')
        os.kill(host.process.pid, signal.SIGTSTP)
        self.addCleanup(lambda: os.kill(host.process.pid,signal.SIGCONT) if host.process.poll() is None else None)
        host.wait(lambda: events.read_bytes().endswith(b'0'), 'suspended host pauses media')
        self.assertTrue(self.status()['suspended'])
        os.kill(host.process.pid, signal.SIGCONT)
        host.wait(lambda: events.read_bytes().endswith(b'1'), 'continued host resumes media')
        self.assertFalse(self.status()['suspended'])
        host.send(b'\x02D');self.assertEqual(host.exited(), 0)
        host.wait(lambda: events.read_bytes().endswith(b'0'), 'detached media paused')
        resumed = self.host('--attach', self.name)
        resumed.wait(lambda: events.read_bytes().endswith(b'1'), 'reattached media resumed')
        self.assertEqual(self.status()['tabs'][0]['pid'], tab['pid'])

    def test_media_control_eof_on_owner_crash(self):
        host, events = self.media_fixture()
        status = self.status();pid = status['tabs'][0]['pid']
        # Keep test cleanup bounded even if the implementation regresses EOF.
        def cleanup_owned_helper():
            if alive(pid):
                try:os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:pass
        self.addCleanup(cleanup_owned_helper)
        os.kill(status['pid'], signal.SIGKILL)
        host.wait(lambda: events.read_bytes().endswith(b'E'), 'helper gets owner EOF')
        host.exited()
        deadline = time.monotonic()+3
        while alive(pid) and time.monotonic()<deadline:time.sleep(.02)
        self.assertFalse(alive(pid), 'media helper outlived its owner channel')

    def test_media_cli_rejects_conflicting_operations(self):
        for args in (('--camera','--image','anything'), ('--image',),
                     ('--camera','--attach',self.name), ('--camera','--list'),
                     ('--image','anything','--stop',self.name),
                     ('--camera','--','/bin/echo','oops')):
            result = self.cli(*args)
            self.assertEqual(result.returncode, 2, (args,result.stdout,result.stderr))
            self.assertNotIn('interactive terminal', result.stderr)

    def lifecycle(self, action, env=None):
        return subprocess.run([str(self.exe),action],env=env or self.env,cwd=self.root,
                              stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=5)

    def test_lifecycle_commands_refuse_outside_or_stale_context(self):
        host = self.start();before=self.status()
        for command in ('exit','detach'):
            for env in (self.env,dict(self.env,HARDCORE_HERE_SESSION=self.name,
                                     HARDCORE_HERE_PID=str(before['pid']),
                                     HARDCORE_HERE_TAB_PID=str(before['tabs'][0]['pid']))):
                result=self.lifecycle(command,env)
                self.assertEqual(result.returncode,2,result.stdout+result.stderr)
                self.assertIn('inside a native hardcore here session',result.stderr)
                self.assertTrue(self.status()['attached'])
        # Owner and tab IDs are checked again by the owner, not trusted merely
        # because a same-user peer can reach the private socket.
        sock=self.socket();sock.sendall(frame(9,struct.pack('!III',before['pid']+1,before['tabs'][0]['pid'],3)))
        self.assertEqual(receive(sock)[0],104)
        self.assertEqual(self.status()['tabs'][0]['pid'],before['tabs'][0]['pid'])

    def test_detach_command_from_child_preserves_shell_and_returns_success(self):
        host=self.start('printf "NATIVE_READY\\n"; exec /bin/sh -i')
        before=self.status()
        command=shlex.quote(str(self.exe))+' detach; printf "DETACH_RETURN:%s\\n" "$?"\n'
        host.send(command.encode());self.assertEqual(host.exited(),0)
        self.assertFalse(self.status()['attached'])
        self.assertTrue(alive(before['tabs'][0]['pid']))
        resumed=self.host('--attach',self.name)
        resumed.wait(lambda:b'DETACH_RETURN:0' in resumed.output,'native detach command returned zero')
        self.assertEqual(self.status()['tabs'][0]['pid'],before['tabs'][0]['pid'])

    def test_exit_command_stops_all_tabs_and_changed_session_descendants(self):
        report=self.root/'exit-owned-jobs.json'
        code=r'''import os,signal,time,json,pathlib,sys
signal.signal(signal.SIGHUP,signal.SIG_IGN)
signal.signal(signal.SIGTERM,signal.SIG_IGN)
pids=[os.getpid()]
for separate_session in (True,False):
 r,w=os.pipe();pid=os.fork()
 if not pid:
  os.close(r)
  if separate_session:os.setsid()
  else:os.setpgid(0,0)
  os.write(w,b'1');os.close(w)
  while True:time.sleep(.1)
 os.close(w);os.read(r,1);os.close(r);pids.append(pid)
pathlib.Path(sys.argv[1]).write_text(json.dumps(pids))
os.write(1,b'NATIVE_READY\r\n')
while True:time.sleep(.1)
'''
        host=self.host('--session',self.name,'--',sys.executable,'-c',code,str(report))
        host.wait(lambda:report.exists(),'owned new-session descendants')
        pids=json.loads(report.read_text())
        self.assertNotEqual(os.getsid(pids[1]),os.getsid(pids[0]))
        def cleanup_jobs():
            for pid in pids:
                try:os.kill(pid,signal.SIGKILL)
                except ProcessLookupError:pass
        self.addCleanup(cleanup_jobs)
        other_name=self.name+'-other'
        other=self.host('--session',other_name,'--','/bin/sh','-c','printf "OTHER_READY\\n"; exec /bin/cat')
        self.addCleanup(lambda:self.stop(other_name))
        other.wait(lambda:b'OTHER_READY' in other.output,'unrelated here session')
        other_status=json.loads(self.cli('--status',other_name).stdout)
        host.send(b'\x02c');host.wait(lambda:len(self.status()['tabs'])==2,'second owned shell')
        all_tabs=self.status()['tabs'];owner=self.status()['pid']
        host.send((shlex.quote(str(self.exe))+' exit\n').encode())
        self.assertEqual(host.exited(),0)
        self.assertIn(b'Stopped here session',host.output)
        self.assertNotIn(b'Reattach:',host.output)
        deadline=time.monotonic()+5
        owned=pids+[tab['pid'] for tab in all_tabs]+[owner]
        while any(alive(pid) for pid in owned) and time.monotonic()<deadline:time.sleep(.03)
        self.assertFalse(any(alive(pid) for pid in owned),f'owned processes survived exit: {owned}')
        self.assertTrue(alive(other_status['tabs'][0]['pid']))
        self.assertIsNone(other.process.poll())
        self.assertEqual(self.cli('--status',other_name).returncode,0)
        self.assertEqual(tty_settings(host.slave),host.original)

    def test_palette_quantizer_matches_nearest_fixed_xterm_color(self):
        compiler = shutil.which('cc')
        if not compiler:self.skipTest('C compiler unavailable')
        source = (EXE.parent.parent/'hardcore_here_transport.h').read_text()
        function = source[source.index('static int here_color_index('):source.index('static int here_host_color256(')]
        probe = self.root/'palette.c'
        probe.write_text('#include <stdint.h>\n#include <stdio.h>\n'+function+r'''
int main(void) {
 int levels[6]={0,95,135,175,215,255};
 for(int r=0;r<256;r+=5)for(int g=0;g<256;g+=5)for(int b=0;b<256;b+=5) {
  int actual=here_color_index((r<<16)|(g<<8)|b),best=0x7fffffff,chosen=-1;
  if(actual<16||actual>255)return 1;
  for(int i=16;i<256;i++) {
   int p=i-16,rr=i<232?levels[p/36]:8+10*(i-232),gg=i<232?levels[(p/6)%6]:rr,bb=i<232?levels[p%6]:rr;
   int dr=rr-r,dg=gg-g,db=bb-b,error=dr*dr+dg*dg+db*db;
   if(error<best)best=error;
   if(i==actual)chosen=error;
  }
  if(chosen!=best){fprintf(stderr,"wrong nearest color for %d %d %d: %d\n",r,g,b,actual);return 2;}
 }
 return 0;
}
''')
        binary = self.root/'palette'
        subprocess.run([compiler,'-O2',str(probe),'-o',str(binary)],check=True,capture_output=True)
        subprocess.run([str(binary)],check=True,capture_output=True)

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
