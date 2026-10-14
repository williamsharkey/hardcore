#!/usr/bin/env python3
"""The fonts besides Unifont, for the settings panel's font grid.

    gen_fonts.py OUT.bin OUT.inc

OUT.bin holds one glyph table per font, the same format as font.bin (see
gen_font.py): "WEF2", 0x1100 page offsets, 256-entry pages, then records of
a width byte and the rows, most significant bit leftmost, (width + 7) / 8
bytes per row. Offsets count from the start of that font's table. Only the
font's own glyphs are in it: hardcore fills the rest from Unifont, scaled to
the cell (or from the font named as fallback first).

OUT.inc is the font list hardcore.c compiles in: key, name, cell size, and
whether the cell is in device pixels (drawn 1:1) or in pixels the display
scale multiplies, like Unifont's.

Terminus 28 and 32 draw box lines two pixels wide, like their letters. Next
to the others' 1-pixel lines that looked heavy, so their box drawing
(U+2500..257F) is redrawn here from the characters' Unicode names: light
lines 1 pixel, heavy 3, double two 1-pixel lines, on Terminus's own centre
lines, and dashes where Terminus has them (it only has the 4-dash one; the
others are drawn evenly). Terminus 24's own box drawing is already 1 pixel:
only the dashed lines it lacks are drawn."""
import gzip, math, struct, sys, unicodedata

#  key  name        BDF                                  w   h  dev  thin fallback  box drawing: centre (col, row), and
#                                                                                   whether to redraw all of it or only what the font lacks
FONTS = [
    ('U', 'Unifont',  None,                              8, 16, 0,  1,   None,     None),
    ('Z', 'Tamzen',   'font/tamzen/Tamzen8x15r.bdf.gz',  8, 15, 0,  1,   None,     None),
    ('F', 'Fixed',    'font/misc-fixed/7x13.bdf.gz',     7, 13, 0,  1,   None,     None),
    ('t', 'Terminus', 'font/terminus/ter-u24n.bdf.gz',  12, 24, 1,  0,   None,     (5, 11, 'missing')),
    ('Y', 'Sony',     'font/sony/12x24.bdf.gz',         12, 24, 1,  0,   't',      None),
    ('E', 'ETL',      'font/etl/lt1-24-etl.bdf.gz',     12, 24, 1,  0,   't',      None),
    ('M', 'Terminus', 'font/terminus/ter-u28n.bdf.gz',  14, 28, 1,  0,   None,     (6, 14, 'all')),
    ('T', 'Terminus', 'font/terminus/ter-u32n.bdf.gz',  16, 32, 1,  0,   None,     (8, 16, 'all')),
]

def read_bdf(path, W, H):
    """cp -> (width, rows), for glyphs W or 2W wide, placed in a W x H cell on the font's baseline"""
    glyphs, asc = {}, None
    lines = gzip.open(path, 'rt', encoding='latin-1').read().split('\n')
    i = 0
    while i < len(lines):
        t = lines[i].split()
        if t and t[0] == 'FONT_ASCENT': asc = int(t[1])
        if t and t[0] == 'STARTCHAR':
            cp = dw = bbx = None; bm = []; i += 1
            while not lines[i].startswith('ENDCHAR'):
                t = lines[i].split()
                if t and t[0] == 'ENCODING': cp = int(t[1])
                elif t and t[0] == 'DWIDTH': dw = int(t[1])
                elif t and t[0] == 'BBX': bbx = list(map(int, t[1:5]))
                elif t and t[0] == 'BITMAP':
                    i += 1
                    while not lines[i].startswith('ENDCHAR'): bm.append(lines[i].strip()); i += 1
                    break
                i += 1
            # printable, not private use (hardcore's own glyphs live there), one or two cells wide
            if cp is not None and cp > 32 and not 0xE000 <= cp <= 0xF8FF and bbx and dw in (W, 2 * W):
                bw, bh, bx, by = bbx; rows = [0] * H; top = asc - (by + bh)
                for k, hx in enumerate(bm):
                    y = top + k
                    if 0 <= y < H and hx:
                        v = int(hx, 16) >> (len(hx) * 4 - bw)
                        sh = dw - bw - bx
                        rows[y] = (v << sh if sh >= 0 else v >> -sh) & ((1 << dw) - 1)
                glyphs[cp] = (dw, rows)
        i += 1
    return glyphs

# ── box drawing from the characters' names ──────────────────────────────

DIRS = {'UP': 'u', 'DOWN': 'd', 'LEFT': 'l', 'RIGHT': 'r', 'VERTICAL': 'ud', 'HORIZONTAL': 'lr'}
WEIGHT = {'LIGHT': 1, 'SINGLE': 1, 'HEAVY': 2, 'DOUBLE': 3}

def arms(name):
    """'DOWN LIGHT AND RIGHT HEAVY' -> {'d': 1, 'r': 2}. A weight before directions applies to
    the directions after it; a weight after directions applies to those."""
    out, pending, prefix = {}, [], None
    for tok in name.split():
        if tok == 'AND':
            if prefix and pending:
                for d in pending: out[d] = prefix
                pending = []
        elif tok in WEIGHT:
            if pending:
                for d in pending: out[d] = WEIGHT[tok]
                pending = []
            else:
                prefix = WEIGHT[tok]
        elif tok in DIRS:
            pending += list(DIRS[tok])
    for d in pending: out[d] = prefix
    return out

def box_glyph(cp, W, H, cx, cy, orig):
    img = [[0] * W for _ in range(H)]
    def put(x, y):
        if 0 <= x < W and 0 <= y < H: img[y][x] = 1
    def hline(y, x0, x1):
        for x in range(min(x0, x1), max(x0, x1) + 1): put(x, y)
    def vline(x, y0, y1):
        for y in range(min(y0, y1), max(y0, y1) + 1): put(x, y)
    def seg(x0, y0, x1, y1):
        dx, dy = abs(x1 - x0), -abs(y1 - y0); sx = 1 if x1 > x0 else -1; sy = 1 if y1 > y0 else -1; e = dx + dy
        while True:
            put(x0, y0)
            if x0 == x1 and y0 == y1: break
            e2 = 2 * e
            if e2 >= dy: e += dy; x0 += sx
            if e2 <= dx: e += dx; y0 += sy
    name = unicodedata.name(chr(cp))[len('BOX DRAWINGS '):]
    if 'DIAGONAL' in name:
        if 'CROSS' in name or 'UPPER RIGHT' in name: seg(W - 1, 0, 0, H - 1)
        if 'CROSS' in name or 'UPPER LEFT' in name: seg(0, 0, W - 1, H - 1)
        return img
    if ' ARC ' in name:                       # LIGHT ARC DOWN AND RIGHT: a quarter circle into the two arms
        sx = 1 if 'RIGHT' in name else -1
        sy = 1 if 'DOWN' in name else -1
        r = min(cx if sx < 0 else W - 1 - cx, cy if sy < 0 else H - 1 - cy, 6)
        ox, oy = cx + sx * r, cy + sy * r
        pts = [(round(ox - sx * r * math.cos(a)), round(oy - sy * r * math.sin(a)))
               for a in [i * math.pi / 2 / 40 for i in range(41)]]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]): seg(x0, y0, x1, y1)
        hline(cy, ox, W - 1 if sx > 0 else 0)
        vline(cx, oy, H - 1 if sy > 0 else 0)
        return img
    if 'DASH' in name:                        # the line, kept only where Terminus draws its dashes
        heavy = name.startswith('HEAVY')
        horiz = name.endswith('HORIZONTAL')
        t = 3 if heavy else 1
        if not any(orig):                     # a size without them: n even dashes, gaps between
            n = {'DOUBLE': 2, 'TRIPLE': 3, 'QUADRUPLE': 4}[name.split()[1]]
            L = W if horiz else H
            gap = max(2, round(L / n / 3))
            mask = [0] * L
            for i in range(n):
                for k in range(round(i * L / n + gap / 2), round((i + 1) * L / n - gap / 2)): mask[k] = 1
            orig = [((1 << W) - 1 if mask[y] else 0) for y in range(H)] if not horiz else \
                   [int(''.join(map(str, mask)), 2)] * H
        if horiz:
            ink = [any((r >> (W - 1 - x)) & 1 for r in orig) for x in range(W)]
            for y in range(cy - t // 2, cy + t // 2 + 1):
                for x in range(W):
                    if ink[x]: put(x, y)
        else:
            ink = [bool(r) for r in orig]
            for x in range(cx - t // 2, cx + t // 2 + 1):
                for y in range(H):
                    if ink[y]: put(x, y)
        return img
    a = arms(name)
    U, D, L, R = a.get('u'), a.get('d'), a.get('l'), a.get('r')
    vdouble = U == 3 or D == 3
    hdouble = L == 3 or R == 3
    vhalf = max(1 if w == 2 else 0 for w in (U, D) if w) if (U or D) and not vdouble else 0
    hhalf = max(1 if w == 2 else 0 for w in (L, R) if w) if (L or R) and not hdouble else 0
    # double lines: each of the two runs from the edge to where it meets what's across it
    def double_end(across, opposite, other, c):
        """where one line of a double arm stops, counted from the centre c towards the arm:
        across: the arm on this line's side; opposite: the arm straight ahead; other: the arm on the far side"""
        if across: return c + 2 if across == 3 else c
        if opposite: return c
        return c - 2 if other == 3 else c
    if R == 3:
        hline(cy - 2, W - 1, double_end(U, L, D, cx))
        hline(cy + 2, W - 1, double_end(D, L, U, cx))
    if L == 3:
        hline(cy - 2, 0, 2 * cx - double_end(U, R, D, cx))
        hline(cy + 2, 0, 2 * cx - double_end(D, R, U, cx))
    if D == 3:
        vline(cx - 2, H - 1, double_end(L, U, R, cy))
        vline(cx + 2, H - 1, double_end(R, U, L, cy))
    if U == 3:
        vline(cx - 2, 0, 2 * cy - double_end(L, D, R, cy))
        vline(cx + 2, 0, 2 * cy - double_end(R, D, L, cy))
    # light and heavy arms: from the edge to the centre, through what crosses there
    for d, w in (('r', R), ('l', L), ('d', D), ('u', U)):
        if not w or w == 3: continue
        t = 1 if w == 1 else 3
        for k in range(-(t // 2), t // 2 + 1):
            if d == 'r': hline(cy + k, W - 1, cx + 2 if vdouble else cx - vhalf)
            if d == 'l': hline(cy + k, 0, cx - 2 if vdouble else cx + vhalf)
            if d == 'd': vline(cx + k, H - 1, cy + 2 if hdouble else cy - hhalf)
            if d == 'u': vline(cx + k, 0, cy - 2 if hdouble else cy + hhalf)
    return img

def table(glyphs):
    NP = 0x1100
    pages = sorted({cp >> 8 for cp in glyphs}); off = [0] * NP
    base = 4 + NP * 4; rb = base + len(pages) * 1024
    blocks = bytearray(len(pages) * 1024); rec = bytearray()
    for pi, pg in enumerate(pages):
        off[pg] = base + pi * 1024
        for lo in range(256):
            g = glyphs.get((pg << 8) | lo)
            if g is None: continue
            w, rows = g; bpr = (w + 7) // 8
            struct.pack_into('<I', blocks, pi * 1024 + lo * 4, rb + len(rec))
            rec.append(w)
            for r in rows: rec += (r << (bpr * 8 - w)).to_bytes(bpr, 'big')
    t = b'WEF2' + struct.pack('<%dI' % NP, *off) + blocks + rec
    return t + b'\0' * (-len(t) % 16)

def main():
    out_bin, out_inc = sys.argv[1], sys.argv[2]
    blob = bytearray(); rows = []
    for key, name, bdf, w, h, dev, thin, fb, centre in FONTS:
        if bdf is None:
            rows.append((key, name, w, h, dev, thin, fb, -1, 0)); continue
        g = read_bdf(bdf, w, h)
        if centre:
            for cp in range(0x2500, 0x2580):
                if centre[2] == 'missing' and cp in g: continue
                orig = g[cp][1] if cp in g else [0] * h
                img = box_glyph(cp, w, h, centre[0], centre[1], orig)
                g[cp] = (w, [int(''.join(map(str, r)), 2) for r in img])
        rows.append((key, name, w, h, dev, thin, fb, len(blob), len(g)))
        blob += table(g)
    open(out_bin, 'wb').write(blob)
    with open(out_inc, 'w') as f:
        f.write('/* tools/gen_fonts.py: the font list. off: this font\'s glyph table in fonts.bin, -1 = Unifont (font.bin) */\n')
        for key, name, w, h, dev, thin, fb, off, n in rows:
            f.write("{'%s', \"%s\", %d, %d, %d, %d, %s, %d},   /* %d glyphs */\n"
                    % (key, name, w, h, dev, thin, "'%s'" % fb if fb else '0', off, n))
    print('%s: %d fonts, %d bytes' % (out_bin, len(rows), len(blob)))

main()
