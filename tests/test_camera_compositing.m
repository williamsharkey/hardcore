/* Native preview composition regression. No capture session, devices, privacy
 * requests, onscreen window, or desktop screenshot is created. The real panel
 * sync function hosts a synthetic red AVCaptureVideoPreviewLayer beneath a
 * CAMetalLayer-backed AppKit view, then Core Animation renders its layer tree.
 *
 * clang -O1 -Wno-deprecated-declarations -Wno-unused-function \
 *   tests/test_camera_compositing.m -o build/test-camera-compositing \
 *   -framework Cocoa -framework AVFoundation -framework CoreMedia \
 *   -framework CoreVideo -framework AudioToolbox -framework Speech \
 *   -framework QuartzCore -framework Metal
 */
#import "../hardcore_camera_capture.m"
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include "../hardcore_tv.h"
#include <assert.h>
#include <wchar.h>
#include <stdio.h>

typedef struct {int w,h;} TestFont;
static TestFont ui_font={8,16},terminal_font={14,28};
static const TestFont *test_font=&terminal_font;
#define CW (test_font->w)
#define CH (test_font->h)
#define SB_COLS 28
#define UI_FONT const TestFont *saved_font=test_font;test_font=&ui_font
#define UI_DONE test_font=saved_font
#define DEF_BG 0x101010
#define BAR_BG 0x151515
#define TAB_HOVER 0x202020
#define AMBER 0xffcc44
#define _snwprintf swprintf

typedef struct {int cols,rows;} Term;
typedef struct {Term t;} Tab;
typedef struct {int top,bottom;} ChromeLayout;
typedef struct {HcTvState state;} ChromeTv;
typedef struct {Term *term;HcTvRect primary;} ChromeTvCostContext;
static ChromeLayout g_chrome={0,8};
static ChromeTv g_tvs[HC_TV_COUNT];
static Tab test_tab={{120,36}};
static HCCameraCapture *g_camera_capture;
static NSString *g_record_status;
static NSView *g_view;
static int g_chrome_x,g_chrome_surface_cols,g_chrome_capture,g_mouse_x=-1,g_mouse_y=-1,g_sel_drag,g_sb_drag;
static float g_draw_alpha;
static struct {uint32_t dim,text,red,detail,text_lo;} g_look;
static int chrome_min(int a,int b){return a<b?a:b;}
static int chrome_max(int a,int b){return a>b?a:b;}
static int sidebar_x(void){return (test_tab.t.cols-SB_COLS)*CW;}
static Tab *active_tab(void){return &test_tab;}
static void plat_invalidate(void){}
static void mac_cutie_blur(void){}
static void chrome_preview_cancel(void){}
static NSCursor *mac_tv_resize_cursor(void){return nil;}
static NSString *record_root(void){return @"/nonexistent-hardcore-camera-test";}
static int mac_record_active(void){return 0;}
static void mac_record_start(void){}
static void mac_record_pause(void){}
static void mac_record_stop(void){}
static void mac_record_discard(void){}
static int mac_record_exports_active(void){return 0;}
static NSArray *mac_record_jobs(void){return @[];}
static NSArray *mac_record_items(void){return @[];}
static BOOL mac_record_export(NSString *path,NSDictionary *edits,BOOL zip){return NO;}
static void mac_record_file_viewer(NSString *path){}
static void record_library_open(NSString *path){}
static void record_library_open_output(NSString *path){}
#define DRIP_MS 80
static unsigned GetTickCount(void){return 0;}
static int g_ndrip;
static uint16_t g_drip[40];
static void chrome_label(const wchar_t *text,int a,int b,int y,uint32_t color){(void)text;(void)a;(void)b;(void)y;(void)color;}
static void fill_rect(float a,float b,float c,float d,uint32_t color,float alpha){(void)a;(void)b;(void)c;(void)d;(void)color;(void)alpha;}
static void utf8_to_wide(const char *s,wchar_t *out,int capacity){(void)s;if(capacity)out[0]=0;}
static int chrome_tv_cost(HcTvRect r,HcTvCost *cost,void *opaque){(void)r;(void)cost;(void)opaque;return 1;}
static void chrome_tv_reflow_apply(Term *t,HcTvRect rect,const HcTvRect *prior,int count){(void)t;(void)rect;(void)prior;(void)count;}
static int g_grid_on;
#import "../hardcore_camera_panel.m"

/* Constructing HCCameraCapture only creates queues and an unattached preview
 * layer. No prepare/start method is called anywhere in this test. */
@interface HCSyntheticPreviewCapture : HCCameraCapture @end
@implementation HCSyntheticPreviewCapture
- (HCCameraCaptureState)state {return HCCameraCaptureReady;}
@end
@interface HCSyntheticMetalView : NSView @end
@implementation HCSyntheticMetalView
- (BOOL)isFlipped {return YES;}
- (BOOL)isOpaque {return YES;}
- (BOOL)wantsUpdateLayer {return YES;}
- (CALayer *)makeBackingLayer {
    CAMetalLayer *layer=[CAMetalLayer layer];layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly=NO;layer.maximumDrawableCount=2;layer.displaySyncEnabled=NO;
    layer.presentsWithTransaction=NO;layer.opaque=YES;layer.contentsGravity=kCAGravityTopLeft;
    return layer;
}
- (void)updateLayer {}
@end
static NSUInteger red_pixels(NSWindow *window) {
    [window.contentView layoutSubtreeIfNeeded];[window displayIfNeeded];[CATransaction flush];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.02]];
    NSUInteger width=(NSUInteger)g_view.bounds.size.width,height=(NSUInteger)g_view.bounds.size.height,count=0;
    unsigned char *pixels=calloc(width*height,4);assert(pixels);
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGContextRef context=CGBitmapContextCreate(pixels,width,height,8,width*4,space,(CGBitmapInfo)kCGImageAlphaPremultipliedLast);assert(context);
    [g_view.layer renderInContext:context];
    for(NSUInteger i=0;i<width*height;i++)if(pixels[i*4]>240 && pixels[i*4+1]<16 && pixels[i*4+2]<16 && pixels[i*4+3]>240)count++;
    CGContextRelease(context);CGColorSpaceRelease(space);free(pixels);return count;
}
int main(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        NSWindow *window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,960,576) styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        window.releasedWhenClosed=NO;
        CGFloat backing=window.backingScaleFactor;
        ui_font=(TestFont){(int)(8*backing),(int)(16*backing)};
        terminal_font=(TestFont){(int)(14*backing),(int)(28*backing)};
        g_view=[[HCSyntheticMetalView alloc] initWithFrame:NSMakeRect(0,0,960,576)];g_view.wantsLayer=YES;
        g_view.layerContentsRedrawPolicy=NSViewLayerContentsRedrawDuringViewResize;window.contentView=g_view;
        g_camera_capture=[[HCSyntheticPreviewCapture alloc] init];
        assert(g_camera_capture.previewLayer.session==nil);
        assert([g_view.layer isKindOfClass:CAMetalLayer.class]);
        g_camera_capture.previewLayer.backgroundColor=NSColor.redColor.CGColor;
        g_camera_visible=g_camera_placed=1;g_camera_rect=(HcTvRect){4,22,32,9};
        /* This timer-like call deliberately enters with terminal metrics. The
           former code placed y=644, entirely outside a 576-point canvas. */
        NSRect wrong=mac_camera_body_pixels();
        wrong=NSMakeRect(wrong.origin.x/backing,wrong.origin.y/backing,wrong.size.width/backing,wrong.size.height/backing);
        assert(NSIsEmptyRect(NSIntersectionRect(wrong,g_view.bounds)));
        mac_camera_preview_sync();assert(test_font==&terminal_font);
        NSRect expected=NSMakeRect(40,368,240,64);
        assert(NSEqualRects(g_camera_preview_view.frame,expected));
        assert(NSEqualRects(g_camera_capture.previewLayer.frame,g_camera_preview_view.bounds));
        assert(!window.isVisible && !g_camera_preview_view.hidden);
        NSUInteger colored=red_pixels(window);
        assert(g_camera_preview_view.layer.superlayer==g_view.layer);
        assert(g_camera_capture.previewLayer.superlayer==g_camera_preview_view.layer);
        assert(colored==(NSUInteger)(g_camera_preview_view.bounds.size.width*g_camera_preview_view.bounds.size.height));
        /* Moving/resizing updates the same preview layer and never loses it
           during the AppKit layer/layout pass. */
        g_camera_rect=(HcTvRect){10,4,42,12};mac_camera_preview_sync();
        assert(red_pixels(window)==(NSUInteger)(g_camera_preview_view.bounds.size.width*g_camera_preview_view.bounds.size.height));
        [window setContentSize:NSMakeSize(800,500)];mac_camera_preview_sync();
        assert(NSEqualSizes(g_view.bounds.size,NSMakeSize(800,500)));
        assert(NSEqualRects(g_camera_preview_view.frame,NSMakeRect(88,80,320,112)));
        assert(red_pixels(window)==320*112);
        g_camera_visible=0;mac_camera_preview_sync();assert(g_camera_preview_view.hidden && red_pixels(window)==0);
        g_camera_visible=1;mac_camera_preview_sync();assert(red_pixels(window)>0);
        g_camera_placed=0;mac_camera_preview_sync();assert(g_camera_preview_view.hidden && red_pixels(window)==0);
        [g_camera_capture.previewLayer removeFromSuperlayer];[g_camera_capture release];g_camera_capture=nil;
        [g_camera_preview_view removeFromSuperview];[g_camera_preview_view release];g_camera_preview_view=nil;
        window.contentView=nil;[g_view release];g_view=nil;[window close];[window release];
        puts("camera compositing: Metal parent, synthetic preview, UI font geometry, move/resize and hide/show passed (no device or visible window)");
    }
    return 0;
}
