/* Separate, bounded secondary chrome surfaces. Included after chrome helpers. */
#include "hardcore_tv.h"
typedef struct {
    ChromePackRow *rows;
    int count, capacity, top, x, cw, ch, raised, cols;
    float alpha;
    int settings_tab, settings_offset, font_rows;
} ChromeMenuFrame;
typedef struct {
    HcTvState state;
    HcTvMotion motion;
    HcTvPlacement placement;
    HcTvPlacement planned;
    ChromeLayout layout;
    ChromeMenuFrame frame;
    int motion_blocked;
    int manual;
    HcTvRect requested;
    int tab_id, scroll;             /* Cuties bind stable tab IDs, never array positions. */
} ChromeTv;
static ChromeTv g_tvs[HC_TV_COUNT];
static int g_tv_drag_slot=-1, g_tv_drag_mode;
static int chrome_tv_slot_for(int tab_id) {
    if(tab_id<=0)return -1;
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++)
        if(g_tvs[i].state.open && g_tvs[i].tab_id==tab_id)return i;
    return -1;
}
static ChromeMenuFrame chrome_frame_save(void) {
    return (ChromeMenuFrame){g_chrome_menu_rows, g_chrome_menu_count, g_chrome_menu_capacity,
        g_chrome_menu_top, g_chrome_menu_x, g_chrome_menu_cw, g_chrome_menu_ch,
        g_chrome_menu_raised, g_chrome_menu_cols, g_chrome_menu_alpha,
        g_settings_tab,g_setting_offset[g_settings_tab],g_font_menu_count};
}
static void chrome_frame_load(ChromeMenuFrame f) {
    g_chrome_menu_rows=f.rows; g_chrome_menu_count=f.count; g_chrome_menu_capacity=f.capacity;
    g_chrome_menu_top=f.top; g_chrome_menu_x=f.x; g_chrome_menu_cw=f.cw; g_chrome_menu_ch=f.ch;
    g_chrome_menu_raised=f.raised; g_chrome_menu_cols=f.cols; g_chrome_menu_alpha=f.alpha;
}
static int chrome_tv_contains(int slot, int mx, int my) {
    if(slot<0 || slot>=HC_TV_COUNT)return 0;
    ChromeTv *tv=g_tvs+slot; HcTvRect r=tv->state.rect;
    if(slot>=HC_TV_CUTIE_FIRST && !tab_by_id(tv->tab_id))return 0;
    return tv->state.open && tv->state.placed && tv->state.alpha*tv->motion.alpha>0 &&
        mx>=r.x*CW && mx<(r.x+r.w)*CW && my>=r.y*CH && my<(r.y+r.h)*CH;
}
static int chrome_tv_hovered(int slot) {
    return !g_sel_drag && chrome_tv_contains(slot,g_mouse_x,g_mouse_y);
}
static int chrome_any_tv_contains(int mx,int my) {
    for(int i=0;i<HC_TV_COUNT;i++)if(chrome_tv_contains(i,mx,my))return 1;
    return 0;
}
static int chrome_any_tv_hovered(void) {
    return !g_sel_drag && chrome_any_tv_contains(g_mouse_x,g_mouse_y);
}
static void chrome_tv_interaction(int slot) {
    if(slot>=0 && slot<HC_TV_COUNT)hc_tv_interact(&g_tvs[slot].state,GetTickCount());
}
static void chrome_tv_close_slot(int slot) {
    if(slot<0 || slot>=HC_TV_COUNT)return;
    ChromeTv *tv=g_tvs+slot;
    if(slot>=HC_TV_CUTIE_FIRST && g_cutie_focus_id==tv->tab_id)g_cutie_focus_id=0;
    if(g_tv_drag_slot==slot)g_tv_drag_slot=-1;
    hc_tv_close(&tv->state);hc_tv_motion_reset(&tv->motion);
    tv->state.placed=0;tv->frame.count=0;tv->frame.alpha=0;
    tv->tab_id=tv->scroll=0;
    if(slot==HC_TV_SETTINGS)g_settings_open=0;
    plat_invalidate();
}
static void chrome_tv_prune(void) {
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++)
        if(g_tvs[i].state.open && !tab_by_id(g_tvs[i].tab_id))chrome_tv_close_slot(i);
}
static int chrome_tv_open_tab(int tab_id) {
    if(tab_id<=0 || !tab_by_id(tab_id))return -1;
    chrome_tv_prune();
    int existing=chrome_tv_slot_for(tab_id);
    if(existing>=0)return existing;
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++)if(!g_tvs[i].state.open) {
        ChromeTv *tv=g_tvs+i;
        /* Preserve the reusable row allocation, but invalidate its old map. */
        ChromePackRow *rows=tv->frame.rows;int capacity=tv->frame.capacity;
        memset(tv,0,sizeof(*tv));tv->frame.rows=rows;tv->frame.capacity=capacity;
        tv->tab_id=tab_id;hc_tv_open(&tv->state,GetTickCount());hc_tv_motion_reset(&tv->motion);
        plat_invalidate();return i;
    }
    return -1;
}
static void chrome_tv_scroll(int slot,int delta) {
    if(slot<HC_TV_CUTIE_FIRST || slot>=HC_TV_COUNT || !g_tvs[slot].state.open)return;
    int64_t next=(int64_t)g_tvs[slot].scroll+delta;
    g_tvs[slot].scroll=next<0?0:next>INT_MAX?INT_MAX:(int)next;
    chrome_tv_interaction(slot);plat_invalidate();
}
static void chrome_tv_toggle(int slot) {
    if(slot<0 || slot>=HC_TV_CUTIE_FIRST)return;
    HcTvState *s=&g_tvs[slot].state;
    if (s->open) chrome_tv_close_slot(slot); else hc_tv_open(s,GetTickCount());
    hc_tv_motion_reset(&g_tvs[slot].motion);
    if (slot==HC_TV_SETTINGS) g_settings_open=s->open;
    plat_invalidate();
}
static void chrome_tv_schedule(void) {
    unsigned next=0; uint32_t now=GetTickCount();
    for (int i=0;i<HC_TV_COUNT;i++) if(g_tvs[i].state.open) {
        unsigned delay=i<HC_TV_CUTIE_FIRST?hc_tv_next_tick_ms(&g_tvs[i].state,now):0;
        unsigned move=hc_tv_motion_next_ms(&g_tvs[i].motion,now,g_tvs[i].motion_blocked);
        if(move && (!delay || move<delay))delay=move;
        if (delay && (!next || delay<next)) next=delay;
    }
    if (next) plat_timer(TIMER_TV,(int)next); else plat_kill_timer(TIMER_TV);
}
static void chrome_tv_clock(void) {
    uint32_t now=GetTickCount();
    chrome_tv_prune();
    /* Snapshot restoration from versions with an attached settings list. */
    if (g_settings_open && !g_tvs[HC_TV_SETTINGS].state.open)
        hc_tv_open(&g_tvs[HC_TV_SETTINGS].state,now);
    for (int i=0;i<HC_TV_COUNT;i++) {
        if(i<HC_TV_CUTIE_FIRST)hc_tv_tick(&g_tvs[i].state,now,chrome_tv_hovered(i) || g_tv_drag_slot==i);
        if(!g_tvs[i].state.open)hc_tv_motion_reset(&g_tvs[i].motion);
    }
    g_settings_open=g_tvs[HC_TV_SETTINGS].state.open;
    chrome_tv_schedule();
}
/* Hit testing uses the previous rendered frame, including any packed words. */
static int chrome_tv_hit(int mx,int my,int *idx) {
    for(int i=0;i<HC_TV_COUNT;i++) if(chrome_tv_contains(i,mx,my)) {
        ChromeTv *tv=g_tvs+i; ChromeLayout a=tv->layout;
        int col=mx/CW-tv->state.rect.x,row=my/CH,at=row-tv->frame.top;
        if(tv->frame.alpha<=0 || at<0 || at>=tv->frame.count || col<0 || col>=tv->state.rect.w || tv->frame.cols!=tv->state.rect.w ||
           !tv->frame.rows || tv->frame.cw!=CW || tv->frame.ch!=CH ||
           tv->frame.x!=tv->state.rect.x*CW || tv->frame.top!=a.top || tv->frame.count!=a.bottom-a.top) return HIT_NONE;
        if(i==HC_TV_SETTINGS && (tv->frame.settings_tab!=g_settings_tab ||
           tv->frame.settings_offset!=g_setting_offset[g_settings_tab] ||
           (!g_settings_tab && tv->frame.font_rows!=g_font_menu_count)))return HIT_NONE;
        *idx=i;
        if(i>=HC_TV_CUTIE_FIRST && row!=a.top)
            return row==a.bottom-1 && col>=tv->state.rect.w-2?HIT_TV_RESIZE:HIT_TV;
        if(row==a.top) {
            /* The full visible title is a handle. Packed button glyphs keep
               their source action, while exposed whitespace also drags.
               A terminal intrusion still belongs to the terminal below. */
            int source=col;
            if(!tv->frame.raised) {
                ChromePackRow *packed=tv->frame.rows+at;
                if(col>=packed->blocked_left && col<packed->blocked_right)return HIT_NONE;
                source=packed->actionable[col]?packed->src[col]:-1;
            }
            if(source==tv->state.rect.w-2)return HIT_TV_CLOSE;
            if(i>=HC_TV_CUTIE_FIRST && source==tv->state.rect.w-4)return HIT_TV_EXPAND;
            return HIT_TV_GRIP;
        }
        if(!tv->frame.raised) {
            if(!tv->frame.rows[at].actionable[col]) return HIT_NONE;
            col=tv->frame.rows[at].src[col];
        }
        if(i==HC_TV_CAF) return HIT_TV;
        if(row==a.section_row){*idx=col<SB_COLS/2?0:1;return HIT_STAB;}
        if(row>=a.list_row && row<a.list_row+a.list_count) {
            int item=g_setting_offset[g_settings_tab]+row-a.list_row,page_hit;
            if(row==a.list_row && (page_hit=chrome_page_hit(col,g_setting_offset[g_settings_tab],a.list_capacity,chrome_item_total(),0,idx)))
                return page_hit==1 ? HIT_SETPAGE:HIT_CHROME_INERT;
            if(!g_settings_tab){*idx=font_menu_hit(item,col);return *idx>=0?HIT_FONT:HIT_CHROME_INERT;}
            int nc,cc,right=SB_COLS-1;
            if(row==a.list_row && chrome_pager_geometry(g_setting_offset[1],a.list_capacity,NFAMILIES,0,&nc,&cc))right=nc;
            *idx=scheme_index(FAMILIES[item].light && col>=right-2 ? FAMILIES[item].light:FAMILIES[item].dark);
            return HIT_SCHEME;
        }
        return HIT_TV;
    }
    return HIT_NONE;
}
