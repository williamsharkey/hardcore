#!/usr/bin/env python3
"""Summarizes HARDCORE_LAT lines ("lat ECHO COMMIT SHOWN KEYDOWN WRITTEN", ms after the key)
from hardcore's stderr: p50 / p90 / p99 of each stage."""
import sys
rows = [list(map(float, l.split()[1:])) for l in open(sys.argv[1]) if l.startswith('lat ')]
rows = [r + [-1] * (5 - len(r)) for r in rows]
def pct(v, p): v = sorted(x for x in v if x >= 0); return v[min(len(v) - 1, int(len(v) * p))] if v else float('nan')
for i, name in [(3, 'key -> keyDown: handler'), (4, 'key -> written to the pty'), (0, 'key -> echo arrived'),
                (1, 'key -> frame committed'), (2, 'key -> frame on display')]:
    col = [r[i] for r in rows]
    print(f'{name:26} p50 {pct(col, .5):6.2f}  p90 {pct(col, .9):6.2f}  p99 {pct(col, .99):6.2f} ms  (n={sum(1 for x in col if x >= 0)})')
