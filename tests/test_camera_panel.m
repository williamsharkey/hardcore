/* Headless UI geometry contract. No app, window, devices, permissions or timer
 * is started; native capture controls are linked but never opened. */
#import "../hardcore_camera_capture.m"
#include "../hardcore_tv.h"
#include <assert.h>
#include <wchar.h>
#include <stdio.h>

#define CW 8
#define CH 16
#define SB_COLS 28
#define UI_FONT do {} while(0)
#define UI_DONE do {} while(0)
#define DEF_BG 0x101010
#define BAR_BG 0x151515
#define TAB_HOVER 0x202020
#define AMBER 0xffcc44
#define _snwprintf swprintf

typedef struct { int cols,rows; } Term;
typedef struct { Term t; } Tab;
typedef struct { int top,bottom; } ChromeLayout;
typedef struct { HcTvState state; } ChromeTv;
typedef struct { Term *term; HcTvRect primary; } ChromeTvCostContext;
static ChromeLayout g_chrome={0,8};
static ChromeTv g_tvs[HC_TV_COUNT];
static Tab test_tab={{120,36}};
static HCCameraCapture *g_camera_capture;
static NSString *g_record_status;
static NSView *g_view;
static int g_chrome_x,g_chrome_surface_cols,g_chrome_capture,g_mouse_x=-1,g_mouse_y=-1,g_sel_drag,g_sb_drag;
static float g_draw_alpha;
static struct { uint32_t dim,text,red,detail,text_lo; } g_look;
static int test_reflow_count;
static HcTvRect test_reflow_rect;
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
static void mac_record_library(void){}
static void chrome_label(const wchar_t *text,int a,int b,int y,uint32_t color){}
static void fill_rect(float a,float b,float c,float d,uint32_t color,float alpha){}
static void utf8_to_wide(const char *s,wchar_t *out,int capacity){if(capacity)out[0]=0;}
static int chrome_tv_cost(HcTvRect r,HcTvCost *cost,void *opaque) {
    ChromeTvCostContext *context=opaque;
    if(hc_tv_rect_overlap(r,context->primary))return 0;
    for(int i=0;i<HC_TV_COUNT;i++)if(g_tvs[i].state.placed && hc_tv_rect_overlap(r,g_tvs[i].state.rect))return 0;
    return 1;
}
static void chrome_tv_reflow_apply(Term *t,HcTvRect rect,const HcTvRect *prior,int count) {
    assert(count<=HC_TV_COUNT+1);
    for(int i=0;i<count;i++)assert(!hc_tv_rect_overlap(rect,prior[i]));
    test_reflow_count++;test_reflow_rect=rect;
}
#import "../hardcore_camera_panel.m"

static int at(int col,int row) {
    return mac_camera_hit((g_camera_rect.x+col)*CW+CW/2,(g_camera_rect.y+row)*CH+CH/2);
}
int main(void) {
    @autoreleasepool {
        assert(!mac_camera_hit(100,100));
        NSDictionary *closed=mac_camera_metadata();
        assert(![closed[@"visible"] boolValue]);
        assert([closed[@"rect"][2] intValue]>0 && [closed[@"rect"][3] intValue]>0);
        g_camera_visible=1;
        mac_camera_layout(&test_tab.t);
        assert(g_camera_placed && test_reflow_count==1);
        assert(hc_tv_rect_fits(g_camera_rect,120,36));
        assert(!hc_tv_rect_overlap(g_camera_rect,mac_camera_primary()));
        assert(hc_tv_rect_equal(g_camera_rect,test_reflow_rect));
        HcTvRect initial=g_camera_rect;
        assert(at(1,0)==MAC_CAMERA_MOVE);
        assert(at(initial.w-2,0)==MAC_CAMERA_MINIMIZE);
        assert(at(2,2)==MAC_CAMERA_BODY);
        assert(at(2,initial.h-4)==MAC_CAMERA_RECORD);
        assert(at(10,initial.h-4)==MAC_CAMERA_PAUSE);
        assert(at(18,initial.h-4)==MAC_CAMERA_SAVE);
        assert(at(24,initial.h-4)==MAC_CAMERA_DISCARD);
        assert(at(2,initial.h-3)==MAC_CAMERA_CAPTIONS);
        assert(at(20,initial.h-3)==MAC_CAMERA_LIBRARY);
        assert(at(initial.w-2,initial.h-1)==MAC_CAMERA_RESIZE);
        assert(at(initial.w,2)==MAC_CAMERA_NONE);
        NSDictionary *metadata=mac_camera_metadata();
        NSArray *rect=metadata[@"rect"];
        assert([metadata[@"visible"] boolValue] && [metadata[@"mirrored"] boolValue]);
        assert([rect isKindOfClass:[NSArray class]] && rect.count==4);
        assert([rect[0] intValue]==(initial.x+1)*CW && [rect[1] intValue]==(initial.y+1)*CH);
        assert([rect[2] intValue]==(initial.w-2)*CW && [rect[3] intValue]==(initial.h-5)*CH);
        /* A newly opened ordinary TV displaces camera without overlapping it. */
        g_tvs[0].state=(HcTvState){.open=1,.placed=1,.rect=initial};
        mac_camera_layout(&test_tab.t);
        assert(g_camera_placed && !hc_tv_rect_overlap(g_camera_rect,initial));
        /* A manually requested legal rectangle persists and joins reflow. */
        g_camera_manual=1;g_camera_requested=(HcTvRect){40,20,36,12};
        mac_camera_layout(&test_tab.t);
        assert(g_camera_placed && hc_tv_rect_equal(g_camera_rect,g_camera_requested));
        /* Narrow viewports never expose stale hit targets or visible media. */
        test_tab.t.cols=24;mac_camera_layout(&test_tab.t);
        assert(!g_camera_placed && ![mac_camera_metadata()[@"visible"] boolValue]);
        assert(at(2,2)==MAC_CAMERA_NONE);
        test_tab.t.cols=120;g_camera_visible=0;mac_camera_layout(&test_tab.t);
        assert(!g_camera_placed && !mac_camera_hit(10,10));
        puts("camera panel: collision-free placement, manual geometry, reflow, control hit targets and replay metadata passed (no devices opened)");
    }
    return 0;
}
