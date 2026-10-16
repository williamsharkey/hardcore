#!/usr/bin/env python3
"""Preview policy plus the production browser projection, without a live UI."""
import os
from pathlib import Path
import shlex
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]

BROWSER_HARNESS = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CW 12
#define CH 26
#define SB_COLS 28
#define BROWSER_IMAGE 8
#define A_INV 16
typedef struct { uint32_t ch,fg,bg; uint8_t attr,wflag; uint16_t link; } Cell;
typedef struct { Cell *cells; int count; } BrowserBlock;
typedef struct { int cols,rows,m_cursor_vis,view; Cell *row[1][10]; int rlen[1][10]; } Term;
typedef struct { Term t; int browser_native; void *browser; } Tab;
@interface HCBrowserImage : NSObject { @public int sw,sh,cols,rows; }
@end
@implementation HCBrowserImage
@end
@interface HCBrowser : NSObject { @public
    int dirty,lastCols,lastRows,lastX,lastY,lastHeight,lastSide,lastPreview,pageRows;
    int nodeCols,nodeRows,blockCount,scroll,total,focusedAction,inspectOpen,editKind;
    uint16_t *nodeGrid; BrowserBlock *blocks; NSArray *images;
}
@end
@implementation HCBrowser
@end
static struct { int top,bottom; } g_chrome={1,5};
static int g_tab_underlay=1,g_scale=1,preview,layouts,grid_calls,grid_scroll,grid_rows;
static Tab *displayed;
static int chrome_preview_active(void) { return preview; }
static Tab *display_tab(void) { return displayed; }
static HCBrowser *browser_for(Tab *tab) { return (HCBrowser *)tab->browser; }
static void tab_layout(void) { layouts++; }
static int sidebar_x(void) { return 52*CW; }
static int mac_browser_page_rows(Tab *tab) { return ((HCBrowser *)tab->browser)->inspectOpen ? tab->t.rows-3 : tab->t.rows; }
static void browser_image_size(int sw,int sh,int cw,int ch,int *w,int *h) {
    (void)sw;(void)sh;(void)cw;(void)ch;*w=2;*h=2;
}
static int browser_grid_layout_nodes(BrowserBlock *blocks,int count,Cell *cells,uint16_t *nodes,
    int cols,int rows,int scroll,int side,int x,int y,int width,int height) {
    (void)blocks;(void)count;(void)nodes;(void)side;(void)x;(void)y;(void)width;(void)height;
    grid_calls++;grid_scroll=scroll;grid_rows=rows;
    for(int i=0;i<cols*rows;i++)cells[i].ch=' ';
    cells[0].ch='P';return 4;
}
/* FUNCTION */
int main(void) {
    @autoreleasepool {
        HCBrowser *browser=[HCBrowser new];browser->dirty=1;browser->scroll=99;
        browser->inspectOpen=1;browser->editKind=2;
        Tab tab={.t={.cols=80,.rows=10},.browser_native=1,.browser=browser};
        for(int row=0;row<10;row++)tab.t.row[0][row]=calloc(80,sizeof(Cell));
        displayed=&tab;
        mac_browser_prepare(&tab);
        assert(!layouts && !grid_calls); /* Native activation stays native. */
        preview=1;mac_browser_prepare(&tab);
        assert(layouts==1 && grid_calls==2 && grid_rows==10 && grid_scroll==0);
        assert(tab.t.row[0][0][0].ch=='P' && tab.t.rlen[0][0]==80);
        assert(tab.browser_native==1 && browser->scroll==99 && browser->inspectOpen==1 && browser->editKind==2);
        int calls=grid_calls;mac_browser_prepare(&tab);
        assert(grid_calls==calls); /* Repeated paints use the cached layout. */
        browser->dirty=1;mac_browser_prepare(&tab);
        assert(grid_calls>calls && browser->scroll==99);
        calls=grid_calls;preview=0;mac_browser_prepare(&tab);
        assert(grid_calls==calls && browser->scroll==99 && tab.browser_native==1);
        tab.browser_native=0;browser->inspectOpen=0;mac_browser_prepare(&tab);
        /* Equal grid dimensions still rebuild when preview becomes active,
           so a temporary preview clamp never becomes a stale active view. */
        assert(grid_calls>calls && browser->scroll==0 && !browser->lastPreview);
        calls=grid_calls;preview=1;displayed=NULL;mac_browser_prepare(&tab);
        assert(grid_calls==calls); /* Unrelated tabs aren't preview targets. */
        for(int row=0;row<10;row++)free(tab.t.row[0][row]);
        free(browser->nodeGrid);[browser release];
        puts("Browser preview: Native cache, no mode/scroll/edit changes, cache reuse and exit invalidation");
    }
    return 0;
}
'''


class TabPreviewTests(unittest.TestCase):
    def compile_and_run(self, source, objective_c=False):
        with tempfile.TemporaryDirectory(prefix='hc-tab-preview-') as directory:
            path = Path(directory)
            harness = path / ('preview.m' if objective_c else 'preview.c')
            if isinstance(source, Path):
                harness = source
            else:
                harness.write_text(source)
            executable = path / 'preview'
            command = shlex.split(os.environ.get('CC', 'clang'))
            command += ['-Wall', '-Wextra', '-Werror', '-std=c99', '-I', str(ROOT)]
            command += shlex.split(os.environ.get('HARDCORE_TEST_CFLAGS', ''))
            if objective_c:
                command += ['-framework', 'Foundation']
            result = bounded_run(command + [str(harness), '-o', str(executable)], 30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = bounded_run([str(executable)], 10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_preview_identity_and_lifecycle_policy(self):
        self.compile_and_run(ROOT / 'tests/test_tab_preview.c')

    @unittest.skipUnless(sys.platform == 'darwin', 'Foundation required')
    def test_native_browser_uses_read_only_cached_projection(self):
        function = production_function(ROOT / 'hardcore_browser_mac.m', 'static void mac_browser_prepare(Tab *tb)')
        self.compile_and_run(BROWSER_HARNESS.replace('/* FUNCTION */', function), objective_c=True)


if __name__ == '__main__':
    unittest.main()
