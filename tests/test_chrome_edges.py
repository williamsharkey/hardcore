#!/usr/bin/env python3
"""Extract real renderer edge/hover helpers into a deterministic pixel harness."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

def function(source,name):
    match=re.search(r"static (?:int|void) "+name+r"\([^)]*\)\s*\{",source)
    if not match: raise AssertionError(f"missing renderer helper {name}")
    depth=1;end=match.end()
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]

HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define CW 12
#define CH 26
#include "hardcore_tv.h"
enum{HIT_NONE,HIT_TAB,HIT_CLOSE,HIT_CUTIE,HIT_TABPAGE,HIT_CHROME_INERT,HIT_TV,HIT_FONT,HIT_SCHEME};
typedef struct{int top,bottom;}ChromeLayout;
typedef struct{HcTvState state;HcTvMotion motion;}ChromeTv;
static ChromeLayout g_chrome;
static ChromeTv g_tvs[HC_TV_COUNT];
static int g_chrome_join_top,g_chrome_join_bottom,g_chrome_x,g_mouse_x,g_mouse_y;
static int lines;static float drawn_y,drawn_bottom;
static int chrome_min(int a,int b){return a<b?a:b;}
static float row_y(int row){return (float)(row*CH);}
static int sidebar_w(void){return 28*CW;}
static int chrome_draw_width(void){return sidebar_w();}
static void fill_rect(float x,float y,float right,float bottom,uint32_t color,float alpha){
    (void)x;(void)right;(void)color;(void)alpha;lines++;drawn_y=y;drawn_bottom=bottom;
}
HELPERS
static void joins(void){
    assert(chrome_join_mask(100,3,10,100,10,20)==2);
    assert(chrome_join_mask(100,10,20,100,3,10)==1);
    assert(!chrome_join_mask(100,3,10,0,10,20));
    assert(!chrome_join_mask(100,3,10,100,12,20));
    assert(!chrome_join_mask(100,3,10,100,3,10));
    assert(!chrome_join_mask(100,3,3,100,3,10));
}
static void corners(void){
    g_chrome.top=10;g_chrome.bottom=20;
    int top=g_chrome.top*CH,bottom=g_chrome.bottom*CH-1;
    assert(chrome_corner_inset(top)>0&&chrome_corner_inset(bottom)>0);
    g_chrome_join_top=1;
    assert(chrome_corner_inset(top)==0&&chrome_corner_inset(bottom)>0);
    g_chrome_join_bottom=1;
    assert(chrome_corner_inset(top)==0&&chrome_corner_inset(bottom)==0);
    g_chrome_join_top=0;
    assert(chrome_corner_inset(top)>0&&chrome_corner_inset(bottom)==0);
}
static void seam(void){
    g_chrome.top=10;g_chrome.bottom=20;g_chrome_join_top=1;
    chrome_rule(10,0);assert(!lines);
    chrome_rule(11,0);assert(lines==1&&drawn_bottom-drawn_y==1);
    g_chrome_join_top=0;chrome_rule(10,0);assert(lines==2&&drawn_y==10*CH&&drawn_bottom==10*CH+1);
}
static void hover(void){
    g_chrome_x=72*CW;g_mouse_y=5*CH+1;g_mouse_x=g_chrome_x+5*CW;
    assert(chrome_tab_hover(HIT_TAB,5)&&chrome_tab_hover(HIT_CLOSE,5));
    assert(chrome_tab_hover(HIT_CUTIE,5));
    assert(chrome_tab_hover(HIT_TABPAGE,5)&&chrome_tab_hover(HIT_CHROME_INERT,5));
    assert(!chrome_tab_hover(HIT_TV,5)&&!chrome_tab_hover(HIT_FONT,5)&&!chrome_tab_hover(HIT_SCHEME,5));
    g_mouse_x=5*CW;assert(!chrome_tab_hover(HIT_TV,5)&&!chrome_tab_hover(HIT_CHROME_INERT,5));
    g_mouse_x=g_chrome_x+5*CW;assert(!chrome_tab_hover(HIT_TAB,4));
}
static void all_surfaces(void){
    g_chrome=(ChromeLayout){10,20};g_chrome_x=72*CW;
    ChromeTv *tv=g_tvs+HC_TV_COUNT-1;tv->state.open=tv->state.placed=1;
    tv->state.alpha=tv->motion.alpha=1;tv->state.rect=(HcTvRect){72,20,28,9};
    chrome_prepare_edges((ChromeLayout){3,10},72*CW,1);
    assert(g_chrome_join_top&&g_chrome_join_bottom);
    tv->motion.alpha=0;chrome_prepare_edges((ChromeLayout){3,10},72*CW,1);
    assert(g_chrome_join_top&&!g_chrome_join_bottom);
    tv->motion.alpha=1;tv->state.placed=0;chrome_prepare_edges((ChromeLayout){3,10},72*CW,0);
    assert(!g_chrome_join_top&&!g_chrome_join_bottom);
    tv->state.placed=1;tv->state.rect.x=0;chrome_prepare_edges((ChromeLayout){3,10},72*CW,0);
    assert(!g_chrome_join_top&&!g_chrome_join_bottom);
}
int main(int argc,char **argv){int n=argc>1?atoi(argv[1]):0;if(!n)joins();else if(n==1)corners();else if(n==2)seam();else if(n==3)hover();else all_surfaces();puts("ok");return 0;}
'''

class Edges(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        renderer=(ROOT/'hardcore_chrome_draw.h').read_text()
        helpers='\n'.join(function(renderer,n) for n in ('chrome_join_mask','chrome_prepare_edges','chrome_corner_inset','chrome_rule','chrome_tab_hover'))
        cls.tmp=tempfile.TemporaryDirectory(prefix='hc-chrome-edges-');source=Path(cls.tmp.name)/'edges.c'
        source.write_text(HARNESS.replace('HELPERS',helpers));cls.binary=Path(cls.tmp.name)/'edges'
        subprocess.run([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror',
            *shlex.split(os.environ.get('HC_TEST_CFLAGS','')),'-I',str(ROOT),str(source),'-o',str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check_case(self,n):subprocess.run([str(self.binary),str(n)],check=True,capture_output=True)
    def test_only_exact_same_edge_contacts_join(self):self.check_case(0)
    def test_touching_corners_square_and_exposed_corners_round(self):self.check_case(1)
    def test_lower_panel_omits_shared_seam_but_keeps_inner_rules(self):self.check_case(2)
    def test_opposite_tv_cannot_highlight_a_primary_tab(self):self.check_case(3)
    def test_last_cutie_joins_only_while_visible_on_the_same_edge(self):self.check_case(4)

if __name__=='__main__':unittest.main()
