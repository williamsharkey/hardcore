#!/usr/bin/env python3
"""Wider, native-pixel companions to the reviewed 12x28 Hairline."""
from __future__ import annotations
import argparse
import gzip
import unicodedata
from pathlib import Path
import gen_fonts
from gen_hairline_sizes import build_font as original_font

ROOT=Path(__file__).resolve().parents[1]
SIZES=((13,28),(14,28),(15,28),(16,26))
KEYS='lmno'


def points(row,width):return [x for x in range(width) if row & (1<<(width-1-x))]
def mask(xs,width):return sum(1<<(width-1-x) for x in set(xs))
def sparse(xs):return all(b-a>1 for a,b in zip(xs,xs[1:]))


def widen(rows,width):
    """Insert columns across counters/bars, never repeat a vertical stem.

    Candidate seams either continue distinct one-pixel horizontal/diagonal paths
    or extend blank counters. A glyph's native vertical strokes remain intact.
    """
    h=len(rows)
    cols=[mask((y for y,r in enumerate(rows) if r&(1<<(11-x))),h) for x in range(12)]
    bridges={}
    for x in range(2,11):
        a,b=set(points(cols[x-1],h)),set(points(cols[x],h))
        shared=a&b
        for y in sorted(a):
            for z in sorted(b):
                if abs(y-z)==1 and y not in shared and z not in shared:
                    # One bridging pixel joins an existing diagonal without
                    # duplicating either of its tall, single-column runs.
                    options=sorted((y,z),key=lambda p:(sum(abs(p-q)==1 for q in shared),p))
                    shared.add(options[0])
        if sparse(sorted(shared)):bridges[x]=mask(shared,h)
    safe=list(bridges)
    if not safe:
        # Dense legacy symbols (filled ornaments etc.) are not hairline paths.
        return [mask((x for x in range(width) if row & (1<<(11-min(11,x*12//width)))),width) for row in rows]
    counts={x:0 for x in safe}
    for i in range(width-12):
        target=2+8*(i+1)/(width-11)
        x=min(safe,key=lambda x:(counts[x],abs(x-target),abs(x-6),x))
        counts[x]+=1
    out=[]
    for x,col in enumerate(cols):
        n=counts.get(x,0)
        if n:out.extend([bridges[x]]*n)
        out.append(col)
    return [mask((x for x,c in enumerate(out) if c&(1<<(h-1-y))),width) for y in range(h)]


def shorten(rows):
    """Remove two continuation rows inside the lowercase body, not cap details.

    Prefer identical sparse rows, then connected one-pixel paths. Preserve
    horizontal bars and detached marks; both baseline and descenders move up 2.
    """
    out=list(rows)
    for target,lo,hi in [(18,15,22),(11,9,14)]:
        candidates=[]
        for y in range(lo,hi):
            a,b,c=points(out[y-1],12),points(out[y],12),points(out[y+1],12)
            if a==b==c and sparse(b):candidates.append((0,abs(y-target),y))
            elif len(a)==len(b)==len(c) and sparse(a) and sparse(b) and sparse(c) and all(abs(p-r)<=1 for p,r in zip(a,c)):
                candidates.append((1,abs(y-target),y))
        if candidates:y=min(candidates)[2]
        else:
            # Dense decorative glyphs retain all ink by merging adjacent rows.
            y=target;out[y-1]|=out[y]
        del out[y]
    return out


def blocks(cp,width,height):
    full=(1<<width)-1
    if cp in (0x2591,0x2592,0x2593):
        # Native one-pixel dither; density adapts to odd-width cells.
        return [mask((x for x in range(width) if ((x+y)%2==0 if cp==0x2592 else (x%2==y%2==0 if cp==0x2591 else not(x%2==y%2==0)))),width) for y in range(height)]
    if cp==0x2580:return [full]*(height//2)+[0]*(height-height//2)
    if 0x2581<=cp<=0x2588:
        n=(height*(cp-0x2580)+4)//8;return [0]*(height-n)+[full]*n
    if 0x2589<=cp<=0x258F:
        n=(width*(0x2590-cp)+4)//8;return [((1<<n)-1)<<(width-n)]*height
    if cp==0x2590:return [(1<<(width-width//2))-1]*height
    if cp==0x2594:
        n=(height+4)//8;return [full]*n+[0]*(height-n)
    if cp==0x2595:return [(1<<((width+4)//8))-1]*height
    q={0x2596:4,0x2597:8,0x2598:1,0x2599:13,0x259A:9,0x259B:7,0x259C:11,0x259D:2,0x259E:6,0x259F:14}[cp]
    left=full^((1<<(width-width//2))-1);right=full^left
    return [(left if q&(1<<(0 if y<height//2 else 2)) else 0)|(right if q&(2<<(0 if y<height//2 else 2)) else 0) for y in range(height)]


def optical(glyphs,source,w,h):
    t,b=4,h-6;m=(t+b)//2;left,right=1,w-3;center=(left+right)//2
    def draw(cp,segments):
        rows=[0]*h
        for x0,y0,x1,y1 in segments:
            n=max(abs(x1-x0),abs(y1-y0))
            for i in range(n+1):
                x=round(x0+(x1-x0)*i/max(1,n));y=round(y0+(y1-y0)*i/max(1,n))
                rows[y]|=1<<(w-1-x)
        glyphs[ord(cp) if isinstance(cp,str) else cp]=(w,rows)
    for c in '/\\':draw(c,[(right if c=='/' else left,t,left if c=='/' else right,b)])
    for c,top in [('X',t),('x',8)]:
        mid=(top+b)//2
        draw(c,[(left,top,center,mid),(right,top,center,mid),(center,mid,left,b),(center,mid,right,b)])
    for c,top in [('V',t),('v',8)]:draw(c,[(left,top,center,b),(right,top,center,b)])
    draw('+',[(left,m,right,m),(center,m-4,center,m+4)])
    draw('-',[(left,m,right,m)])
    draw('=',[(left,m-2,right,m-2),(left,m+2,right,m+2)])
    for c,l,r in [('<',right,left),('>',left,right)]:draw(c,[(l,m-6,r,m),(r,m,l,m+6)])
    draw(0x00D7,[(center-4,m-4,center+4,m+4),(center+4,m-4,center-4,m+4)])
    draw(0x00F7,[(left,m,right,m),(center,m-5,center,m-4),(center,m+4,center,m+5)])
    draw(0x00B1,[(left,m-2,right,m-2),(center,m-6,center,m+2),(left,m+6,right,m+6)])
    for cp in [0x2010,0x2011,0x2012,0x2013,0x2212]:draw(cp,[(left,m,right,m)])
    for cp in [0x2014,0x2015]:draw(cp,[(0,m,w-1,m)])
    for cp in [0x2260,0x2261]:
        segs=[(left,m-2,right,m-2),(left,m+2,right,m+2)] if cp==0x2260 else [(left,m-4,right,m-4),(left,m,right,m),(left,m+4,right,m+4)]
        if cp==0x2260:segs.append((center-3,m+6,center+3,m-6))
        draw(cp,segs)
    for cp,l,r in [(0x2264,right,left),(0x2265,left,right)]:draw(cp,[(l,m-6,r,m-1),(r,m-1,l,m+4),(left,m+7,right,m+7)])
    source_height=len(next(iter(source.values()))[1]);source_ascent=source_height-5
    # Detached accents retain their native small contours, centered above the
    # exact widened plain-letter body, including dotless accented i forms.
    for cp,(_,rows) in source.items():
        dec=unicodedata.normalize('NFD',chr(cp))
        if len(dec)<2 or not dec[0].isascii() or not dec[0].isalpha():continue
        base=ord(dec[0]);top=4 if dec[0].isupper() or dec[0] in 'bdhkl' else 8
        if rows[top:source_ascent]!=source[base][1][top:source_ascent]:continue
        out=list(glyphs[base][1])
        for y in range(top):out[y]=rows[y]<<((w-12+1)//2)
        for y in range(source_ascent,source_height):out[y-(source_height-h)]=rows[y]<<((w-12+1)//2)
        glyphs[cp]=(w,out)


def build_font(width,height):
    if (width,height) not in SIZES:raise ValueError('Unreviewed strike')
    source=original_font(28);glyphs={}
    # Compact marks keep native contours and are centered in the wider advance.
    compact="©®*.,:;'\"`|!·‘’‚“”„°"
    for cp,(_,rows) in source.items():
        if height==26:rows=shorten(rows)
        if chr(cp) in compact:
            out=[r<<((width-12+1)//2) for r in rows]
        else:out=widen(rows,width)
        glyphs[cp]=(width,out)
    optical(glyphs,source,width,height)
    for cp in range(0x2500,0x2580):
        pixels=gen_fonts.box_glyph(cp,width,height,(width-1)//2,(height-1)//2,[0]*height)
        glyphs[cp]=(width,[int(''.join(map(str,row)),2) for row in pixels])
    for cp in range(0x2580,0x25A0):glyphs[cp]=(width,blocks(cp,width,height))
    return glyphs


def generate_bdf(width,height):
    return encode_bdf(build_font(width,height),width,height)


def encode_bdf(glyphs,width,height,generator="tools/gen_hairline_widths.py"):
    ascent=height-5;descent=5
    props=['FAMILY_NAME "Hairline"','FOUNDRY "Hardcore"','WEIGHT_NAME "Medium"','SLANT "R"','SETWIDTH_NAME "Normal"','ADD_STYLE_NAME ""',
           'COPYRIGHT "Copyright (C) 2020 Dimitar Toshkov Zhekov"','NOTICE "SIL OFL 1.1; modified from Terminus; see OFL.txt and README.md"',
           f'PIXEL_SIZE {height}',f'POINT_SIZE {height*10}','RESOLUTION_X 72','RESOLUTION_Y 72','SPACING "C"',f'AVERAGE_WIDTH {width*10}',
           'CHARSET_REGISTRY "ISO10646"','CHARSET_ENCODING "1"',f'MIN_SPACE {width}',f'FONT_ASCENT {ascent}',f'FONT_DESCENT {descent}',
           f'CAP_HEIGHT {ascent-4}',f'X_HEIGHT {ascent-8}','DEFAULT_CHAR 65533']
    lines=['STARTFONT 2.1',f'COMMENT Designed by {generator}; optically reviewed wider Hairline strikes',f'FONT -Hardcore-Hairline-Medium-R-Normal--{height}-{height*10}-72-72-C-{width*10}-ISO10646-1',
           f'SIZE {height} 72 72',f'FONTBOUNDINGBOX {width} {height} 0 -{descent}',f'STARTPROPERTIES {len(props)}',*props,'ENDPROPERTIES',f'CHARS {len(glyphs)}']
    for cp,(w,rows) in sorted(glyphs.items()):
        assert w==width and len(rows)==height and all(0<=r<1<<width for r in rows),cp
        lines += [f'STARTCHAR uni{cp:04X}',f'ENCODING {cp}',f'SWIDTH {round(width*1000/height)} 0',f'DWIDTH {width} 0',f'BBX {width} {height} 0 -{descent}','BITMAP',*(f'{r<<(16-width):04X}' for r in rows),'ENDCHAR']
    return ('\n'.join([*lines,'ENDFONT'])+'\n').encode('ascii')


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--check',action='store_true');args=parser.parse_args()
    for w,h in SIZES:
        path=ROOT/f'font/hairline/hairline-{w}x{h}.bdf.gz';raw=generate_bdf(w,h)
        if args.check:
            if not path.exists() or gzip.decompress(path.read_bytes())!=raw:raise SystemExit(f'{path.name} differs; regenerate after review')
        else:
            data=bytearray(gzip.compress(raw,compresslevel=9,mtime=0));data[9]=255;path.write_bytes(data)
        print(f'{path.name}: '+('verified' if args.check else 'generated'))

if __name__=='__main__':main()
