#!/usr/bin/env python3
"""Render review sheets from the exact generated BDF pixels (Pillow dev-only)."""
from pathlib import Path
from PIL import Image,ImageDraw
from gen_hairline_sizes import build_font, HEIGHTS, ROOT

OUT=ROOT/'font/hairline/proofs'
from hairline_review import GROUPS
BG='#151a20';FG='#e3e6e9';MUTED='#a4b6c8'

def draw_text(im,xy,text,glyphs,height,scale=1):
 d=ImageDraw.Draw(im);x0,y0=xy
 for i,c in enumerate(text):
  for y,row in enumerate(glyphs.get(ord(c),(12,[0]*height))[1]):
   for x in range(12):
    if row&(1<<(11-x)):d.rectangle((x0+i*12*scale+x*scale,y0+y*scale,x0+i*12*scale+(x+1)*scale-1,y0+(y+1)*scale-1),fill=FG)

def main():
 OUT.mkdir(parents=True,exist_ok=True)
 for height in HEIGHTS:
  glyphs=build_font(height)
  for name,groups in GROUPS.items():
   z=4;cw=70;ch=height*z+25;im=Image.new('RGB',(max(map(len,groups))*cw+24,len(groups)*ch+55),BG);d=ImageDraw.Draw(im)
   d.text((12,10),f'Hairline 12 x {height} | {name} | 4x pixel view',fill=FG)
   for j,group in enumerate(groups):
    for i,c in enumerate(group):
     x=12+i*cw;y=45+j*ch
     d.text((x,y),f'{ord(c):04X}',fill=MUTED)
     d.line((x,y+18+(height-6)*z,x+47,y+18+(height-6)*z),fill='#293940')
     draw_text(im,(x,y+18),c,glyphs,height,z)
   im.save(OUT/f'12x{height}-{name}.png')
 sample=['The quick brown fox jumps over the lazy dog.', 'HAMBURGEFONTS 0123456789 O0 I1l | ilij tf', 'minimum maximum aeogjy bdhkl rn m w v x z', 'if (value != 0) { printf("%s: %.2f", name, 12.34); }', '[] {} () <> /\\ -- ++ == <= >= :;., !? @#$%&', '┌──────────┬──────────┐  $ git status', '│ 12xCELL  │ Hairline │  ~/src/main.c', '└──────────┴──────────┘  café naïve £42 → done']
 im=Image.new('RGB',(1250,sum(h*2*len(sample)+60 for h in HEIGHTS)),BG);d=ImageDraw.Draw(im);y=0
 for h in HEIGHTS:
  g=build_font(h);d.text((15,y+12),f'Hairline 12 x {h} | 2x for inspection; all bitmap strokes native 1 pixel',fill=MUTED);y+=45
  panel=Image.new('RGB',(1250,h*2*len(sample)+45),BG);pd=ImageDraw.Draw(panel);pd.text((15,10),f'Hairline 12 x {h} | 2x inspection',fill=MUTED)
  for i,text in enumerate(sample):
   draw_text(im,(15,y),text,g,h,2);y+=h*2
   draw_text(panel,(15,40+i*h*2),text,g,h,2)
  panel.save(OUT/f'12x{h}-text.png')
  y+=15
 im.save(OUT/'text-comparison.png')

if __name__=='__main__':main()
