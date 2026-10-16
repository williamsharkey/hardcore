/* Persistent miniature views. Authoritative terminals and browser grids remain
 * untouched; only explicitly focused Cuties receive keyboard input. */
#ifndef _WIN32
#include "hardcore_cutie_project.h"
typedef struct {
    HcCutieProjection view;
    Cell *input;
    int capacity, tab_id, source_cols, source_rows, first_line, hcount, alt, generation;
    long long hpushed;
    uint64_t hash, terminal_generation;
    int valid;
} CutieCache;
static CutieCache g_cuties[HC_TV_COUNT];
static int cutie_slot_for(int id) {
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++)
        if(g_tvs[i].state.open && g_tvs[i].tab_id==id)return i;
    return -1;
}
static void cutie_input_follow(void) {
    int slot=cutie_slot_for(g_cutie_focus_id);
    if(slot>=0 && g_tvs[slot].scroll){g_tvs[slot].scroll=0;plat_invalidate();}
}
static int cutie_focus_valid(void) {
    int slot=cutie_slot_for(g_cutie_focus_id);Tab *tb=tab_by_id(g_cutie_focus_id);
    return g_focused && slot>=0 && g_tvs[slot].state.placed && tb && tb->kind!=TK_BROWSER;
}
static int cutie_at(int mx,int my) {
    for(int i=HC_TV_CUTIE_FIRST;i<HC_TV_COUNT;i++)if(chrome_tv_contains(i,mx,my))return i;
    return -1;
}
static uint64_t cutie_hash(uint64_t hash,const void *data,size_t n) {
    const unsigned char *p=data;
    while(n--){hash^=*p++;hash*=UINT64_C(1099511628211);}
    return hash;
}
static int cutie_prepare(int slot) {
    ChromeTv *tv=g_tvs+slot;CutieCache *cache=g_cuties+slot;
    Tab *tb=tab_by_id(tv->tab_id);Term *t=tb?&tb->t:NULL;
    int width=tv->state.rect.w-2,height=tv->state.rect.h-2;
    cache->valid=0;
    if(!t || width<2 || height<1)return 0;
    if(tb->kind==TK_BROWSER) {
        int generation=mac_browser_cutie_generation(tb);
        int params[]={tb->id,width,height,tv->scroll,generation};
        uint64_t hash=cutie_hash(1,params,sizeof(params));
        if(cache->hash!=hash || !cache->view.cells) {
            HcCutieProjection next={0};int count=width*height;
            next.cells=calloc(count,sizeof(Cell));next.source=malloc(count*sizeof(int));
            if(!next.cells || !next.source){hc_cutie_project_free(&next);return 0;}
            next.cols=width;next.rows=height;next.cursor=-1;
            next.total_rows=mac_browser_cutie_project(tb,next.cells,width,height,tv->scroll);
            tv->scroll=chrome_min(tv->scroll,chrome_max(0,next.total_rows-height));
            for(int n=0;n<count;n++)next.source[n]=-1;
            next.first_row=tv->scroll;next.scroll_rows=tv->scroll;
            hc_cutie_project_free(&cache->view);cache->view=next;cache->hash=hash;
        }
        cache->generation=generation;cache->tab_id=tb->id;cache->valid=1;return 1;
    }
    if(t->cols<=0 || t->cols>HC_CUTIE_MAX_SOURCE_CELLS || t->rows<1)return 0;
    int rows=chrome_min(HC_CUTIE_MAX_SOURCE_ROWS,HC_CUTIE_MAX_SOURCE_CELLS/t->cols);
    rows=chrome_min(rows,t->rows+(t->alt?0:t->hcount));
    int count=rows*t->cols,first=t->hcount+t->rows-rows;
    if(cache->capacity<count) {
        Cell *p=realloc(cache->input,count*sizeof(Cell));if(!p)return 0;
        cache->input=p;cache->capacity=count;
    }
    memset(cache->input,0,count*sizeof(Cell));
    for(int y=0;y<rows;y++) {
        const Cell *cells;int n,wrapped;term_line(t,first+y,&cells,&n,&wrapped);
        n=chrome_min(n,t->cols);if(n>0)memcpy(cache->input+y*t->cols,cells,n*sizeof(Cell));
        if(n && cache->input[y*t->cols+n-1].wflag==W_WIDE)cache->input[y*t->cols+n-1]=(Cell){0};
    }
    int cursor=t->m_cursor_vis?(t->hcount+t->cy-first)*t->cols+chrome_min(t->cx,t->cols-1):-1;
    if(cursor<0 || cursor>=count)cursor=-1;
    int params[]={tb->id,t->cols,rows,width,height,cursor,tv->scroll,first,t->alt};
    uint64_t hash=cutie_hash(UINT64_C(1469598103934665603),params,sizeof(params));
    hash=cutie_hash(hash,cache->input,count*sizeof(Cell));
    if(cache->hash!=hash || !cache->view.cells) {
        if(!hc_cutie_project_build(&cache->view,cache->input,t->cols,rows,cursor,width,height,tv->scroll))return 0;
        cache->hash=hash;tv->scroll=cache->view.scroll_rows;
    }
    cache->tab_id=tb->id;cache->source_cols=t->cols;cache->source_rows=rows;
    cache->first_line=first;cache->hcount=t->hcount;cache->hpushed=t->hpushed;cache->alt=t->alt;
    cache->terminal_generation=tb->output_generation;
    cache->valid=1;return 1;
}
/* The last painted mapping must still describe the same source frame. A click
 * can give focus after output races it, but cannot click an unrelated control. */
static int cutie_source_at(int slot,int mx,int my,int *col,int *row,int *node) {
    CutieCache *c=g_cuties+slot;ChromeTv *tv=g_tvs+slot;Tab *tb=tab_by_id(tv->tab_id);
    HcTvRect r=tv->state.rect;int x=mx/CW-r.x-1,y=my/CH-r.y-1;
    if(!c->valid || !tb || c->tab_id!=tb->id || x<0 || y<0 || x>=c->view.cols || y>=c->view.rows)return 0;
    int at=y*c->view.cols+x;
    if(tb->kind==TK_BROWSER){if(c->generation!=mac_browser_cutie_generation(tb))return 0;*node=c->view.cells[at].link;return *node!=0;}
    Term *t=&tb->t;int source=c->view.source[at];
    if(source<0 || c->terminal_generation!=tb->output_generation || c->source_cols!=t->cols ||
       c->hpushed!=t->hpushed || c->hcount!=t->hcount || c->alt!=t->alt)return 0;
    *col=source%t->cols;*row=c->first_line+source/t->cols-t->hcount;
    if(*row<0 || *row>=t->rows)return 0;
    if(memcmp(c->input+source,t->row[t->alt][*row]+*col,sizeof(Cell)))return 0;
    return 1;
}
#endif
