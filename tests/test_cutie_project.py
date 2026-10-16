#!/usr/bin/env python3
"""Pure compact terminal projection: exact styles, maps, wide cells and cursor."""
import os
from pathlib import Path
import shlex
import tempfile
import unittest

from test_mac_import_resize import bounded_run

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t ch,fg,bg; uint8_t attr,wflag; uint16_t link; } Cell;
#include "hardcore_cutie_project.h"
static unsigned seed=0x9e3779b9u;
static unsigned random_number(void) { seed=seed*1664525u+1013904223u;return seed; }
static void blank(Cell *cells,int count) {
    for(int i=0;i<count;i++)cells[i]=(Cell){.ch=' ',.fg=(uint32_t)(0x123400+i),
        .bg=(uint32_t)(0x203040+i),.attr=(uint8_t)(i%4),.link=(uint16_t)(i+1)};
}
static void text(Cell *cells,int cols,int row,const char *value) {
    assert((int)strlen(value)<=cols);
    for(int i=0;value[i];i++)cells[row*cols+i].ch=(unsigned char)value[i];
}
static void verify(const HcCutieProjection *out,const Cell *input,int count,int cursor,int complete) {
    int previous=-1,seen_cursor=0;
    unsigned char *seen=calloc((size_t)count,1);assert(seen);
    assert(out->total_rows>=0 && out->first_row>=0 && out->scroll_rows>=0);
    assert(out->cursor>=-1 && out->cursor<out->cols*out->rows);
    for(int i=0;i<out->cols*out->rows;i++) {
        int source=out->source[i];
        if(source<0) {
            assert(source==-1 && out->cells[i].ch==' ' && !out->cells[i].wflag);
            continue;
        }
        assert(source<count && source>previous && !seen[source]);
        assert(!memcmp(out->cells+i,input+source,sizeof(Cell)));
        seen[source]=1;previous=source;
        if(source==cursor){assert(i==out->cursor);seen_cursor=1;}
        if(out->cells[i].wflag==HC_CUTIE_WIDE) {
            assert(i%out->cols+1<out->cols);
            assert(out->cells[i+1].wflag==HC_CUTIE_CONT && out->source[i+1]==source+1);
        }
        if(out->cells[i].wflag==HC_CUTIE_CONT) {
            assert(i%out->cols>0 && out->cells[i-1].wflag==HC_CUTIE_WIDE);
            assert(out->source[i-1]==source-1);
        }
    }
    assert((out->cursor>=0)==seen_cursor);
    if(complete) {
        assert(out->first_row==0 && out->total_rows<=out->rows);
        for(int i=0;i<count;i++)if(input[i].ch>' ' || input[i].wflag==HC_CUTIE_CONT || i==cursor)assert(seen[i]);
    }
    free(seen);
}
static void words_and_styles(void) {
    Cell input[32*9],original[32*9];blank(input,32*9);
    text(input,32,2,"alpha beta gamma");
    text(input,32,6,"/literal/path/no-spaces-1234567");
    text(input,32,7,"-------------------");
    text(input,32,8,"status: do not discard this");
    memcpy(original,input,sizeof(input));
    HcCutieProjection out={0};
    assert(hc_cutie_project_build(&out,input,32,9,-1,8,30,0));
    assert(out.cells[0].ch=='a' && out.source[5]==2*32+5 && out.source[6]==-1);
    assert(out.cells[8].ch=='b' && out.source[8]==2*32+6);
    assert(out.cells[16].ch=='g');
    /* Three redundant physical blanks become one styled separator. */
    assert(out.source[24]==3*32 && out.source[31]==3*32+7);
    assert(out.cells[32].ch=='/' && out.source[32]==6*32);
    verify(&out,input,32*9,-1,1);
    assert(!memcmp(input,original,sizeof(input)));
    hc_cutie_project_free(&out);assert(!out.cells && !out.source && out.cursor==-1);
}
static void wide_and_cursor(void) {
    Cell input[10*3],original[10*3];blank(input,30);
    text(input,10,0,"abcd");
    input[4].ch=0x754c;input[4].wflag=HC_CUTIE_WIDE;
    input[5].ch=0;input[5].wflag=HC_CUTIE_CONT;
    input[6].ch='x';
    text(input,10,1,"> prompt");
    memcpy(original,input,sizeof(input));
    HcCutieProjection out={0};
    assert(hc_cutie_project_build(&out,input,10,3,5,5,10,0));
    assert(out.source[4]==-1 && out.source[5]==4 && out.source[6]==5 && out.cursor==6);
    verify(&out,input,30,5,1);
    assert(!memcmp(input,original,sizeof(input)));
    /* A cursor well past an empty prompt keeps every intervening blank. */
    assert(hc_cutie_project_build(&out,input,10,3,29,4,10,0));
    assert(out.cursor>=0 && out.source[out.cursor]==29);
    for(int source=20;source<=29;source++) {
        int found=0;for(int i=0;i<out.cols*out.rows;i++)found|=out.source[i]==source;assert(found);
    }
    verify(&out,input,30,29,1);
    hc_cutie_project_free(&out);
}
static void viewport(void) {
    Cell input[24*12],original[24*12];blank(input,24*12);
    for(int row=0;row<12;row++){char value[24];snprintf(value,sizeof(value),"recent output %02d",row);text(input,24,row,value);}
    text(input,24,9,"> unsent command        ");
    memcpy(original,input,sizeof(input));
    HcCutieProjection out={0};int cursor=9*24+9;
    assert(hc_cutie_project_build(&out,input,24,12,cursor,26,7,0));
    assert(out.total_rows==12 && out.first_row==3 && out.cursor==6*26+9);
    assert(out.source[0]==3*24 && out.scroll_rows==0);
    verify(&out,input,24*12,cursor,0);
    assert(hc_cutie_project_build(&out,input,24,12,cursor,26,7,2));
    assert(out.first_row==1 && out.scroll_rows==2 && out.cursor==-1);
    verify(&out,input,24*12,cursor,0);
    assert(hc_cutie_project_build(&out,input,24,12,cursor,26,7,INT_MAX));
    assert(out.first_row==0 && out.scroll_rows==3 && out.cursor==-1);
    assert(hc_cutie_project_build(&out,input,24,12,-1,26,7,0));
    assert(out.first_row==5 && out.cursor==-1);
    assert(!memcmp(input,original,sizeof(input)));
    hc_cutie_project_free(&out);
}
static void empty_and_bounds(void) {
    Cell input[8];blank(input,8);HcCutieProjection out={0};
    assert(hc_cutie_project_build(&out,input,4,2,-1,2,2,0));
    assert(!out.total_rows && !out.first_row && out.cursor==-1);
    for(int i=0;i<4;i++)assert(out.source[i]==-1);
    assert(hc_cutie_project_build(&out,input,4,2,7,2,2,0));
    assert(out.total_rows==2 && out.cursor==3 && out.source[3]==7);
    Cell *saved=out.cells;int *mapping=out.source;
    const int bad[][6]={{0,2,-1,2,2,0},{4,0,-1,2,2,0},{4,129,-1,2,2,0},
        {1000,49,-1,2,2,0},{4,2,-2,2,2,0},{4,2,8,2,2,0},{4,2,-1,1,2,0},
        {4,2,-1,INT_MAX,2,0},{4,2,-1,2,129,0},{4,2,-1,2,0,0}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        assert(!hc_cutie_project_build(&out,input,bad[i][0],bad[i][1],bad[i][2],bad[i][3],bad[i][4],bad[i][5]));
        assert(out.cells==saved && out.source==mapping && out.cursor==3 && out.source[3]==7);
    }
    input[3].ch=0x754c;input[3].wflag=HC_CUTIE_WIDE;
    assert(!hc_cutie_project_build(&out,input,4,2,-1,4,2,0));
    assert(out.cells==saved && out.source==mapping);
    input[3].wflag=0;input[3].ch=' ';input[4].wflag=HC_CUTIE_CONT;
    assert(!hc_cutie_project_build(&out,input,4,2,-1,4,2,0));
    assert(out.cells==saved && out.source==mapping);
    hc_cutie_project_free(&out);
    Cell *large=malloc(48000*sizeof(Cell));assert(large);blank(large,48000);
    for(int i=0;i<48000;i++)large[i].ch='X';
    assert(hc_cutie_project_build(&out,large,1000,48,47999,26,7,0));
    assert(out.total_rows==48*39 && out.cursor==6*26+11);
    verify(&out,large,48000,47999,0);
    hc_cutie_project_free(&out);free(large);
}
static void randomized(void) {
    for(int run=0;run<1500;run++) {
        int cols=6+(int)(random_number()%19),rows=1+(int)(random_number()%6),count=cols*rows;
        int width=2+(int)(random_number()%25),cursor=(int)(random_number()%(unsigned)count);
        Cell *input=malloc((size_t)count*sizeof(Cell)),*original=malloc((size_t)count*sizeof(Cell));assert(input&&original);
        blank(input,count);
        for(int row=0;row<rows;row++)if(random_number()%4) {
            for(int col=0;col<cols;col++) {
                unsigned kind=random_number()%7;int at=row*cols+col;
                if(kind==0 && col+1<cols) {
                    input[at].ch=0x754c;input[at].wflag=HC_CUTIE_WIDE;
                    input[at+1].ch=0;input[at+1].wflag=HC_CUTIE_CONT;col++;
                } else if(kind>2)input[at].ch='a'+random_number()%26;
            }
        }
        memcpy(original,input,(size_t)count*sizeof(Cell));
        HcCutieProjection out={0};
        assert(hc_cutie_project_build(&out,input,cols,rows,-1,width,128,0));
        verify(&out,input,count,-1,out.total_rows<=128);
        assert(hc_cutie_project_build(&out,input,cols,rows,cursor,width,7,0));
        assert(out.cursor>=0);verify(&out,input,count,cursor,0);
        assert(hc_cutie_project_build(&out,input,cols,rows,cursor,width,7,(int)(random_number()%30)));
        verify(&out,input,count,cursor,0);
        assert(!memcmp(input,original,(size_t)count*sizeof(Cell)));
        hc_cutie_project_free(&out);free(input);free(original);
    }
}
int main(int argc,char **argv) {
    assert(argc==2);
    if(!strcmp(argv[1],"styles"))words_and_styles();
    else if(!strcmp(argv[1],"wide"))wide_and_cursor();
    else if(!strcmp(argv[1],"viewport"))viewport();
    else if(!strcmp(argv[1],"bounds"))empty_and_bounds();
    else if(!strcmp(argv[1],"random"))randomized();
    else return 2;
    printf("PASS %s\n",argv[1]);return 0;
}
'''


class CutieProjectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="hc-cutie-project-")
        path = Path(cls.temp.name)
        source = path / "project.c"
        source.write_text(HARNESS)
        cls.exe = path / "project"
        command = shlex.split(os.environ.get("CC", "clang"))
        command += ["-std=c99", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT)]
        command += shlex.split(os.environ.get("HARDCORE_TEST_CFLAGS", ""))
        result = bounded_run(command + [str(source), "-o", str(cls.exe)], 30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def check(self, case):
        result = bounded_run([str(self.exe), case], 10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_literal_order_styles_links_and_blank_compaction(self):
        self.check("styles")

    def test_wide_pairs_and_cursor_inside_trailing_blanks(self):
        self.check("wide")

    def test_cursor_follow_and_independent_scrollback(self):
        self.check("viewport")

    def test_empty_inputs_bounds_and_failure_preserves_output(self):
        self.check("bounds")

    def test_1500_randomized_projections_preserve_source_and_style(self):
        self.check("random")


if __name__ == "__main__":
    unittest.main()
