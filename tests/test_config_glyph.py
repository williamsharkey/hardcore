#!/usr/bin/env python3
"""Verify the config cog occupies each font's own cell without wide fallback."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from test_courier_fonts import GEN, ROOT, unpack_record


class ConfigGlyphTests(unittest.TestCase):
    def test_native_font_tables_have_one_cell_gear_with_open_hub(self):
        with tempfile.TemporaryDirectory(prefix='hc-cog-') as directory:
            path = Path(directory)
            subprocess.run([sys.executable, str(ROOT / 'tools/gen_fonts.py'), str(path / 'fonts.bin'), str(path / 'fonts.inc')], cwd=ROOT, check=True, capture_output=True, timeout=30)
            blob = (path / 'fonts.bin').read_bytes()
            records = re.findall(r"\{'(.)', \"[^\"]+\", (\d+), (\d+), \d+, \d+, (?:'[^']'|0), (-?\d+)\}", (path / 'fonts.inc').read_text())
            for key, width, height, offset in records:
                if int(offset) < 0:
                    continue
                width, height = int(width), int(height)
                with self.subTest(font=key):
                    actual_width, rows = unpack_record(blob[int(offset):], 0x2699, height)
                    self.assertEqual(actual_width, width)
                    ink = [(x, y) for y, row in enumerate(rows) for x in range(width) if row & (1 << (width - 1 - x))]
                    self.assertTrue(ink)
                    self.assertGreaterEqual(max(x for x, _ in ink) - min(x for x, _ in ink) + 1, width - 2)
                    self.assertGreaterEqual(max(y for _, y in ink) - min(y for _, y in ink) + 1, height // 2)
                    self.assertFalse(rows[height // 2] & (1 << (width // 2)), 'gear hub stays open')

    def test_unifont_extra_is_current_and_narrow(self):
        result = subprocess.run([sys.executable, str(ROOT / 'tools/draw_extra.py')], cwd=ROOT, check=True, capture_output=True, timeout=10)
        self.assertEqual(result.stdout, (ROOT / 'font/hardcore-extra.hex').read_bytes())
        gear = next(line for line in result.stdout.decode().splitlines() if line.startswith('2699:'))
        self.assertEqual(len(gear.split(':')[1]), 32, '8x16 glyph prevents wide fallback shrinking')


if __name__ == '__main__':
    unittest.main()
