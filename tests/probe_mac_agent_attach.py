#!/usr/bin/env python3
"""Opt-in live research: ordinary Terminal Codex -> new Hardcore frontend.

No host/tmux, debugger, or existing conversations. Uses an isolated CODEX_HOME
and the installed login. A real model request runs a disposable sleeping tool.
This is a research probe, not a production slurp adapter.
"""
import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import struct
import sys
import tempfile
import termios
import time

from test_mac_takeover import Session, wait, apple, send


def main():
    root = Path(tempfile.mkdtemp(prefix='hc-agent-attach-', dir='/tmp'))
    s = Session(root)
    binary = shutil.which('codex')
    home = root/'codex-home'; home.mkdir(mode=0o700)
    auth = Path.home()/'.codex/auth.json'
    if auth.exists(): shutil.copy2(auth, home/'auth.json')
    env = dict(os.environ, CODEX_HOME=str(home))
    work = root/'work'; work.mkdir()
    launcher = root/'launch.py'
    launcher.write_text('import os,sys\nfrom pathlib import Path\n'
                        f'os.chdir({str(work)!r})\n'
                        f'os.environ["CODEX_HOME"]={str(home)!r}\n'
                        f'Path({str(root/"ready")!r}).write_text(f"{{os.getpid()}} {{os.getpgid(os.getppid())}}")\n'
                        'os.execv(sys.argv[1],sys.argv[1:])\n')
    # The pid file lets the test identify the exact active tool across handoff.
    job = work/'job.py'
    job.write_text('import os,time\nfrom pathlib import Path\n'
                   'Path("tool.pid").write_text(str(os.getpid()))\n'
                   'time.sleep(35)\nprint("HC-LIVE-TOOL-COMPLETED",flush=True)\n')
    prompt = ('Run exactly this command with your shell tool: '+sys.executable+' '+str(job)+
              '. Wait for it to finish, then reply HC-ATTACH-DONE. Do nothing else.')
    frontend = None
    try:
        s.start([sys.executable, str(launcher), binary, '-s', 'workspace-write', '-a', 'never', prompt], nohup=False)
        original = s.pid
        def screen():
            return apple(f'tell application "Terminal" to return contents of selected tab of window id {s.window}')
        time.sleep(3)
        (root/'source.txt').write_text(screen())
        wait(lambda: (work/'tool.pid').exists(), 'live tool started', timeout=90)
        tool = int((work/'tool.pid').read_text()); s.child_pids.append(tool)
        rollouts = list((home/'sessions').rglob('*.jsonl'))
        assert len(rollouts) == 1, rollouts
        rollout = rollouts[0]
        meta = json.loads(rollout.read_text().splitlines()[0])['payload']
        thread = meta['id']
        print(f'Ordinary Terminal frontend {original}; live tool {tool}; thread {thread}', flush=True)
        def child_setup():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
        fcntl.ioctl(s.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        frontend = subprocess.Popen([binary, 'resume', thread, '-s', 'workspace-write', '-a', 'never'],
                                    cwd=work, env=env, stdin=s.slave, stdout=s.slave, stderr=s.slave,
                                    preexec_fn=child_setup)
        s.child_pids.append(original)
        s.pid = frontend.pid
        s.stage()
        send(s.sender, {'go': True})
        os.close(s.master); s.master = -1
        os.close(s.slave); s.slave = -1
        wait(s.control.exists, 'Hardcore receiver')
        time.sleep(3)
        os.kill(tool, 0)
        assert 'HC-LIVE-TOOL-COMPLETED' not in rollout.read_text().split('function_call_output')[-1]
        # Terminal can leave the frontend alive (or ask about a busy tab).
        # Prove backend independence even if the old frontend crashes outright.
        try: os.kill(original, signal.SIGKILL)
        except ProcessLookupError: pass
        def source_gone():
            result = subprocess.run(['ps', '-p', str(original), '-o', 'stat='], capture_output=True, text=True)
            return not result.stdout.strip() or result.stdout.strip().startswith('Z')
        wait(source_gone, 'original frontend exited')
        s.close_terminal(require_idle=True)
        os.kill(tool, 0)
        print('Original frontend exited while original tool is still running', flush=True)
        wait(lambda: any('HC-ATTACH-DONE' in str(row.get('payload',{}).get('last_agent_message',''))
                         and row.get('payload',{}).get('type') == 'task_complete'
                         for row in map(json.loads, rollout.read_text().splitlines())),
             'original turn completed after Terminal close', timeout=90)
        assert frontend.poll() is None
        s.ctl('type Reply only with the concatenation of HCNEW and INPUT. Do not use tools.')
        time.sleep(.3); s.ctl('key 36 0 \\r')
        wait(lambda: any(row.get('payload',{}).get('last_agent_message') == 'HCNEWINPUT'
                         for row in map(json.loads, rollout.read_text().splitlines())),
             'new input from Hardcore', timeout=90)
        s.ctl('snap '+str(root/'receiver.bmp'))
        wait((root/'receiver.bmp').exists, 'receiver screenshot')
        print('PASS: same active tool survived unprepared Terminal close; new Hardcore input accepted', flush=True)
    finally:
        # Keep diagnostics, never a copied credential, even on failure.
        (home/'auth.json').unlink(missing_ok=True)
        s.cleanup()
        try:
            subprocess.run([binary, 'app-server', 'daemon', 'stop'], env=env, capture_output=True, timeout=45)
        except subprocess.TimeoutExpired:
            print('Isolated daemon stop timed out; inspect '+str(home), flush=True)
        for pidfile in (home/'app-server-daemon').glob('*.pid'):
            try:
                record = json.loads(pidfile.read_text())
                pid = int(record['pid'] if isinstance(record, dict) else record)
                command = subprocess.run(['ps', '-p', str(pid), '-o', 'comm='], capture_output=True, text=True).stdout
                if str(home.resolve()) in command:
                    os.kill(pid, signal.SIGTERM)
            except (ValueError, ProcessLookupError): pass
        print('Private diagnostics:', root, flush=True)


if __name__ == '__main__': main()
