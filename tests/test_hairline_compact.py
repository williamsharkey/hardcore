#!/usr/bin/env python3
"""Direct Terminus24-derived 13x25/26/27 Hairline proofs and native rendering."""
import gzip,shutil,sys,unittest
from test_courier_fonts import GEN,ROOT
from test_hairline_font import components,solid_blocks
from hairline_renderer import check_renderer,COMMON
sys.path.insert(0,str(ROOT/'tools'))
import gen_hairline_compact as design
from gen_hairline import source_glyphs

class HairlineCompactTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.source=source_glyphs()
  cls.fonts={(w,h):GEN.read_bdf(ROOT/f'font/hairline/hairline-{w}x{h}.bdf.gz',w,h,omit_overhang=True) for w,h in design.SIZES}
 def test_shipped_metrics_payload_coverage_and_bounds(self):
  self.assertEqual(len({s[0] for s in GEN.FONTS}),len(GEN.FONTS))
  for key,(w,h) in zip(design.KEYS,design.SIZES):
   spec=next(s for s in GEN.FONTS if s[0]==key)
   self.assertEqual(spec[1],'Hairline');self.assertEqual(spec[3:],(w,h,1,0,None,None))
   path=ROOT/spec[2];raw=gzip.decompress(path.read_bytes());self.assertEqual(raw,design.generate_bdf(w,h))
   omitted=[];GEN.read_bdf(path,w,h,omit_overhang=True,omitted=omitted);self.assertEqual(omitted,[])
   for cp in self.source:
    if cp>32 and not 0xE000<=cp<=0xF8FF:self.assertIn(cp,self.fonts[w,h])
   for cp,(advance,rows) in self.fonts[w,h].items():
    self.assertEqual((advance,len(rows)),(w,h));self.assertTrue(all(0<=r<1<<w for r in rows))
 def test_every_common_glyph_connected_and_ascii_strokes_not_boldened(self):
  self.assertEqual(len(COMMON),260)
  # Compare components against the reviewed 12-wide contours, including newly
  # supplied Unicode symbols that original Terminus24 does not contain.
  for (w,h),g in self.fonts.items():
   vertical=design.vertical_font(h)
   for cp in COMMON:
    with self.subTest(size=h,char=chr(cp)):
     self.assertTrue(any(g[cp][1]))
     if not 0x2580<=cp<=0x259F:self.assertEqual(components(g[cp][1],w),components(vertical[cp][1]))
     if cp<127:
      self.assertEqual(components(g[cp][1],w),components(self.source[cp][1]))
      self.assertLessEqual(solid_blocks(g[cp][1]),solid_blocks(self.source[cp][1]))
 def test_baseline_dots_accents_and_odd_height_full_cells(self):
  for (w,h),g in self.fonts.items():
   b=h-6
   for c in 'Adh':self.assertEqual(next(y for y,r in enumerate(g[ord(c)][1]) if r),4)
   for c in 'ax':
    self.assertEqual(next(y for y,r in enumerate(g[ord(c)][1]) if r),8)
    self.assertEqual(max(y for y,r in enumerate(g[ord(c)][1]) if r),b)
   for c in 'Iil':self.assertTrue(all(bin(r).count("1")==1 for r in g[ord(c)][1][10:b]))
   for c in 'ij':self.assertEqual(g[ord(c)][1][4],g[ord(c)][1][12])
   self.assertEqual([y for y,r in enumerate(g[46][1]) if r],list(range(b-2,b+1)))
   for accented,plain in [('À','A'),('É','E'),('Ñ','N'),('Ü','U'),('é','e'),('ñ','n'),('ä','a'),('ï','i')]:
    start=4 if plain.isupper() else 8;self.assertEqual(g[ord(accented)][1][start:b+1],g[ord(plain)][1][start:b+1])
   full=(1<<w)-1;self.assertEqual(g[0x2588][1],[full]*h)
   self.assertEqual(max(g[0x2500][1]),full);self.assertEqual(sum(bool(r) for r in g[0x2500][1]),1)
   self.assertEqual(len(set(g[0x2502][1])),1)
   self.assertEqual(g[0x2580][1],[full]*(h//2)+[0]*(h-h//2))
   for y in range(h):
    self.assertEqual(g[0x259A][1][y]|g[0x259E][1][y],full)
    self.assertEqual(g[0x259A][1][y]&g[0x259E][1][y],0)
 @unittest.skipUnless(shutil.which('clang') or shutil.which('cc'),'C compiler required')
 def test_production_renderer_all_common_glyphs(self):check_renderer(self.fonts,design.KEYS)

if __name__=='__main__':unittest.main()
