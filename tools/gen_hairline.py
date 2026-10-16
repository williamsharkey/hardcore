#!/usr/bin/env python3
"""Regenerate the shipped Hairline 12x26 bitmap from its saved lab design.

Normal builds read the checked-in BDF and do not need Node. Font development
uses the exact JavaScript engine from Hairline Lab, then adapts terminal
graphics to the new cell. --check verifies the committed result without writes.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess

import gen_fonts

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'font/terminus/ter-u24n.bdf.gz'
RECIPE_PATH = ROOT / 'font/hairline/design.json'
OUTPUT = ROOT / 'font/hairline/hairline-12x26.bdf.gz'
ENGINE = ROOT / 'experiments/font-lab/variant-engine.js'
SOURCE_SHA256 = 'ff640e9e097983355e8f70ed8fb645bd850184a0d590de17f6fc9ceec1bf8eaf'
WIDTH, HEIGHT, ASCENT = 12, 26, 21


def source_glyphs() -> dict[int, tuple[int, list[int]]]:
    raw = gzip.decompress(SOURCE.read_bytes())
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        raise ValueError('Terminus source changed; review the derivative before regenerating')
    # This pinned source uses a uniform full-cell BDF bounding box. Refuse
    # different metrics instead of silently cropping a future source revision.
    glyphs = {}
    for block in raw.decode('ascii').split('STARTCHAR ')[1:]:
        block = block.split('ENDCHAR', 1)[0]
        cp = int(re.search(r'^ENCODING (\d+)$', block, re.M)[1])
        if cp in glyphs or not re.search(r'^DWIDTH 12 0$', block, re.M) or not re.search(r'^BBX 12 24 0 -5$', block, re.M):
            raise ValueError(f'Unexpected source metrics or duplicate U+{cp:04X}')
        lines = block.split('BITMAP\n', 1)[1].strip().splitlines()
        if len(lines) != 24 or any(not re.fullmatch(r'[0-9A-F]{4}', row) or int(row, 16) & 15 for row in lines):
            raise ValueError(f'Malformed source bitmap U+{cp:04X}')
        glyphs[cp] = (WIDTH, [int(row, 16) >> 4 for row in lines])
    if len(glyphs) != 1356:
        raise ValueError('Unexpected source character coverage')
    return glyphs


def block_glyph(cp: int, source: dict) -> list[int]:
    """Fill cell-relative block shapes; keep original horizontal/shade masks."""
    full = (1 << WIDTH) - 1
    if cp in (0x2591, 0x2592, 0x2593):
        pattern = source[cp][1]
        if any(row != pattern[y % 2] for y, row in enumerate(pattern)):
            raise ValueError('Unexpected shade pattern')
        return [pattern[y % 2] for y in range(HEIGHT)]
    if 0x2589 <= cp <= 0x2590:
        return [source[cp][1][0]] * HEIGHT
    if cp == 0x2595:  # Right one eighth; mirror the original left eighth.
        left = source[0x258F][1][0]
        right = int(f'{left:012b}'[::-1], 2)
        return [right] * HEIGHT
    if cp == 0x2580:
        return [full] * (HEIGHT // 2) + [0] * (HEIGHT // 2)
    if 0x2581 <= cp <= 0x2588:
        # Nearest integer, half upward, for the requested lower eighths.
        count = (HEIGHT * (cp - 0x2580) + 4) // 8
        return [0] * (HEIGHT - count) + [full] * count
    if cp == 0x2594:
        count = (HEIGHT + 4) // 8
        return [full] * count + [0] * (HEIGHT - count)
    # Bit order: upper left, upper right, lower left, lower right.
    quadrants = {0x2596: 4, 0x2597: 8, 0x2598: 1, 0x2599: 13,
                 0x259A: 9, 0x259B: 7, 0x259C: 11, 0x259D: 2,
                 0x259E: 6, 0x259F: 14}
    mask = quadrants[cp]
    rows = []
    for y in range(HEIGHT):
        shift = 0 if y < HEIGHT // 2 else 2
        rows.append((0xFC0 if mask & (1 << shift) else 0) |
                    (0x03F if mask & (2 << shift) else 0))
    return rows


def build_font() -> dict[int, tuple[int, list[int]]]:
    source = source_glyphs()
    recipe = json.loads(RECIPE_PATH.read_text())
    if recipe['format'] != 'hardcore-hairline-design' or recipe['version'] != 1:
        raise ValueError('Unsupported Hairline recipe')
    settings = recipe['settings']
    if (settings['xHeightDelta'], settings['capHeightDelta'], settings['cellHeight']) != (2, 3, HEIGHT):
        raise ValueError('This shipped family is the approved +2 / +3 / 26px design')
    base = {'name': 'Terminus 12x24', 'width': WIDTH, 'height': 24, 'ascent': 19,
            'glyphs': {cp: rows for cp, (_, rows) in source.items()}}
    script = """const fs = require('fs');
const {makeFont} = require(process.argv[1]);
const {base, settings} = JSON.parse(fs.readFileSync(0, 'utf8'));
process.stdout.write(JSON.stringify(makeFont(base, settings)));
"""
    result = subprocess.run(['node', '-e', script, str(ENGINE)],
                            input=json.dumps({'base': base, 'settings': settings}),
                            text=True, capture_output=True, check=True, timeout=30)
    font = json.loads(result.stdout)
    if (font['width'], font['height'], font['ascent']) != (WIDTH, HEIGHT, ASCENT):
        raise ValueError('Generated font metrics changed')
    glyphs = {int(cp): (WIDTH, rows) for cp, rows in font['glyphs'].items()}
    # The browser explores 15 basic light boxes. Ship the complete box range
    # on the same centre (5,12), including rounded/double/heavy/dashed forms.
    # Fresh dash masks are essential: a 24-row mask cannot tile a 26-row cell.
    for cp in range(0x2500, 0x2580):
        pixels = gen_fonts.box_glyph(cp, WIDTH, HEIGHT, 5, 12, [0] * HEIGHT)
        rows = [int(''.join(map(str, row)), 2) for row in pixels]
        if cp in font['experiment']['boxCodepoints'] and rows != glyphs[cp][1]:
            raise ValueError(f'Production box differs from the approved lab U+{cp:04X}')
        glyphs[cp] = (WIDTH, rows)
    for cp in range(0x2580, 0x25A0):
        glyphs[cp] = (WIDTH, block_glyph(cp, source))
    return glyphs


def generate_bdf() -> bytes:
    glyphs = build_font()
    props = [
        'FAMILY_NAME "Hairline"', 'FOUNDRY "Hardcore"', 'WEIGHT_NAME "Medium"',
        'SLANT "R"', 'SETWIDTH_NAME "Normal"', 'ADD_STYLE_NAME ""',
        'COPYRIGHT "Copyright (C) 2020 Dimitar Toshkov Zhekov"',
        'NOTICE "SIL OFL 1.1; modified from Terminus; see OFL.txt and README.md"',
        'PIXEL_SIZE 26', 'POINT_SIZE 260', 'RESOLUTION_X 72', 'RESOLUTION_Y 72',
        'SPACING "C"', 'AVERAGE_WIDTH 120', 'CHARSET_REGISTRY "ISO10646"',
        'CHARSET_ENCODING "1"', 'MIN_SPACE 12', 'FONT_ASCENT 21',
        'FONT_DESCENT 5', 'CAP_HEIGHT 18', 'X_HEIGHT 13', 'DEFAULT_CHAR 65533',
    ]
    lines = ['STARTFONT 2.1',
             'COMMENT Generated by tools/gen_hairline.py; design.json: x+2 caps+3 cell26',
             'FONT -Hardcore-Hairline-Medium-R-Normal--26-260-72-72-C-120-ISO10646-1',
             'SIZE 26 72 72', 'FONTBOUNDINGBOX 12 26 0 -5',
             f'STARTPROPERTIES {len(props)}', *props, 'ENDPROPERTIES', f'CHARS {len(glyphs)}']
    for cp, (width, rows) in sorted(glyphs.items()):
        if width != WIDTH or len(rows) != HEIGHT or any(row < 0 or row >= 1 << WIDTH for row in rows):
            raise ValueError(f'Invalid output glyph U+{cp:04X}')
        lines += [f'STARTCHAR uni{cp:04X}', f'ENCODING {cp}', 'SWIDTH 462 0',
                  'DWIDTH 12 0', 'BBX 12 26 0 -5', 'BITMAP',
                  *(f'{row << 4:04X}' for row in rows), 'ENDCHAR']
    return ('\n'.join([*lines, 'ENDFONT']) + '\n').encode('ascii')


def compressed_bdf() -> bytes:
    data = bytearray(gzip.compress(generate_bdf(), compresslevel=9, mtime=0))
    data[9] = 255  # Python 3.11/3.12 otherwise expose zlib's platform OS byte.
    return bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    if args.check:
        # Compare the font payload: zlib versions may encode identical input
        # differently, without changing any glyph or metadata.
        if not OUTPUT.is_file() or gzip.decompress(OUTPUT.read_bytes()) != generate_bdf():
            raise SystemExit('Hairline BDF differs from its recipe; run python3 tools/gen_hairline.py after review')
        print('Hairline 12x26 matches its saved +2 / +3 / 26px design and terminal graphics')
    else:
        data = compressed_bdf()
        OUTPUT.parent.mkdir(parents=True, exist_ok=True)
        OUTPUT.write_bytes(data)
        print(f'{OUTPUT.relative_to(ROOT)}: {len(data)} bytes')


if __name__ == '__main__':
    main()
