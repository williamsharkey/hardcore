#!/usr/bin/env python3
"""Packed TV clicks are interpreted only against the current rendered frame."""
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
#define SB_COLS 28
#define NFAMILIES 10
#define CW 10
#define CH 20
#define TIMER_TV 7
#include "hardcore_chrome_pack.h"
enum{HIT_NONE,HIT_TV_GRIP,HIT_TV_RESIZE,HIT_TV_CLOSE,HIT_TV,HIT_STAB,HIT_SETPAGE,HIT_CHROME_INERT,HIT_FONT,HIT_SCHEME,HIT_TV_EXPAND};
typedef struct{int top,bottom,section_row,list_row,list_count,list_capacity;}ChromeLayout;
static ChromePackRow *g_chrome_menu_rows;
static int g_chrome_menu_count,g_chrome_menu_capacity,g_chrome_menu_top,g_chrome_menu_x,g_chrome_menu_cw,g_chrome_menu_ch,g_chrome_menu_raised;
static float g_chrome_menu_alpha;
static int g_chrome_menu_cols=SB_COLS;
static int g_settings_tab=1,g_setting_offset[3],g_font_menu_count=8,g_sel_drag,g_mouse_x=-1,g_mouse_y=-1,g_settings_open;
static const struct{int light,dark;}FAMILIES[NFAMILIES]={{0,1}};
typedef struct {int id,browser_native;} Tab;
static Tab tabs[8];
static int g_cutie_focus_id,g_timer,g_invalidations;
static uint32_t now=1000;
static Tab *tab_by_id(int id){for(int i=0;i<8;i++)if(id>0&&tabs[i].id==id)return tabs+i;return NULL;}
static uint32_t GetTickCount(void){return now;}
static void plat_invalidate(void){g_invalidations++;}
static void plat_timer(int id,int ms){(void)id;g_timer=ms;}
static void plat_kill_timer(int id){(void)id;g_timer=0;}
static int chrome_item_total(void){return NFAMILIES;}
static int font_menu_hit(int item,int col){return item+col;}
static int scheme_index(int key){return key;}
static int chrome_page_hit(int a,int b,int c,int d,int e,int *f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
static int chrome_pager_geometry(int a,int b,int c,int d,int *e,int *f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
#include "hardcore_tv_ui.h"
static ChromePackRow rows[12];
static void init(void){
    g_chrome_menu_cols=SB_COLS;g_tv_drag_slot=-1;g_tv_drag_mode=0;
    memset(g_tvs,0,sizeof(g_tvs));memset(rows,0,sizeof(rows));g_settings_tab=1;g_setting_offset[1]=0;
    memset(tabs,0,sizeof(tabs));g_cutie_focus_id=g_timer=g_invalidations=g_settings_open=0;now=1000;
    for(int i=0;i<8;i++)tabs[i].id=100+i;
    for(int y=0;y<12;y++)for(int x=0;x<SB_COLS;x++){rows[y].src[x]=x;rows[y].dst[x]=x;rows[y].actionable[x]=1;}
    g_chrome_menu_rows=rows;g_chrome_menu_count=g_chrome_menu_capacity=12;
    g_chrome_menu_top=10;g_chrome_menu_x=720;g_chrome_menu_cw=CW;g_chrome_menu_ch=CH;g_chrome_menu_alpha=1;
    g_chrome_menu_raised=0;
    ChromeTv *tv=g_tvs;
    hc_tv_open(&tv->state,1000);hc_tv_motion_reset(&tv->motion);
    tv->state.placed=1;tv->state.rect=(HcTvRect){72,10,28,12};
    tv->layout=(ChromeLayout){10,22,11,12,10,10};tv->frame=chrome_frame_save();
}
static int hit(int col,int row,int *idx){return chrome_tv_hit((72+col)*CW+1,row*CH+1,idx);}
static void packed(void){
    int idx=-1;init();assert(hit(3,10,&idx)==HIT_TV_GRIP&&idx==0);assert(hit(26,10,&idx)==HIT_TV_CLOSE);
    rows[0].src[24]=26;rows[0].actionable[26]=0;
    assert(hit(24,10,&idx)==HIT_TV_CLOSE);assert(hit(26,10,&idx)==HIT_TV_GRIP);
    rows[0].blocked_left=25;rows[0].blocked_right=27;
    assert(hit(26,10,&idx)==HIT_NONE);assert(hit(27,10,&idx)==HIT_TV_GRIP);
    rows[3].actionable[5]=0;assert(hit(5,13,&idx)==HIT_NONE);
}
static void stale(void){
    int idx=-1;init();ChromeMenuFrame frame=g_tvs[0].frame;
    g_tvs[0].frame.x+=CW;assert(hit(3,10,&idx)==HIT_NONE);g_tvs[0].frame=frame;
    g_tvs[0].frame.top++;assert(hit(3,10,&idx)==HIT_NONE);g_tvs[0].frame=frame;
    g_tvs[0].frame.count--;assert(hit(3,10,&idx)==HIT_NONE);g_tvs[0].frame=frame;
    g_tvs[0].frame.cw++;assert(hit(3,10,&idx)==HIT_NONE);g_tvs[0].frame=frame;
    g_setting_offset[1]=2;assert(hit(3,13,&idx)==HIT_NONE);g_setting_offset[1]=0;
    g_settings_tab=0;assert(hit(3,13,&idx)==HIT_NONE);g_settings_tab=1;
    g_tvs[0].frame.rows=NULL;assert(hit(3,10,&idx)==HIT_NONE);g_tvs[0].frame=frame;
    g_tvs[0].frame.alpha=0;assert(hit(3,10,&idx)==HIT_NONE);g_tvs[0].frame=frame;
    g_tvs[0].motion.alpha=0;assert(hit(3,10,&idx)==HIT_NONE);
}
static void caf(void){
    int idx=-1;init();g_tvs[1]=g_tvs[0];g_tvs[0].state.open=0;
    assert(hit(3,13,&idx)==HIT_TV&&idx==1);
    assert(hit(26,10,&idx)==HIT_TV_CLOSE&&idx==1);
    for(int col=0;col<SB_COLS;col++)assert(hit(col,10,&idx)==(col==26?HIT_TV_CLOSE:HIT_TV_GRIP));
}
static void lifecycle(void){
    init();assert(chrome_tv_open_tab(0)==-1&&chrome_tv_open_tab(999)==-1);
    assert(chrome_tv_slot_for(0)==-1&&chrome_tv_slot_for(100)==-1);
    tabs[0].browser_native=1;int first=chrome_tv_open_tab(100);
    assert(first==HC_TV_CUTIE_FIRST&&g_tvs[first].tab_id==100);
    assert(chrome_tv_slot_for(100)==first);
    chrome_tv_scroll(first,7);assert(chrome_tv_open_tab(100)==first&&g_tvs[first].scroll==7);
    /* Tab reordering never changes the source identity. */
    Tab tmp=tabs[0];tabs[0]=tabs[1];tabs[1]=tmp;
    assert(chrome_tv_open_tab(100)==first);
    for(int i=1;i<HC_TV_CUTIE_COUNT;i++)assert(chrome_tv_open_tab(100+i)==first+i);
    assert(chrome_tv_open_tab(104)==-1);
    for(int i=0;i<HC_TV_CUTIE_COUNT;i++)assert(g_tvs[first+i].tab_id==100+i);
    chrome_tv_scroll(first,INT_MAX);assert(g_tvs[first].scroll==INT_MAX);
    chrome_tv_scroll(first,INT_MIN);assert(g_tvs[first].scroll==0);
    g_cutie_focus_id=100;chrome_tv_close_slot(first);assert(!g_cutie_focus_id&&!g_tvs[first].state.open);
    assert(chrome_tv_slot_for(100)==-1);
    assert(chrome_tv_open_tab(104)==first&&g_tvs[first].scroll==0&&g_tvs[first].frame.count==0);
    tabs[4].id=0;g_cutie_focus_id=104;chrome_tv_prune();
    assert(!g_tvs[first].state.open&&!g_cutie_focus_id);
    for(int i=first+1;i<HC_TV_COUNT;i++)assert(g_tvs[i].state.open);
    chrome_tv_close_slot(-1);chrome_tv_close_slot(HC_TV_COUNT);
    assert(!chrome_tv_contains(-1,0,0)&&!chrome_tv_contains(HC_TV_COUNT,0,0));
}
static void persistent(void){
    init();int slot=chrome_tv_open_tab(100);now+=600000;chrome_tv_clock();
    assert(g_tvs[slot].state.open&&g_tvs[slot].state.alpha==1);
    assert(!g_tvs[0].state.open&&!g_timer);
    chrome_tv_interaction(slot);now+=600000;chrome_tv_clock();assert(g_tvs[slot].state.open);
    /* A settings drag owns its title even with the pointer outside the TV. */
    init();g_tv_drag_slot=0;now+=600000;chrome_tv_clock();assert(g_tvs[0].state.open&&g_tvs[0].state.alpha==1);
}
static void cutie_hit(void){
    int idx=-1;init();g_tvs[HC_TV_COUNT-1]=g_tvs[0];g_tvs[0].state.open=0;
    g_tvs[HC_TV_COUNT-1].tab_id=100;
    rows[3].actionable[5]=0; /* terminal body uses projection, not word packing */
    assert(hit(5,13,&idx)==HIT_TV&&idx==HC_TV_COUNT-1);
    assert(hit(22,10,&idx)==HIT_TV_GRIP);
    for(int col=0;col<SB_COLS;col++)assert(hit(col,10,&idx)==
        (col==26?HIT_TV_CLOSE:col==24?HIT_TV_EXPAND:HIT_TV_GRIP));
    assert(hit(27,21,&idx)==HIT_TV_RESIZE);
    assert(hit(25,21,&idx)==HIT_TV);
    g_tvs[HC_TV_COUNT-1].frame.cols--;assert(hit(22,10,&idx)==HIT_NONE);
    g_tvs[HC_TV_COUNT-1].frame.cols++;
    rows[0].src[24]=26;rows[0].actionable[26]=0;
    assert(hit(24,10,&idx)==HIT_TV_CLOSE&&idx==HC_TV_COUNT-1);
    assert(hit(26,10,&idx)==HIT_TV_GRIP);
    assert(chrome_any_tv_contains(721,201));g_mouse_x=721;g_mouse_y=201;
    assert(chrome_any_tv_hovered());g_sel_drag=1;assert(!chrome_any_tv_hovered());g_sel_drag=0;
    tabs[0].id=0;assert(hit(24,10,&idx)==HIT_NONE&&!chrome_any_tv_hovered());
}
static void packed_title(void){
    int idx=-1;init();ChromeTv *tv=g_tvs+HC_TV_CUTIE_FIRST;*tv=g_tvs[0];g_tvs[0].state.open=0;tv->tab_id=100;
    unsigned char ink[SB_COLS]={0};
    for(int x=1;x<13;x++)ink[x]=1;ink[22]=ink[24]=ink[26]=1;
    for(int left=0;left<SB_COLS;left++)for(int right=left+1;right<=SB_COLS;right++) {
        chrome_pack_row(rows,ink,SB_COLS,left,right);
        for(int x=0;x<SB_COLS;x++) {
            int result=hit(x,10,&idx);
            if(x>=left&&x<right)assert(result==HIT_NONE);
            else if(rows[0].actionable[x]&&rows[0].src[x]==26)assert(result==HIT_TV_CLOSE);
            else if(rows[0].actionable[x]&&rows[0].src[x]==24)assert(result==HIT_TV_EXPAND);
            else assert(result==HIT_TV_GRIP);
        }
    }
    /* Raised titles ignore the intrusion map and preserve all original controls. */
    tv->frame.raised=1;
    for(int x=0;x<SB_COLS;x++)assert(hit(x,10,&idx)==(x==26?HIT_TV_CLOSE:x==24?HIT_TV_EXPAND:HIT_TV_GRIP));
}
int main(int argc,char **argv){int n=argc>1?atoi(argv[1]):0;
    if(!n)packed();else if(n==1)stale();else if(n==2)caf();else if(n==3)lifecycle();
    else if(n==4)persistent();else if(n==5)cutie_hit();else packed_title();puts("ok");return 0;}
'''
class Hit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix="hc-tv-hit-")
        source=Path(cls.tmp.name)/"hit.c";source.write_text(HARNESS);cls.binary=Path(cls.tmp.name)/"hit"
        subprocess.run([os.environ.get("CC","cc"),"-std=c99","-Wall","-Wextra","-Werror","-Wno-unused-function",
            *shlex.split(os.environ.get("HC_TEST_CFLAGS","")),"-I",str(ROOT),str(source),"-o",str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check_case(self,n):subprocess.run([str(self.binary),str(n)],check=True,capture_output=True)
    def test_packed_word_controls_use_source_columns(self):self.check_case(0)
    def test_stale_geometry_content_and_invisible_frames_are_inert(self):self.check_case(1)
    def test_caf_returns_its_own_slot(self):self.check_case(2)
    def test_cuties_bind_stable_ids_deduplicate_and_prune(self):self.check_case(3)
    def test_cuties_do_not_idle_expire_or_schedule_idle_ticks(self):self.check_case(4)
    def test_last_cutie_slot_header_and_body_are_interactive(self):self.check_case(5)
    def test_every_visible_title_cell_drags_except_actual_packed_button_cells(self):self.check_case(6)
if __name__=="__main__":unittest.main()
