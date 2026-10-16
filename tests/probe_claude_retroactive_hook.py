#!/usr/bin/env python3
"""Opt-in: install a hook after an ordinary Claude session has started.

Uses a disposable project and PTY, never Terminal.app. The hook blocks the
test prompt before inference; no model response is needed. No user settings
are edited. This tests a possible cooperative integration point, not slurp.
"""
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time


def main():
    root = Path(tempfile.mkdtemp(prefix='hc-claude-late-hook-', dir='/tmp'))
    config = root/'.claude'; config.mkdir()
    settings = config/'settings.json'
    settings.write_text('{}')
    env = {k:v for k,v in os.environ.items() if not k.startswith('CLAUDE')
           and k not in ('TMUX', 'HARDCORE_WINDOW_PID')}
    env.update(TERM='xterm-256color', COLORTERM='truecolor')
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH',32,100,0,0))
    def setup():
        os.setsid(); fcntl.ioctl(0,termios.TIOCSCTTY,0)
    proc = subprocess.Popen([shutil.which('claude'), '--setting-sources', 'project',
        '--strict-mcp-config', '--mcp-config', '{"mcpServers":{}}',
        '--no-chrome', '--tools', ''], cwd=root, env=env,
        stdin=slave, stdout=slave, stderr=slave, preexec_fn=setup)
    os.close(slave)
    raw = bytearray()
    def drain():
        while select.select([master],[],[],0)[0]:
            try: data=os.read(master,65536)
            except OSError: break
            if not data: break
            raw.extend(data)
            for query,reply in [(b'\x1b[6n',b'\x1b[1;1R'),
                    (b'\x1b[c',b'\x1b[?1;2c'),
                    (b'\x1b]11;?',b'\x1b]11;rgb:0000/0000/0000\x1b\\')]:
                if query in data: os.write(master,reply)
        return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',raw.decode(errors='replace'))
    def pump(seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            drain(); time.sleep(.05)
    try:
        pump(3)
        if 'Yes,Itrustthisfolder' in re.sub(r'\s+','',drain()):
            os.write(master,b'\x1b[B\r'); pump(3)
        assert proc.poll() is None
        hook = root/'late_hook.py'
        hook.write_text('''import json,os,sys
from pathlib import Path
event=json.load(sys.stdin)
result={'event':event,'pid':os.getpid(),'parentPid':os.getppid(),
        'stdioIsTTY':[os.isatty(i) for i in (0,1,2)]}
try:
 fd=os.open('/dev/tty',os.O_RDWR);os.close(fd);result['controllingTTY']=True
except OSError as e:result['controllingTTY']=False;result['ttyError']=e.errno
Path(__file__).with_suffix('.json').write_text(json.dumps(result,indent=2))
print(json.dumps({'decision':'block','reason':'HCRETROHOOK: disposable probe blocked before inference'}))
''')
        settings.write_text(json.dumps({'hooks':{'UserPromptSubmit':[{'hooks':[
            {'type':'command','command':shlex.join([sys.executable,str(hook)])}]}]}}))
        pump(3)
        os.write(master,b'HCRETROHOOK probe; the hook should block this prompt.')
        pump(.3);os.write(master,b'\r')
        end=time.monotonic()+15
        while not hook.with_suffix('.json').exists() and time.monotonic()<end:
            pump(.1)
        assert hook.with_suffix('.json').exists(), 'late hook did not execute'
        result=json.loads(hook.with_suffix('.json').read_text())
        assert result['event']['hook_event_name']=='UserPromptSubmit'
        assert not result['controllingTTY'] and result['stdioIsTTY']==[False]*3
        assert proc.poll() is None, 'source Claude exited'
        pump(1)
        assert 'disposableprobeblockedbeforeinference' in re.sub(r'\s+','',drain())
        print('PASS hook installed after launch ran in the same live Claude session')
        print('PASS hook has piped stdio and no controlling terminal; no master handle acquired')
        (root/'result.json').write_text(json.dumps({'sourcePid':proc.pid,
            'hotReloaded':True,'sourceStillRunning':True,'hook':result},indent=2))
    finally:
        (root/'source.raw').write_bytes(raw)
        if proc.poll() is None:proc.kill()
        proc.wait(timeout=10);os.close(master)
        print('Private diagnostics:',root)


if __name__=='__main__':main()
