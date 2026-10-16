/* Semantic page blocks laid out on the terminal grid. Images are small cell
   rectangles, with text flowing beside them. No platform dependencies. */
#ifndef HARDCORE_BROWSER_GRID_H
#define HARDCORE_BROWSER_GRID_H
#include "hardcore_browser_image_geometry.h"
#define BROWSER_IMAGE 8
typedef struct {
    Cell *cells;
    int count, indent, gap, literal;
    uint16_t *nodes;              /* DOM inspector identities, separate from links */
    int rule, table, header, columns;
    int *ends;                   /* cumulative cell offsets, one per table column */
    unsigned numeric;            /* right-aligned table columns */
    Cell decoration;             /* quiet rules use the document's dim color */
    int hanging;                 /* continuation indent; article prose uses zero */
} BrowserBlock;
enum { BROWSER_TABLE_COLUMNS=16, BROWSER_DOCUMENT_ROWS=20000 };
static void browser_grid_put(Cell *output,uint16_t *nodes,int cols,int rows,int scroll,
    int y,int x,Cell cell,uint16_t node) {
    y-=scroll;
    if(output && y>=0 && y<rows && x>=0 && x<cols) {
        int at=y*cols+x; output[at]=cell; if(nodes)nodes[at]=node;
    }
}
static void browser_grid_rule(Cell *output,uint16_t *nodes,int cols,int rows,int scroll,
    int y,int left,int right,Cell cell) {
    cell.ch=0x2500;cell.wflag=0;cell.link=0;
    for(int x=left;x<right;x++)browser_grid_put(output,nodes,cols,rows,scroll,y,x,cell,0);
}
static int browser_grid_table_valid(const BrowserBlock *b) {
    if(b->table<=0 || b->columns<1 || b->columns>BROWSER_TABLE_COLUMNS || !b->ends ||
       b->count<0 || b->count>100000 || (b->count && !b->cells))return 0;
    int previous=0;
    for(int c=0;c<b->columns;c++) {
        int end=b->ends[c];
        if(end<previous || end>b->count)return 0;
        /* A DOM cell boundary must never cut a double-width glyph. */
        if(end<b->count && (b->cells[end].wflag&2))return 0;
        previous=end;
    }
    return previous==b->count;
}
typedef struct {int first,last,next;} BrowserGridSlice;
static BrowserGridSlice browser_grid_slice(const Cell *cells,int start,int end,int width) {
    BrowserGridSlice s={start,start,start};
    if(width<2 || start>=end)return s;
    while(start<end && cells[start].ch==' ' && !(cells[start].wflag&2))start++;
    int cut=start,boundary=-1;
    while(cut<end && cut-start<width && cells[cut].ch!='\n') {
        if(cells[cut].ch==' ' && !(cells[cut].wflag&2))boundary=cut;
        if(cells[cut].ch=='/' && cut>start && cut+1<end && cells[cut+1].ch!='/')boundary=cut+1;
        cut++;
    }
    if(cut<end && cells[cut].ch!='\n' && cut-start==width && boundary>start)cut=boundary;
    if(cut<end && (cells[cut].wflag&2))cut--;
    if(cut==start && start<end && cells[start].ch!='\n')cut=start+((cells[start].wflag&1)?2:1);
    if(cut>end)cut=end;
    s.first=start;s.last=cut;s.next=cut;
    while(s.last>s.first && cells[s.last-1].ch==' ' && !(cells[s.last-1].wflag&2))s.last--;
    if(s.next<end && cells[s.next].ch=='\n')s.next++;
    else while(s.next<end && cells[s.next].ch==' ' && !(cells[s.next].wflag&2))s.next++;
    return s;
}
static void browser_grid_table_slice(const BrowserBlock *b,BrowserGridSlice s,Cell *output,uint16_t *nodes,
    int cols,int rows,int scroll,int y,int x) {
    for(int i=s.first;i<s.last;i++) {
        Cell cell=b->cells[i];
        /* Images belong in their own floated block. Keep an actionable marker
           if a third-party adapter nevertheless puts one inside a table. */
        if(cell.wflag&BROWSER_IMAGE){cell.ch=0x25a7;cell.wflag=0;cell.attr=0;cell.fg=b->decoration.fg;cell.bg=b->decoration.bg;}
        browser_grid_put(output,nodes,cols,rows,scroll,y,x+i-s.first,cell,b->nodes?b->nodes[i]:0);
    }
}
static int browser_grid_table_lines(const BrowserBlock *b,int col,int width) {
    int pos=col?b->ends[col-1]:0,end=b->ends[col],lines=0;
    do {BrowserGridSlice s=browser_grid_slice(b->cells,pos,end,width);pos=s.next;lines++;}
    while(pos<end && lines<BROWSER_DOCUMENT_ROWS);
    return lines;
}
static int browser_grid_table_widths(const BrowserBlock *blocks,int first,int end,int available,int *widths) {
    int n=blocks[first].columns,floors[BROWSER_TABLE_COLUMNS],total=3*(n-1);
    if(available>112)available=112;
    for(int c=0;c<n;c++) {
        int ideal=2;
        for(int b=first;b<end;b++) {
            int from=c?blocks[b].ends[c-1]:0,to=blocks[b].ends[c],line=0;
            for(int i=from;i<to;i++) {
                if(blocks[b].cells[i].ch=='\n'){if(line>ideal)ideal=line;line=0;}
                else if(line<48)line++;
            }
            if(line>ideal)ideal=line;
        }
        if(ideal>48)ideal=48;
        widths[c]=ideal;floors[c]=ideal<(n==2?8:6)?ideal:(n==2?8:6);total+=ideal;
    }
    while(total>available) {
        int largest=-1;
        for(int c=0;c<n;c++)if(widths[c]>floors[c] && (largest<0 || widths[c]>widths[largest]))largest=c;
        if(largest<0)return 0;
        widths[largest]--;total--;
    }
    return 1;
}
static int browser_grid_table_pair(const BrowserBlock *label,int labelcol,const BrowserBlock *value,int valuecol,
    Cell *output,uint16_t *nodes,int cols,int rows,int scroll,int row,int left,int labelwidth,int width) {
    int lp=labelcol?label->ends[labelcol-1]:0,le=label->ends[labelcol];
    int vp=valuecol?value->ends[valuecol-1]:0,ve=value->ends[valuecol];
    do {
        BrowserGridSlice l=browser_grid_slice(label->cells,lp,le,labelwidth);
        BrowserGridSlice v=browser_grid_slice(value->cells,vp,ve,width-labelwidth-2);
        browser_grid_table_slice(label,l,output,nodes,cols,rows,scroll,row,left);
        browser_grid_table_slice(value,v,output,nodes,cols,rows,scroll,row,left+labelwidth+2);
        lp=l.next;vp=v.next;row++;
    }while((lp<le || vp<ve) && row<BROWSER_DOCUMENT_ROWS);
    return row;
}
static int browser_grid_table_render(const BrowserBlock *blocks,int first,int end,Cell *output,uint16_t *nodes,
    int cols,int rows,int scroll,int row,int left,int available) {
    int widths[BROWSER_TABLE_COLUMNS],n=blocks[first].columns;
    if(available<4)return row;
    if(!browser_grid_table_widths(blocks,first,end,available,widths)) {
        const BrowserBlock *labels=NULL;
        for(int b=first;b<end;b++)if(blocks[b].header){labels=blocks+b;break;}
        /* On narrow screens a record becomes a compact label/value list,
           retaining the column headings beside the corresponding values. */
        int labelwidth=available/3;if(labelwidth<2)labelwidth=2;
        for(int b=first;b<end && row<BROWSER_DOCUMENT_ROWS;b++) {
            const BrowserBlock *block=blocks+b;
            if(block==labels && end-first>1 && available>=8)continue;
            for(int c=0;c<n && row<BROWSER_DOCUMENT_ROWS;c++) {
                if(labels && block!=labels && available>=8) {
                    row=browser_grid_table_pair(labels,c,block,c,output,nodes,cols,rows,scroll,row,left,labelwidth,available);
                }else {
                    int pos=c?block->ends[c-1]:0,to=block->ends[c];
                    do {BrowserGridSlice s=browser_grid_slice(block->cells,pos,to,available);
                        browser_grid_table_slice(block,s,output,nodes,cols,rows,scroll,row,left);
                        row++;pos=s.next;
                    }while(pos<to && row<BROWSER_DOCUMENT_ROWS);
                }
            }
            if(b+1<end)row++;
        }
        return row;
    }
    for(int b=first;b<end && row<BROWSER_DOCUMENT_ROWS;b++) {
        const BrowserBlock *block=blocks+b;
        int positions[BROWSER_TABLE_COLUMNS],height=1;
        for(int c=0;c<n;c++) {
            positions[c]=c?block->ends[c-1]:0;
            int h=browser_grid_table_lines(block,c,widths[c]);if(h>height)height=h;
        }
        for(int y=0;y<height && row<BROWSER_DOCUMENT_ROWS;y++,row++) {
            int x=left;
            for(int c=0;c<n;c++) {
                BrowserGridSlice s=browser_grid_slice(block->cells,positions[c],block->ends[c],widths[c]);
                int offset=(block->numeric&(1u<<c))?widths[c]-(s.last-s.first):0;
                browser_grid_table_slice(block,s,output,nodes,cols,rows,scroll,row,x+offset);
                positions[c]=s.next;x+=widths[c];
                if(c+1<n){Cell line=block->decoration;line.ch=0x2502;line.wflag=0;line.link=0;
                    browser_grid_put(output,nodes,cols,rows,scroll,row,x+1,line,0);x+=3;}
            }
        }
        if(block->header && b+1<end && !blocks[b+1].header && row<BROWSER_DOCUMENT_ROWS) {
            int x=left;
            for(int c=0;c<n;c++) {
                browser_grid_rule(output,nodes,cols,rows,scroll,row,x,x+widths[c],block->decoration);x+=widths[c];
                if(c+1<n){browser_grid_rule(output,nodes,cols,rows,scroll,row,x,x+3,block->decoration);
                    Cell cross=block->decoration;cross.ch=0x253c;cross.wflag=0;cross.link=0;
                    browser_grid_put(output,nodes,cols,rows,scroll,row,x+1,cross,0);x+=3;}
            }
            row++;
        }
    }
    return row;
}
static void browser_grid_span(int cols,int screen,int side,int dx,int dy,int dw,int dh,int indent,int *left,int *right) {
    *left=0; *right=cols;
    if (screen>=dy && screen<dy+dh) {
        if (side==1) *right=dx;
        else if (side==2) *left=dx+dw;
    }
    if (indent<0) indent=0;
    if (indent>cols-4) indent=cols-4;
    if (*right-*left-indent<4) { *left=0; *right=cols; }
    *left+=indent;
}
static int browser_grid_layout_nodes(const BrowserBlock *blocks,int count,Cell *output,uint16_t *nodes,
    int cols,int rows,int scroll,int side,int dx,int dy,int dw,int dh) {
    int row=0; Cell blank={0}; blank.ch=' ';
    if (!output || cols<4 || cols>4096 || rows<1 || rows>BROWSER_DOCUMENT_ROWS ||
        count<0 || count>100000 || (count && !blocks)) return 0;
    for (int i=0;i<cols*rows;i++) { output[i]=blank; if(nodes) nodes[i]=0; }
    for (int b=0;b<count;b++) {
        const BrowserBlock *block=blocks+b;
        if(browser_grid_table_valid(block)) {
            int end=b+1,left,right;
            while(end<count && blocks[end].table==block->table && blocks[end].columns==block->columns &&
                  browser_grid_table_valid(blocks+end))end++;
            browser_grid_span(cols,-1,0,0,0,0,0,block->indent,&left,&right);
            int bottom=browser_grid_table_render(blocks,b,end,NULL,NULL,cols,rows,scroll,row,left,right-left);
            if(side && dh>0 && row-scroll<dy+dh && bottom-scroll>dy) {
                browser_grid_span(cols,dy,side,dx,dy,dw,dh,block->indent,&left,&right);
            }
            row=browser_grid_table_render(blocks,b,end,output,nodes,cols,rows,scroll,row,left,right-left);
            int gap=blocks[end-1].gap;if(gap<0)gap=0;if(gap>8)gap=8;
            row+=gap;b=end-1;continue;
        }
        if(block->count<0 || block->count>100000 || (block->count && !block->cells))continue;
        struct {int top,bottom,right;} floats[32];
        int nf=0,from=0,continuation=0,cursor=-1,blockBottom=row;
        while(from<block->count && row<20000) {
            int left,right,screen=row-scroll;
            int hanging=block->hanging<0?0:block->hanging>12?12:block->hanging;
            browser_grid_span(cols,screen,side,dx,dy,dw,dh,block->indent+(continuation?hanging:0),&left,&right);
            for(int i=0;i<nf;i++) if(row>=floats[i].top && row<floats[i].bottom && left<floats[i].right+1) left=floats[i].right+1;
            if(cursor<left)cursor=left;
            if(cursor>=right) { row++;cursor=-1;continuation=1;continue; }
            const Cell *token=block->cells+from;
            uint16_t node=block->nodes ? block->nodes[from] : 0;
            if(token->wflag&BROWSER_IMAGE) {
                int width=token->fg>BROWSER_IMAGE_COLUMNS?BROWSER_IMAGE_COLUMNS:(int)token->fg;
                int height=token->bg>BROWSER_IMAGE_ROWS?BROWSER_IMAGE_ROWS:(int)token->bg,fit=1;
                if(width<1)width=1;
                if(height<1)height=1;
                int original_width=width;
                if(width>cols)width=cols;
                for(int y=0;y<height;y++) {
                    int a,z; browser_grid_span(cols,screen+y,side,dx,dy,dw,dh,0,&a,&z);
                    if(cursor<a || cursor>=z)fit=0;
                    else if(width>z-cursor)width=z-cursor;
                }
                if(!fit) { row++;cursor=-1;continuation=0;continue; }
                /* A narrow lane scales the whole image, rather than squeezing
                   its width while retaining the old height. */
                if(width<original_width) {
                    height=(height*width+original_width/2)/original_width;
                    if(height<1)height=1;
                }
                for(int y=0;y<height;y++) if(screen+y>=0 && screen+y<rows)
                    for(int x=0;x<width;x++) {
                        Cell cell={0};cell.ch=token->ch|((uint32_t)width<<8)|((uint32_t)height<<16);cell.wflag=BROWSER_IMAGE;cell.link=token->link;
                        cell.attr=(uint8_t)(x+(y<<BROWSER_IMAGE_TILE_SHIFT));
                        int at=(screen+y)*cols+cursor+x; output[at]=cell; if(nodes)nodes[at]=node;
                    }
                if(nf<32) {floats[nf].top=row;floats[nf].bottom=row+height;floats[nf++].right=cursor+width;}
                if(blockBottom<row+height)blockBottom=row+height;
                cursor+=width+1;from++;continue;
            }
            int cut=from,boundary=-1,capacity=right-cursor;
            while(cut<block->count && cut-from<capacity && block->cells[cut].ch!='\n' && !(block->cells[cut].wflag&BROWSER_IMAGE)) {
                uint32_t cp=block->cells[cut].ch;
                if(cp==' ' && cut>from)boundary=cut;
                if(cp=='/' && cut>from+1 && cut+1<block->count && block->cells[cut+1].ch!='/')boundary=cut+1;
                cut++;
            }
            int full=cut-from==capacity && cut<block->count;
            if(full && boundary>from)cut=boundary;
            if(cut<block->count && (block->cells[cut].wflag&2))cut--;
            if(cut==from && token->ch!='\n') {
                int width=(token->wflag&1)?2:1;
                if(width>capacity){row++;cursor=-1;continue;}
                cut=from+width; if(cut>block->count)cut=block->count;
            }
            if(screen>=0 && screen<rows)for(int x=from;x<cut;x++) {
                int at=screen*cols+cursor+x-from;output[at]=block->cells[x];
                if(nodes)nodes[at]=block->nodes ? block->nodes[x] : 0;
            }
            cursor+=cut-from;from=cut;
            if(from>=block->count && block->rule && right-cursor>=5)
                browser_grid_rule(output,nodes,cols,rows,scroll,row,cursor+2,right,block->decoration);
            if(from<block->count && (block->cells[from].wflag&BROWSER_IMAGE))continue;
            if(from<block->count && block->cells[from].ch=='\n'){from++;continuation=0;}
            else {while(from<block->count && block->cells[from].ch==' ')from++;continuation=1;}
            row++;cursor=-1;
        }
        if(cursor>=0)row++;
        if(row<blockBottom)row=blockBottom;
        int gap=block->gap;if(gap<0)gap=0;if(gap>8)gap=8;
        row+=gap;
        if(row>=20000)break;
    }
    return row;
}
static int browser_grid_layout(const BrowserBlock *blocks,int count,Cell *output,int cols,int rows,int scroll,
    int side,int dx,int dy,int dw,int dh) {
    return browser_grid_layout_nodes(blocks,count,output,NULL,cols,rows,scroll,side,dx,dy,dw,dh);
}
#endif
