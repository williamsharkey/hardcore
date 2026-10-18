#include "../hardcore_grid.h"
#include <assert.h>
#include <stdio.h>
static void check(HcGrid *g) {
    long area=0;
    for(int i=0;i<g->count;i++) {
        HcGridRect a=g->rect[i];
        assert(a.x>=0 && a.y>=0 && a.w>=0 && a.h>=0);
        assert(a.x+a.w<=g->width && a.y+a.h<=g->height);
        area+=(long)a.w*a.h;
        for(int j=0;j<i;j++) {
            HcGridRect b=g->rect[j];
            assert(!a.w || !a.h || !b.w || !b.h || a.x>=b.x+b.w || b.x>=a.x+a.w || a.y>=b.y+b.h || b.y>=a.y+a.h);
        }
    }
    assert(!g->count || area==(long)g->width*g->height);
}
int main(void) {
    HcGrid g={0};
    for(int n=0;n<=64;n++)for(int w=1;w<=250;w+=7)for(int h=1;h<=90;h+=7) {
        hc_grid_build(&g,n,w,h);check(&g);
        for(int vertical=0;vertical<2;vertical++) {
            int count=vertical?g.cols:g.rows;
            for(int b=1;b<count;b++) {
                hc_grid_move(&g,vertical,b,-100);check(&g);
                hc_grid_move(&g,vertical,b,1000);check(&g);
            }
        }
    }
    memset(&g,0,sizeof(g));hc_grid_build(&g,9,120,60);
    assert(g.cols==3 && g.rows==3 && !g.extra);
    for(int i=0;i<9;i++)assert(g.rect[i].x==i%3*40 && g.rect[i].y==i/3*20);
    hc_grid_move(&g,1,1,50);assert(g.x[1]==50);
    hc_grid_build(&g,9,240,120);assert(g.x[1]==100);
    hc_grid_build(&g,8,120,60);assert(g.extra==1 && g.rect[0].w==80);
    /* A dragged divider survives a window that narrows for a moment (wezterm#5011),
       and the rows dragged with it */
    memset(&g,0,sizeof(g));hc_grid_build(&g,9,120,60);
    hc_grid_move(&g,1,1,50);hc_grid_move(&g,0,1,30);
    hc_grid_build(&g,9,25,60);assert(g.cols==2 && g.rows==5);
    hc_grid_build(&g,9,120,60);assert(g.x[1]==50 && g.y[1]==30);
    /* No pane is left only a title row while narrower panes would fit (wezterm#4878) */
    for(int n=1;n<=64;n++)for(int w=1;w<=250;w+=3)for(int h=1;h<=90;h+=3) {
        memset(&g,0,sizeof(g));hc_grid_build(&g,n,w,h);check(&g);
        if(n<=(w/6)*(h/2))for(int i=0;i<n;i++)assert(g.rect[i].h>=2 && g.rect[i].w>=6);
    }
    memset(&g,0,sizeof(g));hc_grid_build(&g,20,52,23);
    for(int i=0;i<20;i++)assert(g.rect[i].h>=2);
    puts("Grid geometry: coverage, bounds, no overlap, row order, expansion, resizing, kept layouts and readable panes passed");
}
