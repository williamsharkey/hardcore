"""Exact production rasterization checks shared by reviewed Hairline strikes."""
from pathlib import Path
import re,shutil,subprocess,sys,tempfile
from test_courier_fonts import GEN,HARNESS,ROOT
sys.path.insert(0,str(ROOT/'tools'))
from hairline_review import GROUPS
COMMON=sorted({ord(c) for group in GROUPS.values() for row in group for c in row})

def check_renderer(fonts,keys):
    source=(ROOT/'hardcore.c').read_text()
    font_struct=re.search(r'typedef struct \{\n    char key;.*?\n\} Font;',source,re.S)[0]
    functions=[re.search(r'^static [^\n]*\b'+name+r'\([^\n]*\) \{.*?^\}',source,re.M|re.S)[0] for name in ('font_record','font_by_key','glyph_bits')]
    functions += [re.search(r'^static int '+name+r'\([^\n]+\n',source,re.M)[0] for name in ('cell_w','cell_h')]
    slot=re.search(r'^#define SLOT \d+',source,re.M)[0]
    expected='{'+','.join('{'+','.join('{'+','.join(map(str,fonts[w,h][cp][1]))+'}' for cp in COMMON)+'}' for w,h in fonts)+'}'
    main=r'''
int main(int argc,const char **argv){
 require(argc==3,"table paths");size_t n;fonts_blob=read_blob(argv[1],&n);font_blob=read_blob(argv[2],&n);font_blob_end=font_blob+n;
 static const uint16_t expected[FONT_COUNT][260][MAX_HEIGHT]=EXPECTED;
 const char keys[]="KEYS";
 const int widths[]={WIDTHS},heights[]={HEIGHTS};
 const int common[]={CODEPOINTS};
 for(int i=0;i<FONT_COUNT;i++){
  const Font *f=font_by_key(keys[i]);require(f && !strcmp(f->name,"Hairline"),"Hairline registration");
  for(g_scale=1;g_scale<=3;g_scale++)require(cell_w(f)==widths[i] && cell_h(f)==heights[i],"native device-pixel dimensions");
  for(int ci=0;ci<260;ci++){
   int cp=common[ci];
   struct {unsigned char before[32],pixels[SLOT*SLOT],after[32];} guarded;
   memset(&guarded,0xA5,sizeof guarded);memset(guarded.pixels,0,sizeof guarded.pixels);
   require(glyph_bits(f,cp,guarded.pixels)==widths[i],"native advance");
   for(int y=0;y<SLOT;y++)for(int x=0;x<SLOT;x++){
int lit=y<f->h && x<f->w && (expected[i][ci][y]&(1<<(f->w-1-x)));
require(guarded.pixels[y*SLOT+x]==(lit?255:0),"native raster differs from reviewed BDF");
   }
   for(int z=0;z<32;z++)require(guarded.before[z]==0xA5 && guarded.after[z]==0xA5,"canary");
  }
  unsigned char wide[SLOT*SLOT]={0};require(glyph_bits(f,0x754c,wide)==2*widths[i],"wide fallback advance");
 }
 free((void*)fonts_blob);free((void*)font_blob);return 0;
}'''.replace('EXPECTED',expected).replace('FONT_COUNT',str(len(fonts))).replace('MAX_HEIGHT',str(max(h for w,h in fonts))).replace('KEYS',keys).replace('WIDTHS',','.join(str(w) for w,h in fonts)).replace('HEIGHTS',','.join(str(h) for w,h in fonts)).replace('CODEPOINTS',','.join(map(str,COMMON)))
    with tempfile.TemporaryDirectory(prefix='hairline-widths-') as directory:
        directory=Path(directory)
        subprocess.run([sys.executable,'tools/gen_fonts.py',str(directory/'fonts.bin'),str(directory/'fonts.inc')],cwd=ROOT,capture_output=True,check=True)
        (directory/'fallback.bin').write_bytes(GEN.table({0xFFFD:(8,[255]*16),0x754C:(16,[65535]*16)}))
        harness=(HARNESS.split('int main(',1)[0]+main).replace('/* FONT_STRUCT */',font_struct).replace('/* FONT_LIST */',(directory/'fonts.inc').read_text()).replace('/* SLOT_DEFINE */',slot).replace('/* FUNCTIONS */','\n'.join(functions))
        (directory/'renderer.c').write_text(harness)
        subprocess.run([shutil.which('clang') or shutil.which('cc'),'-std=c11','-O1','-Wall','-Wextra','-Werror',str(directory/'renderer.c'),'-o',str(directory/'renderer')],check=True,capture_output=True)
        result=subprocess.run([str(directory/'renderer'),str(directory/'fonts.bin'),str(directory/'fallback.bin')],capture_output=True)
        assert result.returncode==0,result.stderr.decode()
