/* Bounded nearest-neighbor RGBA -> theme palette, ordered 4x4 dithering.
   Runs once per thumbnail/font/theme change; drawing uses cached palette IDs. */
#ifndef HARDCORE_BROWSER_PIXELS_H
#define HARDCORE_BROWSER_PIXELS_H
#include "hardcore_browser_image_geometry.h"
static void browser_image_size(int width,int height,int cw,int ch,int *cols,int *rows) {
    if(width<1)width=1;if(height<1)height=1;if(cw<1)cw=1;if(ch<1)ch=1;
    double scale=1;
    /* Twice the old maximum in each direction. Smaller originals retain
       their native size, rounded to the nearest complete character cell. */
    if(width>(double)BROWSER_IMAGE_COLUMNS*cw)scale=(double)BROWSER_IMAGE_COLUMNS*cw/width;
    if(height*scale>(double)BROWSER_IMAGE_ROWS*ch)scale=(double)BROWSER_IMAGE_ROWS*ch/height;
    *cols=(int)(width*scale/cw+.5);*rows=(int)(height*scale/ch+.5);
    if(*cols<1)*cols=1;if(*cols>BROWSER_IMAGE_COLUMNS)*cols=BROWSER_IMAGE_COLUMNS;
    if(*rows<1)*rows=1;if(*rows>BROWSER_IMAGE_ROWS)*rows=BROWSER_IMAGE_ROWS;
}
static void browser_image_quantize(const unsigned char *rgba,int sw,int sh,unsigned char *out,int w,int h,
    const uint32_t *palette,int count,uint32_t background) {
    static const int bayer[]={0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        const unsigned char *p=rgba+4*((y*sh/h)*sw+x*sw/w);
        if(!p[3]) {int exact=0;for(int i=0;i<count;i++)if(palette[i]==background){exact=i;break;}out[y*w+x]=(unsigned char)exact;continue;}
        int channels[3],a=p[3],d=(bayer[(y&3)*4+(x&3)]*2-15)*2;
        for(int k=0;k<3;k++) {
            /* Source is premultiplied RGBA. */
            int c=p[k]+((background>>(16-8*k))&255)*(255-a)/255+d;
            channels[k]=c<0?0:c>255?255:c;
        }
        int best=0;long distance=0x7fffffff;
        for(int i=0;i<count;i++) {
            int r=channels[0]-(int)((palette[i]>>16)&255),g=channels[1]-(int)((palette[i]>>8)&255),b=channels[2]-(int)(palette[i]&255);
            long score=3*r*r+6*g*g+b*b;
            if(score<distance){distance=score;best=i;}
        }
        out[y*w+x]=(unsigned char)best;
    }
}
#endif
