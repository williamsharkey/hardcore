#!/usr/bin/env python3
"""Transactional source/style/cursor preservation for sequential TV reflow."""
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
typedef struct {uint32_t ch,fg,bg;uint8_t attr,wflag;uint16_t link;}Cell;
#include "hardcore_reflow.h"
#include "hardcore_tv_reflow.h"
enum{C=60,R=12,A=1*C+25,B=6*C+45,K=6*C+46,D=7*C+4};
typedef struct{int mode,calls;ChromeReflow *old;}Context;
static int ink(const Cell*c,void*ctx){(void)ctx;return c->ch>' '||c->wflag==2||(c->attr&(8|128))||c->link;}
static void allocate(ChromeReflow*out,const Cell*input,int cols,int rows){
    int count=cols*rows;memset(out,0,sizeof(*out));out->cols=cols;out->rows=rows;out->cursor=-1;
    out->cells=malloc(count*sizeof(Cell));out->source=malloc(count*sizeof(int));out->markers=malloc(rows*sizeof(int));
    assert(out->cells&&out->source&&out->markers);memcpy(out->cells,input,count*sizeof(Cell));
    for(int i=0;i<count;i++)out->source[i]=i;for(int i=0;i<rows;i++)out->markers[i]=-1;
}
static int build(ChromeReflow*out,const Cell*input,int cols,int rows,const unsigned char*protect,int cursor,HcTvRect obstacle,void*opaque){
    Context*c=opaque;c->calls++;
    assert(protect[0]&&protect[1]&&protect[2]);if(cursor>=0)assert(protect[cursor/cols]);
    if(c->mode==12)return chrome_reflow_build_checked(out,input,cols,rows,protect,obstacle.x?1:2,
        obstacle.x,obstacle.y,obstacle.w,obstacle.h,3,NULL);
    if(c->mode==8){*out=*c->old;return 1;}
    allocate(out,input,cols,rows);out->changed=1;
    int target=c->mode==1?1*C+5:D;
    out->cells[target]=input[B];out->source[target]=B;
    out->cells[B]=(Cell){.ch=' '};
    if(cursor>=0){out->cells[target+1]=input[cursor];out->source[target+1]=cursor;out->cursor=target+1;}
    if(c->mode==2)out->cells[target]=(Cell){.ch=' '};
    if(c->mode==3){out->cells[target+3]=input[B];out->source[target+3]=B;}
    if(c->mode==4)out->cells[target].fg++;
    if(c->mode==5)out->source[target]=cols*rows;
    if(c->mode==6)out->cursor=cols*rows;
    if(c->mode==7)return 0;
    if(c->mode==9){out->cells[3*C+4]=input[1*C+5];out->source[3*C+4]=1*C+5;out->cells[1*C+5]=(Cell){.ch=' '};}
    return 1;
}
static void fixture(ChromeReflow*out){
    Cell grid[C*R];for(int i=0;i<C*R;i++)grid[i]=(Cell){.ch=' '};
    grid[A]=(Cell){.ch='A',.fg=0x123456,.bg=0x223344,.attr=1,.link=5};
    grid[B]=(Cell){.ch='B',.fg=0x654321,.bg=0x665544,.attr=4,.link=9};
    allocate(out,grid,C,R);out->source[A]=C+2;out->markers[1]=24;out->changed=2;
}
static void ordinary(int mode){
    ChromeReflow display={0};fixture(&display);
    HcTvRect prior={0,0,20,3},next={40,5,20,6};Context ctx={mode,0,&display};
    if(mode==9)display.cells[C+5]=(Cell){.ch='I'};
    Cell before[C*R];int sources[C*R];memcpy(before,display.cells,sizeof(before));memcpy(sources,display.source,sizeof(sources));
    Cell*old_cells=display.cells;int*old_sources=display.source;
    int accepted=hc_tv_reflow_compose(&display,next,&prior,1,K,build,ink,&ctx);
    assert(ctx.calls==1);
    if(mode==0){
        assert(accepted&&display.changed==3&&display.cursor==D+1);
        assert(display.source[A]==C+2&&display.source[D]==B&&display.source[D+1]==K);
        assert(!memcmp(display.cells+D,before+B,sizeof(Cell)));assert(display.markers[1]==24);
    }else{
        assert(!accepted&&display.cells==old_cells&&display.source==old_sources);
        assert(!memcmp(display.cells,before,sizeof(before))&&!memcmp(display.source,sources,sizeof(sources)));
    }
    chrome_reflow_free(&display);
}
static void bounds(void){
    ChromeReflow display={0};fixture(&display);Context ctx={0,0,&display};HcTvRect next={40,5,20,6};
    HcTvRect prior[5]={{0,0,20,3},{0,3,20,1},{0,4,20,1},{0,5,20,1},{0,6,20,1}};
    assert(!hc_tv_reflow_compose(&display,next,prior,5,K,build,ink,&ctx)&&!ctx.calls);
    assert(!hc_tv_reflow_compose(&display,prior[0],prior,1,K,build,ink,&ctx)&&!ctx.calls);
    display.rows=HC_TV_REFLOW_MAX_CELLS;
    assert(!hc_tv_reflow_compose(&display,next,prior,1,K,build,ink,&ctx)&&!ctx.calls);
    display.rows=R;chrome_reflow_free(&display);
}
static void production(void){
    ChromeReflow display={0};fixture(&display);Context ctx={12,0,&display};HcTvRect prior={0,0,20,3},next={40,5,20,6};
    const char*text="This complete sentence can wrap around a second panel safely.";
    for(int x=0;text[x]&&x<C;x++)display.cells[6*C+x]=(Cell){.ch=(unsigned char)text[x]};
    int before_changed=display.changed;
    assert(hc_tv_reflow_compose(&display,next,&prior,1,-1,build,ink,&ctx));
    assert(display.changed>before_changed&&display.source[A]==C+2&&display.markers[1]==24);
    chrome_reflow_free(&display);
}
int main(int argc,char**argv){int n=argc>1?atoi(argv[1]):0;if(n<=9)ordinary(n);else if(n==10)bounds();else production();puts("ok");return 0;}
'''
class Compose(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix="hc-tv-reflow-");p=Path(cls.tmp.name)/"test.c";p.write_text(HARNESS)
        cls.binary=Path(cls.tmp.name)/"test"
        subprocess.run([os.environ.get("CC","cc"),"-std=c99","-Wall","-Wextra","-Werror",*shlex.split(os.environ.get("HC_TEST_CFLAGS","")),"-I",str(ROOT),str(p),"-o",str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def run_case(self,n):subprocess.run([str(self.binary),str(n)],check=True,capture_output=True)
    def test_transitive_source_style_and_blank_cursor(self):self.run_case(0)
    def test_prior_tv_intrusion_rolls_back(self):self.run_case(1)
    def test_loss_duplicate_style_bad_source_and_cursor_roll_back(self):
        for n in (2,3,4,5,6):
            with self.subTest(case=n):self.run_case(n)
    def test_failed_or_aliasing_builder_keeps_existing_allocations(self):
        for n in (7,8):
            with self.subTest(case=n):self.run_case(n)
    def test_existing_prior_intrusion_cannot_be_moved(self):self.run_case(9)
    def test_budget_and_overlap_fail_before_builder(self):self.run_case(10)
    def test_actual_reflow_engine_composes(self):self.run_case(11)
if __name__=="__main__":unittest.main()
