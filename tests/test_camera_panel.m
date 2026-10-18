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
static int test_reflow_count,test_library_opens;
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
static int mac_record_exports_active(void){return 0;}
static NSMutableArray *test_jobs;
static NSArray *test_items;
static NSString *test_output;
static BOOL test_export_start;
static NSArray *mac_record_jobs(void){return test_jobs ?: @[];}
static NSArray *mac_record_items(void){test_library_opens++;return test_items ?: @[];}
static BOOL mac_record_export(NSString *path,NSDictionary *edits,BOOL zip){
    if(!test_export_start)return NO;
    [test_jobs addObject:@{@"id":@"new-export",@"bundle":path,@"state":@"running",@"output":@"new-output"}];return YES;
}
static void mac_record_file_viewer(NSString *path){}
static void record_library_open(NSString *path){}
static void record_library_open_output(NSString *path){[test_output release];test_output=[path copy];}
#define DRIP_MS 80
static unsigned GetTickCount(void){return 0;}
static int g_ndrip;
static uint16_t g_drip[40];
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
static int g_grid_on;
#import "../hardcore_camera_panel.m"

@interface HCTestWheelEvent:NSObject
@property CGFloat vertical;
@end
@implementation HCTestWheelEvent
- (NSEventType)type{return NSEventTypeScrollWheel;}
- (CGFloat)scrollingDeltaY{return self.vertical;}
- (CGFloat)scrollingDeltaX{return 10;}
@end

static int at(int col,int row) {
    return mac_camera_hit((g_camera_rect.x+col)*CW+CW/2,(g_camera_rect.y+row)*CH+CH/2);
}
static int pointer(NSEventType type,int col,int row) {
    NSEvent *event=[NSEvent mouseEventWithType:type location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:0];
    return mac_camera_mouse(event,(g_camera_rect.x+col)*CW+CW/2,(g_camera_rect.y+row)*CH+CH/2);
}
static void clips_clicks(void) {
    int row=g_camera_rect.h-3;
    assert(pointer(NSEventTypeLeftMouseDown,20,row));
    assert(test_library_opens==0); /* a held press must not order a new window */
    assert(pointer(NSEventTypeLeftMouseDragged,21,row));
    assert(test_library_opens==0);
    assert(pointer(NSEventTypeLeftMouseUp,21,row));
    assert(test_library_opens==1 && g_camera_clips && !g_camera_down && !g_camera_pending_action);
    assert(!g_camera_capture); /* opening Clips never asks for camera access */
    g_camera_clips=0;
    pointer(NSEventTypeLeftMouseUp,21,row);
    assert(test_library_opens==1); /* exactly one action per click */

    pointer(NSEventTypeLeftMouseDown,20,row);
    assert(pointer(NSEventTypeLeftMouseDragged,-1,row));
    assert(pointer(NSEventTypeLeftMouseUp,-1,row));
    assert(test_library_opens==1); /* releasing outside cancels */

    pointer(NSEventTypeLeftMouseDown,20,row);
    pointer(NSEventTypeLeftMouseDragged,-1,row);
    pointer(NSEventTypeLeftMouseDragged,20,row);
    pointer(NSEventTypeLeftMouseUp,20,row);
    assert(test_library_opens==2); /* returning to the button still works */
    g_camera_clips=0;

    pointer(NSEventTypeLeftMouseDown,20,row);
    mac_camera_cancel_press(); /* key window loss/minimization */
    assert(pointer(NSEventTypeLeftMouseUp,20,row));
    assert(test_library_opens==2 && !g_camera_down);

    pointer(NSEventTypeLeftMouseDown,20,row);
    NSEvent *escape=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0
        timestamp:0 windowNumber:0 context:nil characters:@"\033" charactersIgnoringModifiers:@"\033"
        isARepeat:NO keyCode:53];
    assert(mac_camera_mouse(escape,0,0));
    assert(pointer(NSEventTypeLeftMouseUp,20,row));
    assert(test_library_opens==2 && !g_camera_down);

    pointer(NSEventTypeLeftMouseDown,2,2);
    pointer(NSEventTypeLeftMouseUp,20,row);
    assert(test_library_opens==2); /* dragging in from elsewhere isn't a click */

    pointer(NSEventTypeLeftMouseDown,20,row);
    g_camera_placed=0;
    assert(pointer(NSEventTypeLeftMouseUp,20,row));
    assert(test_library_opens==2); /* no stale target after viewport shrinks */
    g_camera_placed=1;
    pointer(NSEventTypeLeftMouseDown,20,row);
    pointer(NSEventTypeLeftMouseUp,20,row);
    assert(test_library_opens==3); /* cancellation never leaves a stuck press */
    g_camera_clips=0;
    [g_camera_timer invalidate];g_camera_timer=nil;
    g_camera_requested=(HcTvRect){0};
}
static void clips_selection(void) {
    g_camera_visible=g_camera_placed=g_camera_clips=1;g_camera_rect=(HcTvRect){30,0,48,12};
    test_items=@[@{@"path":@"alpha"},@{@"path":@"beta"},@{@"path":@"gamma"}];
    mac_clips_action(MAC_CLIPS_RECORDINGS);mac_clips_action(MAC_CLIPS_ITEM+1);
    assert([mac_clips_selected()[@"path"] isEqual:@"beta"]);
    test_items=@[@{@"path":@"newer"},@{@"path":@"gamma"},@{@"path":@"alpha"},@{@"path":@"beta"}];
    mac_clips_refresh();assert(g_clips_selected==3 && [mac_clips_selected()[@"path"] isEqual:@"beta"]);
    mac_clips_action(MAC_CLIPS_PREVIOUS);assert([mac_clips_selected()[@"path"] isEqual:@"alpha"]);
    mac_clips_action(MAC_CLIPS_NEXT);assert([mac_clips_selected()[@"path"] isEqual:@"beta"]);
    /* Removing the selected clip cannot silently redirect a pending action. */
    test_items=@[@{@"path":@"newer"},@{@"path":@"gamma"},@{@"path":@"alpha"}];
    mac_clips_refresh();assert(g_clips_selected==-1 && !mac_clips_selected());
    mac_clips_action(MAC_CLIPS_ITEM+2);assert([mac_clips_selected()[@"path"] isEqual:@"alpha"]);

    test_jobs=[NSMutableArray array];
    for(int i=0;i<32;i++)[test_jobs addObject:@{@"id":[NSString stringWithFormat:@"job-%d",i],
        @"bundle":@"alpha",@"state":@"done",@"output":[NSString stringWithFormat:@"output-%d",i]}];
    mac_clips_action(MAC_CLIPS_EXPORTS);mac_clips_action(MAC_CLIPS_ITEM+20);
    [test_jobs removeObjectAtIndex:0];
    [test_jobs addObject:@{@"id":@"job-32",@"bundle":@"alpha",@"state":@"done",@"output":@"output-32"}];
    mac_clips_action(MAC_CLIPS_OUTPUT);
    assert(g_clips_selected==19 && [test_output isEqual:@"output-20"]);
    assert(g_clips_offset<=19 && 19<g_clips_offset+mac_clips_capacity());
    [test_jobs removeObjectAtIndex:19];mac_clips_action(MAC_CLIPS_OUTPUT);
    assert(g_clips_selected==-1 && !mac_clips_selected() && [test_output isEqual:@"output-20"]);
    mac_clips_action(MAC_CLIPS_NEXT);assert([mac_clips_selected()[@"id"] isEqual:@"job-1"]);
    mac_clips_action(MAC_CLIPS_ITEM+10);NSString *identity=[mac_clips_selected()[@"id"] copy];
    HCTestWheelEvent *wheel=[HCTestWheelEvent new];wheel.vertical=0;
    assert(mac_camera_mouse((NSEvent *)wheel,(g_camera_rect.x+2)*CW,(g_camera_rect.y+2)*CH));
    assert([mac_clips_selected()[@"id"] isEqual:identity]);
    wheel.vertical=-1;mac_camera_mouse((NSEvent *)wheel,(g_camera_rect.x+2)*CW,(g_camera_rect.y+2)*CH);
    assert(g_clips_selected==11);[wheel release];[identity release];
    test_export_start=YES;mac_clips_action(MAC_CLIPS_MP4);test_export_start=NO;
    assert([mac_clips_selected()[@"id"] isEqual:@"new-export"]);
    mac_camera_toggle();assert(!g_camera_visible);
    mac_camera_toggle();assert(g_camera_visible&&g_camera_clips&&[mac_clips_selected()[@"id"] isEqual:@"new-export"]);
    assert(!g_camera_capture);
    [g_camera_timer invalidate];g_camera_timer=nil;test_jobs=nil;test_items=nil;
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
        clips_clicks();
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
        clips_selection();
        puts("camera panel: placement, reflow, metadata, Clips click/release/cancellation, stable selection and scroll passed (no devices opened)");
    }
    return 0;
}
