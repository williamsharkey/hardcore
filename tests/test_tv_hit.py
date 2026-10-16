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
enum{HIT_NONE,HIT_TV_CLOSE,HIT_TV,HIT_STAB,HIT_SETPAGE,HIT_CHROME_INERT,HIT_FONT,HIT_SCHEME};
typedef struct{int top,bottom,section_row,list_row,list_count,list_capacity;}ChromeLayout;
static ChromePackRow *g_chrome_menu_rows;
static int g_chrome_menu_count,g_chrome_menu_capacity,g_chrome_menu_top,g_chrome_menu_x,g_chrome_menu_cw,g_chrome_menu_ch,g_chrome_menu_raised;
static float g_chrome_menu_alpha;
static int g_settings_tab=1,g_setting_offset[3],g_font_menu_count=8,g_sel_drag,g_mouse_x=-1,g_mouse_y=-1,g_settings_open;
static const struct{int light,dark;}FAMILIES[NFAMILIES]={{0,1}};
static uint32_t GetTickCount(void){return 1000;}
static void plat_invalidate(void){}
static void plat_timer(int id,int ms){(void)id;(void)ms;}
static void plat_kill_timer(int id){(void)id;}
static int chrome_item_total(void){return NFAMILIES;}
static int font_menu_hit(int item,int col){return item+col;}
static int scheme_index(int key){return key;}
static int chrome_page_hit(int a,int b,int c,int d,int e,int *f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
static int chrome_pager_geometry(int a,int b,int c,int d,int *e,int *f){(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;return 0;}
#include "hardcore_tv_ui.h"
static ChromePackRow rows[12];
static void init(void){
    memset(g_tvs,0,sizeof(g_tvs));memset(rows,0,sizeof(rows));g_settings_tab=1;g_setting_offset[1]=0;
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
    int idx=-1;init();assert(hit(3,10,&idx)==HIT_TV&&idx==0);assert(hit(26,10,&idx)==HIT_TV_CLOSE);
    rows[0].src[24]=26;rows[0].actionable[26]=0;
    assert(hit(24,10,&idx)==HIT_TV_CLOSE);assert(hit(26,10,&idx)==HIT_NONE);
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
}
int main(int argc,char **argv){int n=argc>1?atoi(argv[1]):0;if(!n)packed();else if(n==1)stale();else caf();puts("ok");return 0;}
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
if __name__=="__main__":unittest.main()
