#!/usr/bin/env python3
"""Opt-in real Windows Terminal -> Hardcore integration; only disposable sessions.
No model prompts are submitted. Interactive CLIs are checked with an unsent draft.
Artifacts and JSON results are retained under build/slurp-apps/.
"""
import argparse
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import subprocess
import termios
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/slurp-apps'
DISTRO = os.environ.get('WSL_DISTRO_NAME', 'Debian')


def winpath(path):
    return subprocess.check_output(['wslpath', '-w', str(path)], text=True).strip()


def proc(pid):
    data = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return {'pid': pid, 'ppid': int(data[1]), 'start': data[19], 'sid': int(data[3]), 'fg': int(data[5]), 'state': data[0]}


def session(sid):
    found = {}
    for entry in Path('/proc').iterdir():
        if entry.name.isdigit():
            try:
                p = proc(int(entry.name))
                if p['sid'] == sid and p['state'] != 'Z':
                    found[p['pid']] = p['start']
            except (FileNotFoundError, ProcessLookupError):
                pass
    return found


def terminal_modes(pid):
    fd = os.open(f'/proc/{pid}/fd/0', os.O_RDONLY | os.O_NOCTTY)
    try:
        return termios.tcgetattr(fd)
    finally:
        os.close(fd)


class Driver:
    def __init__(self):
        self.p = subprocess.Popen([str(ROOT / 'build/slurp-driver.exe'), '--matrix'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def command(self, line, timeout=25, required=True):
        self.p.stdin.write((line + '\n').encode()); self.p.stdin.flush()
        ready = select.select([self.p.stdout], [], [], timeout)[0]
        if not ready:
            raise AssertionError(f'driver timeout: {line.split()[0]}')
        answer = self.p.stdout.readline().strip()
        ok = answer == b'OK'
        if required:
            assert ok, f'driver failed: {line[:160]} ({answer!r})'
        return ok

    def send(self, text):
        self.command('send ' + text.encode().hex())

    def text(self, path):
        self.command('text ' + winpath(path))
        return path.read_text(errors='replace')

    def close(self):
        if self.p.poll() is None:
            try:
                self.command('quit')
                self.p.wait(timeout=8)
            except (AssertionError, BrokenPipeError, subprocess.TimeoutExpired):
                self.p.kill(); self.p.wait()
        self.errors = self.p.stderr.read()
        for stream in (self.p.stdin, self.p.stdout, self.p.stderr):
            stream.close()


def wait_file(path, timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if path.exists() and path.stat().st_size:
            return path.read_text().strip()
        time.sleep(.1)
    raise AssertionError(f'fixture did not start: {path.name}')


def wsl_case(name):
    executable = shutil.which({'shell': 'bash', 'vim': 'vim'}.get(name, name))
    if name == 'vim' and not executable:
        executable = shutil.which('vim.tiny')
    if not executable:
        return {'app': name, 'status': 'skip', 'reason': 'not installed'}
    folder = OUT / (name + '-' + uuid.uuid4().hex[:8]); folder.mkdir(parents=True)
    title = 'hc-slurp-' + folder.name
    metadata = folder / 'pid'; rc = folder / 'rc.bash'; launcher = folder / 'launch.sh'
    document = folder / 'document.txt'
    document.write_text('FIRST-LINE\nSECOND-LINE\nTHIRD-LINE\n')
    command = ''
    if name == 'nano':
        command = shlex.join([executable, '--ignorercfiles', '+2,3', str(document)])
    elif name == 'vim':
        command = shlex.join([executable, '-u', 'NONE', '-i', 'NONE', '-n', '-c',
                             'normal ggccUNSAVED-BEFORE', '-c', 'normal 2G3|', str(document)])
    elif name != 'shell':
        command = shlex.quote(executable)
    rc.write_text("set -m; PS1='SLURP-PROMPT> '; HISTFILE=/dev/null; export HC_MEMORY=retained\n"
                  "history -s 'echo HISTORY-BEFORE'; sleep 300 &\n"
                  + (command + "\nprintf 'APP-RETURNED\\n'\n" if command else ''))
    launcher.write_text('#!/bin/bash\nunset HARDCORE_RC TERM_PROGRAM\n'
                        + f'cd {shlex.quote(str(folder))}\n'
                        + f'printf "%s" "$$" > {shlex.quote(str(metadata))}\n'
                        + f'exec /bin/bash --noprofile --rcfile {shlex.quote(str(rc))} -i\n')
    d = Driver(); pid = None
    result = {'app': name, 'source': 'Windows Terminal / WSL', 'artifacts': str(folder.relative_to(ROOT))}
    try:
        subprocess.run(['wt.exe', '-w', 'new', 'new-tab', '--title', title, '--suppressApplicationTitle',
                        'wsl.exe', '-d', DISTRO, '--exec', '/bin/bash', str(launcher)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        pid = int(wait_file(metadata)); before = proc(pid)
        time.sleep(2 if name in ('shell', 'nano', 'vim') else 6)
        assert b'WT_SESSION=' in Path(f'/proc/{pid}/environ').read_bytes(), 'source is not Windows Terminal'
        before_processes = session(pid)
        foreground = proc(pid)['fg']
        # Commands run from an interactive rc file may share the shell's process
        # group until rc processing finishes. Identify the actual executable too.
        app_processes = [p for p in before_processes if proc(p)['ppid'] == pid
                         and os.path.samefile(f'/proc/{p}/exe', executable)]
        before_modes = terminal_modes(pid)
        assert len(before_processes) >= (2 if name == 'shell' else 3), 'application or background job did not start'
        d.command('source-visible ' + title)
        d.command(f'wsl {pid} {before["start"]} {os.getuid()} {DISTRO}')
        d.command('sleep 1000')
        d.command('alive')
        d.command('close-source ' + title)
        d.command('sleep 500')
        assert proc(pid)['start'] == before['start'], 'shell identity changed'
        assert all(proc(p)['start'] == birth for p, birth in before_processes.items()), 'application/job identity changed'
        assert terminal_modes(pid) == before_modes, 'source terminal modes changed'
        result['same_processes_after_source_close'] = True
        result['terminal_modes_preserved'] = True
        d.text(folder / 'after-attach.txt')
        # Exercise redraw and window resizing, including applications already in raw mode.
        d.command('resize 93 27'); d.command('sleep 400')
        if name == 'shell':
            d.send("printf 'MEMORY-%s-PID-%s\\n' \"$HC_MEMORY\" \"$$\"\r")
            d.command(f'wait MEMORY-retained-PID-{pid}')
            d.send('history\r'); d.command('wait echo HISTORY-BEFORE')
            d.send('stty size\r'); d.command('wait 27 93')
            d.send('sleep 300\r'); d.command('sleep 200'); d.send('\x03')
            d.send("printf 'INTERRUPT-%s\\n' works\r"); d.command('wait INTERRUPT-works')
        elif name == 'vim':
            if not d.command('wait UNSAVED-BEFORE', required=False):
                d.send('\x0c'); d.command('wait UNSAVED-BEFORE')
                result['redraw_shortcut_required'] = True
            d.send('iAFTER-TRANSFER\x1b')
            d.send(':wq\r'); d.command('wait APP-RETURNED')
            assert document.read_text().startswith('UNSAVED-BEFORE\n'), 'unsaved buffer lost'
            assert 'SEAFTER-TRANSFERCOND-LINE' in document.read_text(), 'editor cursor moved'
            result['unsaved_buffer_and_cursor'] = True
        elif name == 'nano':
            d.command('wait SECOND-LINE'); d.send('AFTER-TRANSFER')
            d.send('\x0f'); d.command('sleep 250'); d.send('\r'); d.command('sleep 250'); d.send('\x18')
            d.command('wait APP-RETURNED')
            assert 'SEAFTER-TRANSFERCOND-LINE' in document.read_text(), 'editor cursor/input changed'
            result['cursor_and_edit_save'] = True
        else:
            # A draft proves terminal input and redraw without dispatching model work.
            if name == 'agy':
                screen = d.text(folder / 'before-draft.txt')
                if 'Do you trust the contents of this project?' in screen:
                    # This newly created fixture directory contains only our test files.
                    d.send('\r'); d.command('sleep 1500')
            d.send('SLURP-UNSENT-DRAFT'); d.command('sleep 500')
            screen = d.text(folder / 'draft.txt')
            assert 'SLURP-UNSENT-DRAFT' in screen, 'draft not visible (onboarding, login, or redraw problem)'
            assert app_processes, 'CLI executable was not running before attachment'
            assert all(proc(p)['start'] == before_processes[p] and proc(p)['state'] != 'Z'
                       for p in app_processes), 'CLI exited or restarted after input'
            assert proc(pid)['fg'] == foreground, 'input returned to the shell instead of the CLI'
            result['unsent_draft'] = True
            d.send('\x15')
        d.text(folder / 'final.txt')
        d.command('alive')
        result['status'] = 'pass'
    except Exception as exc:
        result.update(status='fail', reason=str(exc))
        try:
            d.text(folder / 'failure.txt')
        except Exception:
            pass
    finally:
        if pid:
            for child in session(pid):
                try:
                    os.kill(child, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        try:
            d.command('close-source ' + title)
        except Exception:
            pass
        d.close()
    return result


def native_case(name):
    folder = OUT / (name + '-' + uuid.uuid4().hex[:8]); folder.mkdir(parents=True)
    title = 'hc-slurp-' + folder.name
    metadata = folder / 'pid'; d = Driver()
    powershell = '/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe'
    result = {'app': name, 'source': 'classic console', 'artifacts': str(folder.relative_to(ROOT))}
    try:
        if name == 'cmd':
            script = folder / 'fixture.cmd'
            # A child PowerShell reads its own parent PID, the cmd shell being tested.
            ps = "$p=Get-CimInstance Win32_Process -Filter ('ProcessId='+$PID); "
            ps += "Set-Content -Path '" + winpath(metadata) + "' -Value $p.ParentProcessId"
            script.write_text('@echo off\ntitle ' + title + '\nset HC_MEMORY=retained\n'
                + 'prompt NATIVE-PROMPT$G\n'
                + 'powershell.exe -NoProfile -Command "' + ps + '"\n'
                + 'echo NATIVE-READY\n')
            command = ['cmd.exe', '/d', '/k', winpath(script)]
        elif name in ('git-bash', 'mintty'):
            bash = Path('/mnt/c/Program Files/Git/bin/bash.exe')
            if not bash.exists():
                return dict(result, status='skip', reason='Git for Windows not installed')
            script = folder / 'fixture.bash'
            script.write_text("export HC_MEMORY=retained; HISTFILE=/dev/null; PS1='NATIVE-PROMPT> '\n"
                              + "printf '\\033]0;" + title + "\\007'\n"
                              + "/usr/bin/ps -p $$ | /usr/bin/awk 'NR==2 {print $4}' > "
                              + shlex.quote(winpath(metadata).replace('\\', '/'))
                              + "\necho NATIVE-READY\n")
            command = [winpath(bash), '--noprofile', '--rcfile', winpath(script).replace('\\', '/'), '-i']
        else:
            script = folder / 'fixture.ps1'
            script.write_text("$Host.UI.RawUI.WindowTitle = '" + title + "'\n"
                              + "$global:HC_MEMORY='retained'\nSet-Content -Path '" + winpath(metadata)
                              + "' -Value $PID\nfunction prompt { 'NATIVE-PROMPT> ' }\nWrite-Output 'NATIVE-READY'\n")
            command = [powershell, '-NoProfile', '-NoExit', '-ExecutionPolicy', 'Bypass', '-File', winpath(script)]
            if name == 'pwsh' and not shutil.which('pwsh.exe'):
                return dict(result, status='skip', reason='PowerShell 7 not installed')
            command[0] = winpath(shutil.which('pwsh.exe')) if name == 'pwsh' else winpath(command[0])
        if name == 'wt-powershell':
            result['source'] = 'Windows Terminal native pseudoconsole'
            subprocess.run(['wt.exe', '-w', 'new', 'new-tab', '--title', title, '--suppressApplicationTitle', *command],
                           check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15)
        elif name == 'mintty':
            result['source'] = 'Git for Windows mintty / MSYS PTY'
            mintty = Path('/mnt/c/Program Files/Git/usr/bin/mintty.exe')
            if not mintty.exists():
                return dict(result, status='skip', reason='mintty not installed')
            d.command('launch ' + subprocess.list2cmdline([winpath(mintty), '-t', title, '-e', *command]))
        else:
            d.command('launch ' + subprocess.list2cmdline(['conhost.exe', *command]))
        pid = int(wait_file(metadata)); time.sleep(.5)
        if name != 'wt-powershell':
            d.command('show-source ' + title)
        d.command('source-visible ' + title)
        attached = d.command('native ' + str(pid), required=False)
        if name in ('wt-powershell', 'mintty'):
            assert not attached, 'unsupported pseudoconsole was attached'
            d.command('source-visible ' + title)
            result.update(status='pass', unsupported_safely_skipped=True)
        else:
            assert attached, 'native console attach failed'
            d.command('wait NATIVE-PROMPT>'); d.command('commit'); d.command('sleep 500')
            assert not d.command('source-visible ' + title, required=False), 'source was not hidden'
            d.text(folder / 'before-input.txt')
            if name == 'cmd':
                d.send('echo MEMORY-%HC_MEMORY%\r'); d.command('wait MEMORY-retained')
            elif name == 'git-bash':
                d.send("printf 'MEMORY-%s\\n' \"$HC_MEMORY\"\r"); d.command('wait MEMORY-retained')
            else:
                d.send("Write-Output ('MEMORY-'+$HC_MEMORY+'-PID-'+$PID)\r")
                d.command(f'wait MEMORY-retained-PID-{pid}')
            d.text(folder / 'final.txt'); d.command('detach'); d.command('sleep 400')
            d.command('source-visible ' + title)
            result.update(status='pass', memory_and_input=True, source_restored_on_detach=True)
    except Exception as exc:
        result.update(status='fail', reason=str(exc))
        try:
            d.text(folder / 'failure.txt')
        except Exception:
            pass
    finally:
        try:
            d.command('close-source ' + title)
        except Exception:
            pass
        d.close()
        (folder / 'driver.log').write_bytes(d.errors)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps', nargs='+', default=['shell', 'nano', 'vim', 'claude', 'grok', 'codex', 'agy',
                                                    'cmd', 'powershell', 'pwsh', 'git-bash', 'wt-powershell', 'mintty'])
    args = parser.parse_args(); OUT.mkdir(parents=True, exist_ok=True)
    driver = Driver()
    try:
        driver.command('capture-check')
    finally:
        driver.close()
    results = []
    for name in args.apps:
        result = native_case(name) if name in ('cmd', 'powershell', 'pwsh', 'git-bash', 'wt-powershell', 'mintty') else wsl_case(name)
        results.append(result)
        print(json.dumps(result), flush=True)
        (OUT / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return int(any(r['status'] == 'fail' for r in results))


if __name__ == '__main__':
    raise SystemExit(main())
