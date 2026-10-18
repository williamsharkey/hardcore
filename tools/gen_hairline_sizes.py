#!/usr/bin/env python3
"""Build optically reviewed 12-wide Hairline 28/30/32/34 strikes from Terminus24.

Independent of the web lab. Original horizontal strokes are never duplicated;
extra rows bridge isolated stems/diagonals. Explicit glyph groups and optical
anchors keep punctuation, ascenders, dots and operators in proportion.
"""
from __future__ import annotations
import argparse, gzip, json, unicodedata
from pathlib import Path
import gen_fonts
from gen_hairline import source_glyphs

ROOT=Path(__file__).resolve().parents[1]
HEIGHTS=(28,30,32,34)
WIDTH=12
# Ascent is a boundary; all coordinates below are ink rows.
# 25/26/27 are intermediate cuts for the 13-wide family; main() only writes HEIGHTS.
DESIGNS={25:dict(top=4,x=8,baseline=19),26:dict(top=4,x=8,baseline=20),27:dict(top=4,x=8,baseline=21),28:dict(top=4,x=8,baseline=22),30:dict(top=4,x=9,baseline=24),
         32:dict(top=4,x=9,baseline=26),34:dict(top=4,x=10,baseline=28)}


def columns(row): return [x for x in range(WIDTH) if row & (1<<(11-x))]
def mask(xs): return sum(1<<(11-x) for x in set(xs))
def isolated(xs): return all(b-a>1 for a,b in zip(xs,xs[1:]))


def stretch(rows, length, bias=0.0):
    """Keep source rows intact; insert sparse connected strokes at safe seams."""
    if len(rows)==length:return list(rows)
    if length<len(rows):
        # Only short detached accents use compression; combine their ink.
        out=[0]*length
        for y,r in enumerate(rows):out[round(y*(length-1)/max(1,len(rows)-1))]|=r
        return out
    extra=length-len(rows)
    safe=[]
    for y in range(1,len(rows)):
        a,b=columns(rows[y-1]),columns(rows[y])
        if a and len(a)==len(b) and isolated(a) and isolated(b) and all(abs(x-z)<=1 for x,z in zip(a,b)):
            safe.append(y)
    if not safe:
        # For short symbols: insert in blank space, or bridge shared vertical
        # strokes only. Never duplicate a horizontal bar to fill space.
        safe=[y for y in range(1,len(rows)) if not rows[y-1] and not rows[y]]
    if not safe:
        safe=[y for y in range(1,len(rows)) if rows[y-1]&rows[y]]
    if not safe:
        # Sparse disconnected marks keep their contours, spread by their row map.
        out=[0]*length
        for y,r in enumerate(rows):out[round(y*(length-1)/max(1,len(rows)-1))]|=r
        return out
    counts={y:0 for y in safe}
    for i in range(extra):
        target=(len(rows)-1)*(i+1)/(extra+1)+bias
        y=min(safe,key=lambda y:(abs(y-target),counts[y],y))
        counts[y]+=1
    out=[]
    for y,row in enumerate(rows):
        n=counts.get(y,0)
        if n:
            a,b=columns(rows[y-1]),columns(row)
            matching=(len(a)==len(b) and isolated(a) and isolated(b) and all(abs(x-z)<=1 for x,z in zip(a,b)))
            for i in range(n):
                if matching:out.append(mask(round(x+(z-x)*(i+1)/(n+1)) for x,z in zip(a,b)))
                else:
                    shared=rows[y-1]&row
                    out.append(shared & ~(shared<<1) & ~(shared>>1))
        out.append(row)
    return out


def put(out, rows, start):
    for i,row in enumerate(rows):
        y=start+i
        if row and not 0<=y<len(out):raise ValueError('Ink outside cell')
        if 0<=y<len(out):out[y]|=row


def line(out,x0,y0,x1,y1):
    n=max(abs(x1-x0),abs(y1-y0))
    for i in range(n+1):
        x=round(x0+(x1-x0)*i/max(1,n));y=round(y0+(y1-y0)*i/max(1,n))
        if 0<=x<12 and 0<=y<len(out):out[y]|=1<<(11-x)


def letter(rows,c,height):
    d=DESIGNS[height]; top,x,base=d['top'],d['x'],d['baseline'];out=[0]*height
    if c.isupper() or c.isdigit():
        put(out,stretch(rows[4:19],base-top+1),top)
        put(out,rows[19:],base+1)
        return out
    # Source bowls start at row 8; body/descender parts share one baseline.
    put(out,stretch(rows[8:19],base-x+1),x)
    put(out,rows[19:],base+1)
    if c in 'ij':put(out,rows[4:6],x-4)
    elif c=='f':put(out,stretch(rows[4:8],x-top),top)
    elif c=='t':put(out,rows[5:8],x-3)
    elif c in 'bdhkl':put(out,stretch(rows[4:8],x-top),top)
    return out


def punctuation(rows,c,height):
    d=DESIGNS[height];top,x,b=d['top'],d['x'],d['baseline'];mid=round((top+b)/2);out=[0]*height
    if c in '.,:;':
        # Keep marks the original size; increase separation, not dot weight.
        if c=='.':put(out,rows[16:19],b-2)
        elif c==',':put(out,rows[15:20],b-3)
        else:
            put(out,rows[8:11],x+1)
            put(out,rows[15:20] if c==';' else rows[15:18],b-3)
    elif c=='`':put(out,rows[1:4],top)
    elif c in "'\"":put(out,rows[3:9],top-1)
    elif c=='_':put(out,[rows[20]],b+2)
    elif c=='-':put(out,[next(r for r in rows if r)],mid)
    elif c=='=':
        bar=next(r for r in rows if r);out[mid-2]=bar;out[mid+2]=bar
    elif c=='+':
        line(out,2,mid,9,mid);line(out,5,mid-4,5,mid+4)
    elif c in '/\\':line(out,9 if c=='/' else 2,top,2 if c=='/' else 9,b)
    elif c=='|':line(out,5,top,5,b)
    elif c in '<>':
        left,right=(8,2) if c=='<' else (2,8)
        line(out,left,mid-7,right,mid);line(out,right,mid,left,mid+7)
    elif c=='^':put(out,rows[2:7],top)
    elif c=='~':put(out,rows[2:6],x+1)
    elif c=='*':put(out,rows[7:16],mid-4)
    elif c in '[](){}':put(out,stretch(rows[4:20],b-top+2),top)
    elif c in '!?' :
        # Detached question/exclamation dot stays two rows, at the baseline.
        put(out,stretch(rows[4:16],b-top-3),top);put(out,rows[16:19],b-2)
    elif c in '#$%&@':put(out,stretch(rows[3:20],b-top+3),top-1)
    else:put(out,stretch(rows[4:19],b-top+1),top);put(out,rows[19:],b+1)
    return out


def generic(rows,cp,height):
    """Retain full Terminus coverage with sparse, baseline-aligned expansion."""
    d=DESIGNS[height];out=[0]*height
    put(out,rows[:3],0)
    put(out,stretch(rows[3:19],d['baseline']-2),3)
    put(out,rows[19:],d['baseline']+1)
    return out


def unicode_letter(rows,height):
    d=DESIGNS[height];out=[0]*height
    put(out,rows[:4],0)
    if rows[7]&rows[8]:put(out,stretch(rows[4:8],d['x']-d['top']),d['top'])
    else:put(out,rows[4:8],d['x']-4)
    put(out,stretch(rows[8:19],d['baseline']-d['x']+1),d['x'])
    put(out,rows[19:],d['baseline']+1)
    return out


def optical_symbols(glyphs,source,height):
    d=DESIGNS[height];t,x,b=d['top'],d['x'],d['baseline'];m=round((t+b)/2)
    def draw(cp,segments):
        out=[0]*height
        for segment in segments:line(out,*segment)
        glyphs[cp]=(12,out)
    def stamp(cp,rows,y):
        out=[0]*height;put(out,rows,y);glyphs[cp]=(12,out)
    for cp in [0x2010,0x2011,0x2012,0x2013,0x2212]:draw(cp,[(1,m,9,m)])
    for cp in [0x2014,0x2015]:draw(cp,[(0,m,11,m)])
    for cp in [0x2018,0x2019,0x201C,0x201D]:stamp(cp,source[cp][1][2:7],t)
    stamp(0x201A,source[0x2019][1][2:7],b-3)
    stamp(0x201E,source[0x201D][1][2:7],b-3)
    stamp(0x2026,[mask([1,5,9])]*3,b-2)
    stamp(0x00B7,[mask([5])]*2,m)
    stamp(0x00B0,source[0x00B0][1][2:8],t)
    for cp in [0x00A9,0x00AE]:stamp(cp,source[cp][1][6:18],m-6)
    draw(0x00D7,[(2,m-4,9,m+4),(9,m-4,2,m+4)])
    draw(0x00F7,[(1,m,9,m),(5,m-5,5,m-4),(5,m+4,5,m+5)])
    draw(0x00B1,[(1,m-2,9,m-2),(5,m-6,5,m+2),(1,m+6,9,m+6)])
    draw(0x2260,[(1,m-2,9,m-2),(1,m+2,9,m+2),(3,m+6,8,m-6)])
    draw(0x2261,[(1,m-4,9,m-4),(1,m,9,m),(1,m+4,9,m+4)])
    for cp,left,right in [(0x2264,8,2),(0x2265,2,8)]:draw(cp,[(left,m-6,right,m-1),(right,m-1,left,m+4),(2,m+7,8,m+7)])
    for cp in [0x2248,0x2243]:
        out=[0]*height
        for offset in [-3,3]:
            for segment in [(1,m+offset+1,3,m+offset-1),(3,m+offset-1,7,m+offset+1),(7,m+offset+1,9,m+offset-1)]:line(out,*segment)
        glyphs[cp]=(12,out)
    draw(0x221E,[(1,m-2,3,m-3),(3,m-3,8,m+3),(8,m+3,10,m+1),(10,m+1,10,m-2),(10,m-2,8,m-3),(8,m-3,3,m+3),(3,m+3,1,m+2),(1,m+2,1,m-2)])
    draw(0x220F,[(1,t,10,t),(2,t,2,b),(9,t,9,b)])
    draw(0x2211,[(1,t,10,t),(1,t,6,m),(6,m,1,b),(1,b,10,b)])
    draw(0x222B,[(8,t,6,t),(6,t,5,t+2),(5,t+2,5,b-2),(5,b-2,4,b),(4,b,2,b)])
    draw(0x2202,[(3,t+1,5,t),(5,t,8,t+2),(8,t+2,9,t+5),(9,t+5,8,b-2),(8,b-2,6,b),(6,b,3,b),(3,b,1,b-2),(1,b-2,1,m+1),(1,m+1,3,m-1),(3,m-1,6,m-1),(6,m-1,8,m+1)])
    # Straight arrows use the same mathematical axis and fixed-size heads.
    for cp in range(0x2190,0x219A):
        out=[0]*height
        if cp in [0x2190,0x2192,0x2194]:
            line(out,1,m,10,m)
            if cp!=0x2192:line(out,1,m,4,m-3);line(out,1,m,4,m+3)
            if cp!=0x2190:line(out,10,m,7,m-3);line(out,10,m,7,m+3)
        elif cp in [0x2191,0x2193,0x2195]:
            line(out,5,t+1,5,b-1)
            if cp!=0x2193:line(out,5,t+1,2,t+4);line(out,5,t+1,8,t+4)
            if cp!=0x2191:line(out,5,b-1,2,b-4);line(out,5,b-1,8,b-4)
        else:
            left=cp in [0x2196,0x2199];up=cp in [0x2196,0x2197]
            end_x=1 if left else 10;end_y=m-5 if up else m+5
            line(out,10 if left else 1,m+5 if up else m-5,end_x,end_y)
            line(out,end_x,end_y,end_x+(4 if left else -4),end_y)
            line(out,end_x,end_y,end_x,end_y+(4 if up else -4))
        glyphs[cp]=(12,out)
    for cp in [0x21D0,0x21D2,0x21D4]:
        out=[0]*height;line(out,2,m-1,9,m-1);line(out,2,m+1,9,m+1)
        if cp!=0x21D2:line(out,0,m,4,m-4);line(out,0,m,4,m+4)
        if cp!=0x21D0:line(out,11,m,7,m-4);line(out,11,m,7,m+4)
        glyphs[cp]=(12,out)


def blocks(cp,source,height):
    full=4095
    if cp in (0x2591,0x2592,0x2593):return [source[cp][1][y%2] for y in range(height)]
    if 0x2589<=cp<=0x2590:return [source[cp][1][0]]*height
    if cp==0x2595:return [int(f'{source[0x258F][1][0]:012b}'[::-1],2)]*height
    if cp==0x2580:return [full]*(height//2)+[0]*(height-height//2)
    if 0x2581<=cp<=0x2588:
        n=(height*(cp-0x2580)+4)//8;return [0]*(height-n)+[full]*n
    if cp==0x2594:
        n=(height+4)//8;return [full]*n+[0]*(height-n)
    q={0x2596:4,0x2597:8,0x2598:1,0x2599:13,0x259A:9,0x259B:7,0x259C:11,0x259D:2,0x259E:6,0x259F:14}[cp]
    return [(0xFC0 if q&(1<<(0 if y<height//2 else 2)) else 0)|(0x03F if q&(2<<(0 if y<height//2 else 2)) else 0) for y in range(height)]


def build_font(height):
    if height not in DESIGNS:raise ValueError('Unreviewed strike')
    source=source_glyphs();glyphs={}
    for cp,(_,rows) in source.items():
        c=chr(cp)
        if c.isascii() and c.isalnum():out=letter(rows,c,height)
        elif 33<=cp<=126:out=punctuation(rows,c,height)
        elif unicodedata.category(c) in ('Lu','Lt'):out=letter(rows,'A',height);put(out,rows[:4],0)
        elif unicodedata.category(c)=='Ll':out=unicode_letter(rows,height)
        else:out=generic(rows,cp,height)
        glyphs[cp]=(12,out)
    # Reuse adjusted ASCII in decomposable accented Latin letters, with original
    # accents placed above capitals or lowercase at a consistent optical gap.
    for cp,(_,rows) in source.items():
        decomp=unicodedata.normalize('NFD',chr(cp))
        if len(decomp)<2 or ord(decomp[0]) not in glyphs or not decomp[0].isascii() or not decomp[0].isalpha():continue
        base_cp=ord(decomp[0]);orig_base=source[base_cp][1]
        extra=[r&~b for r,b in zip(rows,orig_base)]
        # Only apply if the main letter exactly matches its plain source form.
        if any((r&b)!=b for r,b in zip(rows,orig_base)):continue
        out=list(glyphs[base_cp][1]);d=DESIGNS[height]
        for y,r in enumerate(extra):
            if not r:continue
            target=y if y<4 else y+(d['x']-8) if y<8 else y+(d['baseline']-18)
            if 0<=target<height:out[target]|=r
        glyphs[cp]=(12,out)
    optical_symbols(glyphs,source,height)
    for cp in range(0x2500,0x2580):
        pixels=gen_fonts.box_glyph(cp,12,height,5,(height-1)//2,[0]*height)
        glyphs[cp]=(12,[int(''.join(map(str,row)),2) for row in pixels])
    for cp in range(0x2580,0x25A0):glyphs[cp]=(12,blocks(cp,source,height))
    return glyphs


def generate_bdf(height):
    glyphs=build_font(height);d=DESIGNS[height];ascent=d['baseline']+1;descent=height-ascent
    props=['FAMILY_NAME "Hairline"','FOUNDRY "Hardcore"','WEIGHT_NAME "Medium"','SLANT "R"','SETWIDTH_NAME "Normal"','ADD_STYLE_NAME ""',
           'COPYRIGHT "Copyright (C) 2020 Dimitar Toshkov Zhekov"','NOTICE "SIL OFL 1.1; modified from Terminus; see OFL.txt and README.md"',
           f'PIXEL_SIZE {height}',f'POINT_SIZE {height*10}','RESOLUTION_X 72','RESOLUTION_Y 72','SPACING "C"','AVERAGE_WIDTH 120',
           'CHARSET_REGISTRY "ISO10646"','CHARSET_ENCODING "1"','MIN_SPACE 12',f'FONT_ASCENT {ascent}',f'FONT_DESCENT {descent}',
           f'CAP_HEIGHT {ascent-d["top"]}',f'X_HEIGHT {ascent-d["x"]}','DEFAULT_CHAR 65533']
    lines=['STARTFONT 2.1','COMMENT Designed by tools/gen_hairline_sizes.py; reviewed fixed-width hairline strikes',f'FONT -Hardcore-Hairline-Medium-R-Normal--{height}-{height*10}-72-72-C-120-ISO10646-1',
           f'SIZE {height} 72 72',f'FONTBOUNDINGBOX 12 {height} 0 -{descent}',f'STARTPROPERTIES {len(props)}',*props,'ENDPROPERTIES',f'CHARS {len(glyphs)}']
    for cp,(width,rows) in sorted(glyphs.items()):
        assert width==12 and len(rows)==height and all(0<=r<4096 for r in rows),cp
        lines += [f'STARTCHAR uni{cp:04X}',f'ENCODING {cp}',f'SWIDTH {round(12000/height)} 0','DWIDTH 12 0',f'BBX 12 {height} 0 -{descent}','BITMAP',*(f'{r<<4:04X}' for r in rows),'ENDCHAR']
    return ('\n'.join([*lines,'ENDFONT'])+'\n').encode('ascii')


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--check',action='store_true');args=parser.parse_args()
    for height in HEIGHTS:
        path=ROOT/f'font/hairline/hairline-12x{height}.bdf.gz';raw=generate_bdf(height)
        if args.check:
            if not path.exists() or gzip.decompress(path.read_bytes())!=raw:raise SystemExit(f'{path.name} differs; regenerate after review')
        else:
            data=bytearray(gzip.compress(raw,compresslevel=9,mtime=0));data[9]=255;path.write_bytes(data)
        print(f'{path.name}: '+('verified' if args.check else 'generated'))

if __name__=='__main__':main()
