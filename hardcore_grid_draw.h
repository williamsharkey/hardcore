static void activate(int index);
static void mac_grid_utilities(void);
static void mac_grid_utility_draw(void);
static int mac_grid_utility_click(int x,int y);
static void mac_grid_transition(int entering);
static void mac_grid_browser_sync(void);
static int g_grid_browser_scroll[MAX_TABS],g_grid_browser_generation[MAX_TABS];
static Cell *g_grid_browser_cells[MAX_TABS];
static int g_grid_browser_capacity[MAX_TABS],g_grid_browser_id[MAX_TABS];
static void grid_refit(void) {
    grid_measure(g_cols,g_rows);
    for(int i=0;i<g_ntabs;i++)tab_fit(g_tabs[i]);
    g_display_term=NULL;g_sel_on=g_sel_drag=g_sb_drag=0;
    tab_layout();mac_grid_browser_sync();plat_invalidate();
}
static void grid_toggle(void) {
    chrome_preview_cancel();mac_cutie_blur();mac_tv_drag_end();
    mac_grid_transition(!g_grid_on);
    g_grid_on=!g_grid_on;g_grid_drag_axis=0;
    cm_reset(&g_chrome_motion);g_chrome_motion_alpha=1;
    if(g_grid_on && !g_grid_expand_id && g_ntabs)g_grid_expand_id=g_tabs[0]->id;
    for(int i=0;i<HC_TV_COUNT;i++){g_tvs[i].state.placed=0;hc_tv_motion_reset(&g_tvs[i].motion);}
    grid_refit();
}
static void grid_hide_tab(int index) {
    if(index<0 || index>=g_ntabs)return;
    int id=g_tabs[index]->id;grid_set_hidden(id,!grid_hidden(id));
    grid_measure(g_cols,g_rows);
    if(grid_hidden(id) && index==g_active && g_grid_count)
        for(int i=0;i<g_ntabs;i++)if(g_tabs[i]->id==g_grid_ids[0]){activate(i);break;}
    grid_refit();
}
static void grid_expand_next(void) {
    if(!g_grid_on || !g_grid.extra || !g_grid_count)return;
    int first=g_grid_ids[0],at=-1;
    for(int i=0;i<g_ntabs;i++)if(g_tabs[i]->id==first)at=i;
    for(int step=1;step<=g_ntabs;step++) {
        Tab *tb=g_tabs[(at+step)%g_ntabs];
        if(!grid_hidden(tb->id)){g_grid_expand_id=tb->id;break;}
    }
    grid_refit();
}
static int grid_pane_at(int mx,int my) {
    if(!g_grid_on)return -1;
    for(int i=0;i<g_grid_count;i++) {
        HcGridRect r=grid_rect(g_grid_ids[i]);
        if(mx>=r.x*CW && mx<(r.x+r.w)*CW && my>=r.y*CH && my<(r.y+r.h)*CH)
            for(int tab=0;tab<g_ntabs;tab++)if(g_tabs[tab]->id==g_grid_ids[i])return tab;
    }
    return -1;
}
static int grid_divider_at(int mx,int my,int *boundary) {
    if(!g_grid_on || mx<g_grid_left*CW || my<g_grid_top*CH ||
       mx>(g_grid_left+g_grid.width)*CW || my>(g_grid_top+g_grid.height)*CH)return 0;
    for(int i=1;i<g_grid.rows;i++)if(abs(my-(g_grid_top+g_grid.y[i])*CH)<=4) {*boundary=i;return 2;}
    for(int i=1;i<g_grid.cols;i++) {
        /* A merged first pane has no interior divider to grab. */
        if(g_grid.extra && i<=g_grid.extra && my<(g_grid_top+g_grid.y[1])*CH)continue;
        if(abs(mx-(g_grid_left+g_grid.x[i])*CW)<=4){*boundary=i;return 1;}
    }
    return 0;
}
static int grid_title_action(HcGridRect r,int mx) {
    int col=mx/CW-r.x;
    if(col>=r.w-5)return 1; /* Solo */
    if(r.w>=14 && col>=r.w-11 && col<r.w-7)return 2; /* Hide */
    if(g_grid.extra && r.w>=24 && col>=r.w-19 && col<r.w-13)return 3;
    return 0;
}
/* The title's last column: left of whichever of Expand, Hide and Solo the pane shows */
static int grid_title_end(HcGridRect r) {
    return r.w>=24 && g_grid.extra?r.w-22:r.w>=14?r.w-13:chrome_max(1,r.w-7);
}
static void grid_pane_title(Tab *tb,int index,HcGridRect r,wchar_t *title) {
    /* A pane that wants you is marked as the tab menu marks it */
    int mark=tb->attention || tb->t.ps_state==PS_BLOCKED || tb->t.ps_state==PS_ERROR;
    wchar_t number[16];int digits=_snwprintf(number,16,mark?L"%d!":L"%d",index+1);
    wchar_t label[256];tab_label_fit(tb,label,256,chrome_max(1,grid_title_end(r)-digits-1));
    _snwprintf(title,280,L"%ls %ls",number,label);
}
static void grid_draw_cells(Tab *tb,int index,HcGridRect rect) {
    Term *t=&tb->t;int width=rect.w,height=rect.h-1;
    if(width<1 || height<1)return;
    int x0=rect.x*CW,y0=(rect.y+1)*CH;
    Cell *browser=NULL;int shift=tab_hold_shift(tb,height);
    if(tb->kind==TK_BROWSER) {
        int count=width*height;
        if(g_grid_browser_id[index]!=tb->id){g_grid_browser_scroll[index]=0;g_grid_browser_id[index]=tb->id;}
        if(g_grid_browser_capacity[index]<count) {
            Cell *next=realloc(g_grid_browser_cells[index],count*sizeof(Cell));
            if(next){g_grid_browser_cells[index]=next;g_grid_browser_capacity[index]=count;}
        }
        if(g_grid_browser_capacity[index]>=count) {
            browser=g_grid_browser_cells[index];memset(browser,0,count*sizeof(Cell));
            int total=mac_browser_cutie_project(tb,browser,width,height,g_grid_browser_scroll[index]);
            g_grid_browser_scroll[index]=chrome_min(g_grid_browser_scroll[index],chrome_max(0,total-height));
            g_grid_browser_generation[index]=mac_browser_cutie_generation(tb);
        }
    }
    for(int y=0;y<height;y++) {
        const Cell *cells=NULL;int n=0,wrapped;
        if(browser){cells=browser+y*width;n=width;}
        else if(tb->kind!=TK_BROWSER)term_line(t,t->hcount-t->view+y+shift,&cells,&n,&wrapped);
        n=chrome_min(n,width);
        for(int x=0;x<width;x++) {
            const Cell *c=x<n?cells+x:NULL;uint32_t fg=DEF_FG,bg=DEF_BG;
            if(c)cell_colors(c,&fg,&bg);
            if(tb==active_tab() && g_sel_on && display_selected(t,x,y))bg=SEL_BG;
            if(bg!=DEF_BG)fill_rect(x0+x*CW,y0+y*CH,x0+(x+1)*CW,y0+(y+1)*CH,bg,1);
        }
        for(int x=0;x<n;x++) {
            const Cell *c=cells+x;uint32_t fg,bg;cell_colors(c,&fg,&bg);
            if(c->wflag==W_CONT || (c->wflag&8))continue;
            int w=c->wflag==W_WIDE?2:1;if(x+w>width)continue;
            int px=x0+x*CW,py=y0+y*CH;
            if(c->ch>' ' && fg!=bg) {
                r_color(fg,1);draw_glyph(c->ch,px,py,w,g_scale);
                if((c->attr&A_BOLD) && !(c->ch>=0x2500 && c->ch<=0x259F))draw_glyph(c->ch,px+smear(),py,w,g_scale);
            }
            if(c->attr&A_UNDER || c->link)fill_rect(px,py+CH-1,px+w*CW,py+CH,fg,c->link?.25f:1);
            if(c->attr&A_STRIKE)fill_rect(px,py+CH/2,px+w*CW,py+CH/2+1,fg,1);
        }
    }
    if(tb==active_tab() && tb->kind!=TK_BROWSER && !t->view && t->m_cursor_vis && t->cx<width && t->cy-shift>=0 && t->cy-shift<height) {
        int x=x0+t->cx*CW,y=y0+(t->cy-shift)*CH;
        fill_rect(x,y+CH-2,x+CW,y+CH,AMBER,1);
        draw_preedit(x,y,width-t->cx);
    }
}
/* Each pane's tab to its pane, told before the frame that shows it (paint_now waits for the answers) */
static void grid_fit(int win_w,int win_h) {
    UI_FONT;
    grid_measure(win_w/CW,win_h/CH);
    for(int i=0;i<g_grid_count;i++){Tab *tb=tab_by_id(g_grid_ids[i]);if(tb)tab_fit(tb);}
    UI_DONE;
}
static void render_grid(int win_w,int win_h) {
    UI_FONT;
    grid_fit(win_w,win_h);
    tab_layout();mac_grid_utilities();
    g_display_term=NULL;g_draw_alpha=1;g_chrome_x=0;
    int clip_w=g_clip_w,clip_h=g_clip_h;
    for(int i=0;i<g_grid_count;i++) {
        Tab *tb=tab_by_id(g_grid_ids[i]);if(!tb)continue;
        HcGridRect r=grid_rect(tb->id);if(r.w<1 || r.h<2)continue;
        int index=0;while(index<g_ntabs && g_tabs[index]!=tb)index++;
        g_clip_w=(r.x+r.w)*CW;g_clip_h=(r.y+r.h)*CH;
        grid_draw_cells(tb,index,r);
        int x=r.x*CW,y=r.y*CH,right=(r.x+r.w)*CW;
        fill_rect(x,y,right,y+CH,BAR_BG,1);
        wchar_t title[280];grid_pane_title(tb,index,r,title);
        draw_label(title,x+CW,y,grid_title_end(r)*CW,tb==active_tab()?AMBER:g_look.text_lo);
        draw_label(L"Solo",(r.x+r.w-5)*CW,y,4*CW,g_look.detail);
        if(r.w>=14)draw_label(L"Hide",(r.x+r.w-11)*CW,y,4*CW,g_look.detail);
        if(g_grid.extra && r.w>=24)draw_label(L"Expand",(r.x+r.w-19)*CW,y,6*CW,i?g_look.detail:AMBER);
        fill_rect(x,y+CH-1,right,y+CH,g_look.dim,.5f);
        /* Each pane owns only its left/top edges: shared boundaries never double. */
        if(r.x>g_grid_left)fill_rect(x,y,x+1,(r.y+r.h)*CH,g_look.dim,.5f);
        if(r.y>g_grid_top)fill_rect(x,y,right,y+1,g_look.dim,.5f);
    }
    g_clip_w=clip_w;g_clip_h=clip_h;
    int rail=g_grid_left?g_grid_rail*CW:(win_w/CW-g_grid_rail)*CW;
    int lx=g_grid_left?0:rail,rx=g_grid_left?rail:win_w;
    fill_rect(lx,g_grid_top*CH,rx,win_h,BAR_BG,1);
    fill_rect(rail,g_grid_top*CH,rail+1,win_h,g_look.dim,.5f);
    if(!g_grid_count)draw_label(L"All tabs hidden. Use show in the menu.",g_grid_left*CW+CW,(g_grid_top+2)*CH,g_grid.width*CW-CW,g_look.detail);
    render_tabbar(win_w,win_h);mac_grid_utility_draw();mac_camera_draw();mac_music_draw();
    mac_browser_render_nav_overlay();UI_DONE;
}
