static void mac_grid_transition(int entering) {
    static HcTvRect rects[HC_TV_COUNT+2];
    static HcTvPlacement placements[HC_TV_COUNT+2];
    static int saved;
    if(entering) {
        for(int i=0;i<HC_TV_COUNT;i++){rects[i]=g_tvs[i].state.rect;placements[i]=g_tvs[i].placement;}
        rects[HC_TV_COUNT]=g_music_rect;placements[HC_TV_COUNT]=g_music_placement;
        rects[HC_TV_COUNT+1]=g_camera_rect;placements[HC_TV_COUNT+1]=g_camera_placement;saved=1;
    } else if(saved) {
        for(int i=0;i<HC_TV_COUNT;i++){g_tvs[i].state.rect=rects[i];g_tvs[i].placement=placements[i];}
        g_music_rect=rects[HC_TV_COUNT];g_music_placement=placements[HC_TV_COUNT];
        g_camera_rect=rects[HC_TV_COUNT+1];g_camera_placement=placements[HC_TV_COUNT+1];saved=0;
    }
}
/* Grid utilities occupy a stable side rail as wide as the tab menu; opening them never resizes panes. */
static void mac_grid_browser_sync(void) {mac_browser_sync();}
static int g_grid_utility_choice,g_grid_utility_mask,g_grid_utility_row=-1,g_grid_utility_x;
static const wchar_t *g_grid_utility_names[]={L"Config",L"Music",L"Cam",L"CAF"};
static const int g_grid_utility_cols[]={0,8,16,22};
static int mac_grid_utility_click(int x,int y) {
    if(!g_grid_on || g_grid_utility_row<0 || y/CH!=g_grid_utility_row)return 0;
    int col=x/CW-g_grid_utility_x;
    for(int i=0;i<4;i++)if((g_grid_utility_mask&(1<<i)) && col>=g_grid_utility_cols[i] && col<g_grid_utility_cols[i]+6) {
        g_grid_utility_choice=i;plat_invalidate();return 1;
    }
    return 0;
}
static void mac_grid_utility_draw(void) {
    if(g_grid_utility_row<0)return;
    for(int i=0;i<4;i++)if(g_grid_utility_mask&(1<<i))
        draw_label(g_grid_utility_names[i],(g_grid_utility_x+g_grid_utility_cols[i])*CW,g_grid_utility_row*CH,6*CW,
            g_grid_utility_choice==i?AMBER:g_look.detail);
}
static void mac_grid_utilities(void) {
    int x=g_grid_left?0:g_cols-g_grid_rail,y=g_chrome.bottom+1,room=g_rows-y;
    int music=g_music_visible,camera=g_camera_visible,settings=g_tvs[HC_TV_SETTINGS].state.open;
    int caf=g_tvs[HC_TV_CAF].state.open;
    int minimum=(music?9:0)+(camera?9:0)+(settings?3:0)+(caf?2:0);
    int mask=settings|(music<<1)|(camera<<2)|(caf<<3),opened=mask&~g_grid_utility_mask;
    if(opened)for(int i=0;i<4;i++)if(opened&(1<<i))g_grid_utility_choice=i;
    g_grid_utility_mask=mask;g_grid_utility_row=-1;
    if(minimum>room && mask && room>3) {
        if(!(mask&(1<<g_grid_utility_choice)))for(int i=0;i<4;i++)if(mask&(1<<i)){g_grid_utility_choice=i;break;}
        g_grid_utility_x=x;g_grid_utility_row=y++;room--;
        settings=g_grid_utility_choice==0;music=g_grid_utility_choice==1;camera=g_grid_utility_choice==2;caf=g_grid_utility_choice==3;
        minimum=(music?9:0)+(camera?9:0)+(settings?3:0)+(caf?2:0);
    }
    int extra=chrome_max(0,room-minimum);
    for(int i=0;i<HC_TV_COUNT;i++)g_tvs[i].state.placed=0;
    g_music_placed=g_camera_placed=0;
    if(settings && room>=3) {
        int items=chrome_item_total(),height=chrome_min(items+2,3+chrome_min(extra,10));
        extra-=height-3;int capacity=height-2;
        if(!g_settings_tab) {
            items=font_menu_build(capacity,items);
            height=chrome_min(height,items+2);capacity=height-2;
        }
        g_setting_offset[g_settings_tab]=chrome_page_offset(g_setting_offset[g_settings_tab],items,capacity);
        ChromeTv *tv=g_tvs+HC_TV_SETTINGS;
        tv->state.rect=(HcTvRect){x,y,SB_COLS,height};tv->state.placed=1;
        tv->state.alpha=tv->motion.alpha=1;hc_tv_interact(&tv->state,GetTickCount());
        tv->layout=(ChromeLayout){.top=y,.bottom=y+height,.settings_row=-1,.section_row=y+1,
            .list_row=y+2,.list_capacity=capacity,.list_count=chrome_min(capacity,items-g_setting_offset[g_settings_tab])};
        y+=height;room-=height;
    }
    if(music && room>=9) {
        int height=9+chrome_min(extra,7);extra-=height-9;
        g_music_rect=(HcTvRect){x,y,g_grid_rail,height};g_music_placed=1;y+=height;room-=height;
        g_music_offset=chrome_min(g_music_offset,chrome_max(0,(int)mac_music_items().count-mac_music_capacity()));
    }
    if(camera && room>=9) {
        int height=9+chrome_min(extra,3);extra-=height-9;
        g_camera_rect=(HcTvRect){x,y,g_grid_rail,height};g_camera_placed=1;y+=height;room-=height;
    }
    if(caf && room>=2) {
        int height=chrome_min(room,6);ChromeTv *tv=g_tvs+HC_TV_CAF;
        tv->state.rect=(HcTvRect){x,y,SB_COLS,height};tv->state.placed=1;
        tv->state.alpha=tv->motion.alpha=1;hc_tv_interact(&tv->state,GetTickCount());
        tv->layout=(ChromeLayout){.top=y,.bottom=y+height,.settings_row=-1,.section_row=-1,.list_row=-1};
    }
    mac_camera_preview_sync();
}
static NSDictionary *mac_grid_pack(void) {
    NSMutableArray *hidden=[NSMutableArray array],*x=[NSMutableArray array],*y=[NSMutableArray array];
    for(int i=0;i<g_grid_nhidden;i++)[hidden addObject:@(g_grid_hidden[i])];
    for(int i=0;i<=g_grid.cols;i++)[x addObject:@(g_grid.xr[i])];
    for(int i=0;i<=g_grid.rows;i++)[y addObject:@(g_grid.yr[i])];
    return @{@"enabled":@(g_grid_on),@"expanded":@(g_grid_expand_id),@"hidden":hidden,@"x":x,@"y":y};
}
static void mac_grid_restore(NSDictionary *state) {
    if(![state isKindOfClass:[NSDictionary class]])return;
    g_grid_on=[state[@"enabled"] boolValue];g_grid_expand_id=[state[@"expanded"] intValue];
    g_grid_nhidden=0;
    if([state[@"hidden"] isKindOfClass:[NSArray class]])for(NSNumber *id in state[@"hidden"])
        if([id isKindOfClass:[NSNumber class]] && tab_by_id(id.intValue))grid_set_hidden(id.intValue,1);
    grid_measure(g_cols,g_rows);
    NSArray *x=state[@"x"],*y=state[@"y"];
    if([x isKindOfClass:[NSArray class]] && x.count==(NSUInteger)g_grid.cols+1)
        for(int i=0;i<=g_grid.cols;i++)g_grid.xr[i]=chrome_min(HC_GRID_RATIO,chrome_max(0,[x[i] intValue]));
    if([y isKindOfClass:[NSArray class]] && y.count==(NSUInteger)g_grid.rows+1)
        for(int i=0;i<=g_grid.rows;i++)g_grid.yr[i]=chrome_min(HC_GRID_RATIO,chrome_max(0,[y[i] intValue]));
    grid_refit();
}
