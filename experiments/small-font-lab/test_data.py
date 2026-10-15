#!/usr/bin/env python3
"""Check exported real-renderer pixels against independent source placement.

The C renderer is compiled once for the suite. No installed fonts, GUI state,
preferences, or active terminals are used. Node, when available, also evaluates
the real sample/provenance functions without a browser.
"""
from __future__ import annotations

import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import unittest


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location('small_font_lab_under_test', HERE / 'build.py')
LAB = importlib.util.module_from_spec(spec)
spec.loader.exec_module(LAB)


def original_bdf(path, width, height):
    """Place individually decoded source bits using BDF's baseline coordinates."""
    text = gzip.decompress(path.read_bytes()).decode('latin-1')
    ascent = int(re.search(r'^FONT_ASCENT (\d+)$', text, re.M)[1])
    descent = int(re.search(r'^FONT_DESCENT (\d+)$', text, re.M)[1])
    glyphs, omitted = {}, set()
    for block in text.split('STARTCHAR ')[1:]:
        body = block.split('ENDCHAR', 1)[0]
        cp = int(re.search(r'^ENCODING (-?\d+)$', body, re.M)[1])
        advance = int(re.search(r'^DWIDTH (\d+) ', body, re.M)[1])
        if cp <= 32 or 0xE000 <= cp <= 0xF8FF or advance not in (width, 2 * width):
            continue
        bw, bh, left, bottom = map(int, re.search(r'^BBX (\d+) (\d+) (-?\d+) (-?\d+)$', body, re.M).groups())
        bitmap = body.split('BITMAP\n', 1)[1].splitlines()
        assert len(bitmap) == bh, (path, cp)
        pixels = set()
        for row, hexrow in enumerate(bitmap):
            bits = ''.join(f'{byte:08b}' for byte in bytes.fromhex(hexrow))
            for column in range(bw):
                if bits[column] == '1':
                    pixels.add((left + column, ascent - bottom - bh + row))
        if any(not (0 <= x < advance and 0 <= y < height) for x, y in pixels):
            omitted.add(cp)
            continue
        rows = [sum(1 << (advance - 1 - x) for x, yy in pixels if yy == y) for y in range(height)]
        glyphs[cp] = {'width': advance, 'rows': rows}
    return ascent, descent, glyphs, omitted


def unifont_sources():
    """Read original Unifont hex inputs, avoiding the packed-record decoder."""
    result = {}
    for path in (ROOT / 'font/unifont-18.0.01.hex.gz', ROOT / 'font/hardcore-extra.hex'):
        raw = gzip.decompress(path.read_bytes()).decode('ascii') if path.suffix == '.gz' else path.read_text()
        for line in raw.splitlines():
            if not line or line.startswith('#'):
                continue
            cp, hexdata = line.split(':')
            cp = int(cp, 16)
            if cp in LAB.SELECTED or cp == 0xFFFD:
                data = bytes.fromhex(hexdata)
                width = len(data) // 2
                stride = width // 8
                result[cp] = (width, [int.from_bytes(data[y * stride:(y + 1) * stride], 'big') for y in range(16)])
    return result


def fit_unifont(record, width, height):
    source_width, source_rows = record
    target_width = width if source_width == 8 else 2 * width
    rows = []
    for y in range(height):
        source = source_rows[y * 16 // height]
        row = 0
        for x in range(target_width):
            source_x = x * source_width // target_width
            if source & (1 << (source_width - 1 - source_x)):
                row |= 1 << (target_width - 1 - x)
        rows.append(row)
    return {'width': target_width, 'rows': rows}


def edge(glyph, side):
    width, rows = glyph['width'], glyph['rows']
    if side in ('u', 'd'):
        row = rows[0 if side == 'u' else -1]
        return {x for x in range(width) if row & (1 << (width - 1 - x))}
    mask = 1 << (width - 1) if side == 'l' else 1
    return {y for y, row in enumerate(rows) if row & mask}


def node_path():
    installed = shutil.which('node') or shutil.which('nodejs')
    if installed:
        return installed
    return next((str(p) for p in sorted((Path.home() / '.nvm/versions/node').glob('*/bin/node'), reverse=True)), None)


class SmallFontDataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.metadata_before = (HERE / 'candidates.json').read_bytes()
        specs = json.loads(cls.metadata_before)
        cls.source_paths = {ROOT / spec[field] for spec in specs for field in ('path', 'license_path')}
        cls.before = {p: p.read_bytes() for p in cls.source_paths}
        cls.data = LAB.build_data()  # One C compilation for all tests.
        cls.fonts = cls.data['fonts']
        cls.native = {f['id']: original_bdf(ROOT / f['path'], f['width'], f['height']) for f in cls.fonts}
        cls.unifont = unifont_sources()
        cls.javascript = None
        node = node_path()
        if node:
            html = (HERE / 'lab.html').read_text()
            samples = html[html.index('const pad='):html.index('function glyph(')]
            origin = re.search(r'function origin\(f,cp\)\{[^\n]+\}', html)[0]
            reduced = [{'id': f['id'], 'nativeCodepoints': f['nativeCodepoints'], 'generated': f['generated']} for f in cls.fonts]
            script = samples + '\n' + origin + '\n' + 'const state={boxes:false};\n'
            script += 'const fonts=' + json.dumps(reduced) + ';\nconst cps=' + json.dumps(LAB.SELECTED) + ';\n'
            script += '''
const result={samples:Object.fromEntries(['terminal','boxes','text'].map(k=>[k,sampleLines(k)])),origins:{}};
for(const enabled of [false,true]){state.boxes=enabled;result.origins[enabled]=Object.fromEntries(fonts.map(f=>[f.id,Object.fromEntries(cps.map(cp=>[cp,origin(f,cp)]))]));}
console.log(JSON.stringify(result));
'''
            output = subprocess.check_output([node], input=script.encode(), timeout=15)
            cls.javascript = json.loads(output)

    def test_all_eleven_sources_and_licenses_remain_byte_identical(self):
        self.assertEqual(len(self.fonts), 11)
        self.assertEqual(len({f['id'] for f in self.fonts}), 11)
        self.assertEqual((HERE / 'candidates.json').read_bytes(), self.metadata_before)
        self.assertTrue(all(p.is_file() for p in self.source_paths))
        for path, before in self.before.items():
            self.assertEqual(path.read_bytes(), before, str(path))
        for font in self.fonts:
            raw = gzip.decompress((ROOT / font['path']).read_bytes())
            license_bytes = (ROOT / font['license_path']).read_bytes()
            with self.subTest(font=font['id']):
                self.assertEqual(hashlib.sha256(raw).hexdigest(), font['sha256'])
                self.assertEqual(hashlib.sha256(license_bytes).hexdigest(), font['license_sha256'])
                self.assertEqual(font['licenseText'].splitlines(), license_bytes.decode().splitlines())

    def test_native_pixels_baseline_metrics_and_codepoints_match_original_bdfs(self):
        for font in self.fonts:
            ascent, descent, source, omitted = self.native[font['id']]
            with self.subTest(font=font['id']):
                self.assertEqual(font['ascent'], ascent)
                self.assertEqual(font['height'], ascent + descent)
                self.assertEqual(set(font['nativeCodepoints']), set(LAB.SELECTED) & source.keys())
                self.assertEqual(set(font['omitted']), omitted)
                self.assertEqual(set(map(int, font['sourceGlyphs'])), set(LAB.SELECTED))
                self.assertEqual(set(map(int, font['completedGlyphs'])), set(LAB.SELECTED))
            for cp in set(LAB.SELECTED) & source.keys():
                with self.subTest(font=font['id'], cp=f'U+{cp:04X}'):
                    self.assertEqual(font['sourceGlyphs'][str(cp)], source[cp])
                    self.assertEqual(font['completedGlyphs'][str(cp)], source[cp], 'completion must preserve native glyphs')

    def test_reported_stems_and_cap_heights_are_actual_pixels(self):
        for font in self.fonts:
            expected = 2 if font['id'] == 'fixed20' else 1
            glyphs = font['sourceGlyphs']
            with self.subTest(font=font['id']):
                body = [row for row in glyphs[str(ord('I'))]['rows'] if row]
                self.assertEqual(body[len(body) // 2].bit_count(), expected)
                self.assertEqual(font['iStemPixels'], expected)
                self.assertEqual(font['stem_px'], expected)
                # Bottom body rows of H/n contain two separate upright stems.
                for ch in 'Hn':
                    row = next(row for row in reversed(glyphs[str(ord(ch))]['rows']) if row)
                    runs = re.findall('1+', format(row, f'0{font["width"]}b'))
                    self.assertEqual([len(run) for run in runs], [expected, expected])
                cap = sum(bool(row) for row in glyphs[str(ord('H'))]['rows'])
                self.assertEqual(font['capHeight'], cap)
                self.assertEqual(font['cap_height'], cap)
                self.assertEqual(font['xHeight'], sum(bool(row) for row in glyphs[str(ord('x'))]['rows']))

    def test_missing_glyph_pixels_are_the_real_unifont_fallback(self):
        checked = 0
        for font in self.fonts:
            native = self.native[font['id']][2]
            for cp in set(LAB.SELECTED) - native.keys():
                expected = fit_unifont(self.unifont.get(cp, self.unifont[0xFFFD]), font['width'], font['height'])
                with self.subTest(font=font['id'], cp=f'U+{cp:04X}'):
                    self.assertEqual(font['sourceGlyphs'][str(cp)], expected)
                    if cp not in font['generated']:
                        self.assertEqual(font['completedGlyphs'][str(cp)], expected)
                checked += 1
        self.assertGreater(checked, 300)
        self.assertEqual(self.data['unifontLicense'], (ROOT / 'font/unifont-license.txt').read_text())

    def test_completion_only_adds_missing_boxes_and_preserves_native_centers(self):
        for font in self.fonts:
            native = self.native[font['id']][2]
            missing = set(range(0x2500, 0x2580)) - native.keys()
            with self.subTest(font=font['id']):
                generated = set(font['generated'])
                generation_fallback = set(font['generationFallback'])
                self.assertEqual(generated | generation_fallback, missing)
                self.assertTrue(generated.isdisjoint(generation_fallback))
                # The unchanged production generator cannot draw four dashes
                # at width8. These two must retain useful Unifont pixels.
                self.assertEqual(generation_fallback, {0x2508, 0x2509} if font['id'] == 'tamzen16' else set())
                self.assertEqual(font['nativeBoxes'], 128 - len(missing))
                self.assertEqual(font['nativeBoxes'], font['native_box_count'])
                self.assertEqual({f'U+{cp:04X}' for cp in missing}, set(font['missing_boxes']))
                self.assertEqual(font['native_block_count'], sum(cp in native for cp in range(0x2580, 0x25A0)))
                if font['box_center'] is not None:
                    self.assertEqual(font['center'], font['box_center'])
                    cx, cy = font['center']
                    self.assertEqual(edge(native[0x2502], 'u'), {cx})
                    self.assertEqual(edge(native[0x2500], 'l'), {cy})
            for cp in LAB.SELECTED:
                if cp not in missing:
                    self.assertEqual(font['completedGlyphs'][str(cp)], font['sourceGlyphs'][str(cp)], (font['id'], cp))
            for cp in missing:
                rendered = font['completedGlyphs'][str(cp)]
                self.assertEqual(rendered['width'], font['width'])
                self.assertEqual(len(rendered['rows']), font['height'])
                self.assertTrue(any(rendered['rows']), (font['id'], cp))
                self.assertTrue(all(0 <= row < 1 << font['width'] for row in rendered['rows']))
                if cp in generation_fallback:
                    self.assertEqual(rendered, fit_unifont(self.unifont[cp], font['width'], font['height']))
                    self.assertEqual(rendered, font['sourceGlyphs'][str(cp)])

    def test_completed_box_boundaries_join_light_heavy_double_and_round_corners(self):
        # Each group: horizontal, vertical, top-left, top-right, bottom-left,
        # bottom-right, optional cross. Check actual adjacent boundary pixels.
        groups = ['─│┌┐└┘┼', '━┃┏┓┗┛╋', '═║╔╗╚╝╬', '─│╭╮╰╯']
        for font in self.fonts:
            glyphs = font['completedGlyphs']
            get = lambda ch: glyphs[str(ord(ch))]
            for chars in groups:
                horizontal, vertical, tl, tr, bl, br, *cross = chars
                joins = [(tl, 'r', horizontal, 'l'), (horizontal, 'r', tr, 'l'),
                         (bl, 'r', horizontal, 'l'), (horizontal, 'r', br, 'l'),
                         (tl, 'd', vertical, 'u'), (vertical, 'd', bl, 'u'),
                         (tr, 'd', vertical, 'u'), (vertical, 'd', br, 'u')]
                if cross:
                    joins += [(horizontal, 'r', cross[0], 'l'), (cross[0], 'r', horizontal, 'l'),
                              (vertical, 'd', cross[0], 'u'), (cross[0], 'd', vertical, 'u')]
                for a, side_a, b, side_b in joins:
                    with self.subTest(font=font['id'], join=a + b, sides=side_a + side_b):
                        self.assertTrue(edge(get(a), side_a), 'joining edge must have ink')
                        self.assertEqual(edge(get(a), side_a), edge(get(b), side_b))

    def test_ui_provenance_labels_are_true_for_both_modes(self):
        if self.javascript is None:
            self.skipTest('Node is needed for the template provenance functions')
        for enabled in (False, True):
            mode = str(enabled).lower()
            for font in self.fonts:
                native = self.native[font['id']][2]
                for cp in LAB.SELECTED:
                    if cp == 32:
                        expected = 'space'
                    elif cp in native:
                        expected = 'native'
                    elif enabled and 0x2500 <= cp < 0x2580 and cp not in font['generationFallback']:
                        expected = 'generated'
                    else:
                        expected = 'fallback'
                    self.assertEqual(self.javascript['origins'][mode][font['id']][str(cp)], expected, (mode, font['id'], cp))

    def test_sample_text_fits_cells_and_tui_dividers_join_borders(self):
        if self.javascript is None:
            self.skipTest('Node is needed for the real JavaScript sample strings')
        samples = self.javascript['samples']
        for kind, lines in samples.items():
            for line, _ in lines:
                self.assertLessEqual(len(line), 48, (kind, line))
                self.assertTrue(set(map(ord, line)) <= set(LAB.SELECTED), (kind, line))
        terminal = [line for line, _ in samples['terminal']]
        header_index = next(i for i, line in enumerate(terminal) if 'FILES' in line)
        column = terminal[header_index].index('│', 1)
        self.assertEqual(terminal[header_index - 1][column], '┬')
        self.assertEqual(terminal[header_index + 1][column], '┼')
        bottom = next(line for line in terminal[header_index + 2:] if line.startswith('└'))
        self.assertEqual(bottom[column], '┴')
        self.assertNotIn('native', samples['boxes'][0][0].lower(), 'Tamzen boxes are generated/fallback, not native')


if __name__ == '__main__':
    unittest.main(verbosity=2)
