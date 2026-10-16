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
typedef struct {
    Term *term;HcTvRect primary;
    HcTvRect ordered[HC_TV_CUTIE_COUNT];int nordered;
} ChromeTvCostContext;
static int chrome_tv_ordered(HcTvRect r,const ChromeTvCostContext *ctx) {
    for(int i=0;i<ctx->nordered;i++) {
        HcTvRect before=ctx->ordered[i];
        if(r.x<before.x+before.w && before.x<r.x+r.w && r.y<before.y+before.h)return 0;
    }
    return 1;
}
static int chrome_tv_cost(HcTvRect r,HcTvCost *cost,void *context) {
    ChromeTvCostContext *ctx=context;Term *t=ctx->term;
    if(!chrome_tv_ordered(r,ctx))return 0;
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
static int chrome_tv_rect_available(HcTvRect r,HcTvRect primary,Term *t,int skip) {
    if(!hc_tv_rect_fits(r,t->cols,t->rows) || hc_tv_rect_overlap(r,primary))return 0;
    for(int i=0;i<HC_TV_COUNT;i++)if(i!=skip && g_tvs[i].state.placed && hc_tv_rect_overlap(r,g_tvs[i].state.rect))return 0;
    return 1;
}
static int chrome_tv_placement_valid(HcTvPlacement p,HcTvRect primary,Term *t,int width,int height) {
    return p.valid && p.rect.w==width && p.rect.h==height && chrome_tv_rect_available(p.rect,primary,t,-1);
}
static void chrome_tv_accept(Term *t,int slot,HcTvRect r,int items,int capacity,HcTvRect *obstacles,int *count) {
    ChromeTv *tv=g_tvs+slot;
    tv->state.rect=r;tv->state.placed=1;
    tv->layout=(ChromeLayout){0};tv->layout.top=r.y;tv->layout.bottom=r.y+r.h;
    tv->layout.settings_row=-1;tv->layout.section_row=slot==HC_TV_SETTINGS?r.y+1:-1;
    tv->layout.list_row=slot==HC_TV_SETTINGS?r.y+2:-1;
    tv->layout.list_capacity=capacity;
    tv->layout.list_count=slot==HC_TV_SETTINGS?chrome_min(capacity,items-g_setting_offset[g_settings_tab]):0;
    chrome_tv_reflow_apply(t,r,obstacles,*count);
    obstacles[(*count)++]=r;
}
/* Tab-list order is independent of the slot in which a miniature was opened. */
static int chrome_tv_order(int *order) {
    unsigned used=3;int n=0;
    order[n++]=HC_TV_SETTINGS;order[n++]=HC_TV_CAF;
    for(int tab=0;tab<g_ntabs;tab++)for(int slot=HC_TV_CUTIE_FIRST;slot<HC_TV_COUNT;slot++)
        if(!(used&(1u<<slot)) && g_tvs[slot].tab_id==g_tabs[tab]->id) {
            order[n++]=slot;used|=1u<<slot;
        }
    /* A disappearing source is pruned by the clock. Keep this path bounded
       even if a caller prepares layout before that next clock pass. */
    for(int slot=HC_TV_CUTIE_FIRST;slot<HC_TV_COUNT;slot++)if(!(used&(1u<<slot)))order[n++]=slot;
    return n;
}
static uint64_t g_tv_stack_signature;
static int g_tv_stack_pending;
static void chrome_tv_prepare(Term *t) {
    uint32_t now=GetTickCount();
    int held=chrome_hovered() || chrome_any_tv_hovered() || chrome_preview_active() || g_cutie_focus_id>0 ||
        g_chrome_drag || g_sel_on || g_sel_drag || g_sb_drag || g_tv_drag_slot>=0;
    int blocked=held || (uint32_t)(now-g_chrome_input_at)<700;
    HcTvRect previous[HC_TV_COUNT];int was_placed[HC_TV_COUNT],reserved[HC_TV_COUNT]={0};
    int widths[HC_TV_COUNT],heights[HC_TV_COUNT],capacities[HC_TV_COUNT]={0},items[HC_TV_COUNT]={0};
    int order[HC_TV_COUNT],open=0;chrome_tv_order(order);
    for(int i=0;i<HC_TV_COUNT;i++) {
        previous[i]=g_tvs[i].state.rect;was_placed[i]=g_tvs[i].state.placed;
        g_tvs[i].state.placed=0;g_tvs[i].motion_blocked=blocked;open+=g_tvs[i].state.open;
    }
    if(t->cols<SB_COLS || t->rows<3) {
        for(int i=0;i<HC_TV_COUNT;i++)hc_tv_motion_reset(&g_tvs[i].motion);
        chrome_tv_schedule();return;
    }
    if(!open){g_tv_stack_signature=0;g_tv_stack_pending=0;chrome_tv_schedule();return;}
    HcTvRect primary={sidebar_x()/CW,g_chrome.top,SB_COLS,g_chrome.bottom-g_chrome.top};
    ChromeTvCostContext cost_context={.term=t,.primary=primary};
    HcTvRect obstacles[HC_TV_COUNT+1]={primary};int count=1;
    uint64_t signature=chrome_tv_signature(t),stack=chrome_motion_hash(1,&primary,sizeof(primary));
    stack=chrome_motion_hash(stack,order,sizeof(order));
    for(int i=0;i<HC_TV_COUNT;i++) {
        int fields[]={g_tvs[i].state.open,g_tvs[i].tab_id,g_tvs[i].manual};
        stack=chrome_motion_hash(stack,fields,sizeof(fields));
        int room=t->rows;
        if(t->cols<2*SB_COLS)room=chrome_max(primary.y,t->rows-primary.y-primary.h);
        widths[i]=SB_COLS;heights[i]=chrome_min(6,room);
        if(i>=HC_TV_CUTIE_FIRST) {
            HcTvRect r=hc_tv_requested_rect(g_tvs[i].requested,t->cols,t->rows);
            widths[i]=r.w;heights[i]=r.h;
        } else if(i==HC_TV_SETTINGS && g_tvs[i].state.open) {
            items[i]=chrome_item_total();capacities[i]=chrome_min(items[i],chrome_max(0,room-2));
            if(!g_settings_tab) {
                items[i]=font_menu_build(0,0);
                capacities[i]=chrome_min(items[i],chrome_max(0,room-2));
                for(int pass=0;pass<NFONTS;pass++) {
                    int old_items=items[i],old_capacity=capacities[i];
                    items[i]=font_menu_build(capacities[i],items[i]);
                    capacities[i]=chrome_min(items[i],chrome_max(0,room-2));
                    if(items[i]==old_items && capacities[i]==old_capacity)break;
                }
            }
            g_setting_offset[g_settings_tab]=chrome_page_offset(g_setting_offset[g_settings_tab],items[i],capacities[i]);
            heights[i]=2+capacities[i];
        }
    }
    if(stack!=g_tv_stack_signature){g_tv_stack_signature=stack;g_tv_stack_pending=1;}
    int rebuild_stack=g_tv_stack_pending&&!blocked;
    if(rebuild_stack)g_tv_stack_pending=0;
    /* Manual rectangles reserve space before automatic surfaces. While the
       pointer is interacting, a drag cannot shove an existing panel away. */
    for(int at=HC_TV_CUTIE_FIRST;at<HC_TV_COUNT;at++) {
        int i=order[at];ChromeTv *tv=g_tvs+i;
        if(!tv->state.open || !tv->manual)continue;
        HcTvRect r=hc_tv_requested_rect(tv->requested,t->cols,t->rows);
        int valid=r.w>=HC_TV_MIN_WIDTH && r.h>=HC_TV_MIN_HEIGHT && chrome_tv_rect_available(r,primary,t,i);
        if(valid && blocked)for(int j=0;j<HC_TV_COUNT;j++)if(j!=i && g_tvs[j].state.open &&
            !g_tvs[j].manual && was_placed[j] && hc_tv_rect_overlap(r,previous[j]))valid=0;
        if(!valid && was_placed[i] && chrome_tv_rect_available(previous[i],primary,t,i))r=previous[i],valid=1;
        hc_tv_motion_reset(&tv->motion);
        if(!valid)continue;
        tv->placement=(HcTvPlacement){.rect=r,.valid=1};reserved[i]=1;
        chrome_tv_accept(t,i,r,0,0,obstacles,&count);
    }
    /* Existing surfaces stay still while hovered, typed into, or dragged.
       New surfaces may use the remaining room without moving their neighbors. */
    if(blocked)for(int at=0;at<HC_TV_COUNT;at++) {
        int i=order[at];ChromeTv *tv=g_tvs+i;HcTvRect r=previous[i];
        if(reserved[i] || !tv->state.open || tv->manual || !was_placed[i] ||
           r.w!=widths[i] || r.h!=heights[i] || !chrome_tv_rect_available(r,primary,t,i))continue;
        hc_tv_motion_observe(&tv->motion,now,signature,tv->motion.context,1);
        reserved[i]=1;chrome_tv_accept(t,i,r,items[i],capacities[i],obstacles,&count);
    }
    HcTvRect anchor=primary;
    for(int at=0;at<HC_TV_COUNT;at++) {
        int i=order[at];ChromeTv *tv=g_tvs+i;
        if(!tv->state.open){hc_tv_motion_reset(&tv->motion);continue;}
        if(tv->manual)continue;
        if(reserved[i]) {
            if(i>=HC_TV_CUTIE_FIRST)cost_context.ordered[cost_context.nordered++]=tv->state.rect;
            anchor=tv->state.rect;continue;
        }
        int width=widths[i],height=heights[i];
        uintptr_t key[]={(uintptr_t)t,(uintptr_t)g_font,(uintptr_t)g_scheme,
            (uintptr_t)t->cols,(uintptr_t)t->rows,(uintptr_t)width,(uintptr_t)height,(uintptr_t)g_settings_tab,
            (uintptr_t)g_setting_offset[g_settings_tab],(uintptr_t)g_tab_underlay};
        uint64_t context=chrome_motion_hash(UINT64_C(1469598103934665603),key,sizeof(key));
        context=chrome_motion_hash(context,obstacles,(size_t)count*sizeof(*obstacles));
        int first=!tv->motion.initialized,context_changed=tv->motion.context!=context;
        int valid=chrome_tv_placement_valid(tv->placement,primary,t,width,height);
        int action=hc_tv_motion_observe(&tv->motion,now,signature,context,blocked);
        if(height<(i==HC_TV_SETTINGS?3:i==HC_TV_CAF?2:HC_TV_MIN_HEIGHT)) {tv->motion.searched=1;continue;}
        int regroup=i>=HC_TV_CUTIE_FIRST&&rebuild_stack;
        if(first || regroup || (!valid && context_changed) || action==HC_TV_MOTION_SEARCH) {
            const HcTvPlacement *old=regroup?NULL:&tv->placement;
            HcTvPlacement choice=i>=HC_TV_CUTIE_FIRST?
                hc_tv_place_stacked(t->cols,t->rows,primary,anchor,width,height,obstacles,count,old,chrome_tv_cost,&cost_context):
                hc_tv_place(t->cols,t->rows,anchor,width,height,old,chrome_tv_cost,&cost_context);
            tv->motion.searched=1;
            if(!valid || first || regroup) {
                tv->placement=choice;tv->motion.phase=HC_TV_MOTION_IDLE;tv->motion.alpha=1;
            } else if(choice.valid && !hc_tv_rect_equal(choice.rect,tv->placement.rect)) {
                tv->planned=choice;hc_tv_motion_start(&tv->motion,choice.rect,now);
            }
        } else if(action==HC_TV_MOTION_COMMIT) {
            if(chrome_tv_placement_valid(tv->planned,primary,t,width,height))tv->placement=tv->planned;
            else {tv->motion.phase=HC_TV_MOTION_IDLE;tv->motion.alpha=1;}
        }
        if(!chrome_tv_placement_valid(tv->placement,primary,t,width,height))continue;
        HcTvRect r=tv->placement.rect;
        chrome_tv_accept(t,i,r,items[i],capacities[i],obstacles,&count);anchor=r;
        if(i>=HC_TV_CUTIE_FIRST)cost_context.ordered[cost_context.nordered++]=r;
    }
    chrome_tv_schedule();
    /* A reorder deferred only by a recent keystroke needs one wakeup even
       if every TV was frozen and no animation/idle timer otherwise remains. */
    if(g_tv_stack_pending && !held && (uint32_t)(now-g_chrome_input_at)<700) {
        unsigned delay=700-(uint32_t)(now-g_chrome_input_at);
        for(int i=0;i<HC_TV_CUTIE_FIRST;i++)if(g_tvs[i].state.open) {
            unsigned idle=hc_tv_next_tick_ms(&g_tvs[i].state,now);
            if(idle && idle<delay)delay=idle;
        }
        plat_timer(TIMER_TV,(int)delay);
    }
}
#endif
