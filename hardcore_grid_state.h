/* Workspace state uses stable tab IDs. Normal floating positions are retained. */
#include "hardcore_grid.h"
static int g_grid_on,g_grid_expand_id,g_grid_hidden[MAX_TABS],g_grid_nhidden;
static int g_grid_ids[MAX_TABS],g_grid_count,g_grid_left,g_grid_top=1,g_grid_rail=48;
static HcGrid g_grid;
/* Split view: the tab you were in plus one opened beside it; every other tab
   is hidden (still running). The tab ids, 0 = not split. */
static int g_split_main,g_split_other;
static int grid_split_tall(int cols,int rows);
static int grid_utility_columns(void);
static int g_grid_drag_axis,g_grid_drag_boundary;
static int grid_hidden(int id) {
    for(int i=0;i<g_grid_nhidden;i++)if(g_grid_hidden[i]==id)return 1;
    return 0;
}
static void grid_set_hidden(int id,int hidden) {
    for(int i=0;i<g_grid_nhidden;i++)if(g_grid_hidden[i]==id) {
        if(!hidden){memmove(g_grid_hidden+i,g_grid_hidden+i+1,(g_grid_nhidden-i-1)*sizeof(int));g_grid_nhidden--;}
        return;
    }
    if(hidden && g_grid_nhidden<MAX_TABS)g_grid_hidden[g_grid_nhidden++]=id;
}
static int grid_index(int id) {
    if(!g_grid_on)return -1;
    for(int i=0;i<g_grid_count;i++)if(g_grid_ids[i]==id)return i;
    return -1;
}
static HcGridRect grid_rect(int id) {
    int i=grid_index(id);HcGridRect r={0};
    if(i>=0){r=g_grid.rect[i];r.x+=g_grid_left;r.y+=g_grid_top;}
    return r;
}
static void grid_measure(int cols,int rows) {
    if(!g_grid_on)return;
    int ids[MAX_TABS],count=0;
    for(int i=0;i<g_ntabs;i++)if(!grid_hidden(g_tabs[i]->id))ids[count++]=g_tabs[i]->id;
    /* Prune closed IDs; they must not crowd out future hide choices. */
    for(int i=g_grid_nhidden-1;i>=0;i--)if(!tab_by_id(g_grid_hidden[i]))grid_set_hidden(g_grid_hidden[i],0);
    /* The rail is only as wide as what it holds: panes get every other column. */
    g_grid_rail=grid_utility_columns();
    if(g_grid_rail>cols-10)g_grid_rail=hc_grid_max(1,cols/3);
    g_grid_left=g_tab_underlay==2?g_grid_rail:0;
    /* a split in a window taller than wide stacks its panes */
    g_grid.stack=g_split_main && count<=3 && grid_split_tall(cols-g_grid_rail,rows-g_grid_top);
    hc_grid_build(&g_grid,count,cols-g_grid_rail,hc_grid_max(1,rows-g_grid_top));
    g_grid_count=count;
    int expanded=-1;
    if(g_grid.extra)for(int i=0;i<count;i++)if(ids[i]==g_grid_expand_id)expanded=i;
    int next=0;
    if(expanded>=0)g_grid_ids[next++]=ids[expanded];
    for(int i=0;i<count;i++)if(i!=expanded)g_grid_ids[next++]=ids[i];
}
static void grid_tab_size(Tab *tb,int *cols,int *rows) {
    if(g_grid_on) {
        HcGridRect r=grid_rect(tb->id);
        if(r.w>0 && r.h>1){*cols=r.w;*rows=r.h-1;}
        else {*cols=tb->t.cols;*rows=tb->t.rows;}
    }
}
static void grid_toggle(void);
static void grid_hide_tab(int index);
static void grid_expand_next(void);

