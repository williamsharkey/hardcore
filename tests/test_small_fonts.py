#!/usr/bin/env python3
"""Production small-font fidelity, licenses, box joins, and device-pixel rendering.

Checks every native packed glyph against an independent BDF pixel decoder and
compiles the real C lookup/raster functions against the generated production
font list. All eleven approved lab candidates, including the heavier Fixed20
control and existing Terminus24 reference, must render exactly like the demo.
"""
import gzip
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


GEN = module('small_production_gen', ROOT / 'tools/gen_fonts.py')
LAB = module('small_production_lab', ROOT / 'experiments/small-font-lab/build.py')
REFERENCE = module('small_production_reference', ROOT / 'experiments/small-font-lab/test_data.py')
KEYS = {
    'terminus16': 'a', 'terminus18': 'b', 'terminus20': 'c',
    'terminus22': 'd', 'terminus24': 't', 'tamzen16': 'z',
    'tamzen20': 'x', 'fixed18': 'f', 'fixed20': 'g',
    'ttyp017': 'p', 'ttyp018': 'q',
}


def unpack_record(blob, cp, height):
    page = struct.unpack_from('<I', blob, 4 + (cp >> 8) * 4)[0]
    if not page:
        return None
    offset = struct.unpack_from('<I', blob, page + (cp & 255) * 4)[0]
    if not offset:
        return None
    width = blob[offset]
    stride = (width + 7) // 8
    return width, [int.from_bytes(blob[offset + 1 + y * stride:offset + 1 + (y + 1) * stride], 'big')
                   >> (8 * stride - width) for y in range(height)]


def production_renderer(directory, font_list):
    source = (ROOT / 'hardcore.c').read_text()
    font_struct = re.search(r'typedef struct \{\n    char key;.*?\n\} Font;', source, re.S)[0]
    functions = []
    for name in ('font_record', 'font_by_key', 'glyph_bits'):
        functions.append(re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\) \{.*?^\}', source, re.M | re.S)[0])
    for name in ('cell_w', 'cell_h'):
        functions.append(re.search(r'^static int ' + name + r'\([^\n]+\n', source, re.M)[0])
    slot = re.search(r'^#define SLOT \d+', source, re.M)[0]
    harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
FONT_STRUCT
static const Font FONTS[] = {FONT_LIST};
#define NFONTS ((int)(sizeof(FONTS) / sizeof(FONTS[0])))
SLOT_DEFINE
static int g_scale;
static const unsigned char *fonts_blob, *font_blob, *font_blob_end;
FUNCTIONS
static void require(int yes, const char *message) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static unsigned char *read_blob(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb"); require(f != NULL, "open table");
    require(fseek(f, 0, SEEK_END) == 0, "table seek");
    long length = ftell(f); require(length > 0, "table length"); rewind(f);
    unsigned char *data = malloc((size_t)length);
    require(data != NULL && fread(data, 1, (size_t)length, f) == (size_t)length, "read table");
    fclose(f); *size = (size_t)length; return data;
}
int main(int argc, const char **argv) {
    require(argc == 3, "table paths");
    size_t size;
    fonts_blob = read_blob(argv[1], &size);
    font_blob = read_blob(argv[2], &size); font_blob_end = font_blob + size;
    const char keys[] = "FONT_KEYS";
    const uint32_t cps[] = {CODEPOINTS};
    putchar('{');
    for (size_t i = 0; i < sizeof(keys) - 1; i++) {
        const Font *f = font_by_key(keys[i]);
        require(f && f->key == keys[i], "stable font config key lookup");
        require(f->dev && !f->thin, "native device pixels without synthetic thinning");
        for (g_scale = 1; g_scale <= 3; g_scale++)
            require(cell_w(f) == f->w && cell_h(f) == f->h, "cell dimensions stay native at each display scale");
        if (i) putchar(',');
        printf("\"%c\":{", keys[i]);
        for (size_t j = 0; j < sizeof(cps) / sizeof(cps[0]); j++) {
            struct { unsigned char before[32], pixels[SLOT * SLOT], after[32]; } guarded;
            memset(&guarded, 0xA5, sizeof(guarded));
            memset(guarded.pixels, 0, sizeof(guarded.pixels));
            int width = glyph_bits(f, cps[j], guarded.pixels);
            require(width > 0 && width <= 2 * f->w && width <= SLOT, "one/two-cell width within glyph buffer");
            for (int n = 0; n < 32; n++)
                require(guarded.before[n] == 0xA5 && guarded.after[n] == 0xA5, "glyph buffer canaries");
            for (int y = 0; y < SLOT; y++)
                for (int x = 0; x < SLOT; x++)
                    if (y >= f->h || x >= width)
                        require(guarded.pixels[y * SLOT + x] == 0, "no ink outside the cell");
            if (j) putchar(',');
            printf("\"%u\":{\"width\":%d,\"rows\":[", cps[j], width);
            for (int y = 0; y < f->h; y++) {
                unsigned row = 0;
                for (int x = 0; x < width; x++) row = (row << 1) | (guarded.pixels[y * SLOT + x] != 0);
                if (y) putchar(',');
                printf("%u", row);
            }
            printf("]}");
        }
        putchar('}');
    }
    puts("}"); free((void *)fonts_blob); free((void *)font_blob); return 0;
}
'''
    for key, value in {'FONT_STRUCT': font_struct, 'FONT_LIST': font_list,
                       'SLOT_DEFINE': slot, 'FUNCTIONS': '\n'.join(functions),
                       'FONT_KEYS': ''.join(KEYS.values()),
                       'CODEPOINTS': ','.join(map(str, LAB.SELECTED + [0x754C, 0x1F600]))}.items():
        harness = harness.replace(key, value)
    (directory / 'renderer.c').write_text(harness)
    compiler = shutil.which('clang') or shutil.which('cc')
    subprocess.run([compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    str(directory / 'renderer.c'), '-o', str(directory / 'renderer')],
                   capture_output=True, check=True, timeout=30)
    output = subprocess.check_output([str(directory / 'renderer'), str(directory / 'fonts.bin'),
                                      str(ROOT / 'build/font.bin')], timeout=10)
    return json.loads(output)


class SmallProductionFontTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fonts = {f['id']: f for f in LAB.build_data()['fonts']}
        cls.specs = {f[0]: f for f in GEN.FONTS}
        cls.native = {}
        for name, key in KEYS.items():
            spec = cls.specs[key]
            cls.native[name] = REFERENCE.original_bdf(ROOT / spec[2], spec[3], spec[4])
        with tempfile.TemporaryDirectory(prefix='hc-small-production-') as temp:
            directory = Path(temp)
            subprocess.run([sys.executable, str(ROOT / 'tools/gen_fonts.py'),
                            str(directory / 'fonts.bin'), str(directory / 'fonts.inc')],
                           cwd=ROOT, capture_output=True, check=True, timeout=30)
            cls.blob = (directory / 'fonts.bin').read_bytes()
            font_list = (directory / 'fonts.inc').read_text()
            cls.offsets = {m[0]: int(m[1]) for m in re.findall(
                r"\{'(.)', [^\n]+, (-?\d+)\},", font_list)}
            cls.rasters = production_renderer(directory, font_list)

    def test_all_candidates_have_stable_keys_native_metrics_and_production_sources(self):
        self.assertEqual(len(self.fonts), 11)
        self.assertEqual(set(self.fonts), set(KEYS))
        self.assertEqual(len(self.specs), len(GEN.FONTS))
        self.assertEqual([s[0] for s in GEN.FONTS[:11]], list('UZFtYEMTCRH'))
        for name, key in KEYS.items():
            font, spec = self.fonts[name], self.specs[key]
            with self.subTest(font=name):
                self.assertEqual((spec[3], spec[4], spec[5], spec[6]), (font['width'], font['height'], 1, 0))
                self.assertTrue(spec[2].startswith('font/'))
                raw = gzip.decompress((ROOT / spec[2]).read_bytes())
                self.assertEqual(hashlib.sha256(raw).hexdigest(), font['sha256'])
                self.assertEqual(self.native[name][:2], (font['ascent'], font['height'] - font['ascent']))

    def test_every_native_glyph_survives_packing_without_pixel_or_encoding_changes(self):
        for name, key in KEYS.items():
            spec = self.specs[key]
            blob = self.blob[self.offsets[key]:]
            _, _, native, omitted = self.native[name]
            self.assertFalse(omitted, name)
            for cp, expected in native.items():
                self.assertEqual(unpack_record(blob, cp, spec[4]), (expected['width'], expected['rows']), (name, cp))

    def test_actual_renderer_matches_every_approved_demo_pixel(self):
        for name, key in KEYS.items():
            expected = self.fonts[name]['completedGlyphs']
            for cp in LAB.SELECTED:
                self.assertEqual(self.rasters[key][str(cp)], expected[str(cp)], (name, cp))

    def test_original_stroke_weight_including_heavier_fixed_control(self):
        for name, key in KEYS.items():
            font = self.rasters[key]
            expected = 2 if name == 'fixed20' else 1
            body = [row for row in font[str(ord('I'))]['rows'] if row]
            self.assertEqual(body[len(body) // 2].bit_count(), expected, name)
            for ch in 'Hn':
                row = next(row for row in reversed(font[str(ord(ch))]['rows']) if row)
                self.assertEqual([len(run) for run in re.findall('1+', bin(row)[2:])], [expected, expected], (name, ch))

    def test_box_completion_keeps_tamzen_narrow_dash_fallback_and_uw_designs(self):
        for name, key in KEYS.items():
            font, spec = self.fonts[name], self.specs[key]
            blob = self.blob[self.offsets[key]:]
            native = self.native[name][2]
            generated = set(font['generated'])
            for cp in range(0x2500, 0x2580):
                record = unpack_record(blob, cp, spec[4])
                if cp in native or cp in generated:
                    self.assertIsNotNone(record, (name, cp))
                    self.assertTrue(any(record[1]), (name, cp))
                else:
                    self.assertEqual(name, 'tamzen16')
                    self.assertIn(cp, (0x2508, 0x2509))
                    self.assertIsNone(record)
                    self.assertTrue(any(self.rasters[key][str(cp)]['rows']))
            if name.startswith('ttyp0'):
                self.assertIsNone(spec[8], 'UW ttyp0 must not add or alter glyph designs')
                self.assertFalse(generated)
                self.assertEqual(len(native), font['renderable_glyph_count'])
        for key in KEYS.values():
            for chars in ('─│┌┐└┘┼', '━┃┏┓┗┛╋', '═║╔╗╚╝╬', '─│╭╮╰╯'):
                h, v, tl, tr, bl, br, *cross = chars
                joins = [(tl, 'r', h, 'l'), (h, 'r', tr, 'l'), (bl, 'r', h, 'l'), (h, 'r', br, 'l'),
                         (tl, 'd', v, 'u'), (v, 'd', bl, 'u'), (tr, 'd', v, 'u'), (v, 'd', br, 'u')]
                if cross:
                    joins += [(h, 'r', cross[0], 'l'), (cross[0], 'r', h, 'l'),
                              (v, 'd', cross[0], 'u'), (cross[0], 'd', v, 'u')]
                for a, a_side, b, b_side in joins:
                    a_edge = REFERENCE.edge(self.rasters[key][str(ord(a))], a_side)
                    b_edge = REFERENCE.edge(self.rasters[key][str(ord(b))], b_side)
                    self.assertTrue(a_edge, (key, a))
                    self.assertEqual(a_edge, b_edge, (key, a, b))

    def test_binary_contains_complete_font_copyright_and_permission_notices(self):
        notices = {path for _, path in GEN.FONT_NOTICES}
        for family, name in [('terminus', 'OFL.TXT'), ('tamzen', 'LICENSE'),
                             ('misc-fixed', 'COPYING'), ('uw-ttyp0', 'LICENSE')]:
            path = f'font/{family}/{name}'
            self.assertIn(path, notices)
            self.assertIn((ROOT / path).read_text().encode('utf-8'), self.blob)


if __name__ == '__main__':
    unittest.main()
