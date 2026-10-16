#!/usr/bin/env python3
"""Owned cell fixtures for shaded prompt panels; no GUI or live PTY is used."""
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
typedef struct { uint32_t ch, fg, bg; uint8_t attr,wflag; uint16_t link; } Cell;
#include "hardcore_reflow.h"
#include "hardcore_panel_reflow.h"
#define COLS 100
#define ROWS 16
static uint32_t background(const Cell *c) { return c->bg; }
static int safe(const Cell *c) { return !c->bg; }
static void text(Cell *grid, int y, int x, const char *s) {
    while (*s) { assert(x<COLS); grid[y*COLS+x].ch=(unsigned char)*s++; x++; }
}
static void init(Cell *grid, int top, int bottom) {
    memset(grid,0,COLS*ROWS*sizeof(Cell));
    for(int i=0;i<COLS*ROWS;i++) {grid[i].ch=' ';if(i/COLS>=top&&i/COLS<bottom)grid[i].bg=0x334455;}
}
static void fixture(Cell *grid, int first) {
    grid[first*COLS].ch=0x203a;
    text(grid,first,2,"This is a shaded message whose complete sentence stretches into the menu and needs to wrap safely.");
    text(grid,first+1,2,"The following physical row belongs to the same prompt and keeps its original colors and content.");
    grid[first*COLS+12].fg=0xff2233;grid[first*COLS+12].attr=1;grid[first*COLS+12].link=11;
}
static int run(Cell *grid, ChromeReflow *out, int side, int dock_y, int dock_h, int cursor) {
    unsigned char protected_rows[ROWS]={0}; int wrap[ROWS]={0};
    chrome_panel_protect(grid,COLS,ROWS,0,background,protected_rows);
    assert(chrome_reflow_build_checked(out,grid,COLS,ROWS,protected_rows,side,
        side==1?80:0,dock_y,20,dock_h,3,safe));
    int moved=-1;
    int changes=chrome_panel_reflow_apply(out,grid,COLS,ROWS,wrap,cursor,side,
        side==1?80:0,dock_y,20,dock_h,0,background,&moved);
    out->cursor=moved;
    return changes;
}
static void invariant(Cell *grid, ChromeReflow *out, int top, int bottom, int side, int cursor) {
    int previous=-1;
    for(int i=0;i<COLS*ROWS;i++) {
        if(i/COLS<top||i/COLS>=bottom) assert(!memcmp(grid+i,out->cells+i,sizeof(Cell)));
        if(out->cells[i].ch>' ') {
            int s=out->source[i]; assert(s>=0&&s<COLS*ROWS);
            assert(!memcmp(grid+s,out->cells+i,sizeof(Cell)));
            assert(s>previous); previous=s;
            if(i/COLS>=top&&i/COLS<bottom) assert(side==1?i%COLS<78:i%COLS>=22);
        }
        if(i/COLS>=top&&i/COLS<bottom) assert(out->cells[i].bg==0x334455);
    }
    if(cursor>=0) {assert(out->cursor>=0);assert(out->source[out->cursor]==cursor);assert(!memcmp(out->cells+out->cursor,grid+cursor,sizeof(Cell)));}
}
int main(int argc,char **argv) {
    Cell grid[COLS*ROWS], original[COLS*ROWS]; ChromeReflow out={0};
    int scenario=argc>1?atoi(argv[1]):0;
    init(grid,2,10);fixture(grid,3);
    if(scenario==0||scenario==1) {
        int side=scenario+1;memcpy(original,grid,sizeof(grid));
        assert(run(grid,&out,side,0,12,-1)==1);assert(!memcmp(grid,original,sizeof(grid)));
        invariant(grid,&out,2,10,side,-1);
    } else if(scenario==2||scenario==3) {
        int cursor=scenario==2?3*COLS+86:4*COLS+97;
        assert(run(grid,&out,1,0,12,cursor)==1);invariant(grid,&out,2,10,1,cursor);
    } else if(scenario==4) {
        init(grid,3,5);fixture(grid,3);memcpy(original,grid,sizeof(grid));
        assert(run(grid,&out,1,0,12,-1)==0);assert(!memcmp(grid,out.cells,sizeof(grid)));
        assert(!memcmp(grid,original,sizeof(grid)));
    } else if(scenario==5) {
        grid[3*COLS+30].ch=0x754c;grid[3*COLS+30].wflag=1;
        grid[3*COLS+31].ch=0;grid[3*COLS+31].wflag=2;
        assert(run(grid,&out,1,0,12,3*COLS+30)==1);invariant(grid,&out,2,10,1,3*COLS+30);
        assert(out.cells[out.cursor].wflag==1&&out.cells[out.cursor+1].wflag==2);
    } else if(scenario==6) {
        grid[3*COLS].ch='X';
        assert(run(grid,&out,1,0,12,-1)==0);assert(!memcmp(grid,out.cells,sizeof(grid)));
    } else if(scenario==7) {
        text(grid,2,0,"a status bar is not a prompt");
        assert(run(grid,&out,1,0,12,-1)==0);assert(!memcmp(grid,out.cells,sizeof(grid)));
    } else if(scenario==8) {
        assert(run(grid,&out,1,11,4,-1)==0);assert(!memcmp(grid,out.cells,sizeof(grid)));
    } else if(scenario==9) {
        grid[4*COLS+5].bg=0x999999;
        assert(run(grid,&out,1,0,12,-1)==0); /* uncertain mixed-background widget */
    } else if(scenario==10) {
        int cursor=6*COLS+2;
        assert(run(grid,&out,1,0,12,cursor)==1);invariant(grid,&out,2,10,1,cursor);
        assert(out.cursor/COLS>5);
    } else if(scenario==11) {
        for(int x=0;x<COLS;x++) {grid[5*COLS+x]=grid[4*COLS+x];grid[4*COLS+x].ch=' ';grid[4*COLS+x].attr=0;grid[4*COLS+x].link=0;}
        assert(run(grid,&out,1,0,12,-1)==1);invariant(grid,&out,2,10,1,-1);
        int blank=0;
        for(int y=4;y<8;y++) {int ink=0;for(int x=0;x<COLS;x++)ink+=out.cells[y*COLS+x].ch>' ';if(!ink)blank++;}
        assert(blank>=1);
    } else if(scenario>=12 && scenario<=14) {
        unsigned char own[ROWS]={0}, protected_rows[ROWS]={0};
        int cursor=3*COLS+86,mapped=cursor;
        chrome_panel_protect(grid,COLS,ROWS,0,background,own);
        assert(chrome_reflow_build_checked(&out,grid,COLS,ROWS,own,1,80,0,20,12,3,safe));
        memcpy(original,grid,sizeof(grid));
        /* Body protection and blank padding protection must both veto the
           whole panel. Adjacent rows must not suppress independent work. */
        protected_rows[scenario==12?3:scenario==13?9:10]=1;
        int changed=chrome_panel_reflow_apply_checked(&out,grid,COLS,ROWS,NULL,cursor,
            1,80,0,20,12,0,background,&mapped,protected_rows);
        assert(!memcmp(grid,original,sizeof(grid)));
        if(scenario<14) {
            assert(changed==0 && out.changed==0 && mapped==cursor);
            assert(!memcmp(out.cells,grid,sizeof(grid)));
            for(int i=0;i<COLS*ROWS;i++)assert(out.source[i]==i);
        } else {
            assert(changed==1);out.cursor=mapped;invariant(grid,&out,2,10,1,cursor);
        }
    }
    chrome_reflow_free(&out);puts("ok");return 0;
}
'''

class Panels(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="hc-reflow-panels-")
        source = Path(cls.tmp.name) / "panels.c"
        source.write_text(HARNESS)
        cls.binary = Path(cls.tmp.name) / "panels"
        flags = shlex.split(os.environ.get("HC_TEST_CFLAGS", ""))
        subprocess.run([os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra", "-Werror",
                        *flags, "-I", str(ROOT), str(source), "-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def check_scenario(self, case):
        subprocess.run([str(self.binary), str(case)], check=True, capture_output=True)

    def test_historical_right_and_left(self):
        for case in (0, 1):
            with self.subTest(case=case): self.check_scenario(case)
    def test_live_cursor_in_text_and_final_blank(self):
        for case in (2, 3):
            with self.subTest(case=case): self.check_scenario(case)
    def test_insufficient_own_space_is_unchanged(self): self.check_scenario(4)
    def test_wide_glyph_and_cursor_are_kept_together(self): self.check_scenario(5)
    def test_uncertain_shapes_are_not_rewritten(self):
        for case in (6, 7, 9):
            with self.subTest(case=case): self.check_scenario(case)
    def test_nonoverlapping_panel_is_unchanged(self): self.check_scenario(8)
    def test_cursor_on_empty_input_line_remains_visible(self): self.check_scenario(10)
    def test_paragraph_separator_is_retained(self): self.check_scenario(11)
    def test_protected_body_or_padding_preserves_entire_panel(self):
        for case in (12, 13):
            with self.subTest(case=case): self.check_scenario(case)
    def test_adjacent_protected_row_does_not_block_panel(self): self.check_scenario(14)

if __name__ == "__main__": unittest.main()
