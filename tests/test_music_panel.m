/* clang -fno-objc-arc -Wall -Wextra -Wno-unused-function -Wno-unused-parameter
 * tests/test_music_panel.m -framework Cocoa -o /tmp/hc-music-panel
 * Pure production panel geometry/events with an in-memory catalog. No window,
 * playback, network, device access, or user input is opened. */
#import <Cocoa/Cocoa.h>
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <wchar.h>
#include "../hardcore_tv.h"
#define CW 12
#define CH 26
#define UI_FONT
#define UI_DONE
#define TAB_HOVER 1
#define BAR_BG 2
#define AMBER 3
static struct {uint32_t text,detail,dim,text_lo,red;} g_look={4,5,6,7,8};
typedef struct {int cols,rows;} Term;
typedef struct {Term t;} Tab;
typedef struct {Term *term;HcTvRect primary;} ChromeTvCostContext;
static struct {HcTvState state;} g_tvs[HC_TV_COUNT];
static Tab tab={{120,50}};
static Tab *active_tab(void){return &tab;}
static int g_mouse_x=-1,g_mouse_y=-1,g_sel_drag,g_sb_drag;
static int g_chrome_x,g_chrome_surface_cols=28,g_chrome_capture;
static float g_draw_alpha=1;
static int g_camera_visible,g_camera_placed;
static HcTvRect g_camera_rect;
static NSArray *catalog;
static int selected=-1,playing,loads,pauses,stops,selected_count,opened,invalidated,status_reads;
static double position,duration=100;
static NSString *play_error=@"",*catalog_error=@"";
static int chrome_min(int a,int b){return a<b?a:b;}
static int chrome_max(int a,int b){return a>b?a:b;}
static void plat_invalidate(void){invalidated++;}
static void tab_layout(void){}
static NSArray *mac_music_items(void){return catalog;}
static NSDictionary *mac_music_status(void){status_reads++;return @{@"index":@(selected),@"playing":@(playing),@"position":@(position),@"duration":@(duration),@"progress":@(position/duration),@"error":play_error,@"catalogError":catalog_error};}
static void mac_music_load(void){loads++;}
static void mac_music_next(int delta){selected+=delta;}
static void mac_music_play_pause(void){playing=!playing;pauses++;}
static void mac_music_stop(void){playing=0;position=0;stops++;}
static void mac_music_select(int index,int play){selected=index;playing=play;selected_count++;}
static void mac_music_seek(double seconds){position=seconds;}
static NSDictionary *mac_music_pack(void){return mac_music_status();}
static void mac_music_restore(id value){(void)value;}
static void mac_browser_new(NSString *address,int parent,int native){assert([address isEqual:@"https://musicforprogramming.net/"]);opened++;}
static void utf8_to_wide(const char *s,wchar_t *out,int cap){
    NSData *data=[[NSString stringWithUTF8String:s] dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    int count=chrome_min(cap-1,(int)data.length/4);memcpy(out,data.bytes,count*4);out[count]=0;
}
static HcTvRect mac_camera_primary(void){return (HcTvRect){92,0,28,12};}
static int mac_camera_rect_available(HcTvRect r,Term *t){
    if(!hc_tv_rect_fits(r,t->cols,t->rows)||hc_tv_rect_overlap(r,mac_camera_primary()))return 0;
    for(int i=0;i<HC_TV_COUNT;i++)if(g_tvs[i].state.open&&g_tvs[i].state.placed&&hc_tv_rect_overlap(r,g_tvs[i].state.rect))return 0;
    return 1;
}
static int chrome_tv_cost(HcTvRect r,HcTvCost *cost,void *ctx){
    if(!mac_camera_rect_available(r,((ChromeTvCostContext *)ctx)->term))return 0;
    *cost=(HcTvCost){0};return 1;
}
static void chrome_tv_reflow_apply(Term *t,HcTvRect r,HcTvRect *prior,int count){assert(hc_tv_rect_fits(r,t->cols,t->rows));assert(count<=HC_TV_MAX_SLOTS);}
static int paint_calls;
static void fill_rect(float x,float y,float right,float bottom,uint32_t color,float alpha){
    assert(x>=0 && right>=x && right<=HC_TV_MAX_WIDTH*CW);assert(y>=0 && bottom>=y && bottom<=tab.t.rows*CH);paint_calls++;
}
static void chrome_label(const wchar_t *s,int col,int end,int row,uint32_t color){assert(col>=0&&end>=col&&end<=HC_TV_MAX_WIDTH);assert(row>=0&&row<tab.t.rows);paint_calls++;}
static void grid_text(const wchar_t *s,int col,int row,uint32_t color){chrome_label(s,col,col+(int)wcslen(s),row,color);}
static void mac_cutie_blur(void){}
static void chrome_preview_cancel(void){}
static NSCursor *mac_tv_resize_cursor(void){return NSCursor.arrowCursor;}
#include "../hardcore_music_panel.m"

static NSEvent *event(NSEventType type){
    return [NSEvent mouseEventWithType:type location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil eventNumber:0 clickCount:1 pressure:1];
}
static int px(int col){return (g_music_rect.x+col)*CW+CW/2;}
static int py(int row){return (g_music_rect.y+row)*CH+CH/2;}
static void click(int col,int row){assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),px(col),py(row)));assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),px(col),py(row)));}
int main(void){@autoreleasepool {
    NSMutableArray *items=[NSMutableArray array];
    for(int i=0;i<20;i++)[items addObject:@{@"id":[NSString stringWithFormat:@"mix-%d",20-i],@"episode":@(20-i),@"title":[NSString stringWithFormat:@"%d · Test composer",20-i]}];catalog=[items copy];
    assert(!mac_music_menu_visible());assert(!mac_music_playing()&&!mac_music_loading()&&!status_reads);
    mac_music_toggle();assert(g_music_visible&&mac_music_menu_visible()&&loads==0);
    mac_music_layout(&tab.t);assert(g_music_placed&&g_music_rect.w==48&&g_music_rect.h==16);
    assert(!hc_tv_rect_overlap(g_music_rect,mac_camera_primary()));
    for(int col=0;col<g_music_rect.w;col++)assert(mac_music_hit(px(col),py(0))==(col==g_music_rect.w-2?MUSIC_MINIMIZE:MUSIC_MOVE));
    assert(mac_music_hit(px(-1),py(0))==MUSIC_NONE);assert(mac_music_hit(px(1),py(g_music_rect.h))==MUSIC_NONE);
    assert(mac_music_hit(px(g_music_rect.w-2),py(g_music_rect.h-1))==MUSIC_RESIZE);
    click(2,6);assert(selected==0&&playing&&selected_count==1);
    click(10,4);assert(!playing&&pauses==1);click(10,4);assert(playing&&pauses==2);
    click(19,4);assert(selected==1);click(3,4);assert(selected==0);
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),px(10),py(4)));
    assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),px(10),py(2)));assert(pauses==2);
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),px(10),py(4)));mac_music_cancel_press();
    assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),px(10),py(4)));assert(pauses==2);
    assert(!g_music_down&&!g_music_pending&&!g_music_pending_id);
    /* A refreshed feed can replace an index while the mouse is held down.
       Only the original stable episode identity may complete the click. */
    int selections=selected_count;
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),px(2),py(6)));
    [items exchangeObjectAtIndex:0 withObjectAtIndex:1];[catalog release];catalog=[items copy];
    assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),px(2),py(6)));assert(selected_count==selections&&!g_music_pending_id);
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),px(2),py(6)));
    [catalog release];catalog=[items copy]; /* Same identity in a new catalog is safe. */
    assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),px(2),py(6)));assert(selected_count==selections+1);
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),px(2),py(6)));mac_music_cancel_press();
    assert(g_music_down&&!g_music_pending&&!g_music_pending_id);
    assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),-1,-1));assert(!g_music_down&&selected_count==selections+1);
    assert(!mac_music_mouse(event(NSEventTypeLeftMouseUp),-1,-1)); /* Exactly one release is consumed. */
    [items exchangeObjectAtIndex:0 withObjectAtIndex:1];[catalog release];catalog=[items copy];
    click(2,3);assert(position>0&&position<10);mac_music_action(MUSIC_SEEK,-100);assert(position==0);mac_music_action(MUSIC_SEEK,100000);assert(position==duration);
    click(27,4);assert(!playing&&stops==1&&position==0);
    mac_music_scroll(1000);assert(g_music_offset==12);click(2,6);assert(selected==12);
    click(2,g_music_rect.h-1);assert(g_music_offset==4);click(7,g_music_rect.h-1);assert(g_music_offset==12);
    click(g_music_rect.w-4,5);assert(opened==1);
    click(g_music_rect.w-12,5);assert(loads==1);
    wchar_t compact[12];catalog_error=@"Offline";mac_music_compact_label(compact,12);assert(!wcscmp(compact,L"Retry"));catalog_error=@"";
    paint_calls=0;mac_music_draw();assert(paint_calls>20);
    int last=selected;click(g_music_rect.w-2,0);assert(!g_music_visible&&mac_music_menu_visible()&&selected==last);
    assert(mac_music_hit(px(1),py(0))==MUSIC_NONE);mac_music_toggle();mac_music_layout(&tab.t);assert(g_music_visible&&g_music_placed&&loads==1);
    /* The title's ordinary text drags; the corner resizes independently. */
    HcTvRect before=g_music_rect;int x=px(12),y=py(0);
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),x,y));
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDragged),x-2*CW,y+CH));
    assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),x-2*CW,y+CH));
    assert(g_music_manual&&g_music_rect.x==before.x-2&&g_music_rect.y==before.y+1);
    before=g_music_rect;x=px(g_music_rect.w-2);y=py(g_music_rect.h-1);
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDown),x,y));
    assert(mac_music_mouse(event(NSEventTypeLeftMouseDragged),x+CW,y+CH));
    assert(mac_music_mouse(event(NSEventTypeLeftMouseUp),x+CW,y+CH));
    assert(g_music_rect.w==before.w+1&&g_music_rect.h==before.h+1);
    /* Placement never invades the primary menu, camera, or other TVs. */
    g_camera_visible=g_camera_placed=1;g_camera_rect=g_music_rect;mac_music_layout(&tab.t);assert(!g_music_placed||!hc_tv_rect_overlap(g_music_rect,g_camera_rect));
    g_camera_visible=0;g_music_requested=(HcTvRect){0,0,10000,10000};mac_music_layout(&tab.t);assert(!g_music_placed||(g_music_rect.w<=HC_TV_MAX_WIDTH&&g_music_rect.h<=HC_TV_MAX_HEIGHT));
    tab.t=(Term){31,50};mac_music_layout(&tab.t);assert(!g_music_placed);tab.t=(Term){120,11};mac_music_layout(&tab.t);assert(!g_music_placed);
    /* An exec reload during the first catalog request resumes loading. */
    [catalog release];catalog=[@[] retain];int load_before=loads;
    mac_music_view_restore(@{@"enabled":@YES,@"visible":@YES,@"player":@{}});
    assert(g_music_enabled&&g_music_visible&&loads==load_before+1);
    [catalog release];puts("ok music panel: transport, capture, paging, minimize, bounds, and obstacle placement");
}return 0;}
