/* Pixel-to-terminal tests, independent of Cocoa and the terminal engine. */
#include "../hardcore_textile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static unsigned random_state=0x619ce8a;
static unsigned rng(void) { random_state ^= random_state<<13; random_state ^= random_state>>17; return random_state ^= random_state<<5; }
static void setpixel(unsigned char *p, uint32_t rgb, int alpha) {
    p[0]=(unsigned char)(rgb>>16); p[1]=(unsigned char)(rgb>>8); p[2]=(unsigned char)rgb; p[3]=(unsigned char)alpha;
}

static void patterns(void) {
    unsigned char image[32]; HcTextileCell out;
    static const unsigned braille[8]={1,8,2,16,4,32,64,128};
    for (int mode=0;mode<=HC_TEXTILE_HALF;mode++) {
        int w,h; CHECK(hc_textile_cell_pixels(mode,&w,&h));
        int n=w*h;
        for (unsigned pattern=0;pattern<(1u<<n);pattern++) {
            for (int i=0;i<n;i++) setpixel(image+4*i,(pattern&(1u<<i))?0xffffff:0,255);
            CHECK(hc_textile_render(image,w,h,w*4,&out,1,1,mode,0));
            for (int y=0;y<h;y++) for (int x=0;x<w;x++) {
                uint32_t actual=hc_textile_sample_cell(out,x*(2/w),y,h);
                CHECK(actual==((pattern&(1u<<(y*w+x)))?0xffffffu:0));
            }
            if (mode==HC_TEXTILE_BRAILLE && pattern && pattern<255) {
                unsigned dots=0; for (int i=0;i<8;i++) if (pattern&(1u<<i)) dots |= braille[i];
                CHECK(out.ch==0x2800+dots);
            }
        }
    }
    CHECK(hc_textile_glyph(21,HC_TEXTILE_SEXTANT)==0x258c);
    CHECK(hc_textile_glyph(42,HC_TEXTILE_SEXTANT)==0x2590);
    CHECK(hc_textile_glyph(1,HC_TEXTILE_SEXTANT)==0x1fb00);
    CHECK(hc_textile_glyph(62,HC_TEXTILE_SEXTANT)==0x1fb3b);
    CHECK(!hc_textile_cell_mask('a',NULL,NULL,NULL));
    out=(HcTextileCell){'a',0xabcdef,0x123456};
    CHECK(hc_textile_sample_cell(out,0,0,4)==0xabcdef);
    CHECK(hc_textile_sample_cell(out,0,0,0)==0x123456);
    out=(HcTextileCell){0x2801,0xffffff,0};
    CHECK(hc_textile_sample_cell(out,-1,-3,4)==0xffffff);
    CHECK(hc_textile_sample_cell(out,99,99,4)==0);
}

static void color_and_alpha(void) {
    unsigned char image[64],padded[128]; HcTextileCell a,b;
    for (int i=0;i<8;i++) setpixel(image+i*4,0xff0000,128);
    CHECK(hc_textile_render(image,2,4,8,&a,1,1,HC_TEXTILE_BRAILLE,0x0000ff));
    CHECK(a.ch==' ' && a.bg==0x80007f && a.fg==a.bg);
    for (int i=0;i<8;i++) setpixel(image+i*4,0xff11ff,0);
    CHECK(hc_textile_render(image,2,4,8,&a,1,1,HC_TEXTILE_BRAILLE,0x192a3b));
    CHECK(a.ch==' ' && a.bg==0x192a3b && a.fg==a.bg);
    for (int y=0;y<4;y++) for (int x=0;x<2;x++) setpixel(image+y*8+x*4,y<2?0xabcdef:0x123456,255);
    memset(padded,0xff,sizeof(padded));
    for (int y=0;y<4;y++) memcpy(padded+y*32,image+y*8,8);
    CHECK(hc_textile_render(image,2,4,8,&a,1,1,HC_TEXTILE_BRAILLE,0));
    CHECK(hc_textile_render(padded,2,4,32,&b,1,1,HC_TEXTILE_BRAILLE,0));
    CHECK(!memcmp(&a,&b,sizeof(a)));
    CHECK(hc_textile_sample_cell(a,0,0,4)==0xabcdef);
    CHECK(hc_textile_sample_cell(a,0,3,4)==0x123456); /* top/bottom stay upright */
    for (int i=0;i<16;i++) setpixel(image+i*4,((i/4+i%4)&1)?0xffffff:0,255);
    CHECK(hc_textile_render(image,4,4,16,&a,1,1,HC_TEXTILE_QUADRANT,0));
    CHECK(a.ch==' ' && a.bg==0x808080); /* box sampling averages a checkerboard */
    setpixel(image,0xff0000,255); setpixel(image+4,0x00ff00,0);
    HcTextileCell enlarged[16];
    CHECK(hc_textile_render(image,2,1,8,enlarged,8,2,HC_TEXTILE_BRAILLE,0));
    for (int i=0;i<16;i++) CHECK(!(enlarged[i].fg&0x00ffff) && !(enlarged[i].bg&0x00ffff));
}

static void invalid_geometry(void) {
    unsigned char image[32]={0}; HcTextileCell a={123,456,789},original=a;
    CHECK(!hc_textile_render(NULL,2,4,8,&a,1,1,0,0));
    CHECK(!hc_textile_render(image,2,4,8,NULL,1,1,0,0));
    CHECK(!hc_textile_render(image,0,4,8,&a,1,1,0,0));
    CHECK(!hc_textile_render(image,2,-1,8,&a,1,1,0,0));
    CHECK(!hc_textile_render(image,2,4,7,&a,1,1,0,0));
    CHECK(!hc_textile_render(image,2,4,-8,&a,1,1,0,0));
    CHECK(!hc_textile_render(image,INT_MAX,4,8,&a,1,1,0,0));
    CHECK(!hc_textile_render(image,2,INT_MAX,8,&a,1,1,0,0));
    CHECK(!hc_textile_render(image,2,4,8,&a,0,1,0,0));
    CHECK(!hc_textile_render(image,2,4,8,&a,1,-1,0,0));
    CHECK(!hc_textile_render(image,2,4,8,&a,INT_MAX,1,0,0));
    CHECK(!hc_textile_render(image,2,4,8,&a,8192,8192,0,0));
    CHECK(!hc_textile_render(image,2,4,8,&a,1,1,-1,0));
    CHECK(!hc_textile_render(image,2,4,8,&a,1,1,4,0));
    CHECK(!memcmp(&a,&original,sizeof(a)));
}

/* Refining cluster means must never increase squared reconstruction error
 * over SL-TUI's initial farthest-seed partition. This checks color fidelity,
 * rather than duplicating the implementation's output values. */
static void quantizer_error(void) {
    for (int trial=0;trial<10000;trial++) {
        HcTextileRgb p[8];
        for (int i=0;i<8;i++) p[i]=hc_textile_unpack(rng());
        HcTextileCell cell=hc_textile_quantize(p,8,HC_TEXTILE_BRAILLE);
        int largest=-1,left=0,right=0;
        for (int i=0;i<8;i++) for (int j=i+1;j<8;j++) {
            int d=hc_textile_distance(p[i],p[j]); if (d>largest) { largest=d; left=i; right=j; }
        }
        int groups[8],count[2]={0,0}; HcTextileRgb means[2]={{0,0,0},{0,0,0}};
        for (int i=0;i<8;i++) {
            int g=groups[i]=hc_textile_distance(p[i],p[right])<hc_textile_distance(p[i],p[left]);
            means[g].r+=p[i].r; means[g].g+=p[i].g; means[g].b+=p[i].b; count[g]++;
        }
        for (int g=0;g<2;g++) if (count[g]) {
            means[g].r=(means[g].r+count[g]/2)/count[g];
            means[g].g=(means[g].g+count[g]/2)/count[g];
            means[g].b=(means[g].b+count[g]/2)/count[g];
        }
        int before=0,after=0;
        for (int i=0;i<8;i++) {
            before+=hc_textile_distance(p[i],means[groups[i]]);
            after+=hc_textile_distance(p[i],hc_textile_unpack(hc_textile_sample_cell(cell,i%2,i/2,4)));
        }
        CHECK(after<=before);
    }
}

static void varied_geometry(void) {
    for (int trial=0;trial<300;trial++) {
        int w=1+(int)(rng()%117),h=1+(int)(rng()%101),stride=w*4+(int)(rng()%29);
        int cols=1+(int)(rng()%31),rows=1+(int)(rng()%17),mode=(int)(rng()%4);
        unsigned char *image=malloc((size_t)stride*h);
        HcTextileCell *a=malloc(((size_t)cols*rows+2)*sizeof(*a)),*b=malloc((size_t)cols*rows*sizeof(*b));
        CHECK(image && a && b);
        for (int i=0;i<stride*h;i++) image[i]=(unsigned char)rng();
        a[0]=a[cols*rows+1]=(HcTextileCell){0xdeadbeef,0x12345678,0x90abcdef};
        CHECK(hc_textile_render(image,w,h,stride,a+1,cols,rows,mode,0x283828));
        CHECK(hc_textile_render(image,w,h,stride,b,cols,rows,mode,0x283828));
        CHECK(!memcmp(a+1,b,(size_t)cols*rows*sizeof(*b))); /* repeat frames don't shimmer */
        CHECK(!memcmp(a,a+cols*rows+1,sizeof(*a)) && a[0].ch==0xdeadbeef);
        for (int i=1;i<=cols*rows;i++) {
            CHECK(a[i].fg<=0xffffff && a[i].bg<=0xffffff);
            CHECK(hc_textile_cell_mask(a[i].ch,NULL,NULL,NULL));
        }
        free(image);free(a);free(b);
    }
}

static void benchmark(void) {
    int w=640,h=480,cols=80,rows=24,n=100;
    unsigned char *image=malloc((size_t)w*h*4); HcTextileCell *out=malloc((size_t)cols*rows*sizeof(*out));
    CHECK(image && out);
    for (int i=0;i<w*h;i++) setpixel(image+i*4,rng(),255);
    clock_t start=clock();
    for (int i=0;i<n;i++) CHECK(hc_textile_render(image,w,h,w*4,out,cols,rows,HC_TEXTILE_BRAILLE,0));
    double ms=1000.0*(clock()-start)/CLOCKS_PER_SEC/n;
    printf("640x480 -> 80x24 Braille: %.3f ms/frame (CPU, %d frames)\n",ms,n);
    free(image);free(out);
}
int main(int argc,char **argv) {
    patterns();color_and_alpha();invalid_geometry();quantizer_error();varied_geometry();
    if (argc>1 && !strcmp(argv[1],"--bench")) benchmark();
    printf("PASS: %d textile checks\n",checks); return 0;
}
