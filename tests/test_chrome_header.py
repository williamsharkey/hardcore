#!/usr/bin/env python3
"""Exercise production header layout, painted labels and hit targets headlessly."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r"static (?:int|void|ChromeLayout) " + name + r"\([^)]*\)\s*\{", source)
    if not match:
        raise AssertionError(f"missing production helper {name}")
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define SB_COLS 28
#define CW 12
#define CH 26
#define UI_FONT
#define UI_DONE
#define TAB_HOVER 1
#define AMBER 2
#define TK_BROWSER 1
#define NFAMILIES 10
ENUM
typedef struct {
    int top, grip_rows, tab_row, tab_capacity, tab_count;
    int settings_row, section_row, list_row, list_capacity, list_count, bottom;
} ChromeLayout;
HEADER
static ChromeLayout g_chrome;
static int g_chrome_drag, g_tab_underlay=1, g_chrome_menu_raised, g_chrome_menu_count;
static int g_tab_offset, g_settings_open, g_settings_tab, g_setting_offset[3];
static int g_ntabs=4, source_col=-2;
typedef struct { int kind; } TestTab;
static TestTab tab;
static struct { int light, dark; } FAMILIES[10];
static struct { uint32_t text, detail, dim, red; } g_look={3,4,5,6};
static int chrome_min(int a,int b) { return a<b?a:b; }
static int chrome_max(int a,int b) { return a>b?a:b; }
static int sidebar_x(void) { return 72*CW; }
static int sidebar_w(void) { return SB_COLS*CW; }
static void tab_layout(void) {}
static TestTab *active_tab(void) { return &tab; }
static int chrome_tv_hit(int x,int y,int *idx) { (void)x;(void)y;(void)idx;return HIT_NONE; }
static int chrome_menu_source_col(int row,int col) { (void)row;return source_col==-2?col:source_col; }
static int chrome_page_hit(int col,int offset,int capacity,int total,int tab,int *direction) {
    (void)col;(void)offset;(void)capacity;(void)total;(void)tab;(void)direction;return 0;
}
static int chrome_item_total(void) { return 10; }
static int font_menu_hit(int item,int col) { (void)item;(void)col;return -1; }
static int chrome_pager_geometry(int offset,int capacity,int total,int tab,int *next,int *counter) {
    (void)offset;(void)capacity;(void)total;(void)tab;(void)next;(void)counter;return 0;
}
static int scheme_index(int key) { return key; }
static float row_y(int r) { return (float)(r*CH); }
static int caf_active;
static int mac_caf_active(void) { return caf_active; }
static int mac_record_active(void) { return 0; }
static int export_active;
static int mac_record_exports_active(void) { return export_active; }
#ifndef _WIN32
#define DRIP_MS 80
static unsigned GetTickCount(void) { return 80; }
static int g_ndrip=2;
static uint16_t g_drip[]={0x2801,0x2802};
#endif
static int fills, glyphs, frames, sides, row_fills;
static struct { int col,row; wchar_t text[32]; uint32_t color; } labels[8];
static float fx,fy,fr,fb;
static void fill_rect(float x,float y,float right,float bottom,uint32_t color,float alpha) {
    assert(color==TAB_HOVER && alpha==1); fills++; fx=x;fy=y;fr=right;fb=bottom;
}
static void grid_text(const wchar_t *text,int col,int row,uint32_t color) {
    assert(glyphs<8); assert(col>=0 && col+(int)wcslen(text)<SB_COLS);
    assert(row>=g_chrome.top && row<g_chrome.tab_row);
    labels[glyphs].col=col; labels[glyphs].row=row; labels[glyphs].color=color;
    wcsncpy(labels[glyphs++].text,text,31);
}
static void chrome_row_fill(int row,int active,int hovered) {
    assert(row>=g_chrome.top && row<g_chrome.tab_row && !active && !hovered);row_fills++;
}
static void chrome_frame(int row,uint32_t color) { assert(row==g_chrome.top && color==g_look.dim);frames++; }
static void chrome_sides(int row,uint32_t color) { assert(row==g_chrome.top+1 && color==g_look.dim);sides++; }
FUNCTIONS
static void geometry(void) {
    for(int rows=0;rows<=80;rows++)for(int tabs=0;tabs<=64;tabs+=4)
      for(int dock=-10;dock<100;dock+=11)for(int underlay=0;underlay<=2;underlay++) {
        ChromeLayout a=chrome_primary_measure(rows,4,tabs,0,underlay,dock,11);
        assert(a.top>=0 && a.bottom>=a.top && a.bottom<=rows);
        assert(a.tab_row==a.top+a.grip_rows);
        assert(a.tab_count>=0 && a.tab_count<=tabs && a.tab_count<=a.tab_capacity);
#ifdef _WIN32
        ChromeLayout expected=chrome_measure(rows,4,tabs,0,underlay,dock,11);
        assert(!memcmp(&a,&expected,sizeof(a)));
        assert(a.grip_rows==(rows>0));
#else
        assert(a.grip_rows==chrome_min(2,rows));
        assert(a.settings_row==-1 && a.section_row==-1 && a.list_row==-1);
        assert(a.tab_capacity==chrome_min(tabs,rows-a.tab_row));
        assert(a.bottom==a.tab_row+a.tab_count);
        ChromeLayout open=chrome_primary_measure(rows,4,tabs,1,underlay,dock,37);
        assert(!memcmp(&a,&open,sizeof(a))); /* TVs never resize the primary header. */
#endif
    }
}
static void targets(void) {
    g_chrome=chrome_primary_measure(40,1,g_ntabs,0,1,8,0);
    int idx=-1;
#ifdef _WIN32
    assert(hit_tabbar(sidebar_x()+CW,8*CH,&idx)==HIT_GRIP);
    assert(hit_tabbar(sidebar_x()+4*CW,8*CH,&idx)==HIT_BAR);
#else
    unsigned char occupied[2][SB_COLS]={{0}};
    const wchar_t *expected[]={L"↕ move",L"⚙config",L"cam",L"CAF",L"+term",L"+www",L"files"};
    assert(CHROME_HEADER_CONTROL_COUNT==7);
    for(int i=0;i<CHROME_HEADER_CONTROL_COUNT;i++) {
        const ChromeHeaderControl *c=CHROME_HEADER_CONTROLS+i;
        assert(!wcscmp(c->label,expected[i]) && c->col>0 && c->width>=(int)wcslen(c->label));
        assert(c->row>=0 && c->row<2 && c->col+c->width<SB_COLS);
        for(int x=c->col;x<c->col+c->width;x++) {
            assert(!occupied[c->row][x]++);
            assert(hit_tabbar(sidebar_x()+x*CW,(g_chrome.top+c->row)*CH,&idx)==c->hit);
        }
    }
    for(int y=0;y<2;y++)for(int x=0;x<SB_COLS;x++)if(!occupied[y][x]) {
        assert(chrome_header_hit(y,x)==HIT_CHROME_INERT);
        assert(hit_tabbar(sidebar_x()+x*CW,(g_chrome.top+y)*CH,&idx)==HIT_CHROME_INERT);
    }
    /* A relocated label keeps its action; an occluded gap never becomes a window drag. */
    g_chrome_menu_count=6; source_col=11;
    assert(hit_tabbar(sidebar_x()+3*CW,g_chrome.top*CH,&idx)==HIT_SETTINGS);
    source_col=8;
    assert(hit_tabbar(sidebar_x()+3*CW,g_chrome.top*CH,&idx)==HIT_CHROME_INERT);
    source_col=-1;
    assert(hit_tabbar(sidebar_x()+3*CW,g_chrome.top*CH,&idx)==HIT_NONE);
    source_col=-2;g_chrome_menu_count=0;
#endif
    assert(hit_tabbar(sidebar_x()+3*CW,g_chrome.tab_row*CH,&idx)==HIT_TAB && idx==0);
    assert(hit_tabbar(sidebar_x()+3*CW,(g_chrome.top-1)*CH,&idx)==HIT_NONE);
    assert(hit_tabbar(sidebar_x()-1,g_chrome.top*CH,&idx)==HIT_NONE);
}
static void reset_paint(void) { fills=glyphs=frames=sides=row_fills=0;memset(labels,0,sizeof(labels)); }
static void paint(void) {
    g_chrome=chrome_primary_measure(40,1,g_ntabs,0,1,8,0);
    chrome_render_header(HIT_NONE);
#ifdef _WIN32
    assert(glyphs==2 && frames==1 && !sides && row_fills==1 && !fills);
    assert(!wcscmp(labels[0].text,L"↕") && !wcscmp(labels[1].text,L"hardcore"));
    reset_paint();chrome_render_header(HIT_GRIP);
    assert(fills==1 && fx==CW && fr==2*CW);
#else
    assert(glyphs==7 && frames==1 && sides==1 && row_fills==2 && !fills);
    for(int i=0;i<CHROME_HEADER_CONTROL_COUNT;i++) {
        const ChromeHeaderControl *c=CHROME_HEADER_CONTROLS+i;
        assert(labels[i].col==c->col && labels[i].row==g_chrome.top+c->row);
        assert(!wcscmp(labels[i].text,c->hit==HIT_CAF?L"caf":c->label));
        reset_paint();chrome_render_header(c->hit);
        assert(fills==1 && fx==c->col*CW && fr==(c->col+c->width)*CW);
        assert(fy==(g_chrome.top+c->row)*CH && fb==fy+CH);
        assert(labels[i].color==g_look.text);
    }
    caf_active=1;reset_paint();chrome_render_header(HIT_NONE);
    assert(labels[3].color==AMBER && !wcscmp(labels[3].text,L"CAF"));
    export_active=1;reset_paint();chrome_render_header(HIT_NONE);
    assert(glyphs==8 && labels[3].col==21 && labels[3].color==AMBER);
    assert(labels[3].text[0]==0x2802); /* same drip frame as busy terminal tabs */
    export_active=0;
    g_chrome=chrome_primary_measure(1,1,g_ntabs,0,1,8,0);
    reset_paint();chrome_render_header(HIT_NONE);
    assert(glyphs==4 && row_fills==1 && frames==1 && !sides);
#endif
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"geometry"))geometry();
    else if(!strcmp(argv[1],"targets"))targets();
    else if(!strcmp(argv[1],"paint"))paint();
    else assert(0);
    return 0;
}
'''


class ChromeHeaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        chrome = (ROOT / "hardcore_chrome.h").read_text()
        draw = (ROOT / "hardcore_chrome_draw.h").read_text()
        enum = re.search(r"enum \{.*?\};", chrome, re.S).group(0)
        header_start = chrome.index("#ifndef _WIN32\n/* Label and hit geometry")
        header_end = chrome.index("static ChromeLayout g_chrome;", header_start)
        functions = "\n".join(function(chrome, name) for name in
                              ("chrome_measure", "chrome_primary_measure", "hit_tabbar"))
        functions += "\n" + function(draw, "chrome_render_header")
        cls.temp = tempfile.TemporaryDirectory(prefix="hc-chrome-header-")
        source = Path(cls.temp.name) / "test.c"
        source.write_text(HARNESS.replace("\nENUM\n", "\n" + enum + "\n")
                          .replace("\nHEADER\n", "\n" + chrome[header_start:header_end] + "\n")
                          .replace("\nFUNCTIONS\n", "\n" + functions + "\n"))
        cls.binaries = {}
        for platform in ("mac", "windows"):
            binary = Path(cls.temp.name) / platform
            command = shlex.split(os.environ.get("CC", "cc")) + ["-std=c99", "-Wall", "-Wextra",
                      "-Werror", "-Wno-unused-function", "-O1"]
            command += shlex.split(os.environ.get("CFLAGS", ""))
            if platform == "windows":
                command += ["-D_WIN32"]
            result = subprocess.run(command + [str(source), "-o", str(binary)], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stderr)
            cls.binaries[platform] = binary

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, case):
        for platform, binary in self.binaries.items():
            with self.subTest(platform=platform):
                subprocess.run([str(binary), case], check=True, capture_output=True, text=True)

    def test_bounded_layout_and_windows_compatibility(self):
        self.check("geometry")

    def test_action_targets_gaps_and_packed_mapping(self):
        self.check("targets")

    def test_rendered_labels_and_hit_highlights_match(self):
        self.check("paint")


if __name__ == "__main__":
    unittest.main()
