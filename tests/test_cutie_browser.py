#!/usr/bin/env python3
"""Exercise production Cutie browser projection/action helpers without a live UI."""
import os
from pathlib import Path
import shlex
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t ch,fg,bg; uint8_t attr,wflag; uint16_t link; } Cell;
#include "hardcore_browser_grid.h"
@interface FakeWeb : NSObject { @public NSString *script; void (^pending)(id,NSError *); int calls; }
- (void)evaluateJavaScript:(NSString *)code inFrame:(id)frame inContentWorld:(id)world completionHandler:(void (^)(id,NSError *))completion;
- (void)finish:(id)info;
@end
@implementation FakeWeb
- (void)evaluateJavaScript:(NSString *)code inFrame:(id)frame inContentWorld:(id)world completionHandler:(void (^)(id,NSError *))completion {
    (void)frame;(void)world;calls++;[script release];script=[code copy];[pending release];pending=[completion copy];
}
- (void)finish:(id)info { void (^callback)(id,NSError *)=[pending copy];[pending release];pending=nil;callback(info,nil);[callback release]; }
- (void)dealloc { [script release];[pending release];[super dealloc]; }
@end
@interface HCBrowser : NSObject { @public
    int generation,epoch,cutieGeneration,cutiePopupEpoch,blockCount,scroll,total,dirty,lastCols,lastRows;
    NSTimeInterval cutiePopupUntil;BrowserBlock *blocks;FakeWeb *web;
}
@end
@implementation HCBrowser
@end
typedef struct { int id;HCBrowser *browser;int cols,rows,view; } Tab;
static Tab *g_tabs[2];static int g_ntabs=2,active=0,control_calls=0,control_node;
static HCBrowser *browser_for(Tab *tb) { return tb ? tb->browser : nil; }
static NSString *browser_string(id value) {return [value isKindOfClass:NSString.class] ? value : @"";}
static id browser_world(void) {return nil;}
static Tab *tab_by_id(int identity) {for(int i=0;i<g_ntabs;i++)if(g_tabs[i] && g_tabs[i]->id==identity)return g_tabs[i];return NULL;}
static void activate(int at) {active=at;}
static int g_grid_on;static void grid_toggle(void) {g_grid_on=!g_grid_on;}
static void mac_browser_begin_control(Tab *tb,int node,int revision,int epoch,NSDictionary *info) {
    assert(tb==g_tabs[active] && revision==tb->browser->generation && epoch==tb->browser->epoch && info);
    control_calls++;control_node=node;
}
/* FUNCTIONS */
static void projection(void) {
    Cell source[80],saved[80],output[26*7];
    for(int i=0;i<80;i++)source[i]=(Cell){.ch='a'+i%26,.fg=0x112233,.bg=0x445566,.attr=1,.link=7};
    source[78].ch=0x754c;source[78].wflag=1;source[79].ch=' ';source[79].wflag=2;
    BrowserBlock block={.cells=source,.count=80};
    HCBrowser *b=[HCBrowser new];b->blocks=&block;b->blockCount=1;b->scroll=321;b->total=999;b->dirty=1;b->lastCols=123;b->lastRows=34;
    Tab tab={.id=2,.browser=b,.cols=90,.rows=35,.view=7};
    memcpy(saved,source,sizeof(source));Tab before=tab;
    assert(mac_browser_cutie_project(&tab,output,26,7,0)==4);
    assert(output[0].ch=='a' && output[0].link==7 && output[78].wflag==1 && output[79].wflag==2);
    for(int i=0;i<80;i++)assert(!memcmp(output+i,source+i,sizeof(Cell)));
    assert(!memcmp(source,saved,sizeof(source)) && !memcmp(&before,&tab,sizeof(tab)));
    assert(b->scroll==321 && b->total==999 && b->dirty==1 && b->lastCols==123 && b->lastRows==34);
    Cell small[26*2];assert(mac_browser_cutie_project(&tab,small,26,2,999)==4);
    assert(!memcmp(small,source+52,28*sizeof(Cell))); /* Excess scrolling clamps locally. */
    assert(b->scroll==321 && !memcmp(source,saved,sizeof(source)));
    Cell image={.ch=1,.fg=2,.bg=1,.wflag=BROWSER_IMAGE,.link=8};
    block.cells=&image;block.count=1;
    assert(mac_browser_cutie_project(&tab,output,26,7,0)==1);
    assert(output[0].ch==0x25a7 && !output[0].wflag && output[0].link==8);
    assert(output[1].ch==' ' && output[1].link==8 && image.ch==1 && image.wflag==BROWSER_IMAGE);
    assert(!mac_browser_cutie_project(NULL,output,26,7,0));assert(output[0].ch==' ');
    assert(!mac_browser_cutie_project(&tab,output,3,7,0));
    [b release];
}
static void actions(void) {
    HCBrowser *b=[HCBrowser new];b->web=[FakeWeb new];b->generation=9;b->epoch=3;b->cutieGeneration=8;
    Tab selected={.id=1},source={.id=2,.browser=b};g_tabs[0]=&selected;g_tabs[1]=&source;
    assert(mac_browser_cutie_generation(&source)==8 && !mac_browser_cutie_generation(NULL));
    browser_cutie_changed(b);assert(mac_browser_cutie_generation(&source)==9);
    b->cutieGeneration=0x7fffffff;browser_cutie_changed(b);assert(b->cutieGeneration==1);
    mac_browser_cutie_action(&source,7);assert([b->web->script containsString:@".info(7,9)"]);
    [b->web finish:@{@"tag":@"A",@"href":@"https://example.invalid/"}];
    assert(active==0 && !control_calls && [b->web->script containsString:@".activate(7,9)"]);
    assert(b->cutiePopupEpoch==3 && b->cutiePopupUntil>NSProcessInfo.processInfo.systemUptime);
    /* A replacement snapshot between info request and reply cannot act. */
    mac_browser_cutie_action(&source,8);browser_cutie_changed(b);int calls=b->web->calls;
    [b->web finish:@{@"tag":@"BUTTON"}];assert(b->web->calls==calls && active==0);
    /* An identity reused by another browser object is likewise rejected. */
    mac_browser_cutie_action(&source,8);source.browser=nil;calls=b->web->calls;
    [b->web finish:@{@"tag":@"BUTTON"}];assert(b->web->calls==calls && active==0);source.browser=b;
    mac_browser_cutie_action(&source,8);b->epoch++;calls=b->web->calls;
    [b->web finish:@{@"tag":@"BUTTON"}];assert(b->web->calls==calls);
    mac_browser_cutie_action(&source,8);[b->web finish:@{@"tag":@"INPUT",@"type":@"checkbox"}];
    assert(active==0 && !control_calls && [b->web->script containsString:@".activate(8,9)"]);
    mac_browser_cutie_action(&source,9);[b->web finish:@{@"tag":@"TEXTAREA"}];
    assert(active==1 && control_calls==1 && control_node==9);
    active=0;mac_browser_cutie_action(&source,10);[b->web finish:@{@"tag":@"SELECT"}];
    assert(active==1 && control_calls==2 && control_node==10);
    calls=b->web->calls;mac_browser_cutie_action(&source,4001);mac_browser_cutie_action(NULL,1);assert(b->web->calls==calls);
    [b->web release];[b release];
}
int main(int argc,char **argv) {assert(argc==2);@autoreleasepool {
    if(!strcmp(argv[1],"project"))projection();else if(!strcmp(argv[1],"action"))actions();else return 2;
}return 0;}
'''


@unittest.skipUnless(sys.platform == "darwin", "Foundation required")
class CutieBrowserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="hc-cutie-browser-")
        path = Path(cls.temp.name)
        source = ROOT / "hardcore_browser_mac.m"
        functions = "\n".join(production_function(source, signature) for signature in (
            "static void browser_cutie_changed(HCBrowser *b)",
            "static int mac_browser_cutie_generation(Tab *tb)",
            "static int mac_browser_cutie_project(Tab *tb,Cell *output,int cols,int rows,int scroll)",
            "static void mac_browser_cutie_action(Tab *tb,int action)",
        ))
        harness = path / "browser.m"
        harness.write_text(HARNESS.replace("/* FUNCTIONS */", functions))
        cls.exe = path / "browser"
        command = shlex.split(os.environ.get("CC", "clang"))
        command += ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fblocks", "-I", str(ROOT), "-framework", "Foundation"]
        command += shlex.split(os.environ.get("HARDCORE_TEST_CFLAGS", ""))
        result = bounded_run(command + [str(harness), "-o", str(cls.exe)], 30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, case):
        result = bounded_run([str(self.exe), case], 10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_independent_projection_preserves_source_style_actions_and_scroll(self):
        self.check("project")

    def test_actions_keep_focus_and_reject_stale_snapshots(self):
        self.check("action")


if __name__ == "__main__":
    unittest.main()
