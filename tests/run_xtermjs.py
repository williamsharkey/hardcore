#!/usr/bin/env python3
"""Replays the xterm.js escape-sequence fixtures (tests/xtermjs, expected
screens captured from real xterm at 80x25) through hardcore's parser and diffs
the screen, the same comparison xterm.js's own Terminal2.test.ts makes.

    python3 tests/run_xtermjs.py [-v] [NAME_SUBSTRING...]
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FIX = os.path.join(HERE, 'xtermjs')
# xterm.js skips these too: either xterm itself is inconsistent there or the
# fixture depends on behavior xterm.js deliberately doesn't match.
SKIP = {'t0055-EL', 't0084-CBT', 't0101-NLM', 't0103-reverse_wrap', 't0504-vim'}

def winpath(p):
    return subprocess.check_output(['wslpath', '-w', p], text=True).strip()

verbose = '-v' in sys.argv
filters = [a for a in sys.argv[1:] if not a.startswith('-')]
names = sorted(f[:-3] for f in os.listdir(FIX) if f.endswith('.in'))
names = [n for n in names if not filters or any(f in n for f in filters)]
out = os.path.join(ROOT, 'build', 'replay.txt')
os.makedirs(os.path.dirname(out), exist_ok=True)
subprocess.run([os.path.join(ROOT, 'hardcore.exe'), '--replay', winpath(out), '80', '25']
               + [winpath(os.path.join(FIX, n + '.in')) for n in names], check=True)

screens, cur = {}, None
for line in open(out, encoding='utf-8', errors='replace').read().split('\n'):
    if line.startswith('=== '):
        cur = os.path.basename(line[4:].replace('\\', '/'))[:-3]
        screens[cur] = []
    elif cur is not None:
        screens[cur].append(line)

passed, failed, skipped = [], [], []
for n in names:
    if n in SKIP:
        skipped.append(n)
        continue
    exp = open(os.path.join(FIX, n + '.text'), encoding='utf-8', errors='replace').read().split('\n')
    exp = [l.rstrip() for l in exp][:25]
    exp += [''] * (25 - len(exp))
    got = (screens.get(n, []) + [''] * 25)[:25]
    if got == exp:
        passed.append(n)
    else:
        failed.append(n)
        if verbose:
            print(f'--- {n}')
            for i, (g, e) in enumerate(zip(got, exp)):
                if g != e:
                    print(f'  row {i+1:2}  got {g!r}\n          want {e!r}')
print(f'xterm.js fixtures: {len(passed)} passed, {len(failed)} failed, {len(skipped)} skipped (of {len(names)})')
if failed:
    print('failed:', ' '.join(failed))
sys.exit(1 if failed else 0)
