#!/usr/bin/env python3
"""Exercise the production TV preparation path on an owned, deterministic grid."""
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
#include <string.h>
typedef struct {uint32_t ch,fg,bg;uint8_t attr,wflag;uint16_t link;} Cell;
#include "hardcore_reflow.h"
#include "hardcore_panel_reflow.h"
#include "hardcore_tv.h"
#define SB_COLS 28
#define NFONTS 16
#define W_WIDE 1
#define DEF_BG 0
#define CW 1
#define TIMER_TV 7
typedef struct {int cols,rows,cx,cy,m_cursor_vis,view,hcount;} Term;
typedef struct {int browser_native,id;} Tab;
static Tab tabs[HC_TV_CUTIE_COUNT];static Tab *g_tabs[HC_TV_CUTIE_COUNT];static int g_ntabs=HC_TV_CUTIE_COUNT;
typedef struct {int top,bottom,section_row,list_row,list_capacity,list_count,settings_row;} ChromeLayout;
typedef struct {
    HcTvState state;HcTvMotion motion;HcTvPlacement placement,planned;
    ChromeLayout layout;int motion_blocked,tab_id,scroll,manual;HcTvRect requested;
} ChromeTv;
static ChromeTv g_tvs[HC_TV_COUNT];
static ChromeLayout g_chrome;
static ChromeReflow g_display;
static Term *g_display_term;
static int g_display_view,g_display_hcount,g_chrome_reflow;
static int g_settings_tab=1,g_setting_offset[3],g_font_menu_count=8;
static int g_chrome_drag,g_sel_on,g_sel_drag,g_sb_drag,g_tab_underlay=1;
static int g_hover,g_preview,g_hover_slot=-1,g_sidebar,g_scheduled,g_font_calls,g_cutie_focus_id;
static int g_tv_drag_slot=-1;
static int g_timer_delay;
static uint32_t g_chrome_input_at,now;
static void *g_font,*g_scheme;
static Cell grid[100*60];
static int chrome_min(int a,int b){return a<b?a:b;}
static int chrome_max(int a,int b){return a>b?a:b;}
static uint32_t GetTickCount(void){return now;}
static int chrome_hovered(void){return g_hover;}
static int chrome_any_tv_hovered(void){return g_hover_slot>=0;}
static int chrome_preview_active(void){return g_preview;}
static void chrome_tv_schedule(void){g_scheduled++;g_timer_delay=0;}
static void plat_timer(int timer,int delay){assert(timer==TIMER_TV);g_timer_delay=delay;}
static Tab *display_tab(void){return NULL;}
static int sidebar_x(void){return g_sidebar;}
static int chrome_background_safe(const Cell *c){return !c->bg;}
static uint32_t chrome_panel_background(const Cell *c){return c->bg;}
static int cell_covers_chrome(const Cell *c){return c->ch>' '||c->wflag==2;}
static void display_line(const Term *t,int y,const Cell **cells,int *n){*cells=grid+y*t->cols;*n=t->cols;}
static void display_cursor_position(const Term *t,int *x,int *y){*x=t->cx;*y=t->cy;}
static int chrome_terminal_span(const Cell *cells,int n,int *left,int *right){
    *left=n;*right=0;for(int x=0;x<n;x++)if(cell_covers_chrome(cells+x)){
        if(x<*left)*left=x;*right=x+1;
    }
    return *right>*left;
}
static int chrome_item_total(void){return g_settings_tab?10:g_font_menu_count;}
static int font_menu_build(int capacity,int items){
    g_font_calls++;g_font_menu_count=capacity>0 && capacity<8 && items<9?9:8;
    return g_font_menu_count;
}
static int chrome_page_offset(int offset,int total,int capacity){
    if(total<=0||capacity<=0||offset<0)return 0;
    return chrome_min(offset/capacity,(total-1)/capacity)*capacity;
}
static uint64_t chrome_motion_hash(uint64_t hash,const void *input,size_t length){
    const unsigned char *p=input;while(length--){hash^=*p++;hash*=UINT64_C(1099511628211);}return hash;
}
#include "hardcore_tv_layout.h"
static void init(Term *t,int cols,int rows){
    memset(g_tvs,0,sizeof(g_tvs));memset(grid,0,sizeof(grid));
    for(int i=0;i<HC_TV_CUTIE_COUNT;i++){tabs[i]=(Tab){.id=100+i};g_tabs[i]=tabs+i;g_tvs[i+HC_TV_CUTIE_FIRST].tab_id=100+i;}
    g_ntabs=HC_TV_CUTIE_COUNT;g_tv_drag_slot=-1;g_tv_stack_signature=0;g_tv_stack_pending=0;
    *t=(Term){.cols=cols,.rows=rows};g_sidebar=cols-SB_COLS;
    g_chrome=(ChromeLayout){.top=3,.bottom=11};
    g_settings_tab=1;memset(g_setting_offset,0,sizeof(g_setting_offset));
    g_hover=g_preview=g_sel_on=g_sel_drag=g_chrome_drag=g_sb_drag=g_cutie_focus_id=0;g_hover_slot=-1;
    now=1000;g_chrome_input_at=0;g_scheduled=g_font_calls=0;
    hc_tv_open(&g_tvs[0].state,now);
}
static void intrude(const Term *t){for(int y=0;y<t->rows;y++)grid[y*t->cols+t->cols-2].ch='X';}
static void stacked(void){
    Term t;init(&t,100,50);hc_tv_open(&g_tvs[1].state,now);chrome_tv_prepare(&t);
    HcTvRect settings=g_tvs[0].state.rect,caf=g_tvs[1].state.rect;
    assert(g_tvs[0].state.placed&&g_tvs[1].state.placed);
    assert(settings.x==72&&settings.y==13&&settings.h==12);
    assert(caf.x==settings.x&&caf.y==settings.y+settings.h+2);
    assert(!hc_tv_rect_overlap(settings,caf));
    for(int n=0;n<100;n++){now+=10;chrome_tv_prepare(&t);
        assert(hc_tv_rect_equal(settings,g_tvs[0].state.rect));assert(hc_tv_rect_equal(caf,g_tvs[1].state.rect));}
}
static void motion(void){
    Term t;init(&t,100,50);chrome_tv_prepare(&t);HcTvRect old=g_tvs[0].state.rect;
    intrude(&t);now=1100;chrome_tv_prepare(&t);assert(g_tvs[0].motion.phase==0);
    now=1549;chrome_tv_prepare(&t);assert(g_tvs[0].motion.phase==0);
    now=1550;chrome_tv_prepare(&t);assert(g_tvs[0].motion.phase==1);
    now=1700;chrome_tv_prepare(&t);assert(g_tvs[0].motion.alpha==.5f&&hc_tv_rect_equal(old,g_tvs[0].state.rect));
    now=1850;chrome_tv_prepare(&t);assert(g_tvs[0].motion.phase==2&&g_tvs[0].motion.alpha==0);
    assert(g_tvs[0].state.rect.x==0&&!hc_tv_rect_equal(old,g_tvs[0].state.rect));
    HcTvRect moved=g_tvs[0].state.rect;
    now=2150;chrome_tv_prepare(&t);assert(g_tvs[0].motion.alpha==1&&g_tvs[0].motion.phase==0);
    for(int n=0;n<100;n++){now+=10;chrome_tv_prepare(&t);assert(hc_tv_rect_equal(moved,g_tvs[0].state.rect));}
}
static void frozen(void){
    Term t;init(&t,100,50);chrome_tv_prepare(&t);HcTvRect old=g_tvs[0].state.rect;
    intrude(&t);g_hover_slot=0;
    for(int n=0;n<10;n++){now+=100;chrome_tv_prepare(&t);assert(hc_tv_rect_equal(old,g_tvs[0].state.rect));assert(g_tvs[0].motion.phase==0);}
    g_hover_slot=-1;now=2449;chrome_tv_prepare(&t);assert(g_tvs[0].motion.phase==0);
    now=2450;chrome_tv_prepare(&t);assert(g_tvs[0].motion.phase==1);
    g_preview=1;now=2500;chrome_tv_prepare(&t);assert(g_tvs[0].motion.phase==0&&g_tvs[0].motion.alpha==1);
    assert(hc_tv_rect_equal(old,g_tvs[0].state.rect));
}
static void compact(void){
    Term t;init(&t,50,15);g_setting_offset[1]=8;chrome_tv_prepare(&t);
    assert(g_tvs[0].state.placed&&g_tvs[0].state.rect.h==4);
    assert(g_tvs[0].layout.list_capacity==2&&g_setting_offset[1]==8);
    now+=100;chrome_tv_prepare(&t);assert(g_setting_offset[1]==8);
    t.cols=20;chrome_tv_prepare(&t);assert(!g_tvs[0].state.placed);
    t.cols=50;chrome_tv_prepare(&t);assert(g_tvs[0].state.placed);
    t.rows=12;g_chrome.top=1;chrome_tv_prepare(&t);assert(!g_tvs[0].state.placed);
}
static void masked_anchor(void){
    Term t;init(&t,100,28);hc_tv_open(&g_tvs[1].state,now);chrome_tv_prepare(&t);
    HcTvRect primary={72,3,28,8};
    for(int i=0;i<2;i++)if(g_tvs[i].state.placed)assert(!hc_tv_rect_overlap(g_tvs[i].state.rect,primary));
    assert(g_tvs[0].state.placed&&g_tvs[1].state.placed);
    assert(!hc_tv_rect_overlap(g_tvs[0].state.rect,g_tvs[1].state.rect));
}
static void cuties(void){
    Term t;init(&t,100,60);hc_tv_open(&g_tvs[1].state,now);chrome_tv_prepare(&t);
    HcTvRect settings=g_tvs[0].state.rect,caf=g_tvs[1].state.rect;
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++)hc_tv_open(&g_tvs[i].state,now);
    chrome_tv_prepare(&t);
    HcTvRect rects[HC_TV_COUNT+1]={{72,3,28,8}};
    assert(hc_tv_rect_equal(settings,g_tvs[0].state.rect));assert(hc_tv_rect_equal(caf,g_tvs[1].state.rect));
    for(int i=0;i<HC_TV_COUNT;i++){
        assert(g_tvs[i].state.placed);rects[i+1]=g_tvs[i].state.rect;
        assert(g_tvs[i].placement.candidates<=HC_TV_MAX_CANDIDATES);
        assert(hc_tv_rect_fits(rects[i+1],t.cols,t.rows));
        if(i>=HC_TV_CUTIE_FIRST)assert(rects[i+1].h==HC_TV_CUTIE_ROWS&&g_tvs[i].layout.list_count==0);
        for(int j=0;j<=i;j++)assert(!hc_tv_rect_overlap(rects[i+1],rects[j]));
    }
    for(int n=0;n<100;n++){now+=10;chrome_tv_prepare(&t);
        for(int i=0;i<HC_TV_COUNT;i++)assert(hc_tv_rect_equal(rects[i+1],g_tvs[i].state.rect));}
    /* A focused Cutie and the last slot's hover both freeze every placement. */
    intrude(&t);g_cutie_focus_id=101;
    for(int n=0;n<10;n++){now+=100;chrome_tv_prepare(&t);
        for(int i=0;i<HC_TV_COUNT;i++)assert(g_tvs[i].motion.phase==HC_TV_MOTION_IDLE);}
    g_cutie_focus_id=0;g_hover_slot=HC_TV_COUNT-1;
    for(int n=0;n<10;n++){now+=100;chrome_tv_prepare(&t);
        for(int i=0;i<HC_TV_COUNT;i++)assert(hc_tv_rect_equal(rects[i+1],g_tvs[i].state.rect));}
}
static void cutie_no_fit(void){
    Term t;init(&t,28,60);
    for(int i=1;i<HC_TV_COUNT;i++)hc_tv_open(&g_tvs[i].state,now);
    chrome_tv_prepare(&t);int placed=0;
    for(int i=0;i<HC_TV_COUNT;i++){
        assert(g_tvs[i].state.open);placed+=g_tvs[i].state.placed;
        if(g_tvs[i].state.placed)for(int j=0;j<i;j++)if(g_tvs[j].state.placed)
            assert(!hc_tv_rect_overlap(g_tvs[i].state.rect,g_tvs[j].state.rect));
    }
    assert(placed<HC_TV_COUNT&&g_tvs[0].state.placed&&g_tvs[1].state.placed);
    t.cols=100;g_sidebar=72;now+=100;chrome_tv_prepare(&t);
    for(int i=0;i<HC_TV_COUNT;i++)assert(g_tvs[i].state.placed);
}
static void reverse_order(void){
    Term t;init(&t,100,60);g_tvs[0].state.open=0;g_hover=1;
    for(int i=0;i<HC_TV_CUTIE_COUNT;i++) {
        HcTvRect old[HC_TV_COUNT];for(int n=0;n<HC_TV_COUNT;n++)old[n]=g_tvs[n].state.rect;
        int slot=HC_TV_CUTIE_FIRST+i;g_tvs[slot].tab_id=103-i;hc_tv_open(&g_tvs[slot].state,now);
        now+=10;chrome_tv_prepare(&t);
        for(int n=HC_TV_CUTIE_FIRST;n<slot;n++)assert(hc_tv_rect_equal(old[n],g_tvs[n].state.rect));
    }
    g_hover=0;now+=10;chrome_tv_prepare(&t);
    for(int i=0;i<HC_TV_CUTIE_COUNT;i++) {
        ChromeTv *tv=g_tvs+HC_TV_COUNT-1-i;
        assert(tv->tab_id==100+i&&tv->state.placed&&tv->state.rect.x==72&&tv->state.rect.y==13+11*i);
    }
    /* Reordering the actual tabs also regroups their existing miniatures. */
    Tab *swap=g_tabs[0];g_tabs[0]=g_tabs[3];g_tabs[3]=swap;
    now+=10;chrome_tv_prepare(&t);
    assert(g_tvs[2].state.rect.y==13&&g_tvs[5].state.rect.y==46);
}
static void modest_overlap_stack(void){
    Term t;init(&t,100,60);g_tvs[0].state.open=0;intrude(&t);
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++)hc_tv_open(&g_tvs[i].state,now);
    chrome_tv_prepare(&t);
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++) {
        assert(g_tvs[i].state.placed&&g_tvs[i].state.rect.x==72);
        assert(g_tvs[i].state.rect.y==13+(i-HC_TV_CUTIE_FIRST)*11);
        assert(g_tvs[i].placement.candidates<=HC_TV_MAX_CANDIDATES);
    }
}
static void custom_manual(void){
    Term t;init(&t,100,60);ChromeTv *tv=g_tvs+2;
    tv->manual=1;tv->requested=(HcTvRect){35,25,40,20};hc_tv_open(&tv->state,now);
    hc_tv_open(&g_tvs[3].state,now);chrome_tv_prepare(&t);
    assert(tv->state.placed&&hc_tv_rect_equal(tv->state.rect,tv->requested));
    HcTvRect request=tv->requested;
    t.cols=70;t.rows=45;g_sidebar=42;now+=100;chrome_tv_prepare(&t);
    assert(tv->state.placed&&tv->state.rect.x==30&&tv->state.rect.y==25&&tv->state.rect.w==40&&tv->state.rect.h==20);
    assert(hc_tv_rect_equal(tv->requested,request));
    t.cols=100;t.rows=60;g_sidebar=72;now+=100;chrome_tv_prepare(&t);
    assert(tv->state.placed&&hc_tv_rect_equal(tv->state.rect,request));
    HcTvRect old=tv->state.rect;g_tv_drag_slot=2;tv->requested=(HcTvRect){72,3,28,9};
    now+=100;chrome_tv_prepare(&t);assert(hc_tv_rect_equal(tv->state.rect,old));
    assert(g_tvs[3].motion_blocked);g_tv_drag_slot=-1;
}
static void manual_priority_and_limits(void){
    Term t;init(&t,100,60);ChromeTv *tv=g_tvs+2;
    tv->manual=1;tv->requested=(HcTvRect){72,13,28,12};hc_tv_open(&tv->state,now);
    chrome_tv_prepare(&t);assert(tv->state.placed&&hc_tv_rect_equal(tv->requested,tv->state.rect));
    assert(g_tvs[0].state.placed&&!hc_tv_rect_overlap(tv->state.rect,g_tvs[0].state.rect));
    HcTvRect r=hc_tv_requested_rect((HcTvRect){INT_MAX,INT_MIN,INT_MAX,INT_MIN},100,60);
    assert(r.x==36&&r.y==0&&r.w==64&&r.h==4);
    r=hc_tv_requested_rect((HcTvRect){INT_MIN,INT_MAX,1,INT_MAX},30,50);
    assert(r.x==0&&r.y==0&&r.w==18&&r.h==50);
    r=hc_tv_requested_rect((HcTvRect){0},100,60);assert(r.w==28&&r.h==9);
    /* Another manually positioned panel cannot overlap the reserved first. */
    g_tvs[3].manual=1;g_tvs[3].requested=tv->requested;hc_tv_open(&g_tvs[3].state,now);
    now+=100;chrome_tv_prepare(&t);assert(tv->state.placed&&!g_tvs[3].state.placed);
}
static void pending_wakeup(void){
    Term t;init(&t,100,60);g_tvs[0].state.open=0;
    g_tvs[2].tab_id=101;hc_tv_open(&g_tvs[2].state,now);chrome_tv_prepare(&t);
    g_hover=1;g_chrome_input_at=now;g_tvs[3].tab_id=100;hc_tv_open(&g_tvs[3].state,now);
    chrome_tv_prepare(&t);assert(g_tv_stack_pending&&!g_timer_delay);
    now+=400;g_hover=0;chrome_tv_prepare(&t);assert(g_tv_stack_pending&&g_timer_delay==300);
    now+=300;chrome_tv_prepare(&t);
    assert(!g_tv_stack_pending&&g_tvs[3].state.rect.y<g_tvs[2].state.rect.y);
}
static void varied_geometry(void){
    unsigned rng=1979;
    for(int trial=0;trial<400;trial++) {
        rng=rng*1664525u+1013904223u;int cols=28+rng%73;
        rng=rng*1664525u+1013904223u;int rows=16+rng%45;
        Term t;init(&t,cols,rows);hc_tv_open(&g_tvs[1].state,now);
        for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++) {
            ChromeTv *tv=g_tvs+i;hc_tv_open(&tv->state,now);tv->tab_id=105-i;
            rng=rng*1664525u+1013904223u;tv->manual=(rng%3)==0;
            rng=rng*1664525u+1013904223u;tv->requested.x=(int)(rng%120)-20;
            rng=rng*1664525u+1013904223u;tv->requested.y=(int)(rng%80)-20;
            rng=rng*1664525u+1013904223u;tv->requested.w=rng%80;
            rng=rng*1664525u+1013904223u;tv->requested.h=rng%70;
        }
        for(int frame=0;frame<3;frame++) {
            g_hover=frame==1;now+=1000;chrome_tv_prepare(&t);
            HcTvRect primary={cols-28,3,28,8};
            for(int i=0;i<HC_TV_COUNT;i++)if(g_tvs[i].state.placed) {
                HcTvRect r=g_tvs[i].state.rect;
                assert(hc_tv_rect_fits(r,cols,rows)&&!hc_tv_rect_overlap(r,primary));
                assert(g_tvs[i].placement.candidates<=HC_TV_MAX_CANDIDATES);
                for(int j=0;j<i;j++)if(g_tvs[j].state.placed)assert(!hc_tv_rect_overlap(r,g_tvs[j].state.rect));
            }
        }
    }
}
int main(int argc,char **argv){
    int n=argc>1?atoi(argv[1]):0;
    if(n==0)stacked();else if(n==1)motion();else if(n==2)frozen();else if(n==3)compact();
    else if(n==4)masked_anchor();else if(n==5)cuties();else if(n==6)cutie_no_fit();
    else if(n==7)reverse_order();else if(n==8)modest_overlap_stack();else if(n==9)custom_manual();
    else if(n==10)manual_priority_and_limits();else if(n==11)pending_wakeup();else varied_geometry();
    puts("ok");return 0;
}
'''

class Layout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix="hc-tv-layout-")
        source=Path(cls.tmp.name)/"layout.c";source.write_text(HARNESS)
        cls.binary=Path(cls.tmp.name)/"layout"
        subprocess.run([os.environ.get("CC","cc"),"-std=c99","-Wall","-Wextra","-Werror",
            "-Wno-unused-function",*shlex.split(os.environ.get("HC_TEST_CFLAGS","")),
            "-I",str(ROOT),str(source),"-o",str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check_case(self,n):subprocess.run([str(self.binary),str(n)],check=True,capture_output=True)
    def test_caf_stacks_below_settings_and_stays_stable(self):self.check_case(0)
    def test_production_prepare_commits_once_after_fade(self):self.check_case(1)
    def test_hover_and_preview_cancel_motion(self):self.check_case(2)
    def test_compact_pages_resize_and_no_fit(self):self.check_case(3)
    def test_settings_anchor_never_overlaps_primary(self):self.check_case(4)
    def test_four_cuties_keep_priority_bounds_and_freeze_for_focus(self):self.check_case(5)
    def test_cuties_survive_no_fit_and_appear_when_room_returns(self):self.check_case(6)
    def test_reverse_opening_and_tab_reorder_follow_tab_list(self):self.check_case(7)
    def test_modest_text_overlap_keeps_cuties_stacked_below_menu(self):self.check_case(8)
    def test_manual_geometry_resize_viewport_restore_and_drag_freeze(self):self.check_case(9)
    def test_manual_reservations_precede_auto_and_size_limits_are_safe(self):self.check_case(10)
    def test_recent_input_deferred_reorder_schedules_one_quiet_wakeup(self):self.check_case(11)
    def test_varied_manual_and_auto_geometry_stays_bounded_and_disjoint(self):self.check_case(12)

if __name__=="__main__":unittest.main()
