/* Character-only image/camera view in another mode of the same executable.
 * AVFoundation stays in this PTY child, away from the session owner's forks. */
#ifndef HARDCORE_HERE_MEDIA_H
#define HARDCORE_HERE_MEDIA_H
#include "hardcore_textile.h"
#include "hardcore_here_media_source.h"

static int hm_min(int a,int b) { return a<b?a:b; }
static uint64_t hm_now(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
static const char *hm_modes[] = {"Braille", "sextants", "quadrants", "half blocks"};
static void hm_text(HcTextileCell *cells,int cols,int row,const char *text,uint32_t fg,uint32_t bg) {
    int x=0;size_t at=0,n=strlen(text);
    while(at<n && x<cols) {
        size_t used;uint32_t c=hhh_cp(text+at,n-at,&used);at+=used;
        if(!hhh_printable(c))continue;int w=cp_width(c);if(w<=0||x+w>cols)continue;
        cells[(size_t)row*cols+x++]=(HcTextileCell){c,fg,bg};
        if(w==2)cells[(size_t)row*cols+x++]=(HcTextileCell){0,fg,bg};
    }
}
static void hm_frame(SBuf *out,HcTextileCell *cells,HcTextileCell *old,int cols,int rows,int force) {
    uint32_t fg=UINT32_MAX,bg=UINT32_MAX;
    sb_put(out,"\033[0m\033[?7l",sizeof("\033[0m\033[?7l")-1);
    for(int y=0;y<rows;y++) {
        HcTextileCell *line=cells+(size_t)y*cols,*before=old+(size_t)y*cols;
        if(!force&&!memcmp(line,before,(size_t)cols*sizeof(*line)))continue;
        sb_printf(out,"\033[%d;1H",y+1);
        for(int x=0;x<cols;x++) {
            HcTextileCell c=line[x];if(!c.ch)continue;
            if(c.fg!=fg){fg=c.fg;sb_printf(out,"\033[38;2;%u;%u;%um",fg>>16,(fg>>8)&255,fg&255);}
            if(c.bg!=bg){bg=c.bg;sb_printf(out,"\033[48;2;%u;%u;%um",bg>>16,(bg>>8)&255,bg&255);}
            char u[8];int n=utf8_encode(c.ch,u);sb_put(out,u,(size_t)n);
        }
        memcpy(before,line,(size_t)cols*sizeof(*line));
    }
    sb_put(out,"\033[0m",4);
}
static int hm_control(int fd,int *visible) {
    if(fd<0)return 1;
    char b[128];
    for(;;){ssize_t n=read(fd,b,sizeof(b));if(n>0){for(ssize_t i=0;i<n;i++)if(b[i]=='0'||b[i]=='1')*visible=b[i]=='1';continue;}
        if(n==0)return 0;if(errno==EINTR)continue;return errno==EAGAIN||errno==EWOULDBLOCK;}
}
static void hm_suspend_source(void *context) {
    HcHereMediaSource **source=context;
    if(*source){hc_here_media_close(*source);*source=NULL;}
}
static int hm_run(int argc,char **argv,HHHTerm *parent) {
    int camera=0,mode=HC_TEXTILE_BRAILLE;const char *path=NULL;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--help")){puts("Hardcore character media: --camera | --image PATH\nKeys: m rendering mode, Space pause/resume, r retry, q close.\nCamera preview only; no microphone or recording.");return 0;}
        if(!strcmp(argv[i],"--camera")){camera=1;continue;}
        if(!strcmp(argv[i],"--image")&&i+1<argc){path=argv[++i];continue;}
        fputs("Media: expected --camera or --image PATH\n",stderr);return 2;
    }
    if(camera==!!path){fputs("Media: choose one camera or image source\n",stderr);return 2;}
    HHHTerm term;if(!hhh_start(&term))return 2;
    if(parent){memcpy(term.input,parent->input,(size_t)parent->input_len);term.input_len=parent->input_len;parent->input_len=0;}
    int control=-1,visible=1;const char *control_env=getenv("HC_HERE_MEDIA_FD");
    if(control_env&&*control_env){char *end;long n=strtol(control_env,&end,10);if(!*end&&n>=3&&n<=INT_MAX){control=(int)n;visible=0;fcntl(control,F_SETFL,fcntl(control,F_GETFL)|O_NONBLOCK);}}
    /* This mode owns only its explicitly supplied lifetime channel. */
    unsetenv("HC_HERE_MEDIA_FD");
    printf("\033]0;%s\007",camera?"Camera":"Image");fflush(stdout);
    HereSettings prefs;here_settings_load(&prefs);const Scheme *scheme=scheme_by_key(prefs.theme);if(!scheme)scheme=g_scheme;
    uint32_t bg=scheme->bg,fg=scheme->fg;
    int flags=fcntl(1,F_GETFL);fcntl(1,F_SETFL,flags|O_NONBLOCK);
    HcHereMediaSource *source=NULL;unsigned char *rgba=NULL;size_t capacity=0;
    if(camera){term.before_suspend=hm_suspend_source;term.suspend_context=&source;}
    HcTextileCell *cells=NULL,*old=NULL,*picture=NULL;size_t gridcap=0,piccap=0;
    int width=0,height=0,paused=0,failed=0,dirty=1,force=1,lastcols=0,lastrows=0;
    uint64_t revision=0,next=0;char error[512]="",last_error[512]="";
    SBuf pending={0};size_t offset=0;
    while(!hhh_stop) {
        if(!hm_control(control,&visible))break;
        if(hhh_suspend){if(camera)hm_suspend_source(&source);hhh_leave(&term);hhh_suspend=0;kill(getpid(),SIGSTOP);hhh_enter(&term);dirty=force=1;continue;}
        if(camera&&source&&(!visible||paused)){hc_here_media_close(source);source=NULL;revision=0;dirty=1;}
        if(!source&&!failed&&(camera?(visible&&!paused):1)) {
            error[0]=0;source=camera?hc_here_media_open_camera(error,sizeof(error)):hc_here_media_open_image(path,error,sizeof(error));
            if(!source)failed=1;dirty=1;revision=0;
        }
        hhh_size(&term);
        if(hhh_resize)dirty=force=1;
        struct winsize ws={0};if(!ioctl(1,TIOCGWINSZ,&ws)&&ws.ws_row>0&&ws.ws_row<4)term.rows=ws.ws_row<2?2:ws.ws_row;
        if(term.cols!=lastcols||term.rows!=lastrows){dirty=force=1;lastcols=term.cols;lastrows=term.rows;}
        /* One pending screen only. While output is slow, keep the latest source
         * frame and service lifetime/input; never accumulate video frames. */
        if(offset<pending.n){ssize_t n=write(1,pending.p+offset,pending.n-offset);if(n>0)offset+=(size_t)n;
            else if(n<0&&errno!=EINTR&&errno!=EAGAIN&&errno!=EWOULDBLOCK)break;
            if(offset==pending.n){pending.n=offset=0;}}
        uint64_t now=hm_now();
        if(!pending.n&&source&&!paused&&now>=next) {
            int got=hc_here_media_copy_frame(source,&rgba,&capacity,&width,&height,&revision,error,sizeof(error));
            if(got>0)dirty=1;if(got<0){failed=1;hc_here_media_close(source);source=NULL;dirty=1;}
            if(strcmp(error,last_error)){snprintf(last_error,sizeof(last_error),"%s",error);dirty=1;}
            next=now+100;
        }
        if(!pending.n&&dirty) {
            size_t count=(size_t)term.cols*term.rows;
            if(count>gridcap){HcTextileCell *a=calloc(count,sizeof(*a)),*b=calloc(count,sizeof(*b));if(!a||!b){free(a);free(b);break;}free(cells);free(old);cells=a;old=b;gridcap=count;force=1;}
            for(size_t i=0;i<count;i++)cells[i]=(HcTextileCell){' ',fg,bg};
            if(rgba&&width>0&&height>0) {
                int maxcols=hm_min(term.cols,camera?96:160),maxrows=hm_min(term.rows-2,camera?48:80);
                double scale=fmin((double)maxcols/width,(double)maxrows*2/height);
                int cw=(int)floor(width*scale),ch=(int)floor(height*scale/2);if(cw<1)cw=1;if(ch<1)ch=1;
                size_t need=(size_t)cw*ch;
                if(need>piccap){HcTextileCell *p=realloc(picture,need*sizeof(*p));if(!p)break;picture=p;piccap=need;}
                if(hc_textile_render(rgba,width,height,width*4,picture,cw,ch,mode,bg)) {
                    int x=(term.cols-cw)/2,y=1+(term.rows-2-ch)/2;
                    for(int r=0;r<ch;r++)memcpy(cells+(size_t)(y+r)*term.cols+x,picture+(size_t)r*cw,(size_t)cw*sizeof(*picture));
                }
            }
            char title[1024];snprintf(title,sizeof(title),"%s · %s%s%s%s%s",camera?"Camera":"Image",hm_modes[mode],paused?" · paused":"",camera&&!visible?" · hidden":"",path?" · ":"",path?path:"");
            hm_text(cells,term.cols,0,title,scheme->cursor,bg);
            const char *status=error[0]?error:camera&&!visible?"Camera off while hidden":paused?"Paused · Space resume · m mode · q close":"m mode · Space pause · r retry · q close";
            hm_text(cells,term.cols,term.rows-1,status,fg,bg);
            hm_frame(&pending,cells,old,term.cols,term.rows,force);dirty=force=hhh_resize=0;
        }
        int k=hhh_key(&term,0);
        if(k=='q'||k==3||k==27)break;
        if(k=='m'){mode=(mode+1)%4;dirty=force=1;}
        if(k==' '){paused=!paused;dirty=1;}
        if(k=='r'){if(source)hc_here_media_close(source);source=NULL;failed=0;error[0]=last_error[0]=0;revision=0;dirty=force=1;}
    }
    if(source)hc_here_media_close(source);
    /* Finish a partially emitted escape sequence before restoring the screen. */
    uint64_t until=hm_now()+200;while(offset<pending.n&&hm_now()<until){ssize_t n=write(1,pending.p+offset,pending.n-offset);if(n>0)offset+=(size_t)n;else{struct pollfd p={1,POLLOUT,0};poll(&p,1,10);}}
    fcntl(1,F_SETFL,flags);free(rgba);free(cells);free(old);free(picture);free(pending.p);if(control>=0)close(control);hhh_finish(&term);
    if(parent){memcpy(parent->input,term.input,(size_t)term.input_len);parent->input_len=term.input_len;}
    return 0;
}
static int hc_here_media_main(int argc,char **argv) { return hm_run(argc,argv,NULL); }
static int hm_open_file(const char *path,HHHTerm *parent) { char *args[]={"--here-media","--image",(char*)path,NULL};return hm_run(3,args,parent); }
static int hc_here_files_with_media(int argc,char **argv) { hhh_image_open=hm_open_file;return hc_here_files_main(argc,argv); }
#endif
