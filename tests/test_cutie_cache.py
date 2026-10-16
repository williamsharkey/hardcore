#!/usr/bin/env python3
"""Exercise production Cutie caching and source-click validation without a GUI."""
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
#include <time.h>
typedef struct{uint32_t ch,fg,bg;uint8_t attr,wflag;uint16_t link;}Cell;
#include "hardcore_tv.h"
#define CW 12
#define CH 26
#define W_WIDE 1
enum{TK_TERMINAL,TK_BROWSER};
enum{MAX_COLS=400,MAX_ROWS=128};
typedef struct{int cols,rows,hcount,alt,m_cursor_vis,cx,cy,view;long long hpushed;Cell *row[2][MAX_ROWS];}Term;
typedef struct{Term t;int id,kind;uint64_t output_generation;}Tab;
typedef struct{HcTvState state;int tab_id,scroll;}ChromeTv;
static ChromeTv g_tvs[HC_TV_COUNT];
static Tab tabs[HC_TV_CUTIE_COUNT];
static Cell cells[HC_TV_CUTIE_COUNT][2][MAX_ROWS][MAX_COLS];
static int g_focused=1,g_cutie_focus_id,browser_generation=1,browser_builds,invalidations;
static void plat_invalidate(void){invalidations++;}
static Tab *tab_by_id(int id){for(int i=0;i<HC_TV_CUTIE_COUNT;i++)if(id>0&&tabs[i].id==id)return tabs+i;return NULL;}
static int chrome_min(int a,int b){return a<b?a:b;}
static int chrome_max(int a,int b){return a>b?a:b;}
static int chrome_tv_contains(int slot,int mx,int my){
    HcTvRect r=g_tvs[slot].state.rect;
    return g_tvs[slot].state.open&&g_tvs[slot].state.placed&&mx>=r.x*CW&&mx<(r.x+r.w)*CW&&my>=r.y*CH&&my<(r.y+r.h)*CH;
}
static void term_line(const Term*t,int line,const Cell**out,int*n,int*wrapped){
    int y=line-t->hcount;*wrapped=0;*n=y>=0&&y<t->rows?t->cols:0;*out=*n?t->row[t->alt][y]:NULL;
}
static int mac_browser_cutie_generation(Tab*tb){(void)tb;return browser_generation;}
static int mac_browser_cutie_project(Tab*tb,Cell*out,int width,int height,int scroll){
    (void)tb;(void)height;(void)scroll;browser_builds++;
    out[0]=(Cell){.ch='B',.fg=0x123456,.link=10+browser_generation};
    assert(width>=2);return 20;
}
#include "hardcore_cutie.h"
static void fixture(int cols,int rows){
    for(int i=0;i<HC_TV_COUNT;i++){free(g_cuties[i].input);hc_cutie_project_free(&g_cuties[i].view);}
    memset(g_cuties,0,sizeof(g_cuties));memset(g_tvs,0,sizeof(g_tvs));
    memset(tabs,0,sizeof(tabs));memset(cells,0,sizeof(cells));
    for(int i=0;i<HC_TV_CUTIE_COUNT;i++){
        Tab*tb=tabs+i;tb->id=100+i;tb->t.cols=cols;tb->t.rows=rows;tb->output_generation=1;
        for(int a=0;a<2;a++)for(int y=0;y<rows;y++)tb->t.row[a][y]=cells[i][a][y];
        ChromeTv*tv=g_tvs+HC_TV_CUTIE_FIRST+i;tv->tab_id=tb->id;
        tv->state.open=tv->state.placed=1;tv->state.rect=(HcTvRect){72,i*10,28,9};
    }
    browser_generation=1;browser_builds=0;g_cutie_focus_id=0;g_focused=1;
}
static int locate(int slot,int source,int *mx,int *my){
    HcCutieProjection*p=&g_cuties[slot].view;HcTvRect r=g_tvs[slot].state.rect;
    for(int i=0;i<p->cols*p->rows;i++)if(p->source[i]==source){
        *mx=(r.x+1+i%p->cols)*CW+1;*my=(r.y+1+i/p->cols)*CH+1;return 1;
    }
    return 0;
}
static void stale_generation(void){
    fixture(40,4);Tab*tb=tabs;int slot=HC_TV_CUTIE_FIRST;
    for(int x=0;x<20;x++)tb->t.row[0][1][x]=(Cell){.ch='a'+x%26,.fg=0x123456,.bg=0x223344,.attr=1,.link=3};
    tb->t.cy=1;tb->t.cx=8;tb->t.m_cursor_vis=1;
    Cell before[40];memcpy(before,tb->t.row[0][1],sizeof(before));
    assert(cutie_prepare(slot));int mx,my,x=-1,y=-1,node=-1;
    assert(locate(slot,48,&mx,&my));assert(cutie_source_at(slot,mx,my,&x,&y,&node)&&x==8&&y==1);
    /* A different row changes, while the clicked glyph remains identical. */
    tb->t.row[0][3][0].ch='Z';tb->output_generation++;
    assert(!cutie_source_at(slot,mx,my,&x,&y,&node));
    assert(cutie_prepare(slot)&&locate(slot,48,&mx,&my));
    assert(cutie_source_at(slot,mx,my,&x,&y,&node)&&x==8&&y==1);
    assert(!memcmp(before,tb->t.row[0][1],sizeof(before)));
}
static void identity_and_focus(void){
    fixture(40,4);int slot=HC_TV_CUTIE_FIRST;tabs[0].t.row[0][0][0].ch='A';
    assert(cutie_prepare(slot));int mx,my,x,y,node;
    assert(locate(slot,0,&mx,&my));g_cutie_focus_id=100;assert(cutie_focus_valid());
    g_tvs[slot].scroll=17;tabs[0].t.view=9;int before=invalidations;cutie_input_follow();
    assert(g_tvs[slot].scroll==0 && tabs[0].t.view==9 && invalidations==before+1);
    g_focused=0;assert(!cutie_focus_valid());g_focused=1;
    g_tvs[slot].state.placed=0;assert(!cutie_focus_valid());g_tvs[slot].state.placed=1;
    g_tvs[slot].tab_id=101;assert(!cutie_source_at(slot,mx,my,&x,&y,&node)&&!cutie_focus_valid());
    tabs[1].t.row[0][0][0].ch='B';assert(cutie_prepare(slot));assert(g_cuties[slot].view.cells[0].ch=='B');
    tabs[1].kind=TK_BROWSER;g_cutie_focus_id=101;assert(!cutie_focus_valid());
}
static void browser_cache(void){
    fixture(40,4);int slot=HC_TV_CUTIE_FIRST;tabs[0].kind=TK_BROWSER;
    assert(cutie_prepare(slot)&&browser_builds==1);Cell*old=g_cuties[slot].view.cells;
    assert(cutie_prepare(slot)&&browser_builds==1&&g_cuties[slot].view.cells==old);
    HcTvRect r=g_tvs[slot].state.rect;int mx=(r.x+1)*CW+1,my=(r.y+1)*CH+1,x,y,node;
    assert(cutie_source_at(slot,mx,my,&x,&y,&node)&&node==11);
    browser_generation++;assert(!cutie_source_at(slot,mx,my,&x,&y,&node));
    assert(cutie_prepare(slot)&&browser_builds==2);
    assert(cutie_source_at(slot,mx,my,&x,&y,&node)&&node==12);
}
static void source_bounds(void){
    fixture(MAX_COLS,MAX_ROWS);int slot=HC_TV_CUTIE_FIRST;
    tabs[0].t.row[0][MAX_ROWS-1][0]=(Cell){.ch='Z'};
    assert(cutie_prepare(slot));CutieCache*c=g_cuties+slot;
    assert(c->source_cols*c->source_rows<=HC_CUTIE_MAX_SOURCE_CELLS);
    assert(c->source_rows<=HC_CUTIE_MAX_SOURCE_ROWS&&c->first_line==8);
    int mx,my,x,y,node;assert(locate(slot,(c->source_rows-1)*MAX_COLS,&mx,&my));
    assert(cutie_source_at(slot,mx,my,&x,&y,&node)&&x==0&&y==127);
    tabs[0].t.hpushed++;assert(!cutie_source_at(slot,mx,my,&x,&y,&node));
}
int main(int argc,char**argv){
    int n=argc>1?atoi(argv[1]):0;
    if(!n)stale_generation();else if(n==1)identity_and_focus();else if(n==2)browser_cache();else source_bounds();
    fixture(1,1);puts("ok");return 0;
}
'''

class Cache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix="hc-cutie-cache-")
        source=Path(cls.tmp.name)/"cache.c";source.write_text(HARNESS);cls.binary=Path(cls.tmp.name)/"cache"
        subprocess.run([os.environ.get("CC","cc"),"-std=c99","-Wall","-Wextra","-Werror","-Wno-unused-function",
            *shlex.split(os.environ.get("HC_TEST_CFLAGS","")),"-I",str(ROOT),str(source),"-o",str(cls.binary)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def check_case(self,n):subprocess.run([str(self.binary),str(n)],check=True,capture_output=True)
    def test_other_output_invalidates_unchanged_clicked_cell(self):self.check_case(0)
    def test_stable_identity_and_focus_guards(self):self.check_case(1)
    def test_browser_generation_reuses_cache_and_rejects_stale_nodes(self):self.check_case(2)
    def test_bounded_tail_preserves_live_source_coordinates(self):self.check_case(3)

if __name__=="__main__":unittest.main()
