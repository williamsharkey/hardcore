#!/usr/bin/env python3
"""Launches terminals one at a time running bench/termload.sh, and samples each
terminal's own CPU time and memory at every phase mark. Needs no Screen
Recording or Accessibility permission: nothing is typed and nothing is read
off the screen.

  bench/compare-mac.py [OUTDIR] [TERMINAL...]   (hardcore ghostty kitty wezterm iterm2 terminal)
"""
import os, re, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOAD = ROOT / 'bench/termload.sh'
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else '/tmp/termcompare')
WANT = sys.argv[2:] or ['hardcore', 'ghostty', 'kitty', 'wezterm', 'iterm2', 'terminal']
OUT.mkdir(parents=True, exist_ok=True)


def procs():
    """pid -> (ppid, start seconds ago, command)"""
    rows = subprocess.run(['ps', '-axo', 'pid=,ppid=,etime=,comm='], capture_output=True, text=True).stdout
    table = {}
    for line in rows.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 4:
            continue
        pid, ppid, etime, comm = parts
        d, _, rest = etime.rpartition('-')
        secs = 0
        for v in rest.split(':'):
            secs = secs * 60 + float(v)
        secs += int(d or 0) * 86400
        table[int(pid)] = (int(ppid), secs, comm)
    return table


def cpu_and_rss(pids):
    if not pids:
        return 0.0, 0
    rows = subprocess.run(['ps', '-o', 'time=,rss=', '-p', ','.join(map(str, pids))], capture_output=True, text=True).stdout
    cpu, rss = 0.0, 0
    for line in rows.splitlines():
        t, r = line.split()
        d, _, rest = t.rpartition('-')
        secs = 0.0
        for v in rest.split(':'):
            secs = secs * 60 + float(v)
        cpu += secs + int(d or 0) * 86400
        rss += int(r)
    return cpu, rss


def launch(name, res):
    load = ['/bin/bash', str(LOAD), name, str(OUT)]
    env = dict(os.environ)
    for k in list(env):
        if k.startswith(('HARDCORE_', 'TERM_PROGRAM', 'GHOSTTY', 'KITTY', 'WEZTERM', 'ITERM')):
            del env[k]
    if name == 'hardcore':
        cfg = tempfile.mkdtemp(prefix='hc-compare-')
        shell = Path(cfg) / 'load-shell'   # the shell hardcore starts: the load, except in pre-warmed spares
        shell.write_text('#!/bin/bash\n[ -n "$HARDCORE_SPARE" ] && exec sleep 100000\nexec ' +
                         ' '.join(f"'{a}'" for a in load) + '\n')
        shell.chmod(0o755)
        env.update(HARDCORE_TEST_CONFIG=cfg, HARDCORE_SHELL=str(shell))
        exe = Path(os.environ.get('HC_COMPARE_APP', ROOT / 'hardcore.app')) / 'Contents/MacOS/hardcore'
        frame = os.environ.get('HC_COMPARE_FRAME', '400 300 1128 610 0 0 2240 1235 ')
        return subprocess.Popen([str(exe), '--foreground', '-NSWindow Frame hardcore', frame, '--cd', str(ROOT)],
                                env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if name == 'ghostty':
        return subprocess.Popen(['/Applications/Ghostty.app/Contents/MacOS/ghostty', '--window-width=120', '--window-height=36',
                                 '--quit-after-last-window-closed=true', '--auto-update=off',
                                 # (-e brings up a dialog now and then instead of running it)
                                 '--confirm-close-surface=false', '--initial-command=' + ' '.join(f"'{a}'" for a in load)], env=env,
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if name == 'kitty':
        return subprocess.Popen(['/Applications/kitty.app/Contents/MacOS/kitty', '--config', 'NONE', '-o', 'initial_window_width=120c',
                                 '-o', 'initial_window_height=36c', '-o', 'remember_window_size=no', '-o', 'confirm_os_window_close=0',
                                 *load], env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if name == 'wezterm':
        return subprocess.Popen(['/Applications/WezTerm.app/Contents/MacOS/wezterm-gui', '--config', 'initial_cols=120',
                                 '--config', 'initial_rows=36', 'start', '--always-new-process', '--', *load], env=env,
                                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    script = OUT / f'{name}.command'
    script.write_text('#!/bin/bash\n' + ' '.join(f"'{a}'" for a in load) + '\nexit\n')
    script.chmod(0o755)
    app = {'iterm2': 'iTerm', 'terminal': 'Terminal'}[name]
    subprocess.run(['open', '-a', app, str(script)], env=env)
    return None


PATTERNS = {'hardcore': r'/hardcore\.app/Contents/MacOS/hardcore$', 'ghostty': r'Ghostty\.app/Contents/MacOS/ghostty$',
            'kitty': r'kitty\.app/Contents/MacOS/kitty$', 'wezterm': r'WezTerm\.app/Contents/MacOS/wezterm-gui$',
            'iterm2': r'iTerm\.app/Contents/(MacOS/iTerm2|.*iTermServer.*)$', 'terminal': r'Terminal\.app/Contents/MacOS/Terminal$'}


def terminal_pids(name, proc, since):
    table = procs()
    if proc and name in ('hardcore', 'ghostty', 'kitty', 'wezterm'):
        pids = [proc.pid] if proc.pid in table else []
        # wezterm-gui may re-exec; take any matching process started since launch
    else:
        pids = []
    for pid, (ppid, age, comm) in table.items():
        if re.search(PATTERNS[name], comm) and (name == 'terminal' or age <= since + 2) and pid not in pids:
            pids.append(pid)
    return pids


APPNAME = {'ghostty': 'ghostty', 'kitty': 'kitty', 'wezterm': 'wezterm-gui', 'iterm2': 'iTerm2'}


def run(name, tries=2):
    res = OUT / f'{name}.txt'
    if res.exists():
        res.unlink()
    if name == 'iterm2':         # a window left from an earlier run would be measured with it
        subprocess.run(['pkill', '-x', 'iTerm2'])
        time.sleep(2)
    t0 = time.time()
    proc = launch(name, res)
    samples, seen, pids = [], 0, []
    deadline = t0 + 900
    while time.time() < deadline:
        time.sleep(0.05)
        if not pids or len(pids) < 2 and name == 'iterm2':
            pids = terminal_pids(name, proc, time.time() - t0) or pids
        if not res.exists():
            if time.time() - t0 > 60:
                print(f'{name}: the load script never started', file=sys.stderr)
                if proc and proc.poll() is None:
                    proc.kill()
                if name in APPNAME:
                    subprocess.run(['pkill', '-x', APPNAME[name]])
                if tries > 1:    # (a dialog left from an earlier instance: once more)
                    time.sleep(3)
                    return run(name, tries - 1)
                break
            continue
        lines = res.read_text().splitlines()
        for line in lines[seen:]:
            if line.startswith('mark '):
                cpu, rss = cpu_and_rss(pids)
                samples.append((line.split()[1], time.time(), cpu, rss))
        seen = len(lines)
        if lines and lines[-1] == 'done':
            break
    else:
        print(f'{name}: timed out', file=sys.stderr)
    time.sleep(1.5)
    if name == 'iterm2':
        subprocess.run(['pkill', '-x', 'iTerm2'])
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
    report(name, res.read_text() if res.exists() else '', samples, pids)


def report(name, text, samples, pids):
    lines = text.splitlines()
    size = next((l[5:] for l in lines if l.startswith('size ')), '?')
    cats = {}
    for l in lines:
        if l.startswith('cat '):
            _, f, _, t = l.split()
            cats.setdefault(f, []).append(float(t))
    phases = {}
    for (m1, t1, c1, r1), (m2, t2, c2, r2) in zip(samples, samples[1:]):
        if m1.endswith('_start') and m2 == m1[:-6] + '_end':
            phases[m1[:-6]] = (100 * (c2 - c1) / (t2 - t1), r2)
    row = {'terminal': name, 'size': size, 'pids': pids,
           'cat_best': {f: min(v) for f, v in cats.items()},
           'cpu_pct': {k: round(v[0], 1) for k, v in phases.items()},
           'rss_mb': {k: round(v[1] / 1024) for k, v in phases.items()}}
    with open(OUT / 'results.jsonl', 'a') as f:
        f.write(repr(row) + '\n')
    print(row, flush=True)


for name in WANT:
    run(name)
