#!/usr/bin/env python3
"""13x25/26/27 Hairline: direct sparse expansion of original Terminus 12x24."""
import argparse,gzip
from gen_hairline_sizes import build_font as vertical_font,ROOT
from gen_hairline_widths import widen,blocks,optical,encode_bdf
import gen_fonts
SIZES=((13,25),(13,26),(13,27))
KEYS='rsu'


def build_font(width,height):
    if (width,height) not in SIZES:raise ValueError('Unreviewed strike')
    source=vertical_font(height);glyphs={}
    compact="©®*.,:;'\"`|!·‘’‚“”„°"
    for cp,(_,rows) in source.items():
        glyphs[cp]=(width,[r<<1 for r in rows] if chr(cp) in compact else widen(rows,width))
    optical(glyphs,source,width,height)
    for cp in range(0x2500,0x2580):
        pixels=gen_fonts.box_glyph(cp,width,height,(width-1)//2,(height-1)//2,[0]*height)
        glyphs[cp]=(width,[int(''.join(map(str,row)),2) for row in pixels])
    for cp in range(0x2580,0x25A0):glyphs[cp]=(width,blocks(cp,width,height))
    return glyphs


def generate_bdf(w,h):return encode_bdf(build_font(w,h),w,h,'tools/gen_hairline_compact.py')


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--check',action='store_true');args=p.parse_args()
    for w,h in SIZES:
        path=ROOT/f'font/hairline/hairline-{w}x{h}.bdf.gz';raw=generate_bdf(w,h)
        if args.check:
            if not path.exists() or gzip.decompress(path.read_bytes())!=raw:raise SystemExit(f'{path.name} differs')
        else:
            data=bytearray(gzip.compress(raw,compresslevel=9,mtime=0));data[9]=255;path.write_bytes(data)
        print(f'{path.name}: '+('verified' if args.check else 'generated'))

if __name__=='__main__':main()
