/* Row-first grid geometry. Integer cell boundaries; dividers paint one device
 * pixel, while their mouse targets are deliberately wider. No platform state. */
#ifndef HARDCORE_GRID_H
#define HARDCORE_GRID_H
#include <string.h>
enum { HC_GRID_MAX=64, HC_GRID_AXES=64, HC_GRID_RATIO=65536 };
typedef struct { int x,y,w,h; } HcGridRect;
typedef struct {
    int count,cols,rows,extra,width,height;
    int x[HC_GRID_AXES+1],y[HC_GRID_AXES+1];
    int xr[HC_GRID_AXES+1],yr[HC_GRID_AXES+1];
    /* The last dragged layout of each axis, back when its count returns
       (a window narrowed for a moment, a tab hidden and shown again) */
    int xs[HC_GRID_AXES+1],ys[HC_GRID_AXES+1],xs_n,ys_n;
    HcGridRect rect[HC_GRID_MAX];
} HcGrid;
static inline int hc_grid_min(int a,int b){return a<b?a:b;}
static inline int hc_grid_max(int a,int b){return a>b?a:b;}
static inline void hc_grid_axis(int *out,int *ratios,int count,int length,int minimum) {
    out[0]=0;out[count]=length;
    minimum=hc_grid_min(minimum,length/count);
    for(int i=1;i<count;i++) {
        int value=(int)((long long)ratios[i]*length/HC_GRID_RATIO);
        int lo=out[i-1]+minimum,hi=length-(count-i)*minimum;
        out[i]=hc_grid_min(hc_grid_max(value,lo),hi);
    }
}
static inline void hc_grid_ratios(int *ratios,int count,const int *saved,int saved_n) {
    if(count==saved_n)memcpy(ratios,saved,(count+1)*sizeof(int));
    else for(int i=0;i<=count;i++)ratios[i]=(i*HC_GRID_RATIO+count-1)/count;
}
static inline void hc_grid_build(HcGrid *g,int count,int width,int height) {
    count=hc_grid_min(HC_GRID_MAX,hc_grid_max(0,count));
    width=hc_grid_max(1,width);height=hc_grid_max(1,height);
    int cols=1,rows=1;
    if(count<=3)cols=hc_grid_max(1,count);
    else {while(cols*cols<count)cols++;}
    /* Prefer readable panes when a tall, narrow window cannot fit the square. */
    cols=hc_grid_min(cols,hc_grid_max(1,width/10));
    /* Too short for that many rows: narrower panes, while each keeps a few
       columns, before any is left a title row with no line of text */
    while(cols<count && (count+cols-1)/cols>height/2 && width/(cols+1)>=6)cols++;
    rows=hc_grid_max(1,(count+cols-1)/cols);
    if(g->cols!=cols)hc_grid_ratios(g->xr,cols,g->xs,g->xs_n);
    if(g->rows!=rows)hc_grid_ratios(g->yr,rows,g->ys,g->ys_n);
    g->cols=cols;g->rows=rows;g->count=count;g->width=width;g->height=height;
    g->extra=count?cols*rows-count:0;
    hc_grid_axis(g->x,g->xr,cols,width,10);
    hc_grid_axis(g->y,g->yr,rows,height,4);
    memset(g->rect,0,sizeof(g->rect));
    int cell=0;
    for(int i=0;i<count;i++) {
        int span=i==0?g->extra+1:1,c=cell%cols,r=cell/cols;
        g->rect[i]=(HcGridRect){g->x[c],g->y[r],g->x[c+span]-g->x[c],g->y[r+1]-g->y[r]};
        cell+=span;
    }
}
static inline void hc_grid_move(HcGrid *g,int vertical,int boundary,int position) {
    int count=vertical?g->cols:g->rows,length=vertical?g->width:g->height;
    int *axis=vertical?g->x:g->y,*ratios=vertical?g->xr:g->yr;
    if(boundary<=0 || boundary>=count || length<=0)return;
    int minimum=hc_grid_min(vertical?10:4,length/count);
    position=hc_grid_min(hc_grid_max(position,axis[boundary-1]+minimum),axis[boundary+1]-minimum);
    ratios[boundary]=(int)(((long long)position*HC_GRID_RATIO+length-1)/length);
    if(vertical){memcpy(g->xs,g->xr,sizeof(g->xs));g->xs_n=count;}
    else {memcpy(g->ys,g->yr,sizeof(g->ys));g->ys_n=count;}
    hc_grid_build(g,g->count,g->width,g->height);
}
#endif
