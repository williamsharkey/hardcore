/* Music for Programming, rendered with the same font, theme and bounded TV
 * placement as the terminal chrome. Playback belongs to the app, not the TV. */
static int g_music_enabled,g_music_visible,g_music_placed,g_music_manual;
static int g_music_offset,g_music_down,g_music_pending,g_music_drag;
static int g_music_drag_x,g_music_drag_y;
static NSString *g_music_pending_id;
static HcTvRect g_music_rect,g_music_requested,g_music_drag_origin;
static HcTvPlacement g_music_placement;
enum { MUSIC_NONE,MUSIC_BODY,MUSIC_MOVE,MUSIC_MINIMIZE,MUSIC_RESIZE,
    MUSIC_PREVIOUS,MUSIC_PLAY,MUSIC_NEXT,MUSIC_STOP,MUSIC_REFRESH,MUSIC_SITE,
    MUSIC_SEEK,MUSIC_PAGE_UP,MUSIC_PAGE_DOWN,MUSIC_ITEM=1000 };

static int mac_music_menu_visible(void) {return g_music_enabled;}
static int mac_music_playing(void) {return g_music_enabled && [mac_music_status()[@"playing"] boolValue];}
static int mac_music_loading(void) {
    if(!g_music_enabled)return 0;
    NSDictionary *s=mac_music_status();return [s[@"loading"] boolValue] || [s[@"buffering"] boolValue];
}
static void mac_music_compact_label(wchar_t *out,int cap) {
    NSDictionary *s=mac_music_status();int index=[s[@"index"] intValue];NSArray *items=mac_music_items();
    NSString *episode=index>=0 && index<(int)items.count?[items[index][@"episode"] description]:@"";
    unsigned seconds=(unsigned)fmax(0,[s[@"position"] doubleValue]);
    NSString *text=[s[@"error"] length] || [s[@"catalogError"] length]?@"Retry":episode.length?[NSString stringWithFormat:@"%@ %u:%02u",episode,seconds/60,seconds%60]:[s[@"loading"] boolValue]?@"Loading":@"Music";
    utf8_to_wide(text.UTF8String,out,cap);
}
static int mac_music_capacity(void) {return chrome_max(1,g_music_rect.h-8);}
static void mac_music_scroll(int delta) {
    int count=(int)mac_music_items().count;
    g_music_offset=chrome_min(chrome_max(0,g_music_offset+delta),chrome_max(0,count-mac_music_capacity()));
    plat_invalidate();
}
static void mac_music_cancel_press(void) {
    g_music_pending=g_music_drag=0;
    [g_music_pending_id release];g_music_pending_id=nil;
    /* Keep ownership of the outstanding mouse-up after focus loss/Escape.
       It must not become a click or selection in the terminal underneath. */
}
static void mac_music_toggle(void) {
    g_music_enabled=1;g_music_visible=!g_music_visible;
    mac_music_cancel_press();
    if(g_music_visible) {
        if(!mac_music_items().count && ![mac_music_status()[@"loading"] boolValue])mac_music_load();
        if(!g_music_requested.w)g_music_requested=(HcTvRect){0,0,48,16};
        int index=[mac_music_status()[@"index"] intValue];if(index>=0)g_music_offset=index;
    }
    tab_layout();plat_invalidate();
}
static int mac_music_rect_available(HcTvRect r,Term *t) {
    return mac_camera_rect_available(r,t) && (!g_camera_visible || !g_camera_placed || !hc_tv_rect_overlap(r,g_camera_rect));
}
static int mac_music_cost(HcTvRect r,HcTvCost *cost,void *context) {
    if(g_camera_visible && g_camera_placed && hc_tv_rect_overlap(r,g_camera_rect))return 0;
    return chrome_tv_cost(r,cost,context);
}
static void mac_music_layout(Term *term) {
    g_music_placed=0;
    if(!g_music_visible || !term || term->cols<32 || term->rows<12)return;
    UI_FONT;
    HcTvRect r=hc_tv_requested_rect(g_music_requested,term->cols,term->rows);
    r.w=chrome_max(32,r.w);r.h=chrome_max(12,r.h);
    if(g_music_manual && mac_music_rect_available(r,term)) {
        g_music_rect=r;g_music_placement=(HcTvPlacement){.rect=r,.valid=1};g_music_placed=1;
    } else if(g_music_placement.valid && g_music_rect.w==r.w && g_music_rect.h==r.h && mac_music_rect_available(g_music_rect,term))g_music_placed=1;
    else {
        ChromeTvCostContext context={.term=term,.primary=mac_camera_primary()};
        HcTvPlacement placement=hc_tv_place(term->cols,term->rows,context.primary,r.w,r.h,
            g_music_placement.valid?&g_music_placement:NULL,mac_music_cost,&context);
        if(placement.valid){g_music_rect=placement.rect;g_music_placement=placement;g_music_placed=1;}
    }
    if(g_music_placed) {
        HcTvRect prior[HC_TV_COUNT+2];int count=0;prior[count++]=mac_camera_primary();
        for(int i=0;i<HC_TV_COUNT;i++)if(g_tvs[i].state.open && g_tvs[i].state.placed)prior[count++]=g_tvs[i].state.rect;
        if(g_camera_visible && g_camera_placed)prior[count++]=g_camera_rect;
        chrome_tv_reflow_apply(term,g_music_rect,prior,count);
        int limit=chrome_max(0,(int)mac_music_items().count-mac_music_capacity());
        g_music_offset=chrome_min(chrome_max(0,g_music_offset),limit);
    }
    UI_DONE;
}
static int mac_music_hit(int x,int y) {
    if(!g_music_visible || !g_music_placed || x<0 || y<0)return MUSIC_NONE;
    UI_FONT;int col=x/CW-g_music_rect.x,row=y/CH-g_music_rect.y;UI_DONE;
    HcTvRect r=g_music_rect;
    if(col<0 || col>=r.w || row<0 || row>=r.h)return MUSIC_NONE;
    if(!row)return col==r.w-2?MUSIC_MINIMIZE:MUSIC_MOVE;
    if(row==r.h-1) {
        if(col>=r.w-3)return MUSIC_RESIZE;
        if(col>=1 && col<5)return MUSIC_PAGE_UP;
        if(col>=6 && col<10)return MUSIC_PAGE_DOWN;
    }
    if(row==3 && col>=1 && col<r.w-1)return MUSIC_SEEK;
    if(row==4) {
        if(col>=1 && col<8)return MUSIC_PREVIOUS;
        if(col>=9 && col<16)return MUSIC_PLAY;
        if(col>=17 && col<24)return MUSIC_NEXT;
        if(col>=25 && col<30)return MUSIC_STOP;
    }
    if(row==5) {
        if(col>=r.w-15 && col<r.w-7)return MUSIC_REFRESH;
        if(col>=r.w-6 && col<r.w-1)return MUSIC_SITE;
    }
    if(row>=6 && row<r.h-2 && col>0 && col<r.w-1) {
        int index=g_music_offset+row-6;if(index<(int)mac_music_items().count)return MUSIC_ITEM+index;
    }
    return MUSIC_BODY;
}
static void mac_music_action(int hit,int x) {
    if(hit==MUSIC_MINIMIZE)g_music_visible=0;
    else if(hit==MUSIC_PREVIOUS)mac_music_next(-1);
    else if(hit==MUSIC_PLAY)mac_music_play_pause();
    else if(hit==MUSIC_NEXT)mac_music_next(1);
    else if(hit==MUSIC_STOP)mac_music_stop();
    else if(hit==MUSIC_REFRESH)mac_music_load();
    else if(hit==MUSIC_SITE)mac_browser_new(@"https://musicforprogramming.net/",0,0);
    else if(hit==MUSIC_PAGE_UP)mac_music_scroll(-mac_music_capacity());
    else if(hit==MUSIC_PAGE_DOWN)mac_music_scroll(mac_music_capacity());
    else if(hit>=MUSIC_ITEM)mac_music_select(hit-MUSIC_ITEM,1);
    else if(hit==MUSIC_SEEK) {
        UI_FONT;double fraction=(double)(x-(g_music_rect.x+1)*CW)/((g_music_rect.w-2)*CW);UI_DONE;
        mac_music_seek(fmax(0,fmin(1,fraction))*[mac_music_status()[@"duration"] doubleValue]);
    }
    plat_invalidate();
}
static void mac_music_text(NSString *text,int col,int end,int row,uint32_t color) {
    wchar_t s[512];utf8_to_wide(text.UTF8String ?: "",s,512);chrome_label(s,col,end,row,color);
}
static void mac_music_button(const wchar_t *text,int col,int width,int row,int action) {
    if(action==MUSIC_PREVIOUS || action==MUSIC_PLAY || action==MUSIC_NEXT || action==MUSIC_STOP)chrome_button_outline(col,width,row);
    if(mac_music_hit(g_mouse_x,g_mouse_y)==action)fill_rect(col*CW,row*CH,(col+width)*CW,(row+1)*CH,TAB_HOVER,1);
    chrome_label(text,col,col+width,row,g_look.text);
}
static void mac_music_draw(void) {
    if(!g_music_visible || !g_music_placed)return;
    UI_FONT;
    int origin=g_chrome_x,surface=g_chrome_surface_cols,capture=g_chrome_capture;
    float alpha=g_draw_alpha;HcTvRect r=g_music_rect;int y=r.y;
    g_chrome_x=r.x*CW;g_chrome_surface_cols=r.w;g_draw_alpha=1;g_chrome_capture=0;
    fill_rect(0,y*CH,r.w*CW,(y+r.h)*CH,BAR_BG,1);
    fill_rect(0,y*CH,r.w*CW,y*CH+1,g_look.dim,1);
    fill_rect(0,(y+r.h)*CH-1,r.w*CW,(y+r.h)*CH,g_look.dim,1);
    fill_rect(0,y*CH,1,(y+r.h)*CH,g_look.dim,1);fill_rect(r.w*CW-1,y*CH,r.w*CW,(y+r.h)*CH,g_look.dim,1);
    chrome_label(g_grid_on?L"Music for Programming":L"↕ Music for Programming",1,r.w-3,y,g_look.text);
    mac_music_button(L"−",r.w-2,1,y,MUSIC_MINIMIZE);
    NSDictionary *s=mac_music_status();NSArray *items=mac_music_items();
    mac_music_text([s[@"title"] length]?s[@"title"]:@"Choose a mix below",1,r.w-1,y+1,AMBER);
    NSString *state=[s[@"buffering"] boolValue]?@"Buffering":[s[@"playing"] boolValue]?@"Playing":@"Paused";
    unsigned pos=(unsigned)fmax(0,[s[@"position"] doubleValue]),duration=(unsigned)fmax(0,[s[@"duration"] doubleValue]);
    mac_music_text([NSString stringWithFormat:@"%@ · %u:%02u / %u:%02u",state,pos/60,pos%60,duration/60,duration%60],1,r.w-1,y+2,g_look.detail);
    wchar_t bar[128];int n=chrome_min(r.w-4,120),fill=(int)(fmax(0,fmin(1,[s[@"progress"] doubleValue]))*n);
    bar[0]='[';for(int i=0;i<n;i++)bar[i+1]=i<fill?'=':' ';bar[n+1]=']';bar[n+2]=0;chrome_label(bar,1,r.w-1,y+3,g_look.detail);
    mac_music_button(L"Prev",1,7,y+4,MUSIC_PREVIOUS);
    mac_music_button([s[@"playing"] boolValue]?L"Ⅱ Pause":L"▶ Play",9,7,y+4,MUSIC_PLAY);
    mac_music_button(L"Next",17,7,y+4,MUSIC_NEXT);mac_music_button(L"Stop",25,5,y+4,MUSIC_STOP);
    chrome_label(L"Episodes",1,r.w-16,y+5,g_look.dim);
    mac_music_button(L"Refresh",r.w-15,8,y+5,MUSIC_REFRESH);mac_music_button(L"Site",r.w-6,5,y+5,MUSIC_SITE);
    int current=[s[@"index"] intValue];
    for(int i=0;i<mac_music_capacity() && g_music_offset+i<(int)items.count;i++) {
        int at=g_music_offset+i;NSDictionary *item=items[at];int row=y+6+i;
        if(current==at || mac_music_hit(g_mouse_x,g_mouse_y)==MUSIC_ITEM+at)fill_rect(CW,row*CH,(r.w-1)*CW,(row+1)*CH,TAB_HOVER,1);
        NSString *text=[NSString stringWithFormat:@"%@ %@",current==at?@">":@" ",item[@"title"] ?: @""];
        mac_music_text(text,1,r.w-1,row,current==at?AMBER:g_look.text_lo);
    }
    NSString *message=[s[@"error"] length]?s[@"error"]:[s[@"catalogError"] length]?s[@"catalogError"]:[s[@"loading"] boolValue]?@"Loading episode list…":@"musicforprogramming.net · click a mix to play";
    mac_music_text(message,1,r.w-1,y+r.h-2,[s[@"error"] length]?g_look.red:g_look.dim);
    mac_music_button(L"↑",1,4,y+r.h-1,MUSIC_PAGE_UP);mac_music_button(L"↓",6,4,y+r.h-1,MUSIC_PAGE_DOWN);
    mac_music_text([NSString stringWithFormat:@"%d–%d / %lu",items.count?g_music_offset+1:0,chrome_min(g_music_offset+mac_music_capacity(),(int)items.count),(unsigned long)items.count],11,r.w-3,y+r.h-1,g_look.detail);
    int size=chrome_min(CW-3,CH-5),bottom=(y+r.h)*CH;
    for(int len=size;len>=2;len-=3)for(int k=0;k<len;k++){int x=r.w*CW-3-k,py=bottom-3-(len-1-k);fill_rect(x,py,x+1,py+1,g_look.detail,1);}
    g_chrome_x=origin;g_chrome_surface_cols=surface;g_chrome_capture=capture;g_draw_alpha=alpha;UI_DONE;
}
static void mac_music_drag_move(int x,int y) {
    Tab *tb=active_tab();if(!tb)return;
    UI_FONT;HcTvRect r=g_music_drag_origin;
    int dx=(int)lround((double)(x-g_music_drag_x)/CW),dy=(int)lround((double)(y-g_music_drag_y)/CH);
    if(g_music_drag==MUSIC_RESIZE){r.w=chrome_min(chrome_max(32,r.w+dx),chrome_min(HC_TV_MAX_WIDTH,tb->t.cols-r.x));r.h=chrome_min(chrome_max(12,r.h+dy),chrome_min(HC_TV_MAX_HEIGHT,tb->t.rows-r.y));}
    else {r.x=chrome_min(chrome_max(0,r.x+dx),chrome_max(0,tb->t.cols-r.w));r.y=chrome_min(chrome_max(0,r.y+dy),chrome_max(0,tb->t.rows-r.h));}
    if(mac_music_rect_available(r,&tb->t)){g_music_manual=1;g_music_requested=g_music_rect=r;g_music_placement=(HcTvPlacement){.rect=r,.valid=1};plat_invalidate();}
    UI_DONE;
}
static int mac_music_mouse(NSEvent *event,int x,int y) {
    NSEventType type=event.type;
    if(type==NSEventTypeKeyDown && g_music_down && event.keyCode==53){mac_music_cancel_press();return 1;}
    if(type==NSEventTypeLeftMouseDragged && g_music_down){if(g_music_drag)mac_music_drag_move(x,y);return 1;}
    if(type==NSEventTypeLeftMouseUp && g_music_down) {
        int action=g_music_pending,activate=action && mac_music_hit(x,y)==action;
        if(activate && action>=MUSIC_ITEM) {
            NSArray *items=mac_music_items();int index=action-MUSIC_ITEM;
            activate=index<(int)items.count && g_music_pending_id &&
                [g_music_pending_id isEqual:items[index][@"id"]];
        }
        mac_music_cancel_press();g_music_down=0;
        if(activate)mac_music_action(action,x);return 1;
    }
    int hit=mac_music_hit(x,y);if(!hit)return 0;
    if(type==NSEventTypeScrollWheel){if(event.scrollingDeltaY)mac_music_scroll(event.scrollingDeltaY>0?-1:1);return 1;}
    if(type==NSEventTypeMouseMoved){g_mouse_x=x;g_mouse_y=y;plat_invalidate();return 0;}
    if(type!=NSEventTypeLeftMouseDown)return 0;
    mac_cutie_blur();chrome_preview_cancel();mac_music_cancel_press();g_music_down=1;g_sel_drag=g_sb_drag=0;
    if(g_grid_on && (hit==MUSIC_MOVE || hit==MUSIC_RESIZE))return 1;
    if(hit==MUSIC_MOVE || hit==MUSIC_RESIZE){g_music_drag=hit;g_music_drag_origin=g_music_rect;g_music_drag_x=x;g_music_drag_y=y;}
    else if(hit!=MUSIC_BODY) {
        g_music_pending=hit;
        if(hit>=MUSIC_ITEM) {
            NSArray *items=mac_music_items();int index=hit-MUSIC_ITEM;
            if(index<(int)items.count && [items[index][@"id"] isKindOfClass:NSString.class])
                g_music_pending_id=[items[index][@"id"] copy];
            if(!g_music_pending_id.length)g_music_pending=0;
        }
    }
    plat_invalidate();return 1;
}
static int mac_music_cursor(int x,int y) {
    int hit=mac_music_hit(x,y);if(!hit && !g_music_drag)return 0;
    [(g_music_drag==MUSIC_RESIZE || hit==MUSIC_RESIZE)?mac_tv_resize_cursor():g_music_drag?[NSCursor closedHandCursor]:hit==MUSIC_MOVE?[NSCursor openHandCursor]:hit==MUSIC_BODY?[NSCursor arrowCursor]:[NSCursor pointingHandCursor] set];return 1;
}
static NSDictionary *mac_music_panel_status(void) {
    return @{@"enabled":@(g_music_enabled),@"visible":@(g_music_visible),@"placed":@(g_music_placed),@"offset":@(g_music_offset),
        @"rect":@{@"x":@(g_music_rect.x),@"y":@(g_music_rect.y),@"width":@(g_music_rect.w),@"height":@(g_music_rect.h)},@"player":mac_music_status()};
}
static NSDictionary *mac_music_view_pack(void) {
    return @{@"enabled":@(g_music_enabled),@"visible":@(g_music_visible),@"manual":@(g_music_manual),@"offset":@(g_music_offset),
        @"rect":@[@(g_music_requested.x),@(g_music_requested.y),@(g_music_requested.w),@(g_music_requested.h)],@"player":mac_music_pack()};
}
static void mac_music_view_restore(id state) {
    if(![state isKindOfClass:NSDictionary.class])return;
    g_music_enabled=[state[@"enabled"] boolValue];g_music_visible=g_music_enabled && [state[@"visible"] boolValue];
    g_music_manual=[state[@"manual"] boolValue];g_music_offset=chrome_max(0,[state[@"offset"] intValue]);
    NSArray *rect=state[@"rect"];
    if([rect isKindOfClass:NSArray.class] && rect.count==4) {
        BOOL valid=YES;for(id value in rect)if(![value isKindOfClass:NSNumber.class])valid=NO;
        if(valid)g_music_requested=(HcTvRect){chrome_max(0,[rect[0] intValue]),chrome_max(0,[rect[1] intValue]),
            chrome_min(HC_TV_MAX_WIDTH,chrome_max(32,[rect[2] intValue])),chrome_min(HC_TV_MAX_HEIGHT,chrome_max(12,[rect[3] intValue]))};
    }
    if(g_music_enabled) {
        mac_music_restore(state[@"player"]);
        if(!mac_music_items().count && ![mac_music_status()[@"loading"] boolValue])mac_music_load();
    }
    tab_layout();plat_invalidate();
}
