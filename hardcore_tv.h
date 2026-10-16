/* Small floating terminal panels ("TVs"). Pure grid geometry and clock state:
 * no terminal mutation, allocations, platform timers, or unbounded searches.
 * The caller owns colors/fonts, rendering, obstacle/reflow costs, and events. */
#ifndef HARDCORE_TV_H
#define HARDCORE_TV_H
#include <stdint.h>
#include <limits.h>
#include <string.h>

enum { HC_TV_COUNT = 6, HC_TV_MAX_SLOTS = HC_TV_COUNT + 1, HC_TV_MAX_CANDIDATES = 12 };
enum { HC_TV_CUTIE_FIRST = 2, HC_TV_CUTIE_COUNT = 4, HC_TV_CUTIE_ROWS = 9 };
enum { HC_TV_MIN_WIDTH = 18, HC_TV_MAX_WIDTH = 64, HC_TV_MIN_HEIGHT = 4, HC_TV_MAX_HEIGHT = 64 };
enum { HC_TV_SETTINGS, HC_TV_CAF, HC_TV_AUXILIARY_1, HC_TV_AUXILIARY_2 };
enum { HC_TV_CLOSED, HC_TV_VISIBLE, HC_TV_FADING };
enum { HC_TV_IDLE_MS = 10000, HC_TV_FADE_MS = 300 };
enum {
    HC_TV_BELOW_GAP, HC_TV_BELOW_JOINED, HC_TV_ABOVE_GAP, HC_TV_ABOVE_JOINED,
    HC_TV_OPPOSITE_ALIGNED, HC_TV_OPPOSITE_MIDDLE, HC_TV_OPPOSITE_TOP,
    HC_TV_OPPOSITE_BOTTOM, HC_TV_SAME_TOP, HC_TV_SAME_BOTTOM,
    HC_TV_CENTER, HC_TV_PREVIOUS
};
typedef struct { int x, y, w, h; } HcTvRect;
typedef struct { int collision_rows, collision_cells, reflow_cells; } HcTvCost;
typedef struct {
    HcTvRect rect;
    HcTvCost cost;
    int rank, candidates, valid, joined;
} HcTvPlacement;
/* Return zero to veto a candidate, e.g. because it intersects an already
 * placed TV carried in ctx. Called at most HC_TV_MAX_CANDIDATES times. */
typedef int (*HcTvOccupancy)(HcTvRect rect, HcTvCost *cost, void *ctx);
typedef struct {
    int open, phase, hovered, placed;
    float alpha;
    HcTvRect rect;
    uint32_t idle_ms, fade_ms, last_at;
} HcTvState;
enum { HC_TV_MOTION_IDLE, HC_TV_MOTION_OUT, HC_TV_MOTION_IN };
enum { HC_TV_MOTION_NONE, HC_TV_MOTION_SEARCH, HC_TV_MOTION_COMMIT };
enum { HC_TV_STABLE_MS = 450, HC_TV_MOVE_FADE_MS = 300, HC_TV_DWELL_MS = 2000 };
typedef struct {
    int initialized, phase, searched, cooling;
    uint64_t signature, context;
    uint32_t stable_at, phase_at, cooled_at;
    float alpha;
    HcTvRect target;
} HcTvMotion;

static inline void hc_tv_motion_reset(HcTvMotion *motion) {
    memset(motion, 0, sizeof(*motion)); motion->alpha = 1;
}
/* Observe stable content without invoking a placement callback. One search
 * per unchanged signature and a dwell after cancelled fades prevent pulsing
 * when output arrives just after the stable-content deadline. */
static inline int hc_tv_motion_observe(HcTvMotion *m, uint32_t now, uint64_t signature,
                                        uint64_t context, int blocked) {
    uint32_t elapsed;
    if (!m->initialized) {
        hc_tv_motion_reset(m); m->initialized = 1;
        m->signature = signature; m->context = context; m->stable_at = now;
    }
    if (m->signature != signature || m->context != context || blocked) {
        if (m->phase != HC_TV_MOTION_IDLE) { m->cooling = 1; m->cooled_at = now; }
        m->phase = HC_TV_MOTION_IDLE; m->alpha = 1; m->searched = 0;
        m->signature = signature; m->context = context; m->stable_at = now;
    }
    if (blocked) return HC_TV_MOTION_NONE;
    elapsed = now-m->phase_at;
    if (m->phase == HC_TV_MOTION_OUT) {
        if (elapsed >= HC_TV_MOVE_FADE_MS) {
            m->phase = HC_TV_MOTION_IN; m->phase_at = now; m->alpha = 0;
            return HC_TV_MOTION_COMMIT;
        }
        m->alpha = 1-(float)elapsed/HC_TV_MOVE_FADE_MS;
    } else if (m->phase == HC_TV_MOTION_IN) {
        if (elapsed >= HC_TV_MOVE_FADE_MS) {
            m->phase = HC_TV_MOTION_IDLE; m->alpha = 1;
            m->cooling = 1; m->cooled_at = now;
        } else m->alpha = (float)elapsed/HC_TV_MOVE_FADE_MS;
    } else if (!m->searched && (uint32_t)(now-m->stable_at) >= HC_TV_STABLE_MS &&
               (!m->cooling || (uint32_t)(now-m->cooled_at) >= HC_TV_DWELL_MS)) {
        m->searched = 1;
        return HC_TV_MOTION_SEARCH;
    }
    return HC_TV_MOTION_NONE;
}
static inline void hc_tv_motion_start(HcTvMotion *m, HcTvRect target, uint32_t now) {
    m->target = target; m->phase = HC_TV_MOTION_OUT; m->phase_at = now; m->alpha = 1;
}
static inline unsigned hc_tv_motion_next_ms(const HcTvMotion *m, uint32_t now, int blocked) {
    uint32_t elapsed; unsigned wait = 0;
    if (!m->initialized || blocked) return 0;
    if (m->phase != HC_TV_MOTION_IDLE) return 16;
    if (m->searched) return 0;
    elapsed = now-m->stable_at;
    if (elapsed < HC_TV_STABLE_MS) wait = HC_TV_STABLE_MS-elapsed;
    elapsed = now-m->cooled_at;
    if (m->cooling && elapsed < HC_TV_DWELL_MS && wait < HC_TV_DWELL_MS-elapsed)
        wait = HC_TV_DWELL_MS-elapsed;
    return wait ? wait : 1;
}

static inline int hc_tv_rect_equal(HcTvRect a, HcTvRect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}
static inline int hc_tv_rect_fits(HcTvRect r, int cols, int rows) {
    return r.x >= 0 && r.y >= 0 && r.w > 0 && r.h > 0 &&
           (int64_t)r.x+r.w <= cols && (int64_t)r.y+r.h <= rows;
}
static inline int hc_tv_rect_overlap(HcTvRect a, HcTvRect b) {
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 &&
           (int64_t)a.x < (int64_t)b.x+b.w && (int64_t)b.x < (int64_t)a.x+a.w &&
           (int64_t)a.y < (int64_t)b.y+b.h && (int64_t)b.y < (int64_t)a.y+a.h;
}
static inline int hc_tv_rect_joined(HcTvRect a, HcTvRect primary) {
    return ((int64_t)a.y == (int64_t)primary.y+primary.h ||
            (int64_t)a.y+a.h == primary.y) &&
           (int64_t)a.x < (int64_t)primary.x+primary.w &&
           (int64_t)primary.x < (int64_t)a.x+a.w;
}
static inline int hc_tv_score_better(const HcTvPlacement *a, const HcTvPlacement *b) {
    if (!b->valid) return a->valid;
    if (!a->valid) return 0;
    if (a->cost.collision_rows != b->cost.collision_rows)
        return a->cost.collision_rows < b->cost.collision_rows;
    if (a->cost.collision_cells != b->cost.collision_cells)
        return a->cost.collision_cells < b->cost.collision_cells;
    if (a->cost.reflow_cells != b->cost.reflow_cells)
        return a->cost.reflow_cells < b->cost.reflow_cells;
    return a->rank < b->rank;
}
static inline int hc_tv_material_improvement(const HcTvPlacement *best, const HcTvPlacement *old) {
    if (!old->valid) return best->valid;
    if (!hc_tv_score_better(best, old) || hc_tv_rect_equal(best->rect, old->rect)) return 0;
    if (old->cost.collision_cells && !best->cost.collision_cells) return 1;
    if (best->cost.collision_rows + 2 <= old->cost.collision_rows &&
        (int64_t)best->cost.collision_cells * 4 <= (int64_t)old->cost.collision_cells * 3) return 1;
    if (best->cost.collision_rows <= old->cost.collision_rows &&
        old->cost.collision_cells-best->cost.collision_cells >= 8 &&
        (int64_t)best->cost.collision_cells * 2 <= old->cost.collision_cells) return 1;
    /* A clear, already placed panel stays put for minor text fluctuations.
     * Large reductions in required reflow can justify a deliberate move. */
    return !best->cost.collision_cells && !old->cost.collision_cells &&
           old->cost.reflow_cells-best->cost.reflow_cells >= 64 &&
           (int64_t)best->cost.reflow_cells * 2 <= old->cost.reflow_cells;
}
static inline int hc_tv_clamp(int value, int maximum) {
    return value < 0 ? 0 : value > maximum ? maximum : value;
}
static inline int hc_tv_anchor(int64_t value) {
    return value < 0 || value > INT_MAX ? -1 : (int)value;
}
/* Keep the user's request intact across temporary viewport shrinkage. */
static inline HcTvRect hc_tv_requested_rect(HcTvRect requested,int cols,int rows) {
    if(cols<1 || rows<1)return (HcTvRect){0};
    int w=requested.w?requested.w:28,h=requested.h?requested.h:HC_TV_CUTIE_ROWS;
    w=w<HC_TV_MIN_WIDTH?HC_TV_MIN_WIDTH:w>HC_TV_MAX_WIDTH?HC_TV_MAX_WIDTH:w;
    h=h<HC_TV_MIN_HEIGHT?HC_TV_MIN_HEIGHT:h>HC_TV_MAX_HEIGHT?HC_TV_MAX_HEIGHT:h;
    if(w>cols)w=cols;if(h>rows)h=rows;
    return (HcTvRect){hc_tv_clamp(requested.x,cols-w),hc_tv_clamp(requested.y,rows-h),w,h};
}
/* Jump past rectangles, never scan screen rows. Each pass advances beyond at
 * least one obstacle, so even interior manual panels take at most seven passes. */
static inline HcTvRect hc_tv_lane(int cols,int rows,int x,int y,int width,int height,
                                 const HcTvRect *occupied,int count,int gap) {
    HcTvRect r={x,y,width,height};
    if(count<0 || count>HC_TV_MAX_SLOTS || (count && !occupied))return (HcTvRect){0};
    for(int pass=0;pass<=count;pass++) {
        if(!hc_tv_rect_fits(r,cols,rows))return (HcTvRect){0};
        int next=r.y;
        for(int i=0;i<count;i++)if(hc_tv_rect_overlap(r,occupied[i])) {
            int bottom=hc_tv_anchor((int64_t)occupied[i].y+occupied[i].h+gap);
            if(bottom<0)return (HcTvRect){0};
            if(bottom>next)next=bottom;
        }
        if(next==r.y)return r;
        r.y=next;
    }
    return (HcTvRect){0};
}
static inline int hc_tv_stack_band(const HcTvPlacement *p,HcTvRect primary) {
    /* A short amount of terminal text should not scatter a vertical stack.
       Dense content still gets the normal least-collision placement. */
    int modest=p->cost.collision_cells <= ((int64_t)p->rect.w*p->rect.h)/3;
    if(modest && p->rank<=1)return 0;
    if(modest && p->rect.y>=primary.y+primary.h)return 1;
    return 2;
}
static inline int hc_tv_stack_better(const HcTvPlacement *a,const HcTvPlacement *b,HcTvRect primary) {
    if(!b->valid)return a->valid;if(!a->valid)return 0;
    int aa=hc_tv_stack_band(a,primary),bb=hc_tv_stack_band(b,primary);
    if(aa!=bb)return aa<bb;
    if(!aa && a->rank!=b->rank)return a->rank<b->rank;
    return hc_tv_score_better(a,b);
}
/* Ordered Cuties favor the next slot below their predecessor. Opposite and
 * restarted lanes are a bounded fallback, retaining top-to-bottom order. */
static inline HcTvPlacement hc_tv_place_stacked(int cols,int rows,HcTvRect primary,HcTvRect anchor,
        int width,int height,const HcTvRect *occupied,int count,const HcTvPlacement *previous,
        HcTvOccupancy occupancy,void *ctx) {
    HcTvPlacement best={0},old={0};HcTvRect choices[HC_TV_MAX_CANDIDATES];int calls=0;
    if(width<1 || height<1 || width>cols || height>rows || !hc_tv_rect_fits(primary,cols,rows) ||
       count<0 || count>HC_TV_MAX_SLOTS || (count&&!occupied))return best;
    int edge=(int64_t)anchor.x*2+anchor.w>=cols?cols-width:0,other=edge?0:cols-width;
    int below=hc_tv_anchor((int64_t)primary.y+primary.h);
    int next=hc_tv_anchor((int64_t)anchor.y+anchor.h);
    choices[0]=hc_tv_lane(cols,rows,edge,hc_tv_anchor((int64_t)next+2),width,height,occupied,count,2);
    choices[1]=hc_tv_lane(cols,rows,edge,next,width,height,occupied,count,0);
    choices[2]=hc_tv_lane(cols,rows,other,hc_tv_anchor((int64_t)below+2),width,height,occupied,count,2);
    choices[3]=hc_tv_lane(cols,rows,other,below,width,height,occupied,count,0);
    choices[4]=hc_tv_lane(cols,rows,edge,hc_tv_anchor((int64_t)below+2),width,height,occupied,count,2);
    choices[5]=hc_tv_lane(cols,rows,edge,below,width,height,occupied,count,0);
    choices[6]=hc_tv_lane(cols,rows,other,0,width,height,occupied,count,2);
    choices[7]=hc_tv_lane(cols,rows,other,0,width,height,occupied,count,0);
    choices[8]=hc_tv_lane(cols,rows,edge,0,width,height,occupied,count,2);
    choices[9]=hc_tv_lane(cols,rows,edge,0,width,height,occupied,count,0);
    choices[10]=(HcTvRect){(cols-width)/2,hc_tv_clamp(below,rows-height),width,height};
    choices[11]=previous&&previous->valid?previous->rect:(HcTvRect){0};
    for(int i=0;i<HC_TV_MAX_CANDIDATES;i++) {
        HcTvPlacement p={0};int duplicate=0;
        if(!hc_tv_rect_fits(choices[i],cols,rows) || choices[i].w!=width || choices[i].h!=height ||
           hc_tv_rect_overlap(choices[i],primary))continue;
        for(int j=0;j<i;j++)if(hc_tv_rect_equal(choices[j],choices[i]))duplicate=1;
        if(duplicate)continue;
        p.rect=choices[i];p.rank=i;calls++;
        if(occupancy&&!occupancy(p.rect,&p.cost,ctx))continue;
        if(p.cost.collision_rows<0 || p.cost.collision_cells<0 || p.cost.reflow_cells<0)continue;
        if(p.cost.collision_rows&&!p.cost.collision_cells)p.cost.collision_cells=p.cost.collision_rows;
        if(p.cost.collision_cells&&!p.cost.collision_rows)p.cost.collision_rows=1;
        p.valid=1;p.joined=hc_tv_rect_joined(p.rect,anchor);
        if(previous&&previous->valid&&hc_tv_rect_equal(p.rect,previous->rect))old=p;
        if(hc_tv_stack_better(&p,&best,primary))best=p;
    }
    if(old.valid && hc_tv_stack_band(&best,primary)>=hc_tv_stack_band(&old,primary) &&
       !hc_tv_material_improvement(&best,&old))best=old;
    best.candidates=calls;return best;
}
static inline HcTvPlacement hc_tv_place(int cols, int rows, HcTvRect primary,
                                         int width, int height, const HcTvPlacement *previous,
                                         HcTvOccupancy occupancy, void *ctx) {
    HcTvPlacement best = {0}, old = {0};
    HcTvRect choices[HC_TV_MAX_CANDIDATES];
    int count = 0, calls = 0, aligned, opposite, middle;
    if (cols < 1 || rows < 1 || width < 1 || height < 1 || width > cols || height > rows ||
        !hc_tv_rect_fits(primary, cols, rows)) return best;
    aligned = (int64_t)primary.x+primary.w == cols ? cols-width : hc_tv_clamp(primary.x, cols-width);
    opposite = (int64_t)primary.x*2+primary.w < cols ? cols-width : 0;
    middle = (rows-height)/2;
    /* At most eleven new anchors plus the previous rectangle. An anchor
     * outside the viewport is rejected, never clamped into primary tabs. */
    choices[count++] = (HcTvRect){aligned, hc_tv_anchor((int64_t)primary.y+primary.h+2), width, height};
    choices[count++] = (HcTvRect){aligned, primary.y+primary.h, width, height};
    choices[count++] = (HcTvRect){aligned, hc_tv_anchor((int64_t)primary.y-height-2), width, height};
    choices[count++] = (HcTvRect){aligned, hc_tv_anchor((int64_t)primary.y-height), width, height};
    choices[count++] = (HcTvRect){opposite, hc_tv_clamp(primary.y, rows-height), width, height};
    choices[count++] = (HcTvRect){opposite, middle, width, height};
    choices[count++] = (HcTvRect){opposite, 0, width, height};
    choices[count++] = (HcTvRect){opposite, rows-height, width, height};
    choices[count++] = (HcTvRect){aligned, 0, width, height};
    choices[count++] = (HcTvRect){aligned, rows-height, width, height};
    choices[count++] = (HcTvRect){(cols-width)/2, middle, width, height};
    choices[count++] = previous && previous->valid ? previous->rect : choices[0];
    for (int i = 0; i < count; i++) {
        HcTvPlacement candidate = {0};
        int duplicate = 0;
        if (!hc_tv_rect_fits(choices[i], cols, rows) || choices[i].w != width || choices[i].h != height ||
            hc_tv_rect_overlap(choices[i], primary)) continue;
        for (int j = 0; j < i; j++) if (hc_tv_rect_equal(choices[j], choices[i])) duplicate = 1;
        if (duplicate) continue;
        candidate.rect = choices[i]; candidate.rank = i;
        calls++;
        if (occupancy && !occupancy(candidate.rect, &candidate.cost, ctx)) continue;
        if (candidate.cost.collision_rows < 0 || candidate.cost.collision_cells < 0 ||
            candidate.cost.reflow_cells < 0) continue;
        /* Rows with collisions necessarily contain occupied cells. Normalize
         * inconsistent callbacks so a malformed 'clear' cost cannot win. */
        if (candidate.cost.collision_rows && !candidate.cost.collision_cells)
            candidate.cost.collision_cells = candidate.cost.collision_rows;
        if (candidate.cost.collision_cells && !candidate.cost.collision_rows)
            candidate.cost.collision_rows = 1;
        candidate.valid = 1; candidate.joined = hc_tv_rect_joined(candidate.rect, primary);
        if (previous && previous->valid && hc_tv_rect_equal(candidate.rect, previous->rect)) old = candidate;
        if (hc_tv_score_better(&candidate, &best)) best = candidate;
    }
    if (old.valid && !hc_tv_material_improvement(&best, &old)) best = old;
    best.candidates = calls;
    return best;
}

static inline void hc_tv_open(HcTvState *tv, uint32_t now) {
    tv->open = 1; tv->phase = HC_TV_VISIBLE; tv->alpha = 1;
    tv->idle_ms = tv->fade_ms = 0; tv->last_at = now;
}
static inline void hc_tv_close(HcTvState *tv) {
    tv->open = 0; tv->phase = HC_TV_CLOSED; tv->alpha = 0;
    tv->idle_ms = tv->fade_ms = 0; tv->hovered = 0;
}
static inline void hc_tv_interact(HcTvState *tv, uint32_t now) {
    if (!tv->open) return;
    tv->phase = HC_TV_VISIBLE; tv->alpha = 1;
    tv->idle_ms = tv->fade_ms = 0; tv->last_at = now;
}
static inline void hc_tv_tick(HcTvState *tv, uint32_t now, int hovered) {
    uint32_t elapsed;
    uint64_t total;
    if (!tv->open) return;
    elapsed = now-tv->last_at; tv->last_at = now;
    if (!tv->hovered) {
        total = (uint64_t)tv->idle_ms+elapsed;
        if (total > HC_TV_IDLE_MS) {
            uint64_t fade = (uint64_t)tv->fade_ms+total-HC_TV_IDLE_MS;
            tv->fade_ms = fade > HC_TV_FADE_MS ? HC_TV_FADE_MS : (uint32_t)fade;
            tv->idle_ms = HC_TV_IDLE_MS;
        } else tv->idle_ms = (uint32_t)total;
    }
    tv->hovered = !!hovered;
    if (hovered) {
        /* A hovered control remains fully legible. Hover pauses its elapsed
         * idle time, while actual interaction starts a fresh ten seconds. */
        tv->phase = HC_TV_VISIBLE; tv->alpha = 1; tv->fade_ms = 0;
    } else if (tv->idle_ms < HC_TV_IDLE_MS) {
        tv->phase = HC_TV_VISIBLE; tv->alpha = 1;
    } else if (tv->fade_ms < HC_TV_FADE_MS) {
        tv->phase = HC_TV_FADING; tv->alpha = 1-(float)tv->fade_ms/HC_TV_FADE_MS;
    } else hc_tv_close(tv);
}
static inline unsigned hc_tv_seconds_left(const HcTvState *tv, uint32_t now) {
    uint64_t idle;
    if (!tv->open) return 0;
    idle = tv->idle_ms;
    if (!tv->hovered) idle += (uint32_t)(now-tv->last_at);
    return idle >= HC_TV_IDLE_MS ? 0 : (unsigned)((HC_TV_IDLE_MS-idle+999)/1000);
}
static inline unsigned hc_tv_next_tick_ms(const HcTvState *tv, uint32_t now) {
    uint64_t idle;
    if (!tv->open || tv->hovered) return 0;
    idle = (uint64_t)tv->idle_ms+(uint32_t)(now-tv->last_at);
    if (idle >= HC_TV_IDLE_MS || tv->phase == HC_TV_FADING) return 16;
    return (unsigned)(1000-idle%1000);
}
#endif
