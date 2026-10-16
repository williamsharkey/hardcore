#!/usr/bin/env python3
"""Bounded pure-grid TV placement and deterministic idle/fade clock tests."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "hardcore_tv.h"
typedef struct {
    HcTvRect primary, blocked[HC_TV_MAX_SLOTS], old;
    int cols,rows,blocked_count,calls,mode;
} Context;
static int occupancy(HcTvRect rect,HcTvCost *cost,void *opaque) {
    Context *c=opaque;c->calls++;
    assert(c->calls<=HC_TV_MAX_CANDIDATES);
    assert(hc_tv_rect_fits(rect,c->cols,c->rows));
    assert(!hc_tv_rect_overlap(rect,c->primary));
    for(int i=0;i<c->blocked_count;i++)if(hc_tv_rect_overlap(rect,c->blocked[i]))return 0;
    if(c->mode==1&&hc_tv_rect_equal(rect,c->old))*cost=(HcTvCost){1,9,0};
    if(c->mode==2)*cost=hc_tv_rect_equal(rect,c->old)?(HcTvCost){2,20,0}:(HcTvCost){1,19,0};
    if(c->mode==3&&hc_tv_rect_equal(rect,c->old))cost->reflow_cells=50;
    if(c->mode==4&&hc_tv_rect_equal(rect,c->old))cost->reflow_cells=80;
    if(c->mode==5)cost->collision_rows=-1;
    return 1;
}
static HcTvPlacement place(Context *c,int w,int h,HcTvPlacement *old) {
    c->calls=0;
    HcTvPlacement out=hc_tv_place(c->cols,c->rows,c->primary,w,h,old,occupancy,c);
    assert(out.candidates==c->calls&&out.candidates<=HC_TV_MAX_CANDIDATES);
    if(out.valid)assert(hc_tv_rect_fits(out.rect,c->cols,c->rows)&&!hc_tv_rect_overlap(out.rect,c->primary));
    return out;
}
static void anchors(void) {
    Context c={.cols=100,.rows=40,.primary={72,3,28,8}};
    HcTvPlacement out=place(&c,28,10,NULL);
    assert(out.valid&&out.rank==HC_TV_BELOW_GAP&&out.rect.x==72&&out.rect.y==13&&!out.joined);
    c.rows=21;out=place(&c,28,10,NULL);
    assert(out.valid&&out.rank==HC_TV_BELOW_JOINED&&out.rect.y==11&&out.joined);
    c.rows=40;c.primary.y=24;out=place(&c,28,10,NULL);
    assert(out.valid&&out.rank==HC_TV_ABOVE_GAP&&out.rect.y==12);
    c.rows=30;c.primary=(HcTvRect){72,10,28,15};out=place(&c,28,12,NULL);
    assert(out.valid&&out.rank==HC_TV_OPPOSITE_ALIGNED&&out.rect.x==0&&out.rect.y==10);
    c.blocked[0]=out.rect;c.blocked_count=1;out=place(&c,28,12,NULL);
    assert(out.valid&&out.rank==HC_TV_CENTER&&out.rect.x==36);
    c.rows=10;c.primary=(HcTvRect){0,0,100,10};c.blocked_count=0;out=place(&c,28,5,NULL);
    assert(!out.valid&&out.candidates==0);
    c.rows=40;c.primary=(HcTvRect){0,3,28,8};out=place(&c,28,10,NULL);
    assert(out.valid&&out.rect.x==0&&out.rect.y==13);
}
static void stability(void) {
    Context c={.cols=100,.rows=40,.primary={72,3,28,8},.old={0,0,28,10}};
    HcTvPlacement old={.rect=c.old,.valid=1},out=place(&c,28,10,&old);
    assert(hc_tv_rect_equal(out.rect,old.rect));
    c.mode=1;out=place(&c,28,10,&old);assert(!hc_tv_rect_equal(out.rect,old.rect)&&!out.cost.collision_cells);
    c.mode=2;out=place(&c,28,10,&old);assert(hc_tv_rect_equal(out.rect,old.rect));
    c.mode=3;out=place(&c,28,10,&old);assert(hc_tv_rect_equal(out.rect,old.rect));
    c.mode=4;out=place(&c,28,10,&old);assert(!hc_tv_rect_equal(out.rect,old.rect));
    c.mode=5;out=place(&c,28,10,&old);assert(!out.valid);
    c.mode=0;old.rect.x=-1;out=place(&c,28,10,&old);assert(out.valid&&out.rank==HC_TV_BELOW_GAP);
}
static void clock_test(void) {
    HcTvState tv={0};hc_tv_open(&tv,100);
    assert(tv.open&&tv.alpha==1&&hc_tv_seconds_left(&tv,100)==10);
    assert(hc_tv_next_tick_ms(&tv,100)==1000);
    hc_tv_tick(&tv,1100,0);assert(hc_tv_seconds_left(&tv,1100)==9&&tv.phase==HC_TV_VISIBLE);
    hc_tv_tick(&tv,10099,0);assert(hc_tv_seconds_left(&tv,10099)==1&&tv.alpha==1);
    hc_tv_tick(&tv,10100,0);assert(tv.phase==HC_TV_FADING&&tv.alpha==1&&hc_tv_seconds_left(&tv,10100)==0);
    hc_tv_tick(&tv,10250,0);assert(tv.alpha==.5f&&tv.open&&hc_tv_next_tick_ms(&tv,10250)==16);
    hc_tv_tick(&tv,10400,0);assert(!tv.open&&tv.phase==HC_TV_CLOSED&&tv.alpha==0);
    assert(!hc_tv_next_tick_ms(&tv,10400));
    hc_tv_open(&tv,20000);hc_tv_tick(&tv,28000,0);hc_tv_interact(&tv,28100);
    assert(hc_tv_seconds_left(&tv,28100)==10&&tv.alpha==1);
    hc_tv_tick(&tv,38150,0);assert(tv.phase==HC_TV_FADING);hc_tv_interact(&tv,38150);
    assert(tv.phase==HC_TV_VISIBLE&&tv.alpha==1&&hc_tv_seconds_left(&tv,38150)==10);
    hc_tv_close(&tv);hc_tv_interact(&tv,40000);assert(!tv.open);
}
static void hover_test(void) {
    HcTvState tv={0};hc_tv_open(&tv,0);
    hc_tv_tick(&tv,9000,1);assert(tv.idle_ms==9000&&tv.hovered&&tv.alpha==1);
    hc_tv_tick(&tv,50000,1);assert(hc_tv_seconds_left(&tv,50000)==1&&!hc_tv_next_tick_ms(&tv,50000));
    hc_tv_tick(&tv,60000,0);assert(tv.idle_ms==9000&&tv.alpha==1);
    hc_tv_tick(&tv,61000,0);assert(tv.phase==HC_TV_FADING&&tv.alpha==1);
    hc_tv_tick(&tv,61150,1);assert(tv.phase==HC_TV_VISIBLE&&tv.alpha==1&&tv.open);
    hc_tv_tick(&tv,90000,0);assert(tv.open&&tv.phase==HC_TV_FADING&&tv.alpha==1);
    hc_tv_tick(&tv,90300,0);assert(!tv.open);
}
static void wrap_test(void) {
    HcTvState tv={0};uint32_t start=UINT32_MAX-5000;
    hc_tv_open(&tv,start);hc_tv_tick(&tv,start+9000,0);assert(hc_tv_seconds_left(&tv,start+9000)==1);
    hc_tv_tick(&tv,start+10150,0);assert(tv.alpha==.5f);hc_tv_tick(&tv,start+10300,0);assert(!tv.open);
    hc_tv_open(&tv,0);hc_tv_tick(&tv,UINT32_MAX,0);assert(!tv.open);
    Context c={.cols=INT_MAX,.rows=INT_MAX,.primary={INT_MAX-20,INT_MAX-20,20,20}};
    HcTvPlacement out=place(&c,20,20,NULL);assert(out.valid);
}
static uint32_t random_state=0xface1234;
static uint32_t random_next(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static void bounded(void) {
    for(int run=0;run<12000;run++) {
        Context c={0};c.cols=10+random_next()%190;c.rows=4+random_next()%96;
        c.primary.w=1+random_next()%c.cols;c.primary.h=1+random_next()%c.rows;
        c.primary.x=random_next()%(c.cols-c.primary.w+1);c.primary.y=random_next()%(c.rows-c.primary.h+1);
        int width=1+random_next()%c.cols,height=1+random_next()%c.rows;
        HcTvPlacement a=place(&c,width,height,NULL),b=place(&c,width,height,NULL);
        assert(!memcmp(&a,&b,sizeof(a)));
    }
}
static void motion_test(void) {
    HcTvMotion m={0};HcTvRect target={0,10,28,12};
    assert(hc_tv_motion_observe(&m,100,1,1,0)==HC_TV_MOTION_NONE);
    assert(m.alpha==1&&hc_tv_motion_next_ms(&m,100,0)==450);
    assert(hc_tv_motion_observe(&m,549,1,1,0)==HC_TV_MOTION_NONE);
    assert(hc_tv_motion_observe(&m,550,1,1,0)==HC_TV_MOTION_SEARCH);
    assert(!hc_tv_motion_next_ms(&m,550,0));
    assert(hc_tv_motion_observe(&m,551,1,1,0)==HC_TV_MOTION_NONE);
    hc_tv_motion_start(&m,target,550);assert(hc_tv_motion_next_ms(&m,550,0)==16);
    assert(hc_tv_motion_observe(&m,700,1,1,0)==HC_TV_MOTION_NONE&&m.alpha==.5f);
    assert(hc_tv_motion_observe(&m,850,1,1,0)==HC_TV_MOTION_COMMIT&&m.alpha==0);
    assert(hc_tv_rect_equal(m.target,target));
    assert(hc_tv_motion_observe(&m,1000,1,1,0)==HC_TV_MOTION_NONE&&m.alpha==.5f);
    assert(hc_tv_motion_observe(&m,1150,1,1,0)==HC_TV_MOTION_NONE&&m.alpha==1);
    assert(m.phase==HC_TV_MOTION_IDLE&&!hc_tv_motion_next_ms(&m,1150,0));
}
static void motion_cancel_test(void) {
    HcTvMotion m={0};HcTvRect target={0,10,28,12};
    hc_tv_motion_observe(&m,0,1,1,0);
    assert(hc_tv_motion_observe(&m,450,1,1,0)==HC_TV_MOTION_SEARCH);
    hc_tv_motion_start(&m,target,450);
    assert(hc_tv_motion_observe(&m,550,2,1,0)==HC_TV_MOTION_NONE&&m.alpha==1);
    assert(m.phase==HC_TV_MOTION_IDLE&&hc_tv_motion_next_ms(&m,550,0)==2000);
    for(int now=1100;now<2500;now+=550)
        assert(hc_tv_motion_observe(&m,(uint32_t)now,(uint64_t)now,1,0)==HC_TV_MOTION_NONE);
    assert(hc_tv_motion_observe(&m,2650,2200,1,0)==HC_TV_MOTION_SEARCH);
    hc_tv_motion_start(&m,target,2650);
    assert(hc_tv_motion_observe(&m,2700,2200,1,1)==HC_TV_MOTION_NONE&&m.alpha==1);
    assert(!hc_tv_motion_next_ms(&m,2700,1));
    assert(hc_tv_motion_observe(&m,8000,2200,1,1)==HC_TV_MOTION_NONE);
    assert(hc_tv_motion_observe(&m,8001,2200,1,0)==HC_TV_MOTION_NONE);
    assert(hc_tv_motion_observe(&m,8450,2200,1,0)==HC_TV_MOTION_SEARCH);
}
static void motion_context_wrap_test(void) {
    HcTvMotion m={0};HcTvRect target={0,10,28,12};uint32_t start=UINT32_MAX-200;
    hc_tv_motion_observe(&m,start,1,1,0);
    assert(hc_tv_motion_observe(&m,start+450,1,1,0)==HC_TV_MOTION_SEARCH);
    hc_tv_motion_start(&m,target,start+450);
    assert(hc_tv_motion_observe(&m,start+600,1,2,0)==HC_TV_MOTION_NONE&&m.alpha==1);
    assert(hc_tv_motion_observe(&m,start+1050,1,2,0)==HC_TV_MOTION_NONE);
    assert(hc_tv_motion_observe(&m,start+2600,1,2,0)==HC_TV_MOTION_SEARCH);
    hc_tv_motion_start(&m,target,start+2600);
    assert(hc_tv_motion_observe(&m,start+2900,1,2,0)==HC_TV_MOTION_COMMIT);
    assert(hc_tv_motion_observe(&m,start+3200,1,2,0)==HC_TV_MOTION_NONE&&m.alpha==1);
}
int main(int argc,char **argv) {
    int n=argc>1?atoi(argv[1]):0;
    if(n==0)anchors();else if(n==1)stability();else if(n==2)clock_test();else if(n==3)hover_test();else if(n==4)wrap_test();else if(n==5)bounded();
    else if(n==6)motion_test();else if(n==7)motion_cancel_test();else motion_context_wrap_test();
    puts("ok");return 0;
}
'''
class TV(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix="hc-tv-")
        source=Path(cls.tmp.name)/"tv.c";source.write_text(HARNESS)
        cls.binary=Path(cls.tmp.name)/"tv"
        subprocess.run([os.environ.get("CC","cc"),"-std=c99","-Wall","-Wextra","-Werror",
                        *shlex.split(os.environ.get("HC_TEST_CFLAGS","")),"-I",str(ROOT),str(source),"-o",str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check_case(self,n):subprocess.run([str(self.binary),str(n)],check=True,capture_output=True)
    def test_preferred_joined_above_opposite_and_blocked_positions(self):self.check_case(0)
    def test_hysteresis_and_material_improvement(self):self.check_case(1)
    def test_exact_idle_countdown_and_fade(self):self.check_case(2)
    def test_hover_pauses_and_restores_readability(self):self.check_case(3)
    def test_clock_wrap_and_extreme_geometry(self):self.check_case(4)
    def test_fixed_search_budget_and_12000_deterministic_cases(self):self.check_case(5)
    def test_motion_waits_then_commits_once_at_zero_opacity(self):self.check_case(6)
    def test_content_churn_and_hover_cancel_without_repeated_pulses(self):self.check_case(7)
    def test_motion_context_cancellation_and_clock_wrap(self):self.check_case(8)
if __name__=="__main__":unittest.main()
