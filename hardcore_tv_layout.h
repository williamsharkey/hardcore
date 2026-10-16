/* Place secondary TVs against the primary display result. Twelve anchors per
 * surface, at most six surfaces, and one bounded reflow per accepted surface. */
#ifndef _WIN32
#include "hardcore_tv_reflow.h"
static int chrome_tv_build(ChromeReflow *out,const Cell *input,int cols,int rows,
                           const unsigned char *protected_rows,int cursor,HcTvRect r,void *ctx) {
    int side=r.x?1:2; (void)ctx;
    if(!chrome_reflow_build_checked(out,input,cols,rows,protected_rows,side,r.x,r.y,r.w,r.h,3,chrome_background_safe))return 0;
    chrome_panel_reflow_apply_checked(out,input,cols,rows,NULL,cursor,side,r.x,r.y,r.w,r.h,
        DEF_BG,chrome_panel_background,&out->cursor,protected_rows);
    return 1;
}
static int chrome_tv_ink(const Cell *cell,void *ctx){(void)ctx;return cell_covers_chrome(cell);}
static void chrome_tv_reflow_apply(Term *t,HcTvRect r,const HcTvRect *prior,int count) {
    if(!g_chrome_reflow || !g_display.cells || g_display_term!=t ||
       g_display.cols!=t->cols || g_display.rows!=t->rows ||
       g_display_view!=t->view || g_display_hcount!=t->hcount)return;
    int cursor=-1;
    if(!t->view && t->m_cursor_vis)cursor=g_display.cursor>=0?g_display.cursor:t->cy*t->cols+t->cx;
    hc_tv_reflow_compose(&g_display,r,prior,count,cursor,chrome_tv_build,chrome_tv_ink,NULL);
}
typedef struct {Term *term;HcTvRect primary;} ChromeTvCostContext;
static int chrome_tv_cost(HcTvRect r,HcTvCost *cost,void *context) {
    ChromeTvCostContext *ctx=context;Term *t=ctx->term;
    if(r.x!=0 && r.x+r.w!=t->cols)return 0; /* edge lanes preserve reading order */
    if(hc_tv_rect_overlap(r,ctx->primary))return 0;
    for(int i=0;i<HC_TV_COUNT;i++) if(g_tvs[i].state.placed && hc_tv_rect_overlap(r,g_tvs[i].state.rect))return 0;
    if((size_t)t->cols*t->rows>HC_TV_REFLOW_MAX_CELLS)return 1;
    int cx,cy;display_cursor_position(t,&cx,&cy);
    for(int y=r.y;y<r.y+r.h;y++) {
        const Cell *cells;int n,left,right;
        display_line(t,y,&cells,&n);
        int occupied=chrome_terminal_span(cells,n,&left,&right);
        if(!t->view && t->m_cursor_vis && cy==y && cx>=0 && cx<t->cols) {
            int end=cx+(cx<n && cells[cx].wflag==W_WIDE?2:1);
            if(!occupied || cx<left)left=cx;
            if(!occupied || end>right)right=end;
            occupied=1;
        }
        if(occupied) {
            left=chrome_max(left,r.x);right=chrome_min(right,r.x+r.w);
            if(right>left){cost->collision_rows++;cost->collision_cells+=right-left;}
        }
    }
    return 1;
}
static uint64_t chrome_tv_signature(Term *t) {
    uint64_t hash=UINT64_C(1469598103934665603);
    int state[]={t->cx,t->cy,t->m_cursor_vis,t->view,t->hcount};
    hash=chrome_motion_hash(hash,state,sizeof(state));
    if((size_t)t->cols*t->rows<=HC_TV_REFLOW_MAX_CELLS &&
       !(display_tab() && display_tab()->browser_native))
        for(int y=0;y<t->rows;y++) {
            const Cell *cells;int n;display_line(t,y,&cells,&n);
            hash=chrome_motion_hash(hash,&n,sizeof(n));
            if(n>0)hash=chrome_motion_hash(hash,cells,(size_t)n*sizeof(*cells));
        }
    return hash;
}
static int chrome_tv_placement_valid(HcTvPlacement p,HcTvRect primary,Term *t,int height) {
    if(!p.valid || p.rect.w!=SB_COLS || p.rect.h!=height ||
       !hc_tv_rect_fits(p.rect,t->cols,t->rows) || hc_tv_rect_overlap(p.rect,primary))return 0;
    for(int i=0;i<HC_TV_COUNT;i++)if(g_tvs[i].state.placed && hc_tv_rect_overlap(p.rect,g_tvs[i].state.rect))return 0;
    return 1;
}
static void chrome_tv_prepare(Term *t) {
    uint32_t now=GetTickCount();
    int blocked=chrome_hovered() || chrome_any_tv_hovered() || chrome_preview_active() || g_cutie_focus_id>0 ||
        g_chrome_drag || g_sel_on || g_sel_drag || g_sb_drag ||
        (uint32_t)(now-g_chrome_input_at)<700;
    int open=0;
    for(int i=0;i<HC_TV_COUNT;i++) {g_tvs[i].state.placed=0;g_tvs[i].motion_blocked=blocked;open+=g_tvs[i].state.open;}
    if(t->cols<SB_COLS || t->rows<3) {
        for(int i=0;i<HC_TV_COUNT;i++)hc_tv_motion_reset(&g_tvs[i].motion);
        chrome_tv_schedule();return;
    }
    if(!open){chrome_tv_schedule();return;}
    HcTvRect primary={sidebar_x()/CW,g_chrome.top,SB_COLS,g_chrome.bottom-g_chrome.top};
    ChromeTvCostContext cost_context={t,primary};
    HcTvRect obstacles[HC_TV_COUNT+1]={primary}; int count=1;
    uint64_t signature=chrome_tv_signature(t);
    for(int i=0;i<HC_TV_COUNT;i++) {
        ChromeTv *tv=g_tvs+i;
        if(!tv->state.open){hc_tv_motion_reset(&tv->motion);continue;}
        int items=0,height=i>=HC_TV_CUTIE_FIRST?HC_TV_CUTIE_ROWS:6,capacity=0;
        int room=t->rows;
        if(t->cols<2*SB_COLS)room=chrome_max(primary.y,t->rows-primary.y-primary.h);
        if(i==HC_TV_SETTINGS) {
            items=chrome_item_total();capacity=chrome_min(items,chrome_max(0,room-2));
            if(!g_settings_tab) {
                items=font_menu_build(0,0);
                capacity=chrome_min(items,chrome_max(0,room-2));
                for(int pass=0;pass<NFONTS;pass++) {
                    int previous=items,old_capacity=capacity;
                    items=font_menu_build(capacity,items);
                    capacity=chrome_min(items,chrome_max(0,room-2));
                    if(items==previous && capacity==old_capacity)break;
                }
            }
            g_setting_offset[g_settings_tab]=chrome_page_offset(g_setting_offset[g_settings_tab],items,capacity);
            height=2+capacity;
        } else height=chrome_min(height,room);
        uintptr_t key[]={(uintptr_t)t,(uintptr_t)g_font,(uintptr_t)g_scheme,
            (uintptr_t)t->cols,(uintptr_t)t->rows,(uintptr_t)height,(uintptr_t)g_settings_tab,
            (uintptr_t)g_setting_offset[g_settings_tab],(uintptr_t)g_tab_underlay};
        uint64_t context=chrome_motion_hash(UINT64_C(1469598103934665603),key,sizeof(key));
        context=chrome_motion_hash(context,obstacles,(size_t)count*sizeof(*obstacles));
        int first=!tv->motion.initialized,context_changed=tv->motion.context!=context;
        int valid=chrome_tv_placement_valid(tv->placement,primary,t,height);
        int action=hc_tv_motion_observe(&tv->motion,now,signature,context,blocked);
        if(height<(i==HC_TV_CAF?2:3)) {tv->motion.searched=1;continue;}
        /* New surfaces and invalid geometry need a fitting location now.
         * Content-only improvements wait for the quiet interval and fade. */
        if(first || (!valid && context_changed) || action==HC_TV_MOTION_SEARCH) {
            /* Later TVs extend the visible stack before trying other edges.
             * The callback still vetoes the authoritative primary rectangle. */
            HcTvRect anchor=obstacles[count-1];
            HcTvPlacement choice=hc_tv_place(t->cols,t->rows,anchor,SB_COLS,height,
                                             &tv->placement,chrome_tv_cost,&cost_context);
            tv->motion.searched=1;
            if(!valid || first) {
                tv->placement=choice;
                tv->motion.phase=HC_TV_MOTION_IDLE;tv->motion.alpha=1;
            } else if(choice.valid && !hc_tv_rect_equal(choice.rect,tv->placement.rect)) {
                tv->planned=choice;hc_tv_motion_start(&tv->motion,choice.rect,now);
            }
        } else if(action==HC_TV_MOTION_COMMIT) {
            if(chrome_tv_placement_valid(tv->planned,primary,t,height))tv->placement=tv->planned;
            else {tv->motion.phase=HC_TV_MOTION_IDLE;tv->motion.alpha=1;}
        }
        if(!chrome_tv_placement_valid(tv->placement,primary,t,height))continue;
        HcTvRect r=tv->placement.rect;
        tv->state.rect=r;tv->state.placed=1;
        tv->layout=(ChromeLayout){0};tv->layout.top=r.y;tv->layout.bottom=r.y+r.h;
        tv->layout.settings_row=-1;tv->layout.section_row=i==HC_TV_SETTINGS?r.y+1:-1;
        tv->layout.list_row=i==HC_TV_SETTINGS?r.y+2:-1;
        tv->layout.list_capacity=capacity;
        tv->layout.list_count=i==HC_TV_SETTINGS?chrome_min(capacity,items-g_setting_offset[g_settings_tab]):0;
        /* The following composer is intentionally conservative around every
           earlier surface. It cannot trade away primary-menu clearance. */
        chrome_tv_reflow_apply(t,r,obstacles,count);
        obstacles[count++]=r;
    }
    chrome_tv_schedule();
}
#endif
