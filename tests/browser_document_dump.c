/* Headless snapshots of the production browser grid layout.
   Build: cc -std=c11 -O2 tests/browser_document_dump.c -o build/browser_document_dump
   Usage: browser_document_dump [cols rows scroll side dx dy dw dh]
   Input (whitespace separated, unsigned decimal integers):
     B blockCount
     count indent gap literal rule table header columns numeric hanging
     <columns cumulative end offsets>
     <count cells: ch fg bg attr wflag link node>
     ...
   Output: JSON with document total, viewport lines, and flat cell tuples
     [ch, attr, link, node, fg, bg, wflag]. This deliberately accepts cells rather
   than a second DOM/JSON decoder: fixture producers use the projection contract
   and the exact same C layout code that the app paints. No AppKit or network. */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t ch,fg,bg; uint8_t attr,wflag; uint16_t link; } Cell;
#include "../hardcore_browser_grid.h"

static void fail(const char *message) {
    fprintf(stderr,"browser_document_dump: %s\n",message);
    exit(2);
}
static unsigned long long integer(const char *token,unsigned long long limit) {
    char *end;
    if(!token[0] || token[0]=='-' || token[0]=='+')fail("expected an unsigned decimal integer");
    errno=0;
    unsigned long long value=strtoull(token,&end,10);
    if(errno || *end || value>limit)fail("integer out of range");
    return value;
}
static unsigned read_integer(unsigned limit) {
    char token[64];
    if(scanf("%63s",token)!=1)fail("truncated block/cell input");
    return (unsigned)integer(token,limit);
}
static void *allocate(size_t count,size_t size) {
    void *p=calloc(count?count:1,size);
    if(!p)fail("out of memory");
    return p;
}
static void json_codepoint(uint32_t cp) {
    if(cp=='"' || cp=='\\'){putchar('\\');putchar((int)cp);}
    else if(cp<32)printf("\\u%04x",cp);
    else {
        if(cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))cp=0xfffd;
        if(cp<0x80)putchar((int)cp);
        else if(cp<0x800){putchar(0xc0|(int)(cp>>6));putchar(0x80|(cp&63));}
        else if(cp<0x10000){putchar(0xe0|(int)(cp>>12));putchar(0x80|((cp>>6)&63));putchar(0x80|(cp&63));}
        else {putchar(0xf0|(int)(cp>>18));putchar(0x80|((cp>>12)&63));putchar(0x80|((cp>>6)&63));putchar(0x80|(cp&63));}
    }
}
int main(int argc,char **argv) {
    int geometry[8]={120,200,0,0,0,0,0,0};
    const unsigned limits[8]={400,1000,BROWSER_DOCUMENT_ROWS,2,400,1000,400,1000};
    if(argc>9)fail("usage: browser_document_dump [cols rows scroll side dx dy dw dh]");
    for(int i=1;i<argc;i++)geometry[i-1]=(int)integer(argv[i],limits[i-1]);
    int cols=geometry[0],rows=geometry[1],scroll=geometry[2];
    if(cols<4 || rows<1)fail("viewport must have at least 4 columns and 1 row");
    char marker[3];
    if(scanf("%2s",marker)!=1 || strcmp(marker,"B"))fail("expected B blockCount");
    int count=(int)read_integer(2000);
    BrowserBlock *blocks=allocate((size_t)count,sizeof(*blocks));
    unsigned cellsLeft=200000;
    for(int i=0;i<count;i++) {
        BrowserBlock *b=blocks+i;
        b->count=(int)read_integer(cellsLeft);cellsLeft-=(unsigned)b->count;
        b->indent=(int)read_integer(12);b->gap=(int)read_integer(2);
        b->literal=(int)read_integer(1);b->rule=(int)read_integer(1);
        b->table=(int)read_integer(2000);b->header=(int)read_integer(1);
        b->columns=(int)read_integer(BROWSER_TABLE_COLUMNS);
        b->numeric=read_integer((1u<<b->columns)-1u);b->decoration.attr=2;
        b->hanging=(int)read_integer(12);
        b->ends=allocate((size_t)b->columns,sizeof(int));
        int previous=0;
        for(int c=0;c<b->columns;c++) {
            int end=(int)read_integer((unsigned)b->count);
            if(end<previous)fail("column ends must be nondecreasing");
            b->ends[c]=previous=end;
        }
        if(b->columns && previous!=b->count)fail("last column end must equal count");
        b->cells=allocate((size_t)b->count,sizeof(Cell));
        b->nodes=allocate((size_t)b->count,sizeof(uint16_t));
        for(int c=0;c<b->count;c++) {
            Cell *cell=b->cells+c;
            cell->ch=read_integer(UINT32_MAX);cell->fg=read_integer(UINT32_MAX);cell->bg=read_integer(UINT32_MAX);
            cell->attr=(uint8_t)read_integer(UINT8_MAX);cell->wflag=(uint8_t)read_integer(UINT8_MAX);
            cell->link=(uint16_t)read_integer(4000);b->nodes[c]=(uint16_t)read_integer(4000);
        }
    }
    int leftover;
    do {leftover=getchar();}while(leftover==' ' || leftover=='\n' || leftover=='\r' || leftover=='\t');
    if(leftover!=EOF)fail("unexpected trailing input");
    size_t size=(size_t)cols*rows;
    Cell *output=allocate(size,sizeof(Cell));uint16_t *nodes=allocate(size,sizeof(uint16_t));
    int total=browser_grid_layout_nodes(blocks,count,output,nodes,cols,rows,scroll,
        geometry[3],geometry[4],geometry[5],geometry[6],geometry[7]);
    printf("{\"total\":%d,\"cols\":%d,\"rows\":%d,\"scroll\":%d,\"lines\":[",total,cols,rows,scroll);
    for(int y=0;y<rows;y++) {
        if(y)putchar(',');putchar('"');
        for(int x=0;x<cols;x++) {
            Cell cell=output[(size_t)y*cols+x];
            if(cell.wflag&2)continue;
            uint32_t cp=cell.ch?cell.ch:' ';
            if(cell.wflag&BROWSER_IMAGE)cp=cell.attr? ' ':0x25a7;
            json_codepoint(cp);
        }
        putchar('"');
    }
    printf("],\"cells\":[");
    for(size_t i=0;i<size;i++) {
        Cell c=output[i];if(i)putchar(',');
        printf("[%u,%u,%u,%u,%u,%u,%u]",c.ch,c.attr,c.link,nodes[i],c.fg,c.bg,c.wflag);
    }
    puts("]}");
    for(int i=0;i<count;i++){free(blocks[i].cells);free(blocks[i].nodes);free(blocks[i].ends);}
    free(blocks);free(output);free(nodes);
    return ferror(stdout)?1:0;
}
