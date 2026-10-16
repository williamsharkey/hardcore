#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
typedef struct { uint32_t ch,fg,bg; uint8_t attr,wflag; uint16_t link; } Cell;
#include "../hardcore_browser_grid.h"
#include "../hardcore_browser_pixels.h"
static unsigned seed=271828;
static unsigned rnd(void) { seed=1664525*seed+1013904223; return seed; }
static void check_content(const BrowserBlock *blocks,int count,const Cell *out,int size) {
    int at=0;
    for (int b=0;b<count;b++) for (int i=0;i<blocks[b].count;i++) {
        Cell source=blocks[b].cells[i];
        if (source.ch<=' ' || source.wflag==2) continue;
        while (at<size && (out[at].ch<=' ' || out[at].wflag==2)) at++;
        assert(at<size && out[at].ch==source.ch && out[at].link==source.link && out[at].fg==source.fg);
        if (source.wflag==1) assert(at+1<size && out[at+1].wflag==2);
        at++;
    }
    while (at<size) assert(out[at++].ch<=' ');
}
static int make_table_row(BrowserBlock *b,Cell *cells,uint16_t *nodes,int *ends,
    const char **values,int columns,int header,int firstnode) {
    int at=0;
    for(int c=0;c<columns;c++) {
        for(const unsigned char *p=(const unsigned char *)values[c];*p;p++) {
            cells[at]=(Cell){.ch=*p,.fg=(uint32_t)(0x123400+c),.link=(uint16_t)(c+1),.attr=(uint8_t)header};
            nodes[at]=(uint16_t)(firstnode+at);at++;
        }
        ends[c]=at;
    }
    *b=(BrowserBlock){.cells=cells,.count=at,.nodes=nodes,.table=1,.columns=columns,.ends=ends,
        .header=header,.numeric=1u<<(columns-1),.decoration={.fg=0x555555}};
    return at;
}
static void check_table_maps(const BrowserBlock *blocks,int count,const Cell *out,const uint16_t *nodes,
    int size,int headercopies,int cols) {
    unsigned short seen[4096]={0};Cell source[4096]={0};
    for(int b=0;b<count;b++)for(int i=0;i<blocks[b].count;i++)source[blocks[b].nodes[i]]=blocks[b].cells[i];
    for(int i=0;i<size;i++)if(nodes[i]) {
        assert(nodes[i]<4096);
        Cell expected=source[nodes[i]];
        assert(!memcmp(out+i,&expected,sizeof(Cell)));
        seen[nodes[i]]++;
        if(out[i].wflag&1)assert(i+1<size && out[i+1].wflag==2 && i/cols==(i+1)/cols);
    }
    for(int b=0;b<count;b++)for(int i=0;i<blocks[b].count;i++) {
        if(blocks[b].cells[i].ch<=' ' || blocks[b].cells[i].wflag==2)continue;
        assert(seen[blocks[b].nodes[i]]==(blocks[b].header?headercopies:1));
    }
}
static void table_tests(void) {
    Cell cells[3][400],out[120*200],all[120*200];uint16_t ids[3][400],mapped[120*200];
    int ends[3][16];BrowserBlock b[3];
    const char *head[]={"Parameter","Meaning","Value"};
    const char *mass[]={"Electron mass","Rest mass of a free electron\nin vacuum","9.109e-31 kg"};
    const char *speed[]={"Speed of light","Vacuum propagation speed","299792458 m/s"};
    make_table_row(b,cells[0],ids[0],ends[0],head,3,1,1);
    make_table_row(b+1,cells[1],ids[1],ends[1],mass,3,0,401);
    make_table_row(b+2,cells[2],ids[2],ends[2],speed,3,0,801);
    for(int side=0;side<=2;side++) {
        int cols=100,dx=side==1?72:0,dy=1,dh=20;
        int total=browser_grid_layout_nodes(b,3,out,mapped,cols,120,0,side,dx,dy,28,dh);
        assert(total<20);check_table_maps(b,3,out,mapped,cols*120,1,cols);
        if(side)for(int y=dy;y<dy+dh;y++)for(int x=dx;x<dx+28;x++)assert(out[y*cols+x].ch==' ');
        /* One header rule; no outer border or horizontal rules between records. */
        int rules=0,bars=0;
        for(int y=0;y<total;y++){int rule=0;for(int x=0;x<cols;x++) {
            Cell c=out[y*cols+x];
            if(c.ch==0x2500 || c.ch==0x253c){rule=1;assert(c.fg==0x555555 && !mapped[y*cols+x]);}
            if(c.ch==0x2502){bars++;assert(c.fg==0x555555 && !mapped[y*cols+x]);}
        }rules+=rule;}
        assert(rules==1 && bars>0);
    }
    /* A table wholly above a dock keeps its natural full-width geometry. */
    browser_grid_layout(b,3,all,100,120,0,0,0,0,0,0);
    browser_grid_layout(b,3,out,100,120,0,1,72,60,28,20);
    assert(!memcmp(all,out,100*120*sizeof(Cell)));
    /* Numeric values end at the same column, even when their lengths differ. */
    int massend=-1,speedend=-1;
    for(int i=0;i<100*120;i++) {
        if(mapped[i]==ids[1][ends[1][2]-1])massend=i%100;
        if(mapped[i]==ids[2][ends[2][2]-1])speedend=i%100;
    }
    assert(massend>=0 && massend==speedend);
    /* Narrow records retain column labels alongside each value, without
       repeating a redundant standalone header or drawing nested borders. */
    int total=browser_grid_layout_nodes(b,3,out,mapped,18,120,0,0,0,0,0,0);
    assert(total>10 && total<100);check_table_maps(b,3,out,mapped,18*120,2,18);
    for(int i=0;i<18*120;i++)assert(out[i].ch!=0x2502 && out[i].ch!=0x2500);
    /* Very narrow grids still retain every label and value. */
    total=browser_grid_layout_nodes(b,3,out,mapped,4,200,0,0,0,0,0,0);
    assert(total<200);check_table_maps(b,3,out,mapped,4*200,1,4);
    /* Viewport scrolling is exactly a slice of the complete table. */
    browser_grid_layout(b,3,all,100,120,0,0,0,0,0,0);
    browser_grid_layout(b,3,out,100,117,3,0,0,0,0,0);
    assert(!memcmp(all+300,out,100*117*sizeof(Cell)));
    /* Decorations never borrow the heading's action/DOM identity. */
    BrowserBlock heading=b[0];heading.table=0;heading.columns=0;heading.rule=1;heading.count=9;
    total=browser_grid_layout_nodes(&heading,1,out,mapped,40,5,0,0,0,0,0,0);
    assert(total==1 && out[11].ch==0x2500 && mapped[11]==0 && out[11].link==0);
    /* Invalid table metadata degrades to text, without indexing huge arrays. */
    b[0].columns=INT_MAX;
    browser_grid_layout(b,1,out,100,10,0,0,0,0,0,0);check_content(b,1,out,1000);
    b[0].columns=3;ends[0][1]=INT_MAX;
    browser_grid_layout(b,1,out,100,10,0,0,0,0,0,0);check_content(b,1,out,1000);
    b[0].gap=INT_MAX;
    assert(browser_grid_layout(b,1,out,100,10,0,0,0,0,0,0)<20);
    /* Wide characters and explicit newlines survive cell wrapping intact. */
    Cell wide[]={{.ch='A'},{.ch=0x4e2d,.wflag=1,.link=8},{.ch=' ',.wflag=2,.link=8},
        {.ch='\n'},{.ch='B'},{.ch='C'},{.ch='D'},{.ch='E'}};
    uint16_t wn[]={1,2,3,4,5,6,7,8};int we[]={5,8};
    BrowserBlock wb={.cells=wide,.count=8,.nodes=wn,.table=2,.columns=2,.ends=we};
    total=browser_grid_layout_nodes(&wb,1,out,mapped,12,10,0,0,0,0,0,0);
    assert(total>=2 && total<10);check_table_maps(&wb,1,out,mapped,120,1,12);
    /* An image wider than the indented/docked lane shrinks instead of
       retrying the same impossible placement until the document limit. */
    Cell image={.ch=1,.fg=UINT_MAX,.bg=UINT_MAX,.wflag=BROWSER_IMAGE};
    BrowserBlock ib={.cells=&image,.count=1,.indent=5};
    assert(browser_grid_layout(&ib,1,out,10,10,0,2,0,0,3,10)<=5);
    int tiles=0;for(int i=0;i<100;i++)if(out[i].wflag&BROWSER_IMAGE)tiles++;
    assert(tiles>0 && tiles<=20);
    puts("PASS document tables: quiet rules, natural columns, right alignment, labeled narrow records, DOM maps, both docks and malformed metadata");
}
static void random_table_tests(void) {
    Cell cells[8][256],out[100*800];uint16_t ids[8][256],mapped[100*800];
    int ends[8][16];BrowserBlock blocks[8];
    for(int trial=0;trial<300;trial++) {
        int records=2+rnd()%7,columns=1+rnd()%6,cols=4+rnd()%97,side=rnd()%3;
        int dockwidth=cols>=20?cols/3:0;
        for(int b=0;b<records;b++) {
            int count=0;
            for(int c=0;c<columns;c++) {
                int length=1+rnd()%24;
                for(int i=0;i<length;i++) {
                    Cell cell={.ch='a'+rnd()%26,.fg=(uint32_t)(100+b),.attr=(uint8_t)b,.link=(uint16_t)(1+c)};
                    if(rnd()%13==0)cell.ch=' ';
                    else if(rnd()%17==0)cell.ch='/';
                    else if(rnd()%19==0)cell.ch='\n';
                    else if(rnd()%23==0){cell.ch=0x4e2d;cell.wflag=1;}
                    cells[b][count]=cell;ids[b][count]=(uint16_t)(1+b*256+count);count++;
                    if(cell.wflag==1){cell.ch=' ';cell.wflag=2;cells[b][count]=cell;ids[b][count]=(uint16_t)(1+b*256+count);count++;}
                }
                ends[b][c]=count;
            }
            blocks[b]=(BrowserBlock){.cells=cells[b],.nodes=ids[b],.count=count,.table=1,.columns=columns,
                .header=b==0,.ends=ends[b],.numeric=1u<<(columns-1),.decoration={.fg=0x555555}};
        }
        int dx=side==1?cols-dockwidth:0;
        int total=browser_grid_layout_nodes(blocks,records,out,mapped,cols,800,0,side,dx,0,dockwidth,800);
        assert(total<800);
        int widths[16],available=side?cols-dockwidth:cols;
        int stacked=!browser_grid_table_widths(blocks,0,records,available,widths);
        check_table_maps(blocks,records,out,mapped,cols*800,stacked && available>=8?records-1:1,cols);
        if(side && dockwidth)for(int y=0;y<total;y++)for(int x=dx;x<dx+dockwidth;x++)assert(out[y*cols+x].ch==' ');
    }
    puts("PASS 300 randomized document tables: every glyph/style/link/DOM node preserved, wide glyphs stay whole, dock lanes stay clear");
}
int main(void) {
    table_tests();
    random_table_tests();
    Cell input[1500],out[160*400],all[160*400];
    for (int trial=0;trial<500;trial++) {
        int n=0,cols=40+(rnd()%100),rows=180;
        while (n<550) {
            Cell c={0}; c.ch='a'+rnd()%26; c.link=(uint16_t)(1+trial%4000); c.fg=1234;
            if (rnd()%17==0) c.ch=' ';
            else if (rnd()%23==0) c.ch='/';
            else if (rnd()%37==0) c.ch='\n';
            else if (rnd()%19==0) { c.ch=0x4e2d; c.wflag=1; }
            input[n++]=c;
            if (c.wflag==1) { c.ch=' '; c.wflag=2; input[n++]=c; }
        }
        BrowserBlock block={.cells=input,.count=n,.indent=(int)(rnd()%5),.gap=1,.hanging=2};
        for (int side=1;side<=2;side++) {
            int dx=side==1 ? cols-16 : 0,dy=2+(rnd()%8),dh=2+rnd()%8;
            int total=browser_grid_layout(&block,1,out,cols,rows,0,side,dx,dy,16,dh);
            assert(total<rows);
            check_content(&block,1,out,cols*rows);
            for (int y=dy;y<dy+dh;y++) for (int x=dx;x<dx+16;x++) assert(out[y*cols+x].ch==' ');
        }
        browser_grid_layout(&block,1,all,cols,rows,0,0,0,0,0,0);
        browser_grid_layout(&block,1,out,cols,rows-3,3,0,0,0,0,0);
        assert(!memcmp(all+3*cols,out,(size_t)(rows-3)*cols*sizeof(Cell)));
    }
    /* Tiny windows fall back to full width, preserving wide glyphs. */
    for (int i=0;i<30;i++) input[i]=(Cell){.ch='X',.link=2};
    BrowserBlock narrow={.cells=input,.count=30,.indent=12,.hanging=2};
    browser_grid_layout(&narrow,1,out,10,100,0,2,0,0,28,100);
    check_content(&narrow,1,out,1000);
    Cell literal[]={{.ch='a'},{.ch='\n'},{.ch=' '},{.ch=' '},{.ch='b'}};
    BrowserBlock code={.cells=literal,.count=5,.literal=1,.hanging=2};
    assert(browser_grid_layout(&code,1,out,40,4,0,0,0,0,0,0)==2);
    assert(out[0].ch=='a' && out[42].ch=='b' && out[40].ch==' ');
    /* Inline image cells retain their DOM node, reserve a rectangle, and let
       prose flow alongside it on either dock. */
    Cell mixed[180];uint16_t dom[180],mapped[50*40];
    mixed[0]=(Cell){.ch=1,.fg=8,.bg=3,.wflag=BROWSER_IMAGE,.link=9};dom[0]=42;
    for(int i=1;i<180;i++){mixed[i]=(Cell){.ch=(i%9)?'a':' ',.link=8};dom[i]=43;}
    BrowserBlock illustrated={.cells=mixed,.count=180,.gap=1,.nodes=dom,.hanging=2};
    for(int side=1;side<=2;side++) {
        browser_grid_layout_nodes(&illustrated,1,out,mapped,50,40,0,side,side==1?34:0,0,16,8);
        int count=0;
        for(int i=0;i<50*40;i++) {
            if(out[i].wflag&BROWSER_IMAGE){count++;assert(mapped[i]==42);assert((out[i].ch&255)==1);assert((out[i].attr&BROWSER_IMAGE_TILE_MASK)<8 && (out[i].attr>>BROWSER_IMAGE_TILE_SHIFT)<3);}
            else if(out[i].ch=='a')assert(mapped[i]==43);
        }
        assert(count==24);
    }
    unsigned char rgba[4*16],indexed[64*64];uint32_t palette[]={0x000000,0xffffff,0x808080};
    for(int i=0;i<16;i++){rgba[4*i]=rgba[4*i+1]=rgba[4*i+2]=64;rgba[4*i+3]=255;}
    browser_image_quantize(rgba,4,4,indexed,64,64,palette,3,0);
    int varied=0;for(int i=0;i<64*64;i++){assert(indexed[i]<3);varied|=indexed[i]!=indexed[0];}assert(varied);
    memset(rgba,0,sizeof(rgba));browser_image_quantize(rgba,4,4,indexed,64,64,palette,3,0x808080);
    for(int i=0;i<64*64;i++)assert(indexed[i]==2);
    for(int w=1;w<4000;w+=97)for(int h=1;h<2000;h+=61){int cols,rows;browser_image_size(w,h,8,16,&cols,&rows);assert(cols>=1 && cols<=24 && rows>=1 && rows<=8);}
    int image_cols,image_rows;
    browser_image_size(384,256,8,16,&image_cols,&image_rows);assert(image_cols==24 && image_rows==8);
    browser_image_size(64,48,8,16,&image_cols,&image_rows);assert(image_cols==8 && image_rows==3);
    browser_image_size(12,24,8,16,&image_cols,&image_rows);assert(image_cols==2 && image_rows==2);
    browser_image_size(INT_MAX,INT_MAX,INT_MAX,INT_MAX,&image_cols,&image_rows);assert(image_cols==1 && image_rows==1);
    /* The doubled image width needs five x bits; column16 must not alias
       column0 on the following row. Every tile and its action stays unique. */
    Cell big_image={.ch=3,.fg=24,.bg=8,.wflag=BROWSER_IMAGE,.link=7};uint16_t image_node=91;
    BrowserBlock big_picture={.cells=&big_image,.count=1,.nodes=&image_node};
    browser_grid_layout_nodes(&big_picture,1,out,mapped,50,40,0,0,0,0,0,0);
    unsigned char seen_tiles[256]={0};int image_tiles=0;
    for(int i=0;i<50*40;i++)if(out[i].wflag&BROWSER_IMAGE) {
        int tx=out[i].attr&BROWSER_IMAGE_TILE_MASK,ty=out[i].attr>>BROWSER_IMAGE_TILE_SHIFT;
        assert(tx==i%50 && ty==i/50 && tx<24 && ty<8);
        assert(!seen_tiles[out[i].attr]++ && mapped[i]==91 && out[i].link==7);
        assert(((out[i].ch>>8)&255)==24 && ((out[i].ch>>16)&255)==8);image_tiles++;
    }
    assert(image_tiles==24*8);
    browser_grid_layout_nodes(&big_picture,1,out,mapped,12,40,0,0,0,0,0,0);
    image_tiles=0;
    for(int i=0;i<12*40;i++)if(out[i].wflag&BROWSER_IMAGE) {
        assert(((out[i].ch>>8)&255)==12 && ((out[i].ch>>16)&255)==4);image_tiles++;
    }
    assert(image_tiles==12*4); /* Narrow lanes retain approximate aspect. */
    puts("PASS browser grid/pixels: inline image flow, DOM maps, palette dithering, alpha and image bounds");
    puts("PASS browser grid: both docks, viewport scrolling, literal lines, narrow fallback, wide glyphs and 500 randomized documents");
    return 0;
}
