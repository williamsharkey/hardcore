#!/usr/bin/env python3
"""Exact-pixel review sheets for wider Hairline strikes (Pillow dev-only)."""
from PIL import Image,ImageDraw
from gen_hairline_widths import build_font,SIZES,ROOT
from proof_hairline_sizes import GROUPS,BG,FG,MUTED
OUT=ROOT/'font/hairline/proofs'
SAMPLE=['The quick brown fox jumps over the lazy dog.', 'HAMBURGEFONTS 0123456789 O0 I1l | ilij tf', 'minimum maximum aeogjy bdhkl rn m w v x z', 'if (value != 0) { printf("%s: %.2f", name, 12.34); }', '[] {} () <> /\\ -- ++ == <= >= :;., !? @#$%&', '┌──────────┬──────────┐  $ git status', '│ Wider    │ Hairline │  ~/src/main.c', '└──────────┴──────────┘  café naïve £42 → done']

def draw_text(im,xy,text,glyphs,w,h,scale=1):
 d=ImageDraw.Draw(im);x0,y0=xy
 for i,c in enumerate(text):
  for y,row in enumerate(glyphs.get(ord(c),(w,[0]*h))[1]):
   for x in range(w):
    if row&(1<<(w-1-x)):d.rectangle((x0+(i*w+x)*scale,y0+y*scale,x0+(i*w+x+1)*scale-1,y0+(y+1)*scale-1),fill=FG)

def render(build_font=build_font,SIZES=SIZES,reference=(12,28),comparison="width-comparison.png"):
 OUT.mkdir(parents=True,exist_ok=True)
 for w,h in SIZES:
  glyphs=build_font(w,h)
  for name,groups in GROUPS.items():
   z=4;cw=w*z+22;ch=h*z+25;im=Image.new('RGB',(max(map(len,groups))*cw+24,len(groups)*ch+55),BG);d=ImageDraw.Draw(im)
   d.text((12,10),f'Hairline {w} x {h} | {name} | 4x pixel view',fill=FG)
   for j,group in enumerate(groups):
    for i,c in enumerate(group):
     x=12+i*cw;y=45+j*ch
     d.text((x,y),f'{ord(c):04X}',fill=MUTED)
     d.line((x,y+18+(h-6)*z,x+w*z-1,y+18+(h-6)*z),fill='#293940')
     draw_text(im,(x,y+18),c,glyphs,w,h,z)
   im.save(OUT/f'{w}x{h}-{name}.png')
  panel=Image.new('RGB',(max(map(len,SAMPLE))*w*2+30,h*2*len(SAMPLE)+45),BG);pd=ImageDraw.Draw(panel)
  pd.text((15,10),f'Hairline {w} x {h} | 2x inspection',fill=MUTED)
  for i,text in enumerate(SAMPLE):draw_text(panel,(15,40+i*h*2),text,glyphs,w,h,2)
  panel.save(OUT/f'{w}x{h}-text.png')
 # A compact comparison at native pixels and nearest-neighbour 2x.
 sizes=(reference,*SIZES);im=Image.new('RGB',(1450,sum(h*6+36 for w,h in sizes)+15),BG);d=ImageDraw.Draw(im);y=15
 for w,h in sizes:
  if w==12:
   from gen_hairline_sizes import build_font as original
   if h==24:
    from gen_hairline import source_glyphs
    g=source_glyphs()
   else:g=original(h)
  else:g=build_font(w,h)
  d.text((15,y),f'{"Terminus" if h==24 else "Hairline"} {w} x {h} | native pixels, then 2x',fill=MUTED);y+=24
  for scale in [1,2]:
   for s in [SAMPLE[0],SAMPLE[1]]:draw_text(im,(15,y),s,g,w,h,scale);y+=h*scale
  y+=12
 im.save(OUT/comparison)

if __name__=='__main__':render()
