#!/usr/bin/env python3
"""Offline Dock-summary regressions using the production Cocoa view/controller.

The app and Dock tile are local fakes. No NSApplication, window, shell, user
preferences, or existing terminal session is launched or changed. Font tables
are regenerated privately; real font lookup, theme settings and offscreen Cocoa
drawing verify the same pixels and colors as the application.

From the repository root, optional PNG previews at 128/64/32 pixels:
    python3 tests/test_mac_dock.py --previews build/dock-previews
"""
from pathlib import Path
import json
import re
import shutil
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <sys/stat.h>

/* FONT_AND_THEME */

/* RENDERER */

static int glyph_calls;
/* GLYPH_ADAPTER */
static int mac_dock_glyph(NSString *key, uint32_t cp, unsigned char *pixels, int stride) {
    glyph_calls++;
    return production_mac_dock_glyph(key, cp, pixels, stride);
}

static NSBitmapImageRep *render_summary(NSDictionary *summary, int size) {
    NSBitmapImageRep *bitmap = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
        pixelsWide:size pixelsHigh:size bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
        isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:size * 4 bitsPerPixel:32] autorelease];
    memset([bitmap bitmapData], 0, (size_t)size * size * 4);
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
    hc_dock_draw_summary(NSMakeRect(0, 0, size, size), summary);
    [NSGraphicsContext restoreGraphicsState];
    return bitmap;
}

@interface HCTestDock : NSObject {
    NSView *_contentView;
}
@property(nonatomic, retain) NSView *contentView;
@property(nonatomic) int displays;
@property(nonatomic) int assignments;
@property(nonatomic) BOOL showsApplicationBadge;
- (NSSize)size;
- (void)display;
@end
@implementation HCTestDock
@synthesize contentView = _contentView;
- (void)setContentView:(NSView *)view {
    if (_contentView != view) {
        [_contentView release]; _contentView = [view retain];
    }
    self.assignments++;
}
- (NSSize)size { return NSMakeSize(128, 128); }
- (void)display {
    self.displays++;
    if (_contentView) render_summary([(HCDockSummaryView *)_contentView summary], 128);
}
- (void)dealloc { [_contentView release]; [super dealloc]; }
@end

@interface HCTestApp : NSObject
@property(nonatomic, retain) HCTestDock *dockTile;
@end
@implementation HCTestApp
- (void)dealloc { [_dockTile release]; [super dealloc]; }
@end

static HCTestApp *test_app;
#define NSApp test_app
#define NSDockTile HCTestDock

enum { PS_NONE, PS_IDLE, PS_WORKING, PS_BLOCKED, PS_ERROR, PS_DONE };
enum { TK_TERMINAL, TK_BROWSER };
typedef struct { char cwd[1024]; wchar_t title[256]; int ps_state; } Term;
typedef struct {
    Term t;
    char start_dir[1024], program[64], run[1024], browser_site[512], browser_url[2048];
    int kind, browser_native, exited, help, attention, busy;
} Tab;
static Tab fixture;
static int g_ntabs = 1;
static Tab *current_tab = &fixture;
static const char *g_shell = "/bin/zsh";
static int g_mac_dock_summary = 1, g_mac_dock_ready;
static HCDockSummaryView *g_mac_dock_view;
static double g_mac_dock_last_check, fake_time = 1000;
static double uptime_ms(void) { return fake_time; }
static Tab *active_tab(void) { return current_tab; }

#define MAX_PATH 4096
static char g_cfg_dir[MAX_PATH], g_cfg_dir_line[MAX_PATH], g_start_dir[MAX_PATH], g_last_dir[MAX_PATH];
static void expand_home(const char *value, char *out, int size) {
    snprintf(out, (size_t)size, "%s", value);
}
/* CONTROLLER */

static void require(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void equal(id actual, id expected, const char *message) {
    if (![actual isEqual:expected]) {
        fprintf(stderr, "FAIL: %s (actual %s, expected %s)\n", message,
                [[actual description] UTF8String], [[expected description] UTF8String]);
        exit(1);
    }
}
static NSDictionary *model(NSString *folder, NSString *program, NSString *status, int tabs) {
    return @{@"folder":folder, @"program":program, @"status":status, @"tabs":@(tabs),
             @"appearance":hc_dock_canonical_appearance(nil)};
}
static void start_controller(void) {
    test_app = [[HCTestApp alloc] init];
    test_app.dockTile = [[[HCTestDock alloc] init] autorelease];
    strcpy(fixture.start_dir, "/Users/wm");
    strcpy(fixture.program, "codex");
    g_mac_dock_ready = 1;
    mac_dock_update(1);
    require(test_app.dockTile.displays == 1, "initial summary must display exactly once");
    require(test_app.dockTile.contentView == g_mac_dock_view, "Dock does not own summary view");
}
static void finish_controller(void) {
    [test_app release]; test_app = nil;
    [g_mac_dock_view release]; g_mac_dock_view = nil;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc == 2 || argc == 3, "case required");
        /* These private blobs are regenerated from the repository sources. */
        NSData *fontData = [NSData dataWithContentsOfFile:@"FONT_BLOB_PATH"];
        NSData *fontsData = [NSData dataWithContentsOfFile:@"FONTS_BLOB_PATH"];
        require([fontData length] && [fontsData length], "generated font data missing");
        font_blob = [fontData bytes]; font_blob_end = font_blob + [fontData length];
        fonts_blob = [fontsData bytes];
        config_look("font", "H"); config_look("theme", "g");
        hc_dock_read_glyph = mac_dock_glyph;
        const char *which = argv[1];
        if (!strcmp(which, "canonical-model")) {
            HCDockSummaryView *view = [[HCDockSummaryView alloc] initWithFrame:NSMakeRect(0, 0, 128, 128)];
            require([view setSummary:model(@" wm \n", @" codex\t", @"", 3)], "new model not accepted");
            equal([view summary], model(@"wm", @"codex", @"", 3), "canonical summary differs");
            require(![view setSummary:model(@"wm", @"codex", @"", 3)], "equivalent labels cause redraw");
            require([view setSummary:model(@"wm", @"claude", @"", 3)], "foreground app change ignored");
            require([[view accessibilityLabel] containsString:@"claude"], "accessible description omits program");
            require([[view accessibilityLabel] containsString:@"3 tabs"], "accessible description omits tab count");
            [view release];
        } else if (!strcmp(which, "model-ownership")) {
            HCDockSummaryView *view = [[HCDockSummaryView alloc] initWithFrame:NSZeroRect];
            NSMutableString *folder = [NSMutableString stringWithString:@"original"];
            NSMutableDictionary *source = [NSMutableDictionary dictionaryWithDictionary:model(folder, @"codex", @"working", 1)];
            [view setSummary:source];
            [folder setString:@"mutated"]; source[@"program"] = @"mutated";
            equal([view summary], model(@"original", @"codex", @"working", 1), "caller changed stored model");
            [view release];
        } else if (!strcmp(which, "safe-labels")) {
            NSString *unicode = @"café e\u0301 👩🏽‍💻 界";
            equal(hc_dock_label(unicode, @"fallback"), unicode, "valid composed Unicode was damaged");
            equal(hc_dock_label(@"\t  wm\n codex \033\u202e\007  ", @"fallback"), @"wm codex", "controls or line breaks remain");
            equal(hc_dock_label(@"\007\n\t", @"fallback"), @"fallback", "blank labels need fallback");
            equal(hc_dock_canonical_summary(@{@"folder":@42, @"program":[NSNull null], @"status":@[], @"tabs":@(-8)}),
                  model(@"Hardcore", @"terminal", @"", 0), "invalid model types not normalized");
            equal(hc_dock_canonical_summary(model(@"wm", @"codex", @"", 50000))[@"tabs"], @9999, "tab count unbounded");
            NSMutableString *longLabel = [NSMutableString string];
            for (int i = 0; i < 200; i++) [longLabel appendString:@"e\u0301"];
            NSString *bounded = hc_dock_label(longLabel, @"fallback");
            require([bounded hasSuffix:@"…"] && [bounded length] == 321, "composed label bound differs");
        } else if (!strcmp(which, "label-fitting")) {
            NSString *text = @"👩🏽‍💻e\u0301界 long folder name";
            for (NSString *key in @[@"H", @"C", @"R"]) {
              config_look("font", [key UTF8String]);
              NSDictionary *appearance = mac_dock_appearance();
              for (int width = 0; width < 240; width++) {
                NSString *fitted = hc_dock_fit_label(text, appearance, width);
                require(hc_dock_label_width(fitted, appearance) <= width, "fitted label escapes its rectangle");
                if ([fitted length] && ![fitted isEqual:text]) {
                    require([fitted hasSuffix:@"…"], "truncated label is unmarked");
                    NSString *prefix = [fitted substringToIndex:[fitted length]-1];
                    require(![prefix length] || [text hasPrefix:prefix], "fitted label is not a source prefix");
                    if ([prefix length]) require(NSMaxRange([text rangeOfComposedCharacterSequenceAtIndex:[prefix length]-1]) == [prefix length], "truncated a composed character");
                }
              }
            }
        } else if (!strcmp(which, "startup-gate")) {
            mac_dock_update(1);
            require(!g_mac_dock_view, "pre-AppKit title update allocated a view");
            start_controller(); finish_controller();
        } else if (!strcmp(which, "unchanged-output")) {
            start_controller();
            int initial_glyph_calls = glyph_calls;
            require(initial_glyph_calls > 0, "initial Dock drawing never read native glyphs");
            for (int i = 0; i < 10000; i++) {
                fake_time += .1;
                mac_dock_update(i % 13 == 0);
            }
            require(test_app.dockTile.displays == 1, "unchanged terminal output redraws Dock");
            require(test_app.dockTile.assignments == 1, "unchanged terminal output reassigns Dock view");
            require(glyph_calls == initial_glyph_calls, "unchanged terminal output rerasterizes native glyphs");
            finish_controller();
        } else if (!strcmp(which, "throttle-latest")) {
            start_controller();
            fake_time += 10; strcpy(fixture.program, "claude"); mac_dock_update(0);
            fake_time += 10; strcpy(fixture.program, "grok"); mac_dock_update(0);
            require(test_app.dockTile.displays == 1, "poll rate exceeds throttle");
            fake_time += 250; mac_dock_update(0);
            require(test_app.dockTile.displays == 2, "last throttled update was lost");
            equal([g_mac_dock_view summary][@"program"], @"grok", "throttle displayed stale intermediate model");
            strcpy(fixture.program, "vim"); mac_dock_update(1);
            require(test_app.dockTile.displays == 3, "forced active tab change waits for timer");
            mac_dock_update(1);
            require(test_app.dockTile.displays == 3, "force redraws unchanged model");
            finish_controller();
        } else if (!strcmp(which, "disable-enable")) {
            start_controller();
            g_mac_dock_summary = 0; mac_dock_update(1);
            require(test_app.dockTile.contentView == nil, "disable does not restore original icon");
            int displays = test_app.dockTile.displays, assignments = test_app.dockTile.assignments;
            fake_time += 500; mac_dock_update(0);
            require(test_app.dockTile.displays == displays && test_app.dockTile.assignments == assignments, "disabled icon repeatedly updates");
            g_mac_dock_summary = 1; mac_dock_update(1);
            require(test_app.dockTile.contentView == g_mac_dock_view, "re-enable does not restore custom view");
            require(test_app.dockTile.displays == displays+1, "unchanged model not displayed on re-enable");
            finish_controller();
        } else if (!strcmp(which, "summary-transitions")) {
            start_controller();
            equal([g_mac_dock_view summary][@"folder"], @"wm", "start directory fallback wrong");
            strcpy(fixture.t.cwd, "/work/café"); strcpy(fixture.program, "claude"); g_ntabs = 2;
            mac_dock_update(1);
            equal([g_mac_dock_view summary][@"folder"], @"café", "current directory did not supersede startup directory");
            equal([g_mac_dock_view summary][@"program"], @"claude", "foreground app did not update");
            equal([g_mac_dock_view summary][@"tabs"], @2, "tab count did not update");
            fixture.busy = 1; mac_dock_update(1);
            NSString *working = [[g_mac_dock_view summary][@"status"] copy];
            require([working length] > 0, "working status absent");
            fixture.t.ps_state = PS_BLOCKED; mac_dock_update(1);
            NSString *blocked = [[g_mac_dock_view summary][@"status"] copy];
            require([blocked length] > 0 && ![blocked isEqual:working], "blocked status did not override working");
            fixture.t.ps_state = PS_ERROR; mac_dock_update(1);
            require(![[g_mac_dock_view summary][@"status"] isEqual:blocked], "error and blocked states indistinguishable");
            [working release]; [blocked release];
            fixture.busy = fixture.t.ps_state = 0; fixture.program[0] = 0; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"program"], @"zsh", "foreground exit did not restore shell label");
            strcpy(fixture.run, "terminal:/dev/ttys123"); mac_dock_update(1);
            equal([g_mac_dock_view summary][@"status"], @"bridge", "Terminal mirror is not identified in Dock");
            finish_controller();
        } else if (!strcmp(which, "browser-summary")) {
            start_controller();
            fixture.kind = TK_BROWSER;
            strcpy(fixture.browser_site, "Wikipedia");
            strcpy(fixture.browser_url, "https://en.wikipedia.org/wiki/Caf%C3%A9");
            wcscpy(fixture.t.title, L"Café — Wikipedia");
            mac_dock_update(1);
            equal([g_mac_dock_view summary][@"folder"], @"Wikipedia", "browser Dock used terminal directory");
            equal([g_mac_dock_view summary][@"program"], @"Café — Wikipedia", "browser Dock lost Unicode page title");
            equal([g_mac_dock_view summary][@"status"], @"web", "grid browser mode absent from Dock");
            fixture.browser_native = 1; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"status"], @"native web", "native browser mode absent from Dock");
            fixture.busy = 1; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"status"], @"loading", "browser load status did not override rendering mode");
            fixture.browser_site[0] = 0; fixture.t.title[0] = 0; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"folder"], @"en.wikipedia.org", "browser URL host fallback absent");
            equal([g_mac_dock_view summary][@"program"], @"Web page", "browser without title showed stale terminal program");
            fixture.browser_url[0] = 0; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"folder"], @"Browser", "browser without metadata fallback absent");
            finish_controller();
        } else if (!strcmp(which, "summary-fallbacks")) {
            start_controller();
            strcpy(fixture.t.cwd, "/work/project///"); mac_dock_update(1);
            equal([g_mac_dock_view summary][@"folder"], @"project", "trailing slash hides folder name");
            strcpy(fixture.t.cwd, "/"); mac_dock_update(1);
            equal([g_mac_dock_view summary][@"folder"], @"/", "root directory hidden");
            strcpy(fixture.t.cwd, "/bad-\xff"); strcpy(fixture.program, "\xff"); g_shell = NULL;
            mac_dock_update(1);
            equal([g_mac_dock_view summary][@"folder"], @"~", "invalid UTF-8 folder did not fall back");
            equal([g_mac_dock_view summary][@"program"], @"shell", "invalid UTF-8 program did not fall back");
            fixture.help = 1; fixture.t.ps_state = PS_ERROR; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"program"], @"help", "help tab shows stale process");
            fixture.exited = 1; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"program"], @"exited", "exited tab shows stale process");
            current_tab = NULL; g_ntabs = 0; mac_dock_update(1);
            equal([g_mac_dock_view summary][@"tabs"], @0, "empty tab model retains stale count");
            equal([g_mac_dock_view summary][@"program"], @"hardcore", "empty tab model retains process");
            finish_controller();
        } else if (!strcmp(which, "appearance-invalidation")) {
            start_controller();
            int previous_calls = glyph_calls;
            config_look("font", "R"); mac_dock_update(1);
            require(test_app.dockTile.displays == 2 && glyph_calls > previous_calls, "font-only change did not redraw native glyphs");
            equal([g_mac_dock_view summary][@"appearance"][@"fontKey"], @"R", "Dock kept the prior font");
            previous_calls = glyph_calls;
            config_look("theme", "G"); mac_dock_update(1);
            require(test_app.dockTile.displays == 3 && glyph_calls > previous_calls, "theme-only change did not redraw");
            equal([g_mac_dock_view summary][@"appearance"][@"bg"], @0xfbf1c7, "Dock did not adopt light background");
            /* Preview changes g_shown, without committing g_scheme. */
            look_set(scheme_by_key('h')); mac_dock_update(1);
            require(g_scheme->key == 'G' && g_shown->key == 'h', "preview changed saved scheme");
            require(test_app.dockTile.displays == 4, "live theme preview did not redraw");
            equal([g_mac_dock_view summary][@"appearance"][@"themeKey"], @"h", "Dock ignored preview theme identity");
            g_look.accent ^= 0x102030; mac_dock_update(1);
            require(test_app.dockTile.displays == 5, "palette change with same theme key did not redraw");
            previous_calls = glyph_calls; mac_dock_update(1);
            require(test_app.dockTile.displays == 5 && glyph_calls == previous_calls, "unchanged appearance rerendered");
            finish_controller();
        } else if (!strcmp(which, "appearance-ownership")) {
            HCDockSummaryView *view = [[HCDockSummaryView alloc] initWithFrame:NSZeroRect];
            NSMutableDictionary *appearance = [NSMutableDictionary dictionaryWithDictionary:mac_dock_appearance()];
            NSMutableString *fontKey = [NSMutableString stringWithString:@"H"];
            appearance[@"fontKey"] = fontKey;
            NSMutableDictionary *source = [NSMutableDictionary dictionaryWithDictionary:model(@"wm", @"codex", @"", 1)];
            source[@"appearance"] = appearance; [view setSummary:source];
            [fontKey setString:@"R"]; appearance[@"bg"] = @0xffffff;
            equal([view summary][@"appearance"][@"fontKey"], @"H", "caller mutated stored font identity");
            equal([view summary][@"appearance"][@"bg"], @0x282828, "caller mutated stored palette");
            NSDictionary *bad = hc_dock_canonical_appearance(@{@"fontWidth":@999, @"fontHeight":@(-9), @"bg":@0xff123456,
                                                              @"fg":@"invalid", @"fontKey":@42, @"themeKey":[NSNull null]});
            equal(bad[@"fontWidth"], @40, "unbounded bitmap width"); equal(bad[@"fontHeight"], @1, "invalid bitmap height");
            equal(bad[@"bg"], @0x123456, "RGB value retains unsupported alpha bits");
            equal(bad[@"fontKey"], @"H", "invalid font key did not fall back");
            equal(bad[@"themeKey"], @"g", "invalid theme key did not fall back");
            [view release];
        } else if (!strcmp(which, "native-glyphs")) {
            const uint32_t points[] = {'I', 'f', ' ', 0x0301, 0x200d, 0x754c, 0x1f680};
            for (int fi = 0; fi < NFONTS; fi++) {
                const Font *font = &FONTS[fi];
                NSString *key = [NSString stringWithFormat:@"%c", font->key];
                for (unsigned pi = 0; pi < sizeof(points)/sizeof(points[0]); pi++) {
                    uint32_t cp = points[pi];
                    struct { unsigned char before[32], pixels[40*40], after[32]; } guarded;
                    memset(&guarded, 0xa5, sizeof(guarded)); memset(guarded.pixels, 0, sizeof(guarded.pixels));
                    unsigned char expected[SLOT*SLOT] = {0};
                    int expectedWidth = cp_width(cp) <= 0 ? 0 : cp == ' ' ? font->w : glyph_bits(font, cp, expected);
                    int actualWidth = mac_dock_glyph(key, cp, guarded.pixels, 40);
                    require(actualWidth == expectedWidth, "Dock glyph advance differs from native terminal renderer");
                    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++)
                        require(guarded.pixels[y*40+x] == expected[y*SLOT+x], "Dock changed native glyph pixels or rendered a blank/zero-width character");
                    for (int n = 0; n < 32; n++) require(guarded.before[n] == 0xa5 && guarded.after[n] == 0xa5, "Dock glyph callback escaped buffer");
                    if (cp == 'I' && strchr("HCR", font->key)) {
                        int ink = 0;
                        for (int x = 0; x < font->w; x++) ink += guarded.pixels[(font->h/2)*40+x] != 0;
                        require(ink == 1, "native Hairline/Courier I stem is no longer one pixel");
                    }
                }
            }
            unsigned char fallback[40*40] = {0}, expected[SLOT*SLOT] = {0};
            int width = mac_dock_glyph(@"?invalid", 'I', fallback, 40);
            require(width == glyph_bits(FONTS, 'I', expected) && !memcmp(fallback, expected, sizeof(fallback)), "invalid font identity must use real Unifont fallback");
        } else if (!strcmp(which, "native-fitting")) {
            for (NSString *key in @[@"H", @"C", @"R"]) {
                config_look("font", [key UTF8String]);
                NSDictionary *appearance = mac_dock_appearance();
                CGFloat width = g_font->w;
                require(hc_dock_label_width(@"III", appearance) == width*3, "label uses substituted system font metrics");
                require(hc_dock_label_width(@"界", appearance) == width*2, "wide glyph measured as single cell");
                require(hc_dock_label_width(@"e\u0301", appearance) == hc_dock_label_width(@"é", appearance), "decomposed and NFC label widths differ");
                equal(hc_dock_fit_label(@"abcdef", appearance, width*6), @"abcdef", "exactly fitting native label was truncated");
                equal(hc_dock_fit_label(@"abcdef", appearance, width*4), @"abc…", "native truncation does not reserve ellipsis cell");
                equal(hc_dock_fit_label(@"e\u0301abcdef", appearance, width*2), @"e\u0301…", "native truncation split composed source character");
            }
        } else if (!strcmp(which, "native-label-raster")) {
            for (NSString *key in @[@"H", @"C", @"R"]) {
                config_look("font", [key UTF8String]);
                int width = g_font->w * 3, height = g_font->h;
                NSBitmapImageRep *bitmap = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                    pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
                    isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width * 4 bitsPerPixel:32] autorelease];
                unsigned char *data = [bitmap bitmapData]; memset(data, 0, (size_t)width * height * 4);
                [NSGraphicsContext saveGraphicsState];
                [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap]];
                hc_dock_draw_label(@"If ", NSMakeRect(0, 0, width, height), mac_dock_appearance(), 0x123456, YES, NO);
                [NSGraphicsContext restoreGraphicsState];
                for (int cell = 0; cell < 3; cell++) {
                    unsigned char expected[SLOT*SLOT] = {0};
                    if (cell < 2) glyph_bits(g_font, cell ? 'f' : 'I', expected);
                    for (int y = 0; y < height; y++) for (int x = 0; x < g_font->w; x++) {
                        unsigned char *pixel = data + ((size_t)y * width + cell * g_font->w + x) * 4;
                        require(pixel[3] == expected[y*SLOT+x], "native label raster substituted, smoothed or displaced font pixels");
                        if (pixel[3]) require(abs(pixel[0]-0x12) <= 2 && abs(pixel[1]-0x34) <= 2 && abs(pixel[2]-0x56) <= 2,
                                              "native label ink ignored requested palette color");
                    }
                }
            }
        } else if (!strcmp(which, "light-palette")) {
            config_look("theme", "G");
            NSDictionary *appearance = mac_dock_appearance();
            equal(appearance[@"bg"], @0xfbf1c7, "light theme background mismatch");
            equal(appearance[@"fg"], @0x3c3836, "light theme foreground mismatch");
            equal(appearance[@"red"], @0x9d0006, "light status red does not use current scheme");
            equal(appearance[@"green"], @0x79740e, "light status green does not use current scheme");
            equal(appearance[@"yellow"], @0xb57614, "light status yellow does not use current scheme");
            equal(appearance[@"blue"], @0x076678, "light status blue does not use current scheme");
            NSBitmapImageRep *bitmap = render_summary(mac_dock_summary(), 128);
            unsigned char *pixel = [bitmap bitmapData] + (64 * 128 + 10) * 4;
            require(abs(pixel[0]-0xfb) <= 2 && abs(pixel[1]-0xf1) <= 2 && abs(pixel[2]-0xc7) <= 2 && pixel[3] == 255,
                    "rendered light tile ignores selected background palette");
        } else if (!strcmp(which, "previews")) {
            require(argc == 3, "preview directory required");
            strcpy(fixture.start_dir, "/Users/wm/hardcore"); strcpy(fixture.program, "codex"); g_ntabs = 3;
            for (NSString *variant in @[@"g-H", @"g-R", @"G-C"]) {
                config_look("theme", [[variant substringToIndex:1] UTF8String]);
                config_look("font", [[variant substringFromIndex:2] UTF8String]);
                for (int size = 128; size >= 32; size /= 2) {
                    NSBitmapImageRep *bitmap = render_summary(mac_dock_summary(), size);
                    NSString *path = [[NSString stringWithUTF8String:argv[2]] stringByAppendingPathComponent:
                                      [NSString stringWithFormat:@"%@-%d.png", variant, size]];
                    require([[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES], "write private preview image");
                }
            }
        } else if (!strcmp(which, "settings-roundtrip")) {
            require(argc == 3 && !strncmp(argv[2], "/private/tmp/hc-dock-tests-", 27), "owned config directory required");
            require(!setenv("HARDCORE_TEST_CONFIG", argv[2], 1), "private configuration override failed");
            config_load();
            require(!strcmp(g_cfg_dir, argv[2]), "configuration escaped owned directory");
            require(g_mac_dock_summary == 1, "missing config changed enabled default");
            strcpy(g_cfg_dir_line, "/fixture/project"); strcpy(g_last_dir, "/fixture/recent");
            NSString *path = [[NSString stringWithUTF8String:g_cfg_dir] stringByAppendingPathComponent:@"config.ini"];
            for (int enabled = 0; enabled <= 1; enabled++) {
                g_mac_dock_summary = enabled; config_look("font", "H"); config_look("theme", "h");
                config_save();
                NSString *saved = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:NULL];
                require([saved containsString:enabled ? @"dock_summary=1\n" : @"dock_summary=0\n"], "Dock setting not saved");
                require([saved containsString:@"font=H\n"] && [saved containsString:@"theme=h\n"], "look settings not saved beside Dock setting");
                g_mac_dock_summary = !enabled; config_look("font", "U"); config_look("theme", "g");
                g_start_dir[0] = g_last_dir[0] = 0;
                config_load();
                require(g_mac_dock_summary == enabled, "Dock preference did not survive save/load");
                require(g_font->key == 'H' && g_scheme->key == 'h' && g_shown->key == 'h', "Dock preference displaced other settings");
                equal(mac_dock_appearance()[@"bg"], @0x0c0c0c, "saved Hardcore theme silently became Gruvbox");
                require(!strcmp(g_start_dir, "/fixture/project") && !strcmp(g_last_dir, "/fixture/recent"), "directory settings changed during roundtrip");
            }
        } else require(NO, "unknown case");
        printf("passed %s\n", which);
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Cocoa required')
class MacDockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-dock-tests-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        commands = [
            [sys.executable, str(ROOT/'tools/gen_fonts.py'), str(cls.directory/'fonts.bin'), str(cls.directory/'fonts.inc')],
            [sys.executable, str(ROOT/'tools/gen_font.py'), str(cls.directory/'font.bin'),
             str(ROOT/'font/unifont-18.0.01.hex.gz'), str(ROOT/'font/hardcore-extra.hex')],
        ]
        for command in commands:
            result = bounded_run(command, timeout=30)
            if result.returncode:
                raise AssertionError('font generation failed:\n'+result.stderr)
        widths = bounded_run([sys.executable, str(ROOT/'tools/gen_width.py'), str(ROOT/'font/ucd')], timeout=10)
        if widths.returncode:
            raise AssertionError('Unicode width generation failed:\n'+widths.stderr)
        core = (ROOT/'hardcore.c').read_text()
        font_struct = re.search(r'typedef struct \{\n    char key;.*?\n\} Font;', core, re.DOTALL).group(0)
        scheme_start = core.index('typedef struct {\n    char key;\n    const char *name;\n    int dark;')
        scheme_end = core.index('static char g_start_dir', scheme_start)
        native = [
            font_struct, 'static const Font FONTS[] = {\n'+(cls.directory/'fonts.inc').read_text()+'\n};',
            '#define NFONTS ((int)(sizeof(FONTS) / sizeof(FONTS[0])))',
            re.search(r'^#define SLOT \d+', core, re.MULTILINE).group(0),
            'static int g_scale = 1;\nstatic const Font *g_font = FONTS, *g_tfont = FONTS, *g_cf = FONTS;',
            'static const unsigned char *font_blob, *font_blob_end, *fonts_blob;',
        ]
        for name in ('font_record', 'font_by_key', 'glyph_bits'):
            native.append(re.search(r'^static [^\n]*\b'+name+r'\([^\n]*\) \{.*?^\}', core, re.MULTILINE | re.DOTALL).group(0))
        for name in ('cell_w', 'cell_h'):
            native.append(re.search(r'^static int '+name+r'\([^\n]+\n', core, re.MULTILINE).group(0))
        native += [core[scheme_start:scheme_end], 'static uint32_t PAL[256];',
                   production_function(ROOT/'hardcore.c', 'static void look_set(const Scheme *s)'),
                   'static int g_tab_underlay, g_chrome_reflow = 1, g_chrome_debug, g_dock_row;',
                   production_function(ROOT/'hardcore.c', 'static void config_look(const char *key, const char *v)'),
                   production_function(ROOT/'hardcore.c', 'static void config_default_font(void)'),
                   production_function(ROOT/'hardcore.c', 'static void config_look_save(FILE *f)'),
                   widths.stdout,
                   production_function(ROOT/'hardcore.c', 'static int in_ranges(uint32_t cp, const uint32_t (*r)[2], int n)'),
                   production_function(ROOT/'hardcore.c', 'static int cp_width(uint32_t cp)')]
        adapter = production_function(ROOT/'hardcore_mac.m',
            'static int mac_dock_glyph(NSString *fontKey, uint32_t cp, unsigned char *pixels, int stride)')
        adapter = adapter.replace('mac_dock_glyph(', 'production_mac_dock_glyph(', 1)
        controller = '\n\n'.join([
            production_function(ROOT/'hardcore_mac.m', 'static NSString *mac_dock_text(const char *text, NSString *fallback)'),
            production_function(ROOT/'hardcore_mac.m', 'static NSDictionary *mac_dock_appearance(void)'),
            production_function(ROOT/'hardcore_mac.m', 'static NSDictionary *mac_dock_summary(void)'),
            production_function(ROOT/'hardcore_mac.m', 'static void mac_dock_update(int force)'),
            production_function(ROOT/'hardcore_mac.m', 'static void config_load(void)'),
            production_function(ROOT/'hardcore_mac.m', 'static void config_save(void)'),
        ])
        renderer = (ROOT/'hardcore_mac_dock.m').read_text()
        mutation = 'if ([_summary isEqualToDictionary:next]) return NO;'
        if mutation not in renderer:
            raise AssertionError('Dock negative-control mutation point moved')
        variants = {'dock': renderer, 'always-redraw': renderer.replace(mutation, '')}
        for name, body in variants.items():
            source = cls.directory/(name+'.m')
            source.write_text(HARNESS.replace('/* FONT_AND_THEME */', '\n'.join(native))
                              .replace('/* RENDERER */', body).replace('/* GLYPH_ADAPTER */', adapter)
                              .replace('/* CONTROLLER */', controller)
                              .replace('"FONT_BLOB_PATH"', json.dumps(str(cls.directory/'font.bin')))
                              .replace('"FONTS_BLOB_PATH"', json.dumps(str(cls.directory/'fonts.bin'))))
            result = bounded_run(['clang', '-O0', '-Wall', '-Wextra', '-Werror',
                                  '-mmacosx-version-min=11.0', '-framework', 'Cocoa',
                                  str(source), '-o', str(cls.directory/name)], timeout=60)
            if result.returncode:
                raise AssertionError('Dock harness compile failed:\n'+result.stderr)
        cls.binary = cls.directory/'dock'

    def case(self, name):
        result = bounded_run([str(self.binary), name], timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(result.stdout.strip(), 'passed '+name)

    def test_canonical_model_changes_and_accessibility(self): self.case('canonical-model')
    def test_model_copies_caller_data(self): self.case('model-ownership')
    def test_labels_preserve_unicode_and_remove_controls(self): self.case('safe-labels')
    def test_labels_fit_without_splitting_composed_characters(self): self.case('label-fitting')
    def test_startup_before_appkit_is_deferred(self): self.case('startup-gate')
    def test_heavy_unchanged_output_never_redraws(self): self.case('unchanged-output')
    def test_throttle_coalesces_to_latest_and_force_is_immediate(self): self.case('throttle-latest')
    def test_disabling_restores_icon_and_reenabling_redraws(self): self.case('disable-enable')
    def test_active_tab_directory_process_and_status_transitions(self): self.case('summary-transitions')
    def test_browser_site_title_mode_and_metadata_fallbacks(self): self.case('browser-summary')
    def test_empty_exited_help_invalid_paths_and_shell_fallbacks(self): self.case('summary-fallbacks')
    def test_font_theme_palette_and_preview_changes_redraw(self): self.case('appearance-invalidation')
    def test_appearance_copies_caller_data_and_bounds_inputs(self): self.case('appearance-ownership')
    def test_real_font_glyphs_spaces_and_unicode_fit_guarded_buffers(self): self.case('native-glyphs')
    def test_hairline_and_courier_use_native_advances_for_fitting(self): self.case('native-fitting')
    def test_label_raster_preserves_exact_native_pixels_and_ink_color(self): self.case('native-label-raster')
    def test_light_theme_palette_is_rendered(self): self.case('light-palette')

    @classmethod
    def previews(cls, directory):
        private = cls.directory/'previews'
        private.mkdir()
        result = bounded_run([str(cls.binary), 'previews', str(private)], timeout=10)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)
        directory.mkdir(parents=True, exist_ok=True)
        for image in sorted(private.glob('*.png')):
            shutil.copyfile(image, directory/image.name)
        return sorted(directory.glob('*.png'))

    def test_dock_preference_roundtrip_preserves_other_settings(self):
        config = self.directory/'config-fixture'
        config.mkdir()
        result = bounded_run([str(self.binary), 'settings-roundtrip', str(config)], timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(result.stdout.strip(), 'passed settings-roundtrip')

    def test_negative_control_detects_redrawing_on_every_output(self):
        result = bounded_run([str(self.directory/'always-redraw'), 'unchanged-output'], timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn('unchanged terminal output redraws Dock', result.stderr)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--previews':
        if sys.platform != 'darwin':
            raise SystemExit('Cocoa required for offscreen previews')
        try:
            MacDockTests.setUpClass()
            for image in MacDockTests.previews(Path(sys.argv[2])):
                print(image)
        finally:
            MacDockTests.doClassCleanups()
    else:
        unittest.main()
