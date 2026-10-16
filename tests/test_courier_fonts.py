#!/usr/bin/env python3
"""Courier's native pixels, packed records and real headless renderer paths.

No GUI, installed fonts, user preferences or running terminal sessions are used.
The C harness compiles the production cell/font lookup and glyph_bits functions
against generated tables, including the 40-pixel-wide Unicode fallback.
"""
import gzip
import importlib.util
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


GEN = module('courier_gen_fonts', ROOT / 'tools/gen_fonts.py')
LAB = module('courier_lab_data', ROOT / 'experiments/font-lab/build.py')
COURIER = [spec for spec in GEN.FONTS if spec[0] in ('C', 'R')]


def native(spec, omitted=None):
    return GEN.read_bdf(ROOT / spec[2], spec[3], spec[4], omit_overhang=True, omitted=omitted)


def unpack_record(blob, cp, height):
    page = struct.unpack_from('<I', blob, 4 + (cp >> 8) * 4)[0]
    if not page:
        return None
    offset = struct.unpack_from('<I', blob, page + (cp & 255) * 4)[0]
    if not offset:
        return None
    width = blob[offset]
    bpr = (width + 7) // 8
    rows = [int.from_bytes(blob[offset + 1 + y * bpr:offset + 1 + (y + 1) * bpr], 'big') >> (8 * bpr - width)
            for y in range(height)]
    return width, rows


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* FONT_STRUCT */
static const Font FONTS[] = {
/* FONT_LIST */
};
#define NFONTS ((int)(sizeof(FONTS) / sizeof(FONTS[0])))
/* SLOT_DEFINE */
static int g_scale;
static const unsigned char *fonts_blob, *font_blob, *font_blob_end;
/* FUNCTIONS */
static void require(int yes, const char *message) {
    if (!yes) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static const unsigned char *read_blob(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    require(f != NULL, "open table");
    require(fseek(f, 0, SEEK_END) == 0, "table seek");
    long length = ftell(f);
    require(length > 0, "table length");
    rewind(f);
    unsigned char *data = malloc((size_t)length);
    require(data != NULL && fread(data, 1, (size_t)length, f) == (size_t)length, "read table");
    fclose(f); *size = (size_t)length; return data;
}
int main(int argc, const char **argv) {
    require(argc == 3, "table paths");
    size_t length;
    fonts_blob = read_blob(argv[1], &length);
    font_blob = read_blob(argv[2], &length); font_blob_end = font_blob + length;
    require(SLOT >= 40, "20-pixel font needs a 40-pixel wide fallback slot");
    for (int i = 0; i < 2; i++) {
        const Font *f = font_by_key(i ? 'R' : 'C');
        require(f != NULL, "font config key lookup");
        for (g_scale = 1; g_scale <= 3; g_scale++)
            require(cell_w(f) == (i ? 20 : 15) && cell_h(f) == (i ? 29 : 24), "native device pixels at every display scale");
        struct { unsigned char before[32], pixels[SLOT * SLOT], after[32]; } guarded;
        memset(&guarded, 0xA5, sizeof(guarded));
        memset(guarded.pixels, 0, sizeof(guarded.pixels));
        int width = glyph_bits(f, 'I', guarded.pixels);
        require(width == f->w, "native width");
        int ink = 0;
        for (int x = 0; x < width; x++) ink += guarded.pixels[(f->h / 2) * SLOT + x] != 0;
        require(ink == 1, "native I stem stays one pixel wide");
        memset(guarded.pixels, 0, sizeof(guarded.pixels));
        width = glyph_bits(f, 0x754C, guarded.pixels);
        require(width == 2 * f->w && width <= SLOT, "wide missing glyph fallback width");
        for (int y = 0; y < SLOT; y++)
            for (int x = 0; x < SLOT; x++)
                require(guarded.pixels[y * SLOT + x] == (y < f->h && x < width ? 255 : 0), "fallback fits every row without overlap or clipping");
        for (int n = 0; n < 32; n++) require(guarded.before[n] == 0xA5 && guarded.after[n] == 0xA5, "glyph buffer canaries");
    }
    free((void *)fonts_blob); free((void *)font_blob);
    return 0;
}
'''


class CourierFontTests(unittest.TestCase):
    def test_font_keys_metrics_and_device_pixels(self):
        self.assertEqual(len({font[0] for font in GEN.FONTS}), len(GEN.FONTS))
        self.assertEqual([(f[0], f[3], f[4], f[5]) for f in COURIER], [('C', 15, 24, 1), ('R', 20, 29, 1)])
        self.assertTrue(all(f[8] is None for f in COURIER), 'native box glyphs must not be replaced')

    def test_ascii_and_boxes_match_independent_bdf_placement(self):
        for spec in COURIER:
            glyphs = native(spec)
            props, records = LAB.parse_bdf(gzip.decompress((ROOT / spec[2]).read_bytes()))
            for cp, record in records.items():
                if not (32 <= cp <= 126 or 0x2500 <= cp < 0x2580):
                    continue
                expected = LAB.cell_rows(record, spec[3], spec[4], props['FONT_ASCENT'])
                with self.subTest(font=spec[0], codepoint=cp):
                    if cp == 32:
                        self.assertFalse(any(expected))  # Space is intentionally not stored.
                    else:
                        self.assertEqual(glyphs[cp], (spec[3], expected))

    def test_overhanging_glyphs_fall_back_without_cropped_records(self):
        for spec, expected_count in zip(COURIER, (106, 220)):
            omitted = []
            glyphs = native(spec, omitted)
            self.assertEqual(len(omitted), expected_count)
            self.assertIn(0x2588, omitted)  # Original full-block ink starts above the line cell.
            self.assertTrue(set(omitted).isdisjoint(glyphs))
            self.assertTrue(set(range(33, 127)).isdisjoint(omitted))
            self.assertTrue(set(range(0x2500, 0x2580)).isdisjoint(omitted))

    def test_packed_tables_round_trip_three_byte_rows(self):
        for spec in COURIER:
            glyphs = native(spec)
            blob = GEN.table(glyphs)
            for cp, expected in glyphs.items():
                self.assertEqual(unpack_record(blob, cp, spec[4]), expected, (spec[0], cp))
            self.assertIsNone(unpack_record(blob, 0x2588, spec[4]))
            if spec[3] == 20:
                self.assertTrue(any(row & 0xF for _, rows in glyphs.values() for row in rows), 'third byte contains meaningful right-hand pixels')

    def test_source_hashes_and_lab_share_canonical_fonts(self):
        data = LAB.build_data()  # Includes the pinned SHA-256 checks.
        for key, lab_id in [('C', 'courier24'), ('R', 'courier29')]:
            spec = next(s for s in COURIER if s[0] == key)
            lab = next(f for f in data['fonts'] if f['id'] == lab_id)
            self.assertEqual(lab['sourceFile'], spec[2])
            glyphs = native(spec)
            for cp, rows in lab['glyphs'].items():
                if int(cp) != 32:
                    self.assertEqual(glyphs[int(cp)][1], rows)

    def test_generated_binary_keeps_license_and_new_font_tables(self):
        with tempfile.TemporaryDirectory(prefix='hc-courier-pipeline-') as directory:
            directory = Path(directory)
            result = subprocess.run([sys.executable, str(ROOT / 'tools/gen_fonts.py'), str(directory / 'fonts.bin'), str(directory / 'fonts.inc')],
                                    cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            blob = (directory / 'fonts.bin').read_bytes()
            for _, path in GEN.FONT_NOTICES:
                self.assertIn((ROOT / path).read_text().encode('utf-8'), blob)
            self.assertIn(b'106 overhanging glyphs', result.stdout)
            self.assertIn(b'220 overhanging glyphs', result.stdout)
            records = re.findall(r"\{'(.)', \"([^\"]+)\", (\d+), (\d+), (\d+), (\d+), (?:'[^']'|0), (-?\d+)\}", (directory / 'fonts.inc').read_text())
            self.assertEqual(len(records), len(GEN.FONTS))
            for key, _, width, height, _, _, offset in records:
                if key in ('C', 'R'):
                    spec = next(s for s in COURIER if s[0] == key)
                    for cp, expected in native(spec).items():
                        self.assertEqual(unpack_record(blob[int(offset):], cp, int(height)), expected)

    @unittest.skipUnless(shutil.which('make'), 'make required for output dependency regression')
    def test_parallel_make_generates_both_outputs_once_and_repairs_missing_include(self):
        candidates = [shutil.which('make'), shutil.which('gmake')]
        for candidate in ('/opt/homebrew/opt/make/libexec/gnubin/make', '/usr/local/opt/make/libexec/gnubin/make'):
            if Path(candidate).is_file():
                candidates.append(candidate)
        for make in sorted(set(filter(None, candidates))):
            with self.subTest(make=make), tempfile.TemporaryDirectory(prefix='hc-courier-make-') as directory:
                directory = Path(directory)
                shutil.copyfile(ROOT / 'Makefile', directory / 'Makefile')
                for spec in GEN.FONTS:
                    if spec[2]:
                        path = directory / spec[2]
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(b'fixture source')
                for _, path in GEN.FONT_NOTICES:
                    (directory / path).write_text('fixture notice')
                (directory / 'tools').mkdir()
                (directory / 'tools/gen_fonts.py').write_text(
                    'import pathlib, sys, time\n'
                    'with open("calls.txt", "a") as f: f.write("generate\\n")\n'
                    'time.sleep(0.05)\n'
                    'for name in sys.argv[1:]: pathlib.Path(name).write_text("fixture output")\n')

                def run(expected_calls, *targets):
                    result = subprocess.run([make, '-j8', *targets], cwd=directory,
                                            stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr.decode())
                    self.assertEqual((directory / 'calls.txt').read_text().splitlines(), ['generate'] * expected_calls)
                    self.assertTrue((directory / 'build/fonts.bin').is_file())
                    self.assertTrue((directory / 'build/fonts.inc').is_file())

                run(1, 'build/fonts.bin', 'build/fonts.inc')
                run(1, 'build/fonts.bin', 'build/fonts.inc')
                (directory / 'build/fonts.inc').unlink()
                run(2, 'build/fonts.inc')
                (directory / 'build/fonts.bin').unlink()
                run(3, 'build/fonts.bin', 'build/fonts.inc')

    @unittest.skipUnless(shutil.which('clang') or shutil.which('cc'), 'C compiler required for headless renderer check')
    def test_production_renderer_wide_fallback_and_native_stems(self):
        source = (ROOT / 'hardcore.c').read_text()
        font_struct = re.search(r'typedef struct \{\n    char key;.*?\n\} Font;', source, re.DOTALL).group(0)
        functions = []
        for name in ('font_record', 'font_by_key', 'glyph_bits'):
            functions.append(re.search(r'^static [^\n]*\b' + name + r'\([^\n]*\) \{.*?^\}', source, re.MULTILINE | re.DOTALL).group(0))
        for name in ('cell_w', 'cell_h'):
            functions.append(re.search(r'^static int ' + name + r'\([^\n]+\n', source, re.MULTILINE).group(0))
        slot = re.search(r'^#define SLOT \d+', source, re.MULTILINE).group(0)
        with tempfile.TemporaryDirectory(prefix='hc-courier-renderer-') as directory:
            directory = Path(directory)
            subprocess.run([sys.executable, str(ROOT / 'tools/gen_fonts.py'), str(directory / 'fonts.bin'), str(directory / 'fonts.inc')],
                           cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, check=True, timeout=30)
            (directory / 'fallback.bin').write_bytes(GEN.table({0x754C: (16, [0xFFFF] * 16), 0xFFFD: (8, [0xFF] * 16)}))
            harness = HARNESS.replace('/* FONT_STRUCT */', font_struct).replace('/* FONT_LIST */', (directory / 'fonts.inc').read_text()).replace('/* SLOT_DEFINE */', slot).replace('/* FUNCTIONS */', '\n'.join(functions))
            compiler = shutil.which('clang') or shutil.which('cc')
            for negative in (False, True):
                text = harness.replace(slot, '#define SLOT 32') if negative else harness
                (directory / 'renderer.c').write_text(text)
                built = subprocess.run([compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', str(directory / 'renderer.c'), '-o', str(directory / 'renderer')],
                                       stdin=subprocess.DEVNULL, capture_output=True, timeout=30)
                self.assertEqual(built.returncode, 0, built.stderr.decode())
                ran = subprocess.run([str(directory / 'renderer'), str(directory / 'fonts.bin'), str(directory / 'fallback.bin')],
                                     stdin=subprocess.DEVNULL, capture_output=True, timeout=5)
                if negative:
                    self.assertNotEqual(ran.returncode, 0)
                    self.assertIn(b'40-pixel wide fallback slot', ran.stderr)
                else:
                    self.assertEqual(ran.returncode, 0, ran.stderr.decode())


if __name__ == '__main__':
    unittest.main()
