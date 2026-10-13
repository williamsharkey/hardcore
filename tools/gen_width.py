#!/usr/bin/env python3
"""Generate the cell-width tables (build/width.inc) from the Unicode data files
in font/ucd (EastAsianWidth.txt, DerivedGeneralCategory.txt).

  0 cells: nonspacing/enclosing marks (Mn, Me), format chars (Cf, except U+00AD),
           Hangul medial/final jamo, ZWSP
  2 cells: East Asian Wide/Fullwidth, plus the unassigned rest of planes 2 and 3
  1 cell:  everything else
That's the classic wcwidth rule, which is what ConPTY measures with in its
GLYPH_WIDTH_WCSWIDTH mode, so the grid agrees with the console host."""
import sys

def ranges(path, want):
    out = []
    for line in open(path, encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        r, prop = [x.strip() for x in line.split(';')[:2]]
        if prop not in want:
            continue
        a, _, b = r.partition('..')
        out.append((int(a, 16), int(b or a, 16)))
    return out

def merge(rs):
    rs.sort()
    out = []
    for a, b in rs:
        if out and a <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out

ucd = sys.argv[1] if len(sys.argv) > 1 else 'font/ucd'
zero = ranges(ucd + '/DerivedGeneralCategory.txt', {'Mn', 'Me', 'Cf'})
zero = [r for r in zero if r != (0xAD, 0xAD)]
zero += [(0x1160, 0x11FF), (0x200B, 0x200B)]
wide = ranges(ucd + '/EastAsianWidth.txt', {'W', 'F'}) + [(0x20000, 0x2FFFD), (0x30000, 0x3FFFD)]
zero, wide = merge(zero), merge(wide)
# a zero-width range never also counts as wide (cp_width checks zero first)
for name, rs in (('W0', zero), ('W2', wide)):
    print('static const uint32_t %s[][2] = {' % name)
    for i in range(0, len(rs), 4):
        print('    ' + ' '.join('{0x%X, 0x%X},' % r for r in rs[i:i + 4]))
    print('};')
