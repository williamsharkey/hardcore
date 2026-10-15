#!/usr/bin/env python3
"""Test a Terminal bridge against a dedicated test tab, never existing tabs.
Requires macOS Automation (Terminal) and Accessibility permission.
"""
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shlex
import signal
import subprocess
import sys
import struct
import termios
import tempfile
import time

exe=str(Path('build/hardcore').resolve())

def apple(source):
    r=subprocess.run(['osascript','-e',source],capture_output=True,text=True,timeout=15)
    if r.returncode: raise RuntimeError(r.stderr)
    return r.stdout.strip()

def wait(fn, timeout=10):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if fn(): return
        time.sleep(.05)
    raise AssertionError('timed out')

with tempfile.TemporaryDirectory(prefix='hc-terminal-',dir='/tmp') as temp:
    root=Path(temp)
    worker=root/'worker.py'
    worker.write_text('''import os, sys, tty
from pathlib import Path
tty.setraw(0)
Path(sys.argv[1]).write_text(str(os.getpid()))
os.write(1,b"TERMINAL-BRIDGE-READY\\r\\n")
while True:
    data=os.read(0,4096)
    if not data: break
    with open(sys.argv[2],"ab") as out: out.write(data)
    os.write(1,b"ACK:"+data.hex().encode()+b"\\r\\n")
''')
    command='exec '+shlex.join([sys.executable,str(worker),str(root/'pid'),str(root/'input')])
    title="Hardcore bridge test " + root.name
    target=apple('tell application "Terminal"\nset t to do script '+json.dumps(command)+'\nset custom title of t to '+json.dumps(title)+'\nreturn tty of t\nend tell')
    window=apple('tell application "Terminal"\nrepeat with w in windows\nrepeat with t in tabs of w\nif custom title of t is '+json.dumps(title)+' then return id of w\nend repeat\nend repeat\nend tell')
    master,slave=pty.openpty()
    fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack("HHHH",24,80,0,0))
    fcntl.fcntl(master,fcntl.F_SETFL,os.O_NONBLOCK)
    app=None
    output=bytearray()
    def drain():
        try:
            while True:
                chunk=os.read(master,65536)
                if not chunk: break
                output.extend(chunk)
        except (BlockingIOError,OSError): pass
        return output
    try:
        wait(lambda:(root/'pid').exists())
        source_pid=int((root/'pid').read_text())
        app=subprocess.Popen([exe,'--terminal-bridge',target,window],stdin=slave,stdout=slave,stderr=slave)
        os.close(slave); slave=-1
        wait(lambda:b'TERMINAL-BRIDGE-READY' in drain() or app.poll() is not None)
        if app.poll() is not None:
            drain(); print(output.decode(errors='replace'))
            if b'permission' in output or b'automation failed' in output: sys.exit(77)
            raise AssertionError(f'bridge failed: {app.returncode}')
        os.write(master,b'hello\r\x03\x1b[A')
        wait(lambda:(root/'input').exists() and b'hello\r\x03' in (root/'input').read_bytes())
        got=(root/'input').read_bytes()
        assert b'\x1b[A' in got, got
        wait(lambda:b'ACK:' in drain())
        os.kill(source_pid,0)
        app.terminate();app.wait(timeout=5)
        os.kill(source_pid,0)
        # Exercise the public slurp command and reload its bridge while the
        # original Terminal job stays alive, using an isolated Hardcore window.
        runtime=root/'runtime'; ctl=root/'control'
        home=root/'home';(home/'Library/Application Support').mkdir(parents=True)
        env=dict(os.environ,HOME=str(home),HARDCORE_RUNTIME_DIR=str(runtime),HARDCORE_CTL=str(ctl))
        env.pop('HARDCORE_NO_ACTIVATE',None)
        r=subprocess.run([exe,'slurp','--terminal',target],env=env,capture_output=True,text=True,timeout=25)
        assert r.returncode==0,(r.stdout,r.stderr)
        gui_pid=int(r.stdout.strip().split()[-1])
        try:
            wait(ctl.exists)
            def type_in_gui(text):
                fd=os.open(ctl,os.O_WRONLY|os.O_NONBLOCK)
                try:os.write(fd,('type '+text+'\\n\n').encode())
                finally:os.close(fd)
            type_in_gui('PUBLIC-SLURP')
            wait(lambda:b'PUBLIC-SLURP' in (root/'input').read_bytes())
            r=subprocess.run([exe,'update','--local'],env=env,capture_output=True,text=True,timeout=25)
            assert r.returncode==0,(r.stdout,r.stderr)
            type_in_gui('BRIDGE-AFTER-UPDATE')
            wait(lambda:b'BRIDGE-AFTER-UPDATE' in (root/'input').read_bytes())
            os.kill(source_pid,0)
            snap=Path('build/terminal-slurp.bmp').resolve()
            snap.unlink(missing_ok=True)
            time.sleep(.2)
            fd=os.open(ctl,os.O_WRONLY|os.O_NONBLOCK)
            try:os.write(fd,('snap '+str(snap)+'\n').encode())
            finally:os.close(fd)
            wait(snap.exists)
        finally:
            for endpoint in runtime.glob('*.sock'):
                try:os.kill(int(endpoint.stem),signal.SIGTERM)
                except ProcessLookupError:pass
        os.kill(source_pid,0)
        print('PASS: Terminal live screen, targeted text/Enter/Ctrl-C/arrows, public slurp and live update, source PID survives detach')
    finally:
        Path('build/terminal-bridge-test.log').write_bytes(drain())
        if app and app.poll() is None:app.terminate();app.wait(timeout=5)
        if (root/'pid').exists():
            try:os.kill(int((root/'pid').read_text()),signal.SIGTERM)
            except ProcessLookupError:pass
        try:apple('tell application "Terminal"\nrepeat with w in windows\nrepeat with t in tabs of w\nif tty of t is '+json.dumps(target)+' and id of w is '+window+' then\nclose w\nreturn\nend if\nend repeat\nend repeat\nend tell')
        except Exception:pass
        os.close(master)
        if slave>=0:os.close(slave)
