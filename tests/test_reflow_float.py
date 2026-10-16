#!/usr/bin/env python3
"""Intact horizontal status/badge shifts through the production reflow engine."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {uint32_t ch,fg,bg;uint8_t attr,wflag;uint16_t link;} Cell;
#include "hardcore_reflow.h"
#include "hardcore_tv_reflow.h"
static Cell *grid(int cols,int rows) {
    Cell *p=calloc((size_t)cols*rows,sizeof(Cell)); assert(p);
    for(int i=0;i<cols*rows;i++)p[i]=(Cell){.ch=' ',.bg=0x202020};
    return p;
}
static void put(Cell *p,int cols,int y,int x,const char *s) {
    for(int k=0;s[k];k++)p[y*cols+x+k]=(Cell){.ch=(unsigned char)s[k],.fg=0x01000001u+k%15,
        .bg=0x303030,.attr=(uint8_t)(k%8),.link=(uint16_t)(1+k%3)};
}
static int ink(const Cell *c,void *ctx){(void)ctx;return !chrome_float_space(c);}
static int safe(const Cell *c){return c->bg==0x202020;}
static void preserved(const Cell *input,const ChromeReflow *out) {
    int count=out->cols*out->rows,previous=-1;
    unsigned char *seen=calloc((size_t)count,1);assert(seen);
    for(int i=0;i<count;i++) {
        int source=out->source[i];assert(source>=0&&source<count);
        if(ink(out->cells+i,NULL)) {
            assert(ink(input+source,NULL)&&!seen[source]);seen[source]=1;
            assert(!memcmp(out->cells+i,input+source,sizeof(Cell)));
            assert(source>previous);previous=source;
        }
    }
    for(int i=0;i<count;i++)assert(!ink(input+i,NULL)||seen[i]);
    free(seen);
}
static ChromeReflow run(Cell *p,int cols,int rows,unsigned char *protected_rows,
                         int side,int x,int y,int w,int h) {
    ChromeReflow out={0};size_t bytes=(size_t)cols*rows*sizeof(Cell);
    Cell *before=malloc(bytes);assert(before);memcpy(before,p,bytes);
    assert(chrome_reflow_build_checked(&out,p,cols,rows,protected_rows,side,x,y,w,h,3,safe));
    assert(!memcmp(before,p,bytes));preserved(p,&out);free(before);return out;
}
static void span_at(const ChromeReflow *out,const Cell *input,int row,int from,int to,int len) {
    for(int k=0;k<len;k++) {
        int at=row*out->cols+to+k,source=row*out->cols+from+k;
        assert(out->source[at]==source&&!memcmp(out->cells+at,input+source,sizeof(Cell)));
    }
}
static void status(void) {
    const char *text="Weekly limit: 34% left - /status";int n=(int)strlen(text),cols=132,rows=7;
    Cell *p=grid(cols,rows);int from=cols-n-1;
    put(p,cols,3,from,text);put(p,cols,0,2,"unrelated fixed row");
    ChromeReflow out=run(p,cols,rows,NULL,1,104,1,28,5);
    assert(out.changed==1);span_at(&out,p,3,from,102-n,n);
    for(int y=0;y<rows;y++)if(y!=3)assert(!memcmp(out.cells+y*cols,p+y*cols,cols*sizeof(Cell)));
    for(int x=102;x<cols;x++)assert(chrome_float_space(out.cells+3*cols+x));
    /* Moving the same status around a narrow secondary surface in mid-row
       must use that actual surface edge, not the primary menu's width. */
    chrome_reflow_free(&out);out=run(p,cols,rows,NULL,1,112,3,18,1);
    assert(out.changed==1);span_at(&out,p,3,from,110-n,n);
    chrome_reflow_free(&out);free(p);
}
static void flush_and_no_fit(void) {
    const char *text="Weekly limit: 34% left - /status";int n=(int)strlen(text),cols=80;
    Cell *p=grid(cols,3);put(p,cols,1,cols-n,text);
    ChromeReflow out=run(p,cols,3,NULL,1,n,1,cols-n,1);
    assert(out.changed==1&&out.markers[1]==-1);span_at(&out,p,1,cols-n,0,n);
    chrome_reflow_free(&out);out=run(p,cols,3,NULL,1,n-1,1,cols-n+1,1);
    assert(!out.changed&&!memcmp(out.cells,p,cols*3*sizeof(Cell)));
    chrome_reflow_free(&out);out=run(p,cols,3,NULL,2,0,1,12,1);
    assert(!out.changed&&!memcmp(out.cells,p,cols*3*sizeof(Cell)));
    chrome_reflow_free(&out);free(p);
}
static void badge(void) {
    int cols=140,rows=5,left=70;Cell *p=grid(cols,rows);
    const char *tail=" Back to bottom ";int x=left;
    p[2*cols+x++]=(Cell){.ch=0x2193,.fg=0x123456,.bg=0x404040};
    for(int k=0;tail[k];k++)p[2*cols+x++]=(Cell){.ch=(unsigned char)tail[k],.bg=0x404040};
    p[2*cols+x++]=(Cell){.ch=0xb7,.bg=0x404040};
    for(const char *s=" esc";*s;s++)p[2*cols+x++]=(Cell){.ch=(unsigned char)*s,.bg=0x404040};
    p[2*cols+left-1].bg=p[2*cols+x].bg=0x404040;int len=x-left+2;
    ChromeReflow out=run(p,cols,rows,NULL,1,90,1,28,3);
    assert(out.changed==1);span_at(&out,p,2,left-1,88-len,len);
    chrome_reflow_free(&out);out=run(p,cols,rows,NULL,2,60,1,28,3);
    assert(out.changed==1);span_at(&out,p,2,left-1,90,len);
    chrome_reflow_free(&out);
    /* Earlier paragraph reflow can occupy one of the two gutter cells. A
       flush fit still preserves the whole badge and the borrowed text. */
    out=run(p,cols,rows,NULL,1,90,0,28,1);
    int flush=90-len, occupied=flush-1;
    p[cols+1]=(Cell){.ch='X',.fg=0x123456,.bg=0x202020};
    out.cells[2*cols+occupied]=p[cols+1];out.source[2*cols+occupied]=cols+1;
    assert(chrome_float_shift(&out,p,2,left-1,x+1,1,90,28));
    span_at(&out,p,2,left-1,flush,len);preserved(p,&out);
    assert(out.cells[2*cols+occupied].ch=='X');
    chrome_reflow_free(&out);free(p);
}
static void protected_shapes(void) {
    enum{C=100,R=5};const char *text="Weekly limit: 34% left - /status";int n=(int)strlen(text);
    for(int mode=0;mode<7;mode++) {
        Cell *p=grid(C,R);unsigned char protect[R]={0};put(p,C,2,C-n,text);
        if(mode==0)protect[2]=1; /* live cursor, panel, or earlier TV row */
        if(mode==1){put(p,C,1,2,"```");put(p,C,3,2,"```");}
        if(mode==2)put(p,C,1,C-n,"aligned previous table row");
        if(mode==3)p[2*C+C-n+4].ch='|';
        if(mode==4){p[2*C+C-n+4].ch=' ';p[2*C+C-n+5].ch=' ';p[2*C+C-n+4].link=p[2*C+C-n+5].link=0;}
        if(mode==5)p[2*C+C-n].wflag=2; /* orphan continuation */
        if(mode==6)p[2*C+C-n].attr=64; /* hidden terminal content */
        ChromeReflow out=run(p,C,R,protect,1,72,2,28,1);
        assert(!memcmp(out.cells,p,C*R*sizeof(Cell)));
        chrome_reflow_free(&out);free(p);
    }
}
static int build(ChromeReflow*out,const Cell*input,int cols,int rows,const unsigned char*protect,
                  int cursor,HcTvRect r,void*ctx) {
    (void)cursor;(void)ctx;
    return chrome_reflow_build_checked(out,input,cols,rows,protect,r.x?1:2,r.x,r.y,r.w,r.h,3,safe);
}
static void tv_composition(void) {
    enum{C=120,R=18};Cell*p=grid(C,R);const char*text="Weekly limit: 34% left - /status";int n=(int)strlen(text);
    put(p,C,11,C-n,text);
    ChromeReflow out=run(p,C,R,NULL,2,0,0,28,4);
    HcTvRect prior={0,0,28,4},next={96,9,24,6};
    assert(hc_tv_reflow_compose(&out,next,&prior,1,-1,build,ink,NULL));
    assert(out.changed==1);preserved(p,&out);span_at(&out,p,11,C-n,94-n,n);
    chrome_reflow_free(&out);
    /* A later TV cannot move text back over a previously placed surface. */
    out=run(p,C,R,NULL,2,0,0,28,4);prior=(HcTvRect){0,10,20,2};
    assert(hc_tv_reflow_compose(&out,next,&prior,1,-1,build,ink,NULL));
    assert(!out.changed&&!memcmp(out.cells,p,C*R*sizeof(Cell)));
    chrome_reflow_free(&out);free(p);
}
static uint32_t rng=0x5398a7;
static unsigned next(void){rng=rng*1664525u+1013904223u;return rng;}
static void randomized(void) {
    for(int k=0;k<1000;k++) {
        int cols=80+(int)(next()%121),n=12+(int)(next()%20),row=1+(int)(next()%3),rows=5;
        int right=cols-(int)(next()%3),from=right-n,width=18+(int)(next()%35),dock=cols-width;
        Cell*p=grid(cols,rows);
        for(int i=0;i<n;i++)p[row*cols+from+i]=(Cell){.ch=(uint32_t)('a'+next()%26),
            .fg=next(),.bg=next(),.attr=(uint8_t)(next()%8),.link=(uint16_t)next()};
        if(k%2==0){p[row*cols+from+3].ch=0x754c;p[row*cols+from+3].wflag=1;
            p[row*cols+from+4].ch=0;p[row*cols+from+4].wflag=2;}
        ChromeReflow out=run(p,cols,rows,NULL,1,dock,0,width,rows);
        assert(out.changed==1);span_at(&out,p,row,from,dock-2-n,n);
        chrome_reflow_free(&out);free(p);
    }
}
int main(int argc,char**argv){int scenario=argc>1?atoi(argv[1]):0;
    if(scenario==0)status();else if(scenario==1)flush_and_no_fit();else if(scenario==2)badge();
    else if(scenario==3)protected_shapes();else if(scenario==4)tv_composition();else randomized();
    puts("PASS horizontal status reflow");return 0;}
'''


class FloatReflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='hc-float-reflow-')
        source=Path(cls.temp.name)/'test.c';source.write_text(HARNESS)
        cls.binary=Path(cls.temp.name)/'test'
        subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c99','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT),str(source),'-o',str(cls.binary)],check=True)

    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()

    def scenario(self,n):subprocess.run([str(self.binary),str(n)],check=True,capture_output=True,timeout=20)
    def test_right_aligned_styled_status(self):self.scenario(0)
    def test_flush_and_insufficient_room(self):self.scenario(1)
    def test_centered_painted_badge(self):self.scenario(2)
    def test_fixed_cursor_table_code_and_hidden_rows(self):self.scenario(3)
    def test_secondary_tv_and_prior_obstacle(self):self.scenario(4)
    def test_thousand_random_styled_and_wide_spans(self):self.scenario(5)


if __name__=='__main__':unittest.main()
