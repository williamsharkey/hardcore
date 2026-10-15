#!/usr/bin/env python3
"""Opt-in live Grok experiment, with a fresh ordinary Terminal session.

The socket override isolates the automatically started leader; no host or tmux.
Only the disposable test process is terminated. Not a production adapter.
"""
import fcntl
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import uuid
from test_mac_takeover import Session, wait, apple, send


def main():
    root = Path(tempfile.mkdtemp(prefix='hc-grok-attach-', dir='/tmp'))
    s = Session(root)
    binary = shutil.which('grok')
    thread = str(uuid.uuid4())
    leader = root/'leader.sock'
    work = root/'work'; work.mkdir()
    job = work/'job.py'
    job.write_text('import os,time\nfrom pathlib import Path\n'
                   'Path("tool.pid").write_text(str(os.getpid()))\n'
                   'time.sleep(45)\nprint("HC-LIVE-TOOL-COMPLETED",flush=True)\n')
    launcher = root/'launch.py'
    launcher.write_text('import os,sys\nfrom pathlib import Path\n'
                        f'os.chdir({str(work)!r})\n'
                        f'Path({str(root/"ready")!r}).write_text(f"{{os.getpid()}} {{os.getpgid(os.getppid())}}")\n'
                        'os.execv(sys.argv[1],sys.argv[1:])\n')
    common = [binary, '--leader-socket', str(leader), '--no-subagents', '--disable-web-search',
              '--tools', 'Bash', '--allow', 'Bash('+sys.executable+' '+str(job)+')']
    prompt = ('Run exactly this command with Bash: '+shlex.join([sys.executable,str(job)])+
              '. Wait for completion, then reply with the concatenation of HCGROK and DONE. Do nothing else.')
    def transcript():
        result = subprocess.run([binary, '--leader-socket', str(leader), 'export', thread],
                                cwd=work, capture_output=True, text=True, timeout=15)
        return result.stdout
    try:
        s.start([sys.executable,str(launcher),*common,'--session-id',thread,prompt], nohup=False)
        original = s.pid
        time.sleep(3)
        (root/'source.txt').write_text(apple(f'tell application "Terminal" to return contents of selected tab of window id {s.window}'))
        wait(lambda: (work/'tool.pid').exists(), 'Grok started tool', timeout=90)
        tool = int((work/'tool.pid').read_text()); s.child_pids.append(tool)
        print(f'Grok source {original}, active tool {tool}, session {thread}', flush=True)
        def setup():
            os.setsid(); fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        fcntl.ioctl(s.slave, termios.TIOCSWINSZ, struct.pack('HHHH',24,80,0,0))
        frontend = subprocess.Popen([*common,'--resume',thread],cwd=work,
                                    stdin=s.slave,stdout=s.slave,stderr=s.slave,preexec_fn=setup)
        s.child_pids.append(original);s.pid=frontend.pid
        s.stage();send(s.sender,{'go':True})
        os.close(s.master);s.master=-1;os.close(s.slave);s.slave=-1
        wait(s.control.exists);time.sleep(4)
        assert 'HCGROKDONE' not in transcript(), 'tool finished before handoff'
        os.kill(original,signal.SIGKILL);s.close_terminal(require_idle=True)
        os.kill(tool,0)
        print('Original Grok frontend terminated with tool still active',flush=True)
        wait(lambda:'HCGROKDONE' in transcript(),'original Grok turn completed',timeout=100)
        s.ctl('type Reply only with the concatenation of HCGROK and INPUT. Do not use tools.')
        time.sleep(.3);s.ctl('key 36 0 \\r')
        wait(lambda:'HCGROKINPUT' in transcript(),'Grok responds in Hardcore',timeout=90)
        s.ctl('snap '+str(root/'receiver.bmp'));wait((root/'receiver.bmp').exists)
        print('PASS Grok: original tool and turn survive source exit; Hardcore input works',flush=True)
    finally:
        s.cleanup()
        # Discover only processes explicitly launched with this private socket.
        rows=subprocess.run(['ps','-axo','pid=,args='],capture_output=True,text=True).stdout.splitlines()
        for row in rows:
            if str(leader) in row:
                try:os.kill(int(row.split()[0]),signal.SIGTERM)
                except ProcessLookupError:pass
        print('Private diagnostics:',root,flush=True)


if __name__=='__main__':main()
