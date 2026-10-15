#!/usr/bin/env python3
"""Validate the shipped Hairline bitmap and its real production rendering path.

Ordinary checks only read the committed BDF. Regeneration checks use Node when
available; compiling/running Hardcore or changing installed fonts is unnecessary.
"""
import gzip
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from test_courier_fonts import GEN, HARNESS, ROOT, module


ALNUM = {ord(ch) for ch in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'}


def components(rows, width=12):
    pixels = {(x, y) for y, row in enumerate(rows) for x in range(width)
              if row & (1 << (width - 1 - x))}
    count = 0
    while pixels:
        count += 1
        pending = [pixels.pop()]
        while pending:
            x, y = pending.pop()
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    adjacent = x + dx, y + dy
                    if adjacent in pixels:
                        pixels.remove(adjacent)
                        pending.append(adjacent)
    return count


def solid_blocks(rows):
    return sum(((a & b) & ((a & b) << 1)).bit_count() for a, b in zip(rows, rows[1:]))


H_MAIN = r'''
int main(int argc, const char **argv) {
    require(argc == 3, "table paths");
    size_t length;
    fonts_blob = read_blob(argv[1], &length);
    font_blob = read_blob(argv[2], &length); font_blob_end = font_blob + length;
    const Font *f = font_by_key('H');
    require(f && !strcmp(f->name, "Hairline"), "Hairline is selectable by its own H config key");
    require(f->w == 12 && f->h == 26 && f->dev, "Hairline uses its native 12x26 device pixel cell");
    for (g_scale = 1; g_scale <= 3; g_scale++)
        require(cell_w(f) == 12 && cell_h(f) == 26, "display scale does not thicken Hairline");
    struct { unsigned char before[32], pixels[SLOT * SLOT], after[32]; } guarded;
    const uint16_t expected[2][26] = {/* EXPECTED_ROWS */};
    const uint32_t codepoints[2] = {'I', 'f'};
    for (int ch = 0; ch < 2; ch++) {
        memset(&guarded, 0xA5, sizeof(guarded));
        memset(guarded.pixels, 0, sizeof(guarded.pixels));
        int width = glyph_bits(f, codepoints[ch], guarded.pixels);
        require(width == 12, "native glyph keeps cell width");
        for (int y = 0; y < SLOT; y++) {
            for (int x = 0; x < SLOT; x++) {
                int lit = y < 26 && x < 12 && (expected[ch][y] & (1 << (11 - x)));
                require(guarded.pixels[y * SLOT + x] == (lit ? 255 : 0), "production renderer matches native I/f pixels");
            }
        }
        for (int n = 0; n < 32; n++) require(guarded.before[n] == 0xA5 && guarded.after[n] == 0xA5, "glyph buffer canaries");
    }
    free((void *)fonts_blob); free((void *)font_blob);
    return 0;
}
'''


class HairlineFontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with mock.patch.dict(sys.modules, {'gen_fonts': GEN}):
            cls.generator = module('hairline_generator_tests', ROOT / 'tools/gen_hairline.py')
        cls.spec = next(spec for spec in GEN.FONTS if spec[0] == 'H')
        cls.raw = gzip.decompress((ROOT / cls.spec[2]).read_bytes())
        cls.text = cls.raw.decode('ascii')
        cls.omitted = []
        cls.glyphs = GEN.read_bdf(ROOT / cls.spec[2], 12, 26, omit_overhang=True, omitted=cls.omitted)
        cls.source = GEN.read_bdf(ROOT / 'font/terminus/ter-u24n.bdf.gz', 12, 24, omit_overhang=True)

    def test_named_metrics_and_registration(self):
        self.assertEqual(self.spec[1], 'Hairline')
        self.assertEqual(self.spec[3:], (12, 26, 1, 0, None, None))
        for declaration in ('FAMILY_NAME "Hairline"', 'FONT_ASCENT 21', 'FONT_DESCENT 5', 'CAP_HEIGHT 18', 'X_HEIGHT 13'):
            self.assertRegex(self.text, '(?m)^' + re.escape(declaration) + '$')
        name = re.search(r'^FONT (.+)$', self.text, re.MULTILINE)[1]
        self.assertIn('hairline', name.lower())
        self.assertNotIn('terminus', name.lower(), 'derived font needs its own family name')
        self.assertIn('font/hairline/OFL.txt', [path for _, path in GEN.FONT_NOTICES])

    def test_full_source_coverage_and_no_pixel_clipping(self):
        self.assertGreater(len(self.source), 1300)
        self.assertTrue(self.source.keys() <= self.glyphs.keys())
        self.assertFalse(self.omitted, 'shipped Hairline pixels must fit its actual cell')
        self.assertTrue(set(range(33, 127)) <= self.glyphs.keys())
        for cp, (width, rows) in self.glyphs.items():
            self.assertEqual(width, 12, cp)
            self.assertEqual(len(rows), 26, cp)
            self.assertTrue(all(0 <= row < 4096 for row in rows), cp)

    def test_cap_x_height_and_f_hook_stay_hairline(self):
        capital = self.glyphs[ord('I')][1]
        self.assertEqual(next(y for y, row in enumerate(capital) if row), 3)
        self.assertEqual(capital[3], self.source[ord('I')][1][4])
        self.assertEqual(capital[20], self.source[ord('I')][1][18])
        self.assertEqual(sum(row.bit_count() > 1 for row in capital), 2)
        self.assertTrue(all(row.bit_count() == 1 for row in capital[4:20]))
        self.assertFalse(any(capital[:3] + capital[21:]))
        for ch in 'acegmnopqrsuvwxyz':
            self.assertEqual(next(y for y, row in enumerate(self.glyphs[ord(ch)][1]) if row), 8, ch)
        # +2 cell leading and +2 lowercase growth move f's whole hook together,
        # preserving its original top six rows instead of compressing its cap.
        f = self.glyphs[ord('f')][1]
        self.assertEqual(f[4:10], self.source[ord('f')][1][4:10])
        self.assertEqual(components(f), components(self.source[ord('f')][1]))

    def test_alphanumerics_keep_descenders_connections_and_stroke_weight(self):
        for cp in sorted(ALNUM):
            old, new = self.source[cp][1], self.glyphs[cp][1]
            with self.subTest(character=chr(cp)):
                self.assertEqual(new[19:], old[17:])
                self.assertEqual(components(new), components(old))
                self.assertLessEqual(solid_blocks(new), solid_blocks(old))

    def test_other_characters_keep_their_original_pixels(self):
        for cp, (width, rows) in self.source.items():
            if cp not in ALNUM and not 0x2500 <= cp <= 0x259F:
                self.assertEqual(self.glyphs[cp], (width, [0, 0] + rows), f'U+{cp:04X}')

    def test_box_lines_join_and_blocks_fill_the_26_pixel_cell(self):
        self.assertTrue(set(range(0x2500, 0x25A0)) <= self.glyphs.keys())
        horizontal = self.glyphs[0x2500][1]
        vertical = self.glyphs[0x2502][1]
        self.assertEqual(sum(bool(row) for row in horizontal), 1)
        self.assertEqual(max(horizontal), 4095)
        self.assertTrue(all(row == vertical[0] for row in vertical))
        self.assertEqual(vertical[0].bit_count(), 1)
        self.assertEqual(self.glyphs[0x2588][1], [4095] * 26)
        upper, lower = self.glyphs[0x2580][1], self.glyphs[0x2584][1]
        self.assertEqual(upper, [4095] * 13 + [0] * 13)
        self.assertEqual(lower, [0] * 13 + [4095] * 13)
        quadrants = {
            0x2596: [0] * 13 + [0xFC0] * 13,
            0x2597: [0] * 13 + [0x03F] * 13,
            0x2598: [0xFC0] * 13 + [0] * 13,
            0x259D: [0x03F] * 13 + [0] * 13,
        }
        for cp, expected in quadrants.items():
            self.assertEqual(self.glyphs[cp][1], expected)
        for cp in (0x2591, 0x2592, 0x2593):
            expected = [self.source[cp][1][y % 2] for y in range(26)]
            self.assertEqual(self.glyphs[cp][1], expected)
            # Two neighboring cells continue the same phase across the seam.
            tiled = self.glyphs[cp][1] * 2
            self.assertEqual(tiled, [expected[y % 2] for y in range(52)])

    @unittest.skipUnless(shutil.which('node'), 'Node is only needed to verify explicit font regeneration')
    def test_committed_bdf_payload_exactly_matches_the_exported_recipe(self):
        generated = self.generator.build_font()
        for cp, expected in generated.items():
            if cp > 32 and not 0xE000 <= cp <= 0xF8FF:
                self.assertEqual(self.glyphs[cp], expected, cp)
        self.assertEqual(self.generator.generate_bdf(), self.raw)
        # zlib versions can encode identical payloads differently. The source
        # bitmap payload, not a particular deflate stream, is the invariant.
        self.assertEqual(gzip.decompress(self.generator.compressed_bdf()), self.raw)
        self.assertEqual(gzip.decompress(self.generator.OUTPUT.read_bytes()), self.raw)
        checked = subprocess.run([sys.executable, str(ROOT / 'tools/gen_hairline.py'), '--check'],
                                 cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stderr.decode())

    @unittest.skipUnless(shutil.which('clang') or shutil.which('cc'), 'C compiler required for headless renderer check')
    def test_production_key_pixels_and_device_scale(self):
        source = (ROOT / 'hardcore.c').read_text()
        font_struct = re.search(r'typedef struct \{\n    char key;.*?\n\} Font;', source, re.DOTALL).group(0)
        functions = []
        for name in ('font_record', 'font_by_key', 'glyph_bits'):
            functions.append(re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\) \{.*?^\}', source, re.MULTILINE | re.DOTALL).group(0))
        for name in ('cell_w', 'cell_h'):
            functions.append(re.search(r'^static int ' + name + r'\([^\n]+\n', source, re.MULTILINE).group(0))
        slot = re.search(r'^#define SLOT \d+', source, re.MULTILINE).group(0)
        expected = ','.join('{' + ','.join(map(str, self.glyphs[ord(ch)][1])) + '}' for ch in 'If')
        harness = HARNESS.split('int main(', 1)[0] + H_MAIN.replace('/* EXPECTED_ROWS */', expected)
        with tempfile.TemporaryDirectory(prefix='hc-hairline-renderer-') as directory:
            directory = Path(directory)
            subprocess.run([sys.executable, str(ROOT / 'tools/gen_fonts.py'), str(directory / 'fonts.bin'), str(directory / 'fonts.inc')],
                           cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, check=True, timeout=30)
            (directory / 'fallback.bin').write_bytes(GEN.table({0xFFFD: (8, [0xFF] * 16)}))
            harness = harness.replace('/* FONT_STRUCT */', font_struct).replace('/* FONT_LIST */', (directory / 'fonts.inc').read_text()).replace('/* SLOT_DEFINE */', slot).replace('/* FUNCTIONS */', '\n'.join(functions))
            (directory / 'renderer.c').write_text(harness)
            built = subprocess.run([shutil.which('clang') or shutil.which('cc'), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', str(directory / 'renderer.c'), '-o', str(directory / 'renderer')],
                                   stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stderr.decode())
            ran = subprocess.run([str(directory / 'renderer'), str(directory / 'fonts.bin'), str(directory / 'fallback.bin')],
                                 stdin=subprocess.DEVNULL, capture_output=True, timeout=5)
            self.assertEqual(ran.returncode, 0, ran.stderr.decode())


if __name__ == '__main__':
    unittest.main()
