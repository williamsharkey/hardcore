#!/usr/bin/env python3
"""Wider Hairline: reviewed glyphs, native pixels, joins and shipped data."""
import gzip
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_courier_fonts import GEN,HARNESS,ROOT
from test_hairline_font import components,solid_blocks
sys.path.insert(0,str(ROOT/'tools'))
import gen_hairline_widths as design
from hairline_review import GROUPS
SIZES=design.SIZES
COMMON=sorted({ord(c) for group in GROUPS.values() for row in group for c in row})

class HairlineWidthsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=design.original_font(28)
        cls.fonts={(w,h):GEN.read_bdf(ROOT/f'font/hairline/hairline-{w}x{h}.bdf.gz',w,h,omit_overhang=True) for w,h in SIZES}

    def test_registration_reproducibility_and_coverage(self):
        self.assertEqual(len({s[0] for s in GEN.FONTS}),len(GEN.FONTS))
        for key,(w,h) in zip(design.KEYS,SIZES):
            spec=next(s for s in GEN.FONTS if s[0]==key)
            self.assertEqual(spec[1],'Hairline');self.assertEqual(spec[3:],(w,h,1,0,None,None))
            path=ROOT/spec[2];raw=gzip.decompress(path.read_bytes())
            self.assertEqual(raw,design.generate_bdf(w,h))
            omitted=[];GEN.read_bdf(path,w,h,omit_overhang=True,omitted=omitted)
            self.assertEqual(omitted,[])
            for cp in self.source:
                if cp>32 and not 0xE000<=cp<=0xF8FF:self.assertIn(cp,self.fonts[w,h])
            for cp,(advance,rows) in self.fonts[w,h].items():
                self.assertEqual((advance,len(rows)),(w,h))
                self.assertTrue(all(0<=r<1<<w for r in rows))

    def test_all_common_characters_keep_connections_and_thin_ascii_strokes(self):
        self.assertEqual(len(COMMON),260)
        for (w,h),g in self.fonts.items():
            for cp in COMMON:
                old,new=self.source[cp][1],g[cp][1]
                with self.subTest(size=(w,h),char=chr(cp)):
                    self.assertTrue(any(new))
                    if not 0x2580<=cp<=0x259F:self.assertEqual(components(new,w),components(old))
                    if cp<127:self.assertLessEqual(solid_blocks(new),solid_blocks(old))
            for c in 'Iil':
                rows=g[ord(c)][1]
                self.assertTrue(all(bin(r).count("1")==1 for r in rows[10:h-7]))
            # Wider means real open counters, not extra blank advance alone.
            self.assertGreater(max(bin(r).count("1") for r in g[ord('H')][1]),max(bin(r).count("1") for r in self.source[ord('H')][1]))

    def test_optical_anchors_accents_and_marks(self):
        for (w,h),g in self.fonts.items():
            base=h-6
            for c in 'Adh':self.assertEqual(next(y for y,r in enumerate(g[ord(c)][1]) if r),4)
            for c in 'ax':
                self.assertEqual(next(y for y,r in enumerate(g[ord(c)][1]) if r),8)
                self.assertEqual(max(y for y,r in enumerate(g[ord(c)][1]) if r),base)
            for c in 'ij':
                dot=g[ord(c)][1][4:6];self.assertEqual(len(set(dot)),1)
                self.assertEqual(bin(dot[0]).count("1"),1)
                stem=g[ord(c)][1][12];self.assertEqual(stem,dot[0])
            self.assertEqual([y for y,r in enumerate(g[ord('.')][1]) if r],list(range(base-2,base+1)))
            for c in '*.,:;\'"`|!©®°':
                self.assertEqual(sum(bin(r).count("1") for r in g[ord(c)][1]),sum(bin(r).count("1") for r in (design.shorten(self.source[ord(c)][1]) if h==26 else self.source[ord(c)][1])))
            for accented,plain in [('À','A'),('É','E'),('Ñ','N'),('Ü','U'),('é','e'),('ñ','n'),('ä','a'),('ï','i')]:
                top=4 if plain.isupper() else 8
                self.assertEqual(g[ord(accented)][1][top:base+1],g[ord(plain)][1][top:base+1])

    def test_full_cell_box_block_and_quadrant_geometry(self):
        for (w,h),g in self.fonts.items():
            full=(1<<w)-1
            self.assertEqual(g[0x2588][1],[full]*h)
            self.assertEqual(g[0x2580][1],[full]*(h//2)+[0]*(h//2))
            self.assertEqual(max(g[0x2500][1]),full)
            self.assertEqual(sum(bool(r) for r in g[0x2500][1]),1)
            self.assertEqual(len(set(g[0x2502][1])),1)
            self.assertEqual(bin(g[0x2502][1][0]).count("1"),1)
            for y in range(h):
                self.assertEqual(g[0x258C][1][y]|g[0x2590][1][y],full)
                self.assertEqual(g[0x259A][1][y]|g[0x259E][1][y],full)
                self.assertEqual(g[0x259A][1][y]&g[0x259E][1][y],0)

    @unittest.skipUnless(shutil.which('clang') or shutil.which('cc'),'C compiler required')
    def test_actual_renderer_all_common_pixels_device_scale_and_buffer_bounds(self):
        from hairline_renderer import check_renderer
        check_renderer(self.fonts,design.KEYS)

if __name__=='__main__':unittest.main()
