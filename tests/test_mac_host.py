#!/usr/bin/env python3
"""Portable Terminal host tests. --agent/--live are opt-in installed CLI tests.

Creates only new Terminal tabs, private tmux servers and Hardcore windows.
Live cases use the user's existing login and send tiny tool-free test prompts.
Never resumes an existing conversation. No debugger modification is attempted.
"""
import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from test_mac_takeover import Session, EXE, REPO, apple, wait, children


class HostSession(Session):
    def tmux(self, *args, check=True):
        return subprocess.run(['tmux', '-S', str(self.root/'runtime/host.tmux'), *args],
                              check=check, capture_output=True, text=True, timeout=10).stdout.strip()

    def launch(self, command):
        work = self.root/'work'; work.mkdir()
        launcher = self.root/'launch.py'
        launcher.write_text('''import os, sys
from pathlib import Path
os.chdir(''' + repr(str(work)) + ''')
os.environ['HARDCORE_RUNTIME_DIR'] = ''' + repr(str(self.root/'runtime')) + '''
Path(''' + repr(str(self.root/'ready')) + ''').write_text(f'{os.getpid()} {os.getpgid(os.getppid())}')
os.execv(sys.argv[1], sys.argv[1:])
''')
        self.start([sys.executable, str(launcher), EXE, 'host', '--', *command])
        wait(lambda: self.tmux('list-panes', '-a', '-F', '#{pane_pid}', check=False), 'host pane')
        self.job = int(self.tmux('list-panes', '-a', '-F', '#{pane_pid}'))
        self.child_pids.append(self.job)
        self.original_tty = self.tmux('display-message', '-p', '#{pane_tty}')

    def screen(self):
        return self.tmux('capture-pane', '-p', '-S', '-150')

    def type_source(self, text):
        self.tmux('send-keys', '-l', '--', text)
        time.sleep(.3)
        self.tmux('send-keys', 'Enter')

    def type_receiver(self, text):
        self.ctl('type ' + text)
        # Some agent TUIs interpret a burst including Enter as pasted text.
        time.sleep(.3)
        # Use Hardcore's keyboard path, including the negotiated key protocol.
        self.ctl('key 36 0 \\r')

    def attach(self, detached=False):
        args = ['--terminal', self.device]
        if detached:
            name = self.tmux('display-message', '-p', '#{session_name}')
            self.close_terminal()
            wait(lambda: not self.tmux('list-clients', '-F', '#{client_pid}'), 'source detached before slurp')
            args = ['--tmux', name]
        result = subprocess.run([EXE, 'slurp', *args], env=self.env,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        wait(self.control.exists, 'Hardcore receiver')
        wait(lambda: len(self.tmux('list-clients', '-F', '#{client_pid}').splitlines()) >= (1 if detached else 2),
             'tmux client actually attached')
        if not detached: self.close_terminal()
        wait(lambda: len(self.tmux('list-clients', '-F', '#{client_pid}').splitlines()) == 1,
             'source client detached')
        assert int(self.tmux('display-message', '-p', '#{pane_pid}')) == self.job
        assert self.tmux('display-message', '-p', '#{pane_tty}') == self.original_tty
        os.kill(self.job, 0)

    def cleanup(self):
        # All descendants in this server belong to our disposable test session.
        self.tmux('kill-server', check=False)
        super().cleanup()


def fixture(s, detached=False):
    script = s.root/'tree.py'
    child_script = s.root/'child.py'
    child_script.write_text('import sys,time\nfrom pathlib import Path\np=Path(sys.argv[1])\n'
                            'for i in range(1200):\n p.write_text(str(i)); time.sleep(.1)\n')
    script.write_text('''import os, subprocess, sys, signal
from pathlib import Path
child = subprocess.Popen([sys.executable,
    ''' + repr(str(child_script)) + ''',
    ''' + repr(str(s.root/'ticks')) + '''])
Path(''' + repr(str(s.root/'child')) + ''').write_text(str(child.pid))
secret='PRESERVED'
print('HOST-READY', flush=True)
for line in sys.stdin:
    print('REPLY-' + secret + '-' + line.strip(), flush=True)
''')
    s.launch([sys.executable, str(script)])
    wait(lambda: 'HOST-READY' in s.screen())
    child = int((s.root/'child').read_text()); s.child_pids.append(child)
    s.type_source('before')
    wait(lambda: 'REPLY-PRESERVED-before' in s.screen())
    listing = subprocess.run([EXE, 'slurp', '--list', '--terminal', s.device], env=s.env,
                             capture_output=True, text=True, check=True, timeout=20).stdout
    assert 'portable host' in listing
    before = int(wait(lambda: (s.root/'ticks').read_text() if (s.root/'ticks').exists() else None))
    s.attach(detached)
    s.ctl('type after-close\\n')
    wait(lambda: 'REPLY-PRESERVED-after-close' in s.screen())
    s.reload()
    s.ctl('type after-reload\\n')
    wait(lambda: 'REPLY-PRESERVED-after-reload' in s.screen())
    for pid in (s.job, child): os.kill(pid, 0)
    wait(lambda: (s.root/'ticks').read_text().strip() and int((s.root/'ticks').read_text()) > before, 'child keeps working')
    print(f'PASS host: same job {s.job}, active child {child}, PTY, state, input and reload; detached={detached}', flush=True)


def agent_command(name, root):
    binary = shutil.which(name)
    if not binary: raise RuntimeError(f'{name} is not installed')
    if name == 'claude':
        return [binary, '--strict-mcp-config', '--mcp-config', '{"mcpServers":{}}',
                '--settings', '{"disableAllHooks":true}', '--tools', '', '--no-chrome']
    if name == 'codex':
        # Separate daemon/config/history; copy login only, never display it.
        home = root/'codex-home'; home.mkdir(mode=0o700)
        auth = Path.home()/'.codex/auth.json'
        if auth.exists(): shutil.copy2(auth, home/'auth.json')
        return ['/usr/bin/env', f'CODEX_HOME={home}', binary, '--no-daemon', '-s', 'read-only', '-a', 'never']
    if name == 'grok':
        return [binary, '--leader-socket', str(root/'grok-leader.sock'), '--no-subagents',
                '--disable-web-search', '--tools', '', '--permission-mode', 'plan']
    return [binary, '--sandbox', '--mode', 'plan', '--log-file', str(root/'agy.log')]


def agent(s, name, live, debug_check, in_flight=False):
    s.launch(agent_command(name, s.root))
    time.sleep(3)
    if debug_check:
        # Read-only attach/detach attempt on our own newly started binary.
        candidates = [s.job] + children(s.job)
        for pid in candidates:
            command = subprocess.run(['ps', '-p', str(pid), '-o', 'comm='], capture_output=True, text=True).stdout
            if name not in command: continue
            result = subprocess.run(['lldb', '--batch', '-p', str(pid), '-o', 'process detach'],
                                    capture_output=True, text=True, timeout=20)
            print(f'{name}: debugger attach ' + ('allowed' if result.returncode == 0 else 'denied'), flush=True)
            break
    # Report initial screen for interactive test development, kept in private temp.
    (s.root/'initial-screen.txt').write_text(s.screen())
    print(f'{name}: started PID {s.job}; initial screen stored at {s.root}/initial-screen.txt', flush=True)
    if live:
        screen = s.screen()
        if name in ('claude', 'agy') and 'Yes, I trust this folder' in screen:
            # This is the empty workspace created by this test, not a user repo.
            if name == 'claude': s.tmux('send-keys', 'Down')
            s.tmux('send-keys', 'Enter')
            wait(lambda: 'Yes, I trust this folder' not in s.screen(), 'test workspace trust')
            time.sleep(2)
        if name == 'agy' and 'esc to dismiss' in s.screen():
            s.tmux('send-keys', 'Escape')
            time.sleep(.3)
        prompt = 'Do not use any tools. For this disposable test my favorite fruit is pineapple. Remember that for later. Reply only with the concatenation of HCBEFORE and 2947, no spaces.'
        s.type_source(prompt)
        wait(lambda: 'HCBEFORE2947' in s.screen(), name+' initial answer', timeout=90)
        if in_flight:
            s.type_source('Without tools, output the integers 1 through 80, each on its own line. End with the concatenation of HCDURING and 5876, no spaces.')
            assert 'HCDURING5876' not in s.screen(), 'response completed before takeover started'
    s.attach()
    s.ctl('cmd resize 1100,760')
    if live:
        if in_flight:
            wait(lambda: 'HCDURING5876' in s.screen(), name+' in-flight answer survives attachment', timeout=90)
        s.type_receiver('Do not use tools. What is my favorite fruit? Reply with its name in uppercase.')
        wait(lambda: 'PINEAPPLE' in s.screen(), name+' conversation memory after Terminal closes', timeout=90)
    s.reload()
    if live:
        s.type_receiver('Do not use tools. Reply with my favorite fruit in uppercase followed immediately by -R.')
        wait(lambda: 'PINEAPPLE-R' in s.screen(), name+' conversation memory after reload', timeout=90)
    assert int(s.tmux('display-message', '-p', '#{pane_pid}')) == s.job
    print(f'PASS {name}: same PID/PTY after Terminal close + Hardcore reload; live_answers={live}, in_flight={in_flight}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--agent', choices=['claude', 'codex', 'grok', 'agy'], action='append')
    parser.add_argument('--live', action='store_true')
    parser.add_argument('--check-debugger', action='store_true')
    parser.add_argument('--in-flight', action='store_true', help='attach while a model response is pending (requires --live)')
    parser.add_argument('--keep', action='store_true', help='retain private logs on failure')
    args = parser.parse_args()
    if args.in_flight and not args.live: parser.error('--in-flight requires --live')
    for name in args.agent or ['fixture', 'detached']:
        root = Path(tempfile.mkdtemp(prefix='hc-host-', dir='/tmp'))
        s = HostSession(root)
        success = False
        try:
            if name in ('fixture', 'detached'): fixture(s, name == 'detached')
            else: agent(s, name, args.live, args.check_debugger, args.in_flight)
            success = True
        except BaseException:
            (root/'failure-screen.txt').write_text(s.tmux('capture-pane', '-p', '-S', '-150', check=False))
            print(f'FAIL {name}; private diagnostic screen in {root}/failure-screen.txt', flush=True)
            raise
        finally:
            s.cleanup()
            if success or not args.keep: shutil.rmtree(root)


if __name__ == '__main__': main()
