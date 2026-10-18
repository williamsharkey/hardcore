#!/usr/bin/env python3
"""Reviewed Hairline strikes: bitmaps, reproducibility and native renderer."""
import gzip
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_courier_fonts import GEN, HARNESS, ROOT
from test_hairline_font import components, solid_blocks
sys.path.insert(0,str(ROOT/'tools'))
import gen_hairline_sizes as design

HEIGHTS=(28,30,32,34)
KEYS='hijk'

class HairlineSizesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source=design.source_glyphs()
        cls.fonts={h:GEN.read_bdf(ROOT/f'font/hairline/hairline-12x{h}.bdf.gz',12,h,omit_overhang=True) for h in HEIGHTS}

    def test_registration_native_sizes_and_existing_font(self):
        self.assertEqual(len({s[0] for s in GEN.FONTS}),len(GEN.FONTS))
        for key,h in zip(KEYS,HEIGHTS):
            spec=next(s for s in GEN.FONTS if s[0]==key)
            self.assertEqual(spec[1],'Hairline')
            self.assertEqual(spec[3:],(12,h,1,0,None,None))
        self.assertEqual(next(s for s in GEN.FONTS if s[0]=='H')[3:5],(12,26))

    def test_reproducible_full_source_coverage_and_no_clipping(self):
        for h,g in self.fonts.items():
            raw=gzip.decompress((ROOT/f'font/hairline/hairline-12x{h}.bdf.gz').read_bytes())
            self.assertEqual(raw,design.generate_bdf(h))
            for cp in self.source:
                if cp>32 and not 0xE000<=cp<=0xF8FF:self.assertIn(cp,g)
            omitted=[];GEN.read_bdf(ROOT/f'font/hairline/hairline-12x{h}.bdf.gz',12,h,omit_overhang=True,omitted=omitted)
            self.assertEqual(omitted,[])
            for cp,(w,rows) in g.items():
                self.assertEqual((w,len(rows)),(12,h),cp)
                self.assertTrue(all(0<=r<4096 for r in rows),cp)
            self.assertIn(b'FAMILY_NAME "Hairline"',raw)

    def test_every_printable_ascii_preserves_connections_and_one_pixel_strokes(self):
        for h,g in self.fonts.items():
            for cp in range(33,127):
                old,new=self.source[cp][1],g[cp][1]
                with self.subTest(size=h,character=chr(cp)):
                    self.assertTrue(any(new))
                    self.assertEqual(components(new),components(old))
                    self.assertLessEqual(solid_blocks(new),solid_blocks(old))
            d=design.DESIGNS[h];top,b=d['top'],d['baseline']
            stem=g[ord('I')][1]
            self.assertTrue(all(r.bit_count()==1 for r in stem[top+1:b]))
            self.assertEqual(sum(r.bit_count()>1 for r in stem),2)
            self.assertEqual(next(y for y,r in enumerate(g[ord('d')][1]) if r),top)
            self.assertEqual(next(y for y,r in enumerate(g[ord('x')][1]) if r),d['x'])
            self.assertEqual(max(y for y,r in enumerate(g[ord('x')][1]) if r),b)

    def test_dots_operators_and_accents_have_optical_not_scaled_shapes(self):
        for h,g in self.fonts.items():
            d=design.DESIGNS[h];b=d['baseline']
            self.assertEqual([y for y,r in enumerate(g[ord('.')][1]) if r],list(range(b-2,b+1)))
            self.assertEqual(sum(bool(r) for r in g[ord('_')][1]),1)
            self.assertEqual(sum(bool(r) for r in g[ord('=')][1]),2)
            self.assertEqual(sum(bool(r) for r in g[0x2026][1]),3)
            for c in 'ij':self.assertEqual(g[ord(c)][1][d['x']-4:d['x']-2],self.source[ord(c)][1][4:6])
            for accented,plain in [('À','A'),('É','E'),('Ñ','N'),('Ü','U'),('é','e'),('ñ','n')]:
                start=d['top'] if plain.isupper() else d['x']
                self.assertEqual(g[ord(accented)][1][start:b+1],g[ord(plain)][1][start:b+1])
            for cp in [*range(0x2190,0x219A),0x2202,0x220F,0x2211,0x222B,0x2014,0x2026]:self.assertTrue(any(g[cp][1]))

    def test_boxes_join_and_blocks_tile_at_every_size(self):
        for h,g in self.fonts.items():
            v=g[0x2502][1];hor=g[0x2500][1]
            self.assertEqual(len(set(v)),1);self.assertEqual(v[0].bit_count(),1)
            self.assertEqual(sum(bool(r) for r in hor),1);self.assertEqual(max(hor),4095)
            self.assertEqual(g[0x2588][1],[4095]*h)
            self.assertEqual(g[0x2580][1],[4095]*(h//2)+[0]*(h//2))
            for cp in [0x2591,0x2592,0x2593]:self.assertEqual(g[cp][1]*2,[self.source[cp][1][y%2] for y in range(h*2)])

    @unittest.skipUnless(shutil.which('clang') or shutil.which('cc'),'C compiler required')
    def test_actual_renderer_all_ascii_pixels_device_scale_and_buffer_bounds(self):
        source=(ROOT/'hardcore.c').read_text()
        font_struct=re.search(r'typedef struct \{\n    char key;.*?\n\} Font;',source,re.S)[0]
        functions=[re.search(r'^static [^\n]*\b'+name+r'\([^\n]*\) \{.*?^\}',source,re.M|re.S)[0] for name in ('font_record','font_by_key','glyph_bits')]
        functions += [re.search(r'^static int '+name+r'\([^\n]+\n',source,re.M)[0] for name in ('cell_w','cell_h')]
        slot=re.search(r'^#define SLOT \d+',source,re.M)[0]
        expected='{'+','.join('{'+','.join('{'+','.join(map(str,self.fonts[h][cp][1]))+'}' for cp in range(33,127))+'}' for h in HEIGHTS)+'}'
        main=r'''
int main(int argc,const char **argv){
 require(argc==3,"table paths");size_t n;fonts_blob=read_blob(argv[1],&n);font_blob=read_blob(argv[2],&n);font_blob_end=font_blob+n;
 static const uint16_t expected[4][94][34]=EXPECTED;
 const char keys[]="hijk";
 for(int i=0;i<4;i++){
  const Font *f=font_by_key(keys[i]);require(f && !strcmp(f->name,"Hairline"),"Hairline registration");
  for(g_scale=1;g_scale<=3;g_scale++)require(cell_w(f)==12 && cell_h(f)==28+2*i,"native device-pixel dimensions");
  for(int cp=33;cp<127;cp++){
   struct {unsigned char before[32],pixels[SLOT*SLOT],after[32];} guarded;
   memset(&guarded,0xA5,sizeof guarded);memset(guarded.pixels,0,sizeof guarded.pixels);
   require(glyph_bits(f,cp,guarded.pixels)==12,"native advance");
   for(int y=0;y<SLOT;y++)for(int x=0;x<SLOT;x++){
    int lit=y<f->h && x<12 && (expected[i][cp-33][y]&(1<<(11-x)));
    require(guarded.pixels[y*SLOT+x]==(lit?255:0),"native raster differs from reviewed BDF");
   }
   for(int z=0;z<32;z++)require(guarded.before[z]==0xA5 && guarded.after[z]==0xA5,"canary");
  }
  unsigned char wide[SLOT*SLOT]={0};require(glyph_bits(f,0x754c,wide)==24,"wide fallback advance");
 }
 free((void*)fonts_blob);free((void*)font_blob);return 0;
}'''.replace('EXPECTED',expected)
        with tempfile.TemporaryDirectory(prefix='hairline-sizes-') as directory:
            directory=Path(directory)
            subprocess.run([sys.executable,'tools/gen_fonts.py',str(directory/'fonts.bin'),str(directory/'fonts.inc')],cwd=ROOT,capture_output=True,check=True)
            (directory/'fallback.bin').write_bytes(GEN.table({0xFFFD:(8,[255]*16),0x754C:(16,[65535]*16)}))
            harness=(HARNESS.split('int main(',1)[0]+main).replace('/* FONT_STRUCT */',font_struct).replace('/* FONT_LIST */',(directory/'fonts.inc').read_text()).replace('/* SLOT_DEFINE */',slot).replace('/* FUNCTIONS */','\n'.join(functions))
            (directory/'renderer.c').write_text(harness)
            subprocess.run([shutil.which('clang') or shutil.which('cc'),'-std=c11','-O1','-Wall','-Wextra','-Werror',str(directory/'renderer.c'),'-o',str(directory/'renderer')],check=True,capture_output=True)
            result=subprocess.run([str(directory/'renderer'),str(directory/'fonts.bin'),str(directory/'fallback.bin')],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())

if __name__=='__main__':unittest.main()
