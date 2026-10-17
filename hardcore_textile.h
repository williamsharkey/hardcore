/* Color images made from ordinary terminal characters. No graphics protocol,
 * allocation, platform API or mutable global state is required.
 *
 * The farthest-pair, two-color cell quantizer follows William Sharkey's SL-TUI:
 * https://github.com/williamsharkey/sl-tui/blob/6f0992258bc2d8bc0ad584c9ae54cff183eba281/server/pixel-to-cells.ts
 * This implementation adds 8-dot Braille, quadrant/half-block modes, bounded
 * resampling, proper partial-alpha compositing and centroid refinement.
 *
 * Copyright (c) 2025, 2026 William Sharkey
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef HARDCORE_TEXTILE_H
#define HARDCORE_TEXTILE_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>

typedef struct { uint32_t ch, fg, bg; } HcTextileCell;
enum {
    HC_TEXTILE_BRAILLE = 0, /* 2 x 4 dots; widely supported Unicode */
    HC_TEXTILE_SEXTANT = 1, /* 2 x 3 solid tiles; requires U+1FB00 glyphs */
    HC_TEXTILE_QUADRANT = 2, /* 2 x 2 solid tiles; older fonts */
    HC_TEXTILE_HALF = 3 /* 1 x 2 solid tiles; broadest font compatibility */
};

static inline int hc_textile_cell_pixels(int mode, int *width, int *height) {
    if (mode < HC_TEXTILE_BRAILLE || mode > HC_TEXTILE_HALF) return 0;
    if (width) *width = mode == HC_TEXTILE_HALF ? 1 : 2;
    if (height) *height = mode == HC_TEXTILE_BRAILLE ? 4 : mode == HC_TEXTILE_SEXTANT ? 3 : 2;
    return 1;
}

typedef struct { int r, g, b; } HcTextileRgb;
static inline uint32_t hc_textile_pack(HcTextileRgb a) {
    return ((uint32_t)a.r << 16) | ((uint32_t)a.g << 8) | (uint32_t)a.b;
}
static inline HcTextileRgb hc_textile_unpack(uint32_t color) {
    HcTextileRgb a = {(int)((color >> 16) & 255), (int)((color >> 8) & 255), (int)(color & 255)};
    return a;
}
static inline int hc_textile_distance(HcTextileRgb a, HcTextileRgb b) {
    int r = a.r - b.r, g = a.g - b.g, bl = a.b - b.b;
    return r*r + g*g + bl*bl;
}
static inline int hc_textile_luma(HcTextileRgb a) { return 299*a.r + 587*a.g + 114*a.b; }

/* Straight RGBA8, composited before filtering so invisible RGB does not bleed
 * into image edges. Coordinates are clamped before any pointer arithmetic. */
static inline HcTextileRgb hc_textile_pixel(const unsigned char *rgba, int stride,
                                           int x, int y, HcTextileRgb background) {
    const unsigned char *p = rgba + (size_t)y * (size_t)stride + (size_t)x * 4;
    int a = p[3], inv = 255 - a;
    HcTextileRgb c = {(p[0]*a + background.r*inv + 127)/255,
                      (p[1]*a + background.g*inv + 127)/255,
                      (p[2]*a + background.b*inv + 127)/255};
    return c;
}
static inline HcTextileRgb hc_textile_bilinear(const unsigned char *rgba,
                                              int width, int height, int stride,
                                              int64_t x16, int64_t y16,
                                              HcTextileRgb background) {
    int64_t maxx = (int64_t)(width-1) * 65536, maxy = (int64_t)(height-1) * 65536;
    if (x16 < 0) x16 = 0;
    if (y16 < 0) y16 = 0;
    if (x16 > maxx) x16 = maxx;
    if (y16 > maxy) y16 = maxy;
    int x = (int)(x16 >> 16), y = (int)(y16 >> 16);
    int nx = x + (x+1 < width), ny = y + (y+1 < height);
    int fx = (int)(x16 & 65535), fy = (int)(y16 & 65535);
    HcTextileRgb a = hc_textile_pixel(rgba,stride,x,y,background);
    if (!fx && !fy) return a;
    HcTextileRgb b = hc_textile_pixel(rgba,stride,nx,y,background);
    HcTextileRgb c = hc_textile_pixel(rgba,stride,x,ny,background);
    HcTextileRgb d = hc_textile_pixel(rgba,stride,nx,ny,background);
    uint64_t w0 = (uint64_t)(65536-fx)*(65536-fy), w1 = (uint64_t)fx*(65536-fy);
    uint64_t w2 = (uint64_t)(65536-fx)*fy, w3 = (uint64_t)fx*fy;
    HcTextileRgb result = {
        (int)((w0*a.r+w1*b.r+w2*c.r+w3*d.r+UINT64_C(2147483648)) >> 32),
        (int)((w0*a.g+w1*b.g+w2*c.g+w3*d.g+UINT64_C(2147483648)) >> 32),
        (int)((w0*a.b+w1*b.b+w2*c.b+w3*d.b+UINT64_C(2147483648)) >> 32)};
    return result;
}

/* At most 4x4 stratified bilinear samples per output subpixel, independent of
 * source size. Upscaling uses one bilinear sample. This is deliberately
 * deterministic: no frame-dependent dithering that would shimmer on webcams. */
static inline HcTextileRgb hc_textile_sample(const unsigned char *rgba,
                                            int width, int height, int stride,
                                            int x, int y, int gridw, int gridh,
                                            HcTextileRgb background) {
    int sx = (width + gridw - 1)/gridw, sy = (height + gridh - 1)/gridh;
    if (sx > 4) sx = 4;
    if (sy > 4) sy = 4;
    HcTextileRgb sum = {0,0,0};
    for (int j=0;j<sy;j++) for (int i=0;i<sx;i++) {
        int64_t x16 = ((2*(int64_t)x*sx+2*i+1)*width*32768)/((int64_t)gridw*sx)-32768;
        int64_t y16 = ((2*(int64_t)y*sy+2*j+1)*height*32768)/((int64_t)gridh*sy)-32768;
        HcTextileRgb a = hc_textile_bilinear(rgba,width,height,stride,x16,y16,background);
        sum.r+=a.r; sum.g+=a.g; sum.b+=a.b;
    }
    int n=sx*sy;
    sum.r=(sum.r+n/2)/n; sum.g=(sum.g+n/2)/n; sum.b=(sum.b+n/2)/n;
    return sum;
}

static inline uint32_t hc_textile_glyph(unsigned pattern, int mode) {
    if (mode == HC_TEXTILE_BRAILLE) {
        /* Row-major samples -> Unicode dot numbering (1,4 / 2,5 / 3,6 / 7,8). */
        static const unsigned bits[8] = {1,8,2,16,4,32,64,128};
        unsigned dots=0;
        for (int i=0;i<8;i++) if (pattern & (1u<<i)) dots |= bits[i];
        return 0x2800u+dots;
    }
    if (mode == HC_TEXTILE_SEXTANT) {
        if (!pattern) return ' ';
        if (pattern==63) return 0x2588;
        if (pattern==21) return 0x258c;
        if (pattern==42) return 0x2590;
        return 0x1fb00u+pattern-1-(pattern>21)-(pattern>42);
    }
    if (mode == HC_TEXTILE_HALF) {
        static const uint32_t half[4] = {' ',0x2580,0x2584,0x2588};
        return half[pattern & 3];
    }
    static const uint32_t quad[16] = {' ',0x2598,0x259d,0x2580,0x2596,0x258c,0x259e,0x259b,
                                     0x2597,0x259a,0x2590,0x259c,0x2584,0x2599,0x259f,0x2588};
    return quad[pattern & 15];
}

/* Recover the rectangular color regions of a rendered cell. Braille dots are
 * treated as filled subpixels here, rather than reproducing host glyph gaps;
 * this lets a smaller TV resample the image instead of cropping its text grid.
 * Bits are row-major. Returns zero for unrelated text glyphs. */
static inline int hc_textile_cell_mask(uint32_t ch, unsigned *mask, int *width, int *height) {
    unsigned bits=0; int w=2,h=2;
    if (ch==0 || ch==' ') bits=0;
    else if (ch>=0x2800 && ch<=0x28ff) {
        static const unsigned dots[8]={1,8,2,16,4,32,64,128};
        for (int i=0;i<8;i++) if ((ch-0x2800)&dots[i]) bits |= 1u<<i;
        h=4;
    } else if (ch>=0x1fb00 && ch<=0x1fb3b) {
        bits=ch-0x1fb00+1;
        if (bits>=21) bits++;
        if (bits>=42) bits++;
        h=3;
    } else {
        static const uint32_t quad[16]={' ',0x2598,0x259d,0x2580,0x2596,0x258c,0x259e,0x259b,
                                       0x2597,0x259a,0x2590,0x259c,0x2584,0x2599,0x259f,0x2588};
        int found=0;
        for (int i=1;i<16;i++) if (quad[i]==ch) { bits=(unsigned)i; found=1; break; }
        if (!found) return 0;
    }
    if (mask) *mask=bits;
    if (width) *width=w;
    if (height) *height=h;
    return 1;
}

/* Sample a previously encoded cell on a normalized 2 x sample_rows grid.
 * Unknown text glyphs use foreground; invalid sample_rows uses background.
 * x/y are clamped. This is pure geometry and does not consult a font. */
static inline uint32_t hc_textile_sample_cell(HcTextileCell cell, int x, int y, int sample_rows) {
    unsigned mask; int w,h;
    if (sample_rows<=0) return cell.bg;
    if (!hc_textile_cell_mask(cell.ch,&mask,&w,&h)) return cell.fg;
    if (x<0) x=0; else if (x>1) x=1;
    if (y<0) y=0; else if (y>=sample_rows) y=sample_rows-1;
    int sy=(int)((int64_t)y*h/sample_rows), sx=x*w/2;
    return (mask & (1u<<(sy*w+sx))) ? cell.fg : cell.bg;
}

static inline HcTextileCell hc_textile_quantize(const HcTextileRgb *samples, int n, int mode) {
    int farthest=-1, first=0, second=0;
    for (int i=0;i<n;i++) for (int j=i+1;j<n;j++) {
        int d=hc_textile_distance(samples[i],samples[j]);
        if (d>farthest) { farthest=d; first=i; second=j; }
    }
    if (!farthest) {
        uint32_t color=hc_textile_pack(samples[0]);
        HcTextileCell flat={' ',color,color}; return flat;
    }
    HcTextileRgb centers[2]={samples[first],samples[second]};
    unsigned pattern=0;
    for (int pass=0;pass<2;pass++) {
        HcTextileRgb sums[2]={{0,0,0},{0,0,0}};
        int count[2]={0,0}; pattern=0;
        for (int i=0;i<n;i++) {
            int group=hc_textile_distance(samples[i],centers[1]) < hc_textile_distance(samples[i],centers[0]);
            sums[group].r+=samples[i].r; sums[group].g+=samples[i].g; sums[group].b+=samples[i].b;
            count[group]++; if (!group) pattern |= 1u<<i;
        }
        for (int g=0;g<2;g++) if (count[g]) {
            centers[g].r=(sums[g].r+count[g]/2)/count[g];
            centers[g].g=(sums[g].g+count[g]/2)/count[g];
            centers[g].b=(sums[g].b+count[g]/2)/count[g];
        }
    }
    /* Bright dots on the darker cell color read well with sparse Braille fonts.
     * A deterministic tie break keeps equal-luminance colored edges stable. */
    int l0=hc_textile_luma(centers[0]), l1=hc_textile_luma(centers[1]);
    uint32_t fg=hc_textile_pack(centers[0]), bg=hc_textile_pack(centers[1]);
    if (l0<l1 || (l0==l1 && fg<bg)) {
        uint32_t swap=fg; fg=bg; bg=swap; pattern ^= (1u<<n)-1;
    }
    if (fg==bg) pattern=0;
    HcTextileCell result={pattern ? hc_textile_glyph(pattern,mode) : ' ',fg,bg};
    return result;
}

/* Render straight RGBA8 (top row first) into exactly cols*rows cells, each
 * color packed as 0xRRGGBB. The caller owns both buffers and must ensure their
 * sizes are height*stride and cols*rows*sizeof(HcTextileCell), respectively.
 * Stride is positive and may contain row padding. Buffers must not overlap.
 *
 * The full input is resampled into the supplied cell rectangle; the caller
 * selects/letterboxes that rectangle using the HOST font's cell aspect ratio
 * (normally about 1:2). Do not infer physical aspect ratio from Braille dots.
 *
 * Returns 1 on success, 0 on invalid/oversized geometry without touching out.
 * Limits: 32768 pixels per source axis, 8192 cells per output axis, 1M cells.
 * Fixed work and stack space per cell; large source frames cannot turn a tiny
 * preview into a source-sized scan. */
static inline int hc_textile_render(const unsigned char *rgba,
                                   int width, int height, int stride,
                                   HcTextileCell *out, int cols, int rows,
                                   int mode, uint32_t background) {
    int cw,ch;
    if (!rgba || !out || width<=0 || height<=0 || width>32768 || height>32768 ||
        cols<=0 || rows<=0 || cols>8192 || rows>8192 ||
        (size_t)cols*(size_t)rows>1048576 || stride<=0 || (int64_t)stride<(int64_t)width*4 ||
        (uint64_t)stride*(uint64_t)height>(uint64_t)PTRDIFF_MAX ||
        !hc_textile_cell_pixels(mode,&cw,&ch)) return 0;
    HcTextileRgb base=hc_textile_unpack(background);
    int gridw=cols*cw, gridh=rows*ch;
    for (int row=0;row<rows;row++) for (int col=0;col<cols;col++) {
        HcTextileRgb samples[8];
        for (int y=0;y<ch;y++) for (int x=0;x<cw;x++)
            samples[y*cw+x]=hc_textile_sample(rgba,width,height,stride,col*cw+x,row*ch+y,gridw,gridh,base);
        out[(size_t)row*(size_t)cols+col]=hc_textile_quantize(samples,cw*ch,mode);
    }
    return 1;
}
#endif
