#!/usr/bin/env python3
"""Run the production capture/emission path at each supported Cutie width."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from test_chrome_edges import function
from test_mac_import_resize import bounded_run

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CW 12
#define CH 26
#define SB_COLS 28
#include "hardcore_chrome_pack.h"
typedef struct {int x0,y0,x1,y1;unsigned u0,v0,u1,v1;uint32_t rgba;} Inst;
typedef struct {int top,bottom;} ChromeLayout;
static ChromeLayout g_chrome={2,4};
static Inst input[16],emitted[4096],*g_chrome_quads=input;
static int emitted_count;
static int g_chrome_quad_count,g_chrome_capture_failed,g_chrome_capture;
static int g_chrome_menu_count,g_chrome_menu_raised,g_chrome_menu_capacity,g_chrome_menu_cols;
static int g_chrome_menu_top,g_chrome_menu_x,g_chrome_menu_cw,g_chrome_menu_ch;
static int g_chrome_surface_cols=SB_COLS,g_chrome_x=10*CW,g_clip_w=100*CW,g_tab_underlay=1;
static int g_chrome_round_x,g_chrome_round_w,g_chrome_round_right,g_chrome_join_top,g_chrome_join_bottom;
static int intrusion_left,intrusion_right;
static float g_chrome_motion_alpha=.75f,g_chrome_menu_alpha;
static uint32_t g_rgba;
static struct{uint32_t dim;}g_look={0xabcdef};
static ChromePackRow *g_chrome_menu_rows;
static int chrome_min(int a,int b){return a<b?a:b;}
static int chrome_max(int a,int b){return a>b?a:b;}
static int chrome_intrusion_span_at(int row,int dock,int top,int bottom,int *left,int *right){
    assert(row>=top && row<bottom);*left=dock+intrusion_left;*right=dock+intrusion_right;
    return intrusion_right>intrusion_left;
}
static void r_quad_uv(float x0,float y0,float x1,float y1,unsigned u0,unsigned v0,unsigned u1,unsigned v1){
    assert(emitted_count<4096);emitted[emitted_count++]=(Inst){x0,y0,x1,y1,u0,v0,u1,v1,g_rgba};
}
static void quad(float x0,float y0,float x1,float y1){r_quad_uv(x0,y0,x1,y1,0xffff,0,0,0);}
static void r_color(uint32_t color,float alpha){(void)alpha;g_rgba=color;}
HELPERS
static void scene(int width){
    g_chrome_surface_cols=width;g_chrome_x=10*CW;g_clip_w=100*CW;
    chrome_capture_begin();emitted_count=0;intrusion_left=intrusion_right=0;
    int x=g_chrome_x,y=(g_chrome.top+1)*CH;
    /* Exact atlas/style identity must survive, including a wide glyph whose
       right half lies well beyond the old fixed 28-column menu boundary. */
    input[0]=(Inst){x,y,x+width*CW,y+CH,0xffff,0,0,0,0x102030ff};
    input[1]=(Inst){x+(width-2)*CW,y,x+width*CW,y+CH,17,23,31,47,0x456789ff};
    input[2]=(Inst){x+CW,y,x+2*CW,y+CH,19,29,37,41,0xfedcbaff};
    g_chrome_quad_count=3;
}
static Inst *glyph(unsigned u){for(int i=0;i<emitted_count;i++)if(emitted[i].u0==u)return emitted+i;return NULL;}
static void identity(void){
    const int widths[]={18,28,42,64,18};
    for(unsigned n=0;n<sizeof(widths)/sizeof(widths[0]);n++){
        int width=widths[n];scene(width);chrome_capture_end(1);
        assert(!g_chrome_capture && g_chrome_menu_count==2 && g_chrome_menu_cols==width);
        assert(g_chrome_menu_x==10*CW && g_chrome_menu_cw==CW && g_chrome_menu_ch==CH);
        assert(g_chrome_menu_raised && g_chrome_menu_alpha==g_chrome_motion_alpha);
        for(int row=0;row<2;row++)for(int x=0;x<CHROME_PACK_MAX;x++){
            ChromePackRow *p=g_chrome_menu_rows+row;
            assert(p->src[x]==(x<width?x:-1) && p->dst[x]==(x<width?x:-1));
        }
        Inst *wide=glyph(17);assert(wide && !memcmp(wide,input+1,sizeof(*wide)));
        assert(glyph(19) && !memcmp(glyph(19),input+2,sizeof(Inst)));
        assert(g_chrome_round_w==width*CW && g_chrome_x==10*CW);
    }
}
static void packed(void){
    scene(64);intrusion_left=60;intrusion_right=63;chrome_capture_end(0);
    assert(g_chrome_menu_cols==64 && !g_chrome_menu_raised);
    ChromePackRow *p=g_chrome_menu_rows+1;int shift=0;
    assert(chrome_pack_translation(p,62,64,&shift) && shift<0);
    assert(p->dst[63]==p->dst[62]+1);
    Inst *wide=glyph(17);assert(wide && wide->x1-wide->x0==2*CW);
    assert(wide->x0==input[1].x0+shift && wide->rgba==input[1].rgba && wide->u1==input[1].u1);
    for(int x=60;x<63;x++)assert(p->src[x]==-1 && !p->actionable[x]);
    int right_edge=0;
    for(int i=0;i<emitted_count;i++)if(emitted[i].u0==0xffff && emitted[i].x0==10*CW+63*CW && emitted[i].x1==10*CW+63*CW+1)right_edge++;
    assert(right_edge==2); /* Both rows notch at the actual 64-column edge. */
    assert(!chrome_pack_translation(p,63,65,&shift));
}
static void invalid(void){
    scene(18);input[1].x0=10*CW+18*CW;input[1].x1+=CW;chrome_capture_end(1);
    assert(!glyph(17)); /* A glyph outside the smaller surface cannot leak. */
    scene(65);chrome_capture_end(1);
    assert(!emitted_count && g_chrome_menu_count==-1 && !g_chrome_menu_cols);
    scene(18);g_chrome_capture_failed=1;chrome_capture_end(1);
    assert(!emitted_count && g_chrome_menu_count==-1 && !g_chrome_menu_cols);
}
int main(int argc,char **argv){assert(argc==2);
    if(!strcmp(argv[1],"identity"))identity();else if(!strcmp(argv[1],"packed"))packed();else invalid();
    free(g_chrome_menu_rows);return 0;
}
'''


class CaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        renderer = (ROOT / "hardcore_chrome_draw.h").read_text()
        names = ("chrome_capture_begin", "chrome_capture_plan", "chrome_emit_raw", "chrome_corner_inset",
                 "chrome_emit_inst", "chrome_pack_translation", "chrome_capture_end")
        source = HARNESS.replace("HELPERS", "\n".join(function(renderer, name) for name in names))
        cls.temp = tempfile.TemporaryDirectory(prefix="hc-chrome-capture-")
        path = Path(cls.temp.name)
        harness = path / "capture.c"
        harness.write_text(source)
        cls.exe = path / "capture"
        command = shlex.split(os.environ.get("CC", "clang"))
        command += ["-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT)]
        command += shlex.split(os.environ.get("HC_TEST_CFLAGS", ""))
        result = bounded_run(command + [str(harness), "-o", str(cls.exe)], 30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, case):
        result = bounded_run([str(self.exe), case], 10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_resized_capture_preserves_atlas_styles_wide_glyphs_and_maps(self):
        self.check("identity")

    def test_intrusions_and_glyph_translation_beyond_column_28(self):
        self.check("packed")

    def test_outside_glyphs_and_invalid_width_fail_closed(self):
        self.check("invalid")


if __name__ == "__main__":
    unittest.main()
