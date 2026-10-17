/* Native auxiliary views for `hardcore here`. Included by the one executable;
   the views run as ordinary child PTYs and therefore use Hardcore's VT engine.
   No interpreter, downloaded renderer, shell command construction, or plug-in
   is involved. The HTML reader deliberately exposes text and HTTP(S) links. */
#ifndef HARDCORE_HERE_HELPERS_H
#define HARDCORE_HERE_HELPERS_H
#ifndef _WIN32
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#endif

#define HHH_TEXT_MAX 160000
#define HHH_BODY_MAX (2 * 1024 * 1024)
#define HHH_URL_MAX 4096
#define HHH_LINK_MAX 300
static int hhh_image_path(const char *path) {
    const char *e=strrchr(path,'.');if(!e)return 0;
    return !strcasecmp(e,".png")||!strcasecmp(e,".jpg")||!strcasecmp(e,".jpeg")||!strcasecmp(e,".gif")||!strcasecmp(e,".webp")||!strcasecmp(e,".tif")||!strcasecmp(e,".tiff")||!strcasecmp(e,".heic")||!strcasecmp(e,".bmp");
}
typedef struct { char theme; int allow_host_font,font_size; char font_name[128]; } HereSettings;
typedef struct {
    int available,active; long window_id; double original_size;
    char tty[128],original_name[256],error[256];
} HCHereHostFont;

static volatile sig_atomic_t hhh_stop, hhh_suspend, hhh_resize;
static void hhh_signal(int sig) {
    if (sig == SIGWINCH) hhh_resize = 1;
    else if (sig == SIGTSTP) hhh_suspend = 1;
    else hhh_stop = 1;
}
typedef struct {
    struct termios saved;
    struct sigaction old[5];
    int entered, cols, rows, paste, string_control, string_escape;
    unsigned char input[8192];
    int input_len;
    void (*before_suspend)(void *);
    void *suspend_context;
} HHHTerm;
static int (*hhh_image_open)(const char *path, HHHTerm *parent);
static const int hhh_signals[] = {SIGTERM, SIGHUP, SIGINT, SIGWINCH, SIGTSTP};
static void hhh_size(HHHTerm *t) {
    struct winsize ws = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    t->cols = ws.ws_col >= 2 ? ws.ws_col : 80;
    t->rows = ws.ws_row >= 4 ? ws.ws_row : 24;
    if (t->cols > 500) t->cols = 500;
    if (t->rows > 200) t->rows = 200;
}
static void hhh_enter(HHHTerm *t) {
    struct termios raw = t->saved;
    cfmakeraw(&raw);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    fputs("\033[?1049h\033[?25l\033[?2004h\033[?1000h\033[?1006h", stdout);
    fflush(stdout); t->entered = 1; hhh_size(t);
}
static void hhh_leave(HHHTerm *t) {
    if (!t->entered) return;
    fputs("\033[0m\033[?7h\033[?1006l\033[?1000l\033[?2004l\033[?25h\033[?1049l", stdout);
    fflush(stdout); tcsetattr(STDIN_FILENO, TCSANOW, &t->saved); t->entered = 0;
}
static int hhh_start(HHHTerm *t) {
    memset(t, 0, sizeof(*t));
    if (!isatty(0) || !isatty(1) || tcgetattr(0, &t->saved)) {
        fputs("This Hardcore view needs an interactive terminal.\n", stderr); return 0;
    }
    hhh_stop = hhh_suspend = hhh_resize = 0;
    struct sigaction sa = {0}; sa.sa_handler = hhh_signal; sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 5; ++i) sigaction(hhh_signals[i], &sa, &t->old[i]);
    hhh_enter(t); return 1;
}
static void hhh_finish(HHHTerm *t) {
    hhh_leave(t);
    for (int i = 0; i < 5; ++i) sigaction(hhh_signals[i], &t->old[i], NULL);
}
static uint32_t hhh_cp(const char *s, size_t n, size_t *used) {
    unsigned char a = (unsigned char)*s;
    *used = 1;
    if (a < 128) return a;
    int k = a >= 0xc2 && a <= 0xdf ? 2 : a >= 0xe0 && a <= 0xef ? 3 : a >= 0xf0 && a <= 0xf4 ? 4 : 0;
    if (!k || n < (size_t)k) return 0xfffd;
    uint32_t cp = a & ((1u << (7-k)) - 1);
    for (int i = 1; i < k; ++i) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xc0) != 0x80) return 0xfffd;
        cp = cp << 6 | (b & 63);
    }
    if ((k == 2 && cp < 0x80) || (k == 3 && cp < 0x800) || (k == 4 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0xfffd;
    *used = k; return cp;
}
static int hhh_printable(uint32_t c) {
    /* Remote text must never become terminal controls, including C1 and bidi. */
    return c >= 32 && !(c >= 0x7f && c <= 0x9f) && c != 0x061c &&
        !(c >= 0x200b && c <= 0x200f) && !(c >= 0x202a && c <= 0x202e) &&
        !(c >= 0x2060 && c <= 0x206f) && c != 0xfeff;
}
static void hhh_line(int row, int cols, const char *style, const char *text) {
    printf("\033[%d;1H%s", row, style);
    if(!text)text="";
    size_t n = strlen(text), p = 0; int x = 0;
    while (p < n) {
        size_t z; uint32_t c = hhh_cp(text + p, n - p, &z); p += z;
        if (c == '\t') c = ' ';
        if (!hhh_printable(c)) continue;
        int w = cp_width(c); if (w < 0) w = 1;
        if (x + w > cols) break;
        char u[4]; int len = utf8_encode(c, u); fwrite(u, 1, len, stdout); x += w;
    }
    while (x++ < cols) putchar(' ');
}
enum { HHH_UP = 0x110000, HHH_DOWN, HHH_LEFT, HHH_RIGHT, HHH_PGUP, HHH_PGDN, HHH_HOME, HHH_END };
static void hhh_consume(HHHTerm *t, int n) {
    t->input_len -= n; memmove(t->input, t->input+n, t->input_len);
}
/* Complete escape sequences are consumed atomically. Paste is text only and
   never interpreted as menu shortcuts, even across fragmented reads. */
static int hhh_key(HHHTerm *t, int allow_paste) {
    if (hhh_suspend) {
        if (t->before_suspend) t->before_suspend(t->suspend_context);
        hhh_leave(t); hhh_suspend = 0; kill(getpid(), SIGSTOP); hhh_enter(t); hhh_resize = 1;
    }
    struct pollfd pfd = {0, POLLIN, 0};
    if (!t->input_len) {
        int ready = poll(&pfd, 1, 80);
        if (ready <= 0) return 0;
        ssize_t n = read(0, t->input, sizeof(t->input));
        if (n <= 0) { if (n == 0 || errno != EINTR) hhh_stop = 1; return 0; }
        t->input_len = (int)n;
    }
    while (t->input_len) {
        unsigned char *s = t->input; int n = t->input_len;
        if(t->string_control) {
            unsigned char c=s[0];hhh_consume(t,1);
            if(c==7 || (t->string_escape && c=='\\'))t->string_control=0;
            t->string_escape=c==27;continue;
        }
        if (s[0] == 27) {
            if (n == 1 || (s[1] == '[' && n < 3)) {
                if (poll(&pfd, 1, 25) > 0) {
                    ssize_t z = read(0, s+n, sizeof(t->input)-n);
                    if (z > 0) { t->input_len += (int)z; continue; }
                }
                hhh_consume(t, n); return t->paste ? 0 : 27;
            }
            if (s[1] == '[') {
                int end = 2;
                while (end < n && !(s[end] >= 0x40 && s[end] <= 0x7e)) ++end;
                if (end == n) {
                    if (n < 128 && poll(&pfd, 1, 25) > 0) {
                        ssize_t z = read(0, s+n, sizeof(t->input)-n);
                        if (z > 0) { t->input_len += (int)z; continue; }
                    }
                    hhh_consume(t, n); return 0;
                }
                char seq[130]; int count = end+1;
                int l = count < 129 ? count : 129; memcpy(seq, s, l); seq[l] = 0;
                hhh_consume(t, count);
                if (!strcmp(seq, "\033[200~")) { t->paste = 1; continue; }
                if (!strcmp(seq, "\033[201~")) { t->paste = 0; continue; }
                if (t->paste) continue;
                if (!strcmp(seq, "\033[A")) return HHH_UP;
                if (!strcmp(seq, "\033[B")) return HHH_DOWN;
                if (!strcmp(seq, "\033[C")) return HHH_RIGHT;
                if (!strcmp(seq, "\033[D")) return HHH_LEFT;
                if (!strcmp(seq, "\033[5~")) return HHH_PGUP;
                if (!strcmp(seq, "\033[6~")) return HHH_PGDN;
                if (!strcmp(seq, "\033[H") || !strcmp(seq, "\033[1~")) return HHH_HOME;
                if (!strcmp(seq, "\033[F") || !strcmp(seq, "\033[4~")) return HHH_END;
                int b, x, y;
                if (sscanf(seq, "\033[<%d;%d;%dM", &b, &x, &y) == 3 && seq[strlen(seq)-1] == 'M') {
                    if (b == 64) return HHH_UP;
                    if (b == 65) return HHH_DOWN;
                }
                continue;
            }
            /* Consume OSC/DCS reports rather than executing their text. */
            if (s[1] == ']' || s[1] == 'P') {
                hhh_consume(t,2);t->string_control=1;t->string_escape=0;continue;
            }
            hhh_consume(t, 2); continue;
        }
        int expected=s[0]>=0xc2&&s[0]<=0xdf?2:s[0]>=0xe0&&s[0]<=0xef?3:s[0]>=0xf0&&s[0]<=0xf4?4:1;
        if(n<expected && poll(&pfd,1,25)>0) {
            ssize_t z=read(0,s+n,sizeof(t->input)-n);
            if(z>0){t->input_len+=(int)z;continue;}
        }
        size_t used; uint32_t cp = hhh_cp((const char*)s, n, &used);
        hhh_consume(t, (int)used);
        if (t->paste) return allow_paste && hhh_printable(cp) ? (int)cp : 0;
        return (int)cp;
    }
    return 0;
}
static int hhh_open_external(const char *target) {
    pid_t pid = fork();
    if (!pid) {
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) { dup2(nullfd,0); dup2(nullfd,1); dup2(nullfd,2); if (nullfd > 2) close(nullfd); }
#ifdef __APPLE__
        execl("/usr/bin/open", "open", "--", target, (char*)NULL);
#else
        execlp("xdg-open", "xdg-open", target, (char*)NULL);
#endif
        _exit(127);
    }
    if (pid < 0) return 0;
    int status; while (waitpid(pid,&status,0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) && !WEXITSTATUS(status);
}

typedef struct { char *name; int dir; off_t size; } HHHFile;
static int hhh_file_compare(const void *aa,const void *bb) {
    const HHHFile *a=aa,*b=bb;
    if (a->dir != b->dir) return b->dir-a->dir;
    return strcasecmp(a->name,b->name);
}
static void hhh_files_free(HHHFile *f,int n) { for(int i=0;i<n;++i) free(f[i].name); free(f); }
static HHHFile *hhh_files_read(const char *path,int *count,char *error,size_t cap) {
    *count=0; DIR *d=opendir(path);
    if(!d) { snprintf(error,cap,"%s",strerror(errno)); return NULL; }
    HHHFile *files=calloc(8192,sizeof(*files));
    if(!files) { closedir(d); snprintf(error,cap,"Out of memory"); return NULL; }
    struct dirent *ent;
    while ((ent=readdir(d)) && *count<8192) {
        if(!strcmp(ent->d_name,".")) continue;
        struct stat st={0}; char full[PATH_MAX];
        if(snprintf(full,sizeof(full),"%s/%s",path,ent->d_name)>=(int)sizeof(full)) continue;
        if(stat(full,&st)) continue;
        char *name=strdup(ent->d_name); if(!name) break;
        files[(*count)++]=(HHHFile){name,S_ISDIR(st.st_mode),st.st_size};
    }
    closedir(d); qsort(files,*count,sizeof(*files),hhh_file_compare); return files;
}
typedef struct { char **line; int count,cap; } HHHLines;
static void hhh_lines_free(HHHLines *ls) {
    for(int i=0;i<ls->count;++i) free(ls->line[i]);
    free(ls->line); memset(ls,0,sizeof(*ls));
}
static int hhh_lines_add(HHHLines *ls,const char *s,size_t n) {
    if(ls->count>=HHH_TEXT_MAX) return 0;
    if(ls->count==ls->cap) {
        int cap=ls->cap ? ls->cap*2 : 128;
        char **p=realloc(ls->line,(size_t)cap*sizeof(*p)); if(!p) return 0;
        ls->line=p; ls->cap=cap;
    }
    char *p=malloc(n+1); if(!p) return 0;
    memcpy(p,s,n); p[n]=0; ls->line[ls->count++]=p; return 1;
}
static void hhh_wrap(HHHLines *ls,const char *text,int cols) {
    hhh_lines_free(ls); if(cols<1) cols=1;
    size_t n=strlen(text), start=0,p=0,lastspace=0; int width=0;
    while(p<n) {
        size_t used; uint32_t cp=hhh_cp(text+p,n-p,&used);
        if(cp=='\n') { if(!hhh_lines_add(ls,text+start,p-start)) return; p+=used;start=p;width=0;lastspace=0;continue; }
        if(cp==' ' || cp=='\t') lastspace=p;
        int w=cp=='\t' ? 1 : cp_width(cp); if(w<0) w=1;
        if(width+w>cols && p>start) {
            size_t cut=lastspace>start+(p-start)/2 ? lastspace : p;
            if(!hhh_lines_add(ls,text+start,cut-start)) return;
            p=cut; if(text[p]==' ') ++p;
            start=p;width=0;lastspace=0;continue;
        }
        width+=w;p+=used;
    }
    hhh_lines_add(ls,text+start,p-start);
}
static int hhh_preview(HHHTerm *t,const char *path) {
    FILE *f=fopen(path,"rb"); if(!f) return 0;
    char *text=calloc(1,HHH_BODY_MAX+1); if(!text) { fclose(f);return 0; }
    size_t n=fread(text,1,HHH_BODY_MAX,f); fclose(f); text[n]=0;
    if(memchr(text,0,n)) { free(text);text=strdup("[Binary file: press o to open in the system application]"); }
    HHHLines lines={0}; int offset=0,oldcols=0,dirty=1;
    while(!hhh_stop) {
        hhh_size(t);
        if(oldcols!=t->cols) { hhh_wrap(&lines,text,t->cols);oldcols=t->cols;dirty=1; }
        int body=t->rows-2;
        if(offset>lines.count-body) offset=lines.count-body;
        if(offset<0) offset=0;
        if(dirty || hhh_resize) {
            hhh_line(1,t->cols,"\033[1;7m",path);
            for(int r=0;r<body;++r) hhh_line(r+2,t->cols,"\033[0m",offset+r<lines.count ? lines.line[offset+r] : "");
            hhh_line(t->rows,t->cols,"\033[0;7m","j/k scroll | Space page | o system app | q/Esc back");
            fputs("\033[0m",stdout);fflush(stdout);dirty=hhh_resize=0;
        }
        int k=hhh_key(t,0); if(!k) continue;dirty=1;
        if(k=='q'||k==27||k==HHH_LEFT||k==127) break;
        if(k==3) { hhh_stop=1;break; }
        if(k=='j'||k==HHH_DOWN) ++offset;
        if(k=='k'||k==HHH_UP) --offset;
        if(k==' '||k==HHH_PGDN) offset+=body;
        if(k==HHH_PGUP) offset-=body;
        if(k=='g'||k==HHH_HOME) offset=0;
        if(k=='G'||k==HHH_END) offset=lines.count;
        if(k=='o') hhh_open_external(path);
    }
    hhh_lines_free(&lines);free(text);return 1;
}
static int hc_here_files_main(int argc,char **argv) {
    const char *requested=argc>1 ? argv[1] : "."; char path[PATH_MAX],error[256]="";
    if(!realpath(requested,path)) { fprintf(stderr,"Files: %s\n",strerror(errno));return 1; }
    HHHTerm t; if(!hhh_start(&t)) return 2;
    int count=0,selected=0,top=0,dirty=1; HHHFile *files=hhh_files_read(path,&count,error,sizeof(error));
    while(!hhh_stop) {
        hhh_size(&t);int body=t.rows-3;
        if(selected>=count) selected=count-1; if(selected<0) selected=0;
        if(selected<top) top=selected; if(selected>=top+body) top=selected-body+1;
        if(dirty || hhh_resize) {
            hhh_line(1,t.cols,"\033[1;7m","Hardcore files");
            hhh_line(2,t.cols,"\033[0;36m",path);
            for(int r=0;r<body;++r) {
                char line[PATH_MAX+64]="";int i=top+r;
                if(i<count) snprintf(line,sizeof(line),"%s %s%s",i==selected?">":" ",files[i].name,files[i].dir?"/":"");
                hhh_line(r+3,t.cols,i==selected?"\033[0;7m":"\033[0m",line);
            }
            hhh_line(t.rows,t.cols,"\033[0;7m",error[0]?error:"j/k select | Enter open | Backspace parent | o system app | r refresh | q close");
            fputs("\033[0m",stdout);fflush(stdout);dirty=hhh_resize=0;
        }
        int k=hhh_key(&t,0);if(!k)continue;dirty=1;error[0]=0;
        if(k=='q'||k==3||k==27)break;
        if(k=='j'||k==HHH_DOWN)++selected;
        if(k=='k'||k==HHH_UP)--selected;
        if(k==HHH_PGDN)selected+=body;
        if(k==HHH_PGUP)selected-=body;
        if(k=='g'||k==HHH_HOME)selected=0;
        if(k=='G'||k==HHH_END)selected=count-1;
        int reload=k=='r';
        if(k==127||k==8||k==HHH_LEFT) {
            char full[PATH_MAX];if(snprintf(full,sizeof(full),"%s/..",path)<(int)sizeof(full) && realpath(full,path)) {reload=1;selected=top=0;}
        }
        if((k=='\r'||k=='\n'||k==HHH_RIGHT||k=='o') && selected>=0 && selected<count) {
            char full[PATH_MAX];
            if(snprintf(full,sizeof(full),"%s/%s",path,files[selected].name)>=(int)sizeof(full)) snprintf(error,sizeof(error),"Path is too long");
            else if(k=='o') { if(!hhh_open_external(full)) snprintf(error,sizeof(error),"Could not open system application"); }
            else if(files[selected].dir) { if(realpath(full,path)) {reload=1;selected=top=0;} }
            else if(hhh_image_open && hhh_image_path(full)){hhh_image_open(full,&t);hhh_enter(&t);fputs("\033]0;Files\007",stdout);dirty=1;}
            else if(!hhh_preview(&t,full)) snprintf(error,sizeof(error),"Could not read file: %s",strerror(errno));
        }
        if(reload) {hhh_files_free(files,count);files=hhh_files_read(path,&count,error,sizeof(error));}
    }
    hhh_files_free(files,count);hhh_finish(&t);return 0;
}

#ifdef __APPLE__
static NSString *hhh_settings_path(void) {
    const char *xdg=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");
    NSString *root=xdg&&*xdg?[NSString stringWithUTF8String:xdg]:[[NSString stringWithUTF8String:home?home:""] stringByAppendingPathComponent:@"Library/Application Support"];
    return [root stringByAppendingPathComponent:@"hardcore/here.json"];
}
static void here_settings_load(HereSettings *s) {
    memset(s,0,sizeof(*s));s->theme='g';s->font_size=14;snprintf(s->font_name,sizeof(s->font_name),"Menlo-Regular");
    @autoreleasepool {
        int fd=open([hhh_settings_path() fileSystemRepresentation],O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0)return;struct stat st;
        if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||st.st_size>16384||st.st_size<0){close(fd);return;}
        char buf[16385];size_t total=0;
        while(total<sizeof(buf)){ssize_t n=read(fd,buf+total,sizeof(buf)-total);if(n<=0)break;total+=(size_t)n;}
        close(fd);if(total>16384)return;
        id obj=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:buf length:total] options:0 error:NULL];
        if(![obj isKindOfClass:[NSDictionary class]])return;
        id theme=obj[@"theme"],name=obj[@"font_name"],size=obj[@"font_size"],allow=obj[@"allow_host_font"];
        if([theme isKindOfClass:[NSString class]]&&[theme length]==1&&[theme characterAtIndex:0]>0&&[theme characterAtIndex:0]<128&&strchr("hdmngsockrGSOCKR",[theme characterAtIndex:0]))s->theme=(char)[theme characterAtIndex:0];
        if([name isKindOfClass:[NSString class]]&&[name length]&&[name lengthOfBytesUsingEncoding:NSUTF8StringEncoding]<sizeof(s->font_name)) {
            BOOL valid=YES;for(NSUInteger i=0;i<[name length];++i){unichar c=[name characterAtIndex:i];if(c<32||c==127)valid=NO;}
            if(valid)snprintf(s->font_name,sizeof(s->font_name),"%s",[name UTF8String]);
        }
        if([size isKindOfClass:[NSNumber class]]&&CFGetTypeID((CFTypeRef)size)!=CFBooleanGetTypeID()&&[size doubleValue]==[size intValue]&&[size intValue]>=6&&[size intValue]<=48)s->font_size=[size intValue];
        if([allow isKindOfClass:[NSNumber class]]&&CFGetTypeID((CFTypeRef)allow)==CFBooleanGetTypeID())s->allow_host_font=[allow boolValue];
    }
}
static int here_settings_save(const HereSettings *s) {
    if(!s->theme||!strchr("hdmngsockrGSOCKR",s->theme)||s->font_size<6||s->font_size>48||!memchr(s->font_name,0,sizeof(s->font_name))||!s->font_name[0])return 0;
    for(const unsigned char *p=(const unsigned char*)s->font_name;*p;++p)if(*p<32||*p==127)return 0;
    @autoreleasepool {
        NSString *path=hhh_settings_path(),*dir=[path stringByDeletingLastPathComponent];
        if(![[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:NULL])return 0;
        NSString *name=[NSString stringWithUTF8String:s->font_name];if(!name)return 0;
        NSDictionary *obj=@{@"theme":[NSString stringWithFormat:@"%c",s->theme],@"font_name":name,@"font_size":@(s->font_size),@"allow_host_font":s->allow_host_font?@YES:@NO};
        NSData *data=[NSJSONSerialization dataWithJSONObject:obj options:NSJSONWritingSortedKeys error:NULL];if(!data)return 0;
        char temporary[PATH_MAX];if(snprintf(temporary,sizeof(temporary),"%s/.here-XXXXXX",[dir fileSystemRepresentation])>=(int)sizeof(temporary))return 0;
        int fd=mkstemp(temporary);if(fd<0)return 0;fchmod(fd,0600);
        size_t off=0,len=[data length];const char *bytes=[data bytes];int ok=1;
        while(off<len){ssize_t n=write(fd,bytes+off,len-off);if(n<0&&errno==EINTR)continue;if(n<=0){ok=0;break;}off+=(size_t)n;}
        if(fsync(fd))ok=0;if(close(fd))ok=0;
        if(ok&&rename(temporary,[path fileSystemRepresentation]))ok=0;
        if(!ok)unlink(temporary);return ok;
    }
}
static NSString *hhh_safe(NSString *s) {
    if(!s) return @"";
    NSUInteger limit=MIN([s length],(NSUInteger)HHH_TEXT_MAX);
    if(limit && CFStringIsSurrogateHighCharacter([s characterAtIndex:limit-1]))--limit;
    unichar *buffer=malloc(MAX(limit,(NSUInteger)1)*sizeof(*buffer));if(!buffer)return @"";
    NSUInteger length=0;
    for(NSUInteger i=0;i<limit;++i) {
        unichar c=[s characterAtIndex:i];
        if(c=='\n'||c=='\t'||hhh_printable(c))buffer[length++]=c;
    }
    NSString *result=[NSString stringWithCharacters:buffer length:length];free(buffer);
    return result;
}
static NSString *hhh_url(NSString *value,NSString *base) {
    if(![value isKindOfClass:[NSString class]] || [value length]>HHH_URL_MAX) return nil;
    for(NSUInteger i=0;i<[value length];++i) {unichar c=[value characterAtIndex:i];if(c<32||c==127)return nil;}
    value=[value stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if(![value length])return nil;
    if(!base && [value rangeOfString:@":"].location==NSNotFound)value=[@"https://" stringByAppendingString:value];
    NSURL *url=base ? [NSURL URLWithString:value relativeToURL:[NSURL URLWithString:base]] : [NSURL URLWithString:value];
    url=[url absoluteURL]; NSString *scheme=[[url scheme] lowercaseString];
    if(!([scheme isEqual:@"http"]||[scheme isEqual:@"https"]) || ![[url host] length] || [url user] || [url password])return nil;
    NSString *result=[url absoluteString];return [result length]<=HHH_URL_MAX ? result : nil;
}
static NSString *hhh_entities(NSString *s) {
    NSMutableString *out=[NSMutableString string];NSUInteger n=[s length];
    for(NSUInteger i=0;i<n;++i) {
        unichar c=[s characterAtIndex:i];
        if(c=='&') {
            NSUInteger end=i+1;while(end<n && end<i+16 && [s characterAtIndex:end]!=';' && [s characterAtIndex:end]!=' ')++end;
            if(end<n && [s characterAtIndex:end]==';') {
                NSString *e=[s substringWithRange:NSMakeRange(i+1,end-i-1)];uint32_t cp=0;
                if([e isEqual:@"amp"])cp='&';else if([e isEqual:@"lt"])cp='<';else if([e isEqual:@"gt"])cp='>';
                else if([e isEqual:@"quot"])cp='"';else if([e isEqual:@"apos"])cp='\'';else if([e isEqual:@"nbsp"])cp=' ';
                else if([e isEqual:@"ndash"])cp=0x2013;else if([e isEqual:@"mdash"])cp=0x2014;else if([e isEqual:@"hellip"])cp=0x2026;
                else if([e hasPrefix:@"#"]) {
                    const char *p=[e UTF8String]+1;int base=10;if(*p=='x'||*p=='X'){++p;base=16;}
                    char *tail=NULL;unsigned long number=strtoul(p,&tail,base);
                    if(tail!=p && !*tail && number<=0x10ffff && !(number>=0xd800&&number<=0xdfff))cp=(uint32_t)number;
                }
                if(cp && (hhh_printable(cp)||cp=='\n'||cp=='\t')) {
                    if(cp<=0xffff)[out appendFormat:@"%C",(unichar)cp];else {cp-=0x10000;[out appendFormat:@"%C%C",(unichar)(0xd800+(cp>>10)),(unichar)(0xdc00+(cp&1023))];}
                    i=end;continue;
                }
                if([e hasPrefix:@"#"]){i=end;continue;}
            }
        }
        [out appendFormat:@"%C",c];
    }
    return hhh_safe(out);
}
static NSDictionary *hhh_attributes(NSString *tag) {
    NSMutableDictionary *attrs=[NSMutableDictionary dictionary];NSUInteger n=[tag length],p=0;
    NSCharacterSet *ws=[NSCharacterSet whitespaceAndNewlineCharacterSet];
    while(p<n && ![ws characterIsMember:[tag characterAtIndex:p]])++p;
    while(p<n) {
        while(p<n && ([ws characterIsMember:[tag characterAtIndex:p]]||[tag characterAtIndex:p]=='/'))++p;
        NSUInteger start=p;
        while(p<n && ![ws characterIsMember:[tag characterAtIndex:p]] && [tag characterAtIndex:p]!='=' && [tag characterAtIndex:p]!='/')++p;
        if(p==start){++p;continue;}
        NSString *name=[[tag substringWithRange:NSMakeRange(start,p-start)] lowercaseString];
        while(p<n && [ws characterIsMember:[tag characterAtIndex:p]])++p;
        NSString *value=@"";
        if(p<n && [tag characterAtIndex:p]=='=') {
            ++p;while(p<n && [ws characterIsMember:[tag characterAtIndex:p]])++p;
            unichar quote=p<n?[tag characterAtIndex:p]:0;
            if(quote=='\''||quote=='"') {
                start=++p;while(p<n&&[tag characterAtIndex:p]!=quote)++p;
                value=[tag substringWithRange:NSMakeRange(start,p-start)];if(p<n)++p;
            } else {start=p;while(p<n&&![ws characterIsMember:[tag characterAtIndex:p]])++p;value=[tag substringWithRange:NSMakeRange(start,p-start)];}
        }
        if(!attrs[name])attrs[name]=hhh_entities(value);
    }
    return attrs;
}
static void hhh_append(NSMutableString *text,NSString *value) {
    NSUInteger room=HHH_TEXT_MAX-[text length];
    if([value length]>room) {
        if(room && CFStringIsSurrogateHighCharacter([value characterAtIndex:room-1]))--room;
        value=[value substringToIndex:room];
    }
    [text appendString:value];
}
static void hhh_string_set(NSString **target,NSString *value) {
    [value retain];[*target release];*target=value;
}
static NSDictionary *hhh_html(NSString *html,NSString *url,int plain) {
    if(plain)return @{@"url":url,@"title":[[NSURL URLWithString:url] host]?:@"Web",@"text":hhh_safe(html),@"links":@[]};
    NSMutableString *text=[NSMutableString string],*title=[NSMutableString string];
    NSMutableArray *stack=[NSMutableArray array],*links=[NSMutableArray array];
    NSSet *blocks=[NSSet setWithArray:@[@"address",@"article",@"aside",@"blockquote",@"br",@"dd",@"div",@"dl",@"dt",@"footer",@"h1",@"h2",@"h3",@"h4",@"h5",@"h6",@"header",@"hr",@"li",@"main",@"nav",@"ol",@"p",@"pre",@"section",@"table",@"tr",@"ul"]];
    NSSet *hiddenTags=[NSSet setWithArray:@[@"head",@"title",@"script",@"style",@"template",@"noscript",@"svg"]];
    NSSet *voidTags=[NSSet setWithArray:@[@"area",@"base",@"br",@"col",@"embed",@"hr",@"img",@"input",@"link",@"meta",@"param",@"source",@"track",@"wbr"]];
    NSCharacterSet *ws=[NSCharacterSet whitespaceAndNewlineCharacterSet];
    NSUInteger p=0,n=[html length];NSString *base=url;
    while(p<n && [text length]<HHH_TEXT_MAX) {
        if([html characterAtIndex:p]!='<') {
            NSUInteger begin=p;while(p<n&&[html characterAtIndex:p]!='<')++p;
            NSString *raw=[html substringWithRange:NSMakeRange(begin,p-begin)];
            if([stack count] && [stack.lastObject[@"tag"] isEqual:@"title"] && [title length]<512) hhh_append(title,hhh_entities(raw));
            if([stack count] && [stack.lastObject[@"hidden"] boolValue])continue;
            NSString *value=hhh_entities(raw);BOOL pre=NO;
            for(NSDictionary *s in stack)if([s[@"tag"] isEqual:@"pre"]){pre=YES;break;}
            if(!pre) {
                NSMutableString *collapsed=[NSMutableString string];BOOL space=![text length]||[text hasSuffix:@" "]||[text hasSuffix:@"\n"];
                for(NSUInteger i=0;i<[value length];++i) {
                    unichar c=[value characterAtIndex:i];
                    if([ws characterIsMember:c]) {if(!space)[collapsed appendString:@" "];space=YES;}
                    else {[collapsed appendFormat:@"%C",c];space=NO;}
                }
                value=collapsed;
            }
            hhh_append(text,value);continue;
        }
        if(p+4<=n && [[html substringWithRange:NSMakeRange(p,4)] isEqual:@"<!--"]) {
            NSRange end=[html rangeOfString:@"-->" options:0 range:NSMakeRange(p+4,n-p-4)];p=end.location==NSNotFound?n:NSMaxRange(end);continue;
        }
        NSUInteger begin=++p;unichar quote=0;
        while(p<n) {unichar c=[html characterAtIndex:p];if(quote){if(c==quote)quote=0;}else if(c=='\''||c=='"')quote=c;else if(c=='>')break;++p;}
        if(p==n)break;
        NSString *raw=[[html substringWithRange:NSMakeRange(begin,p-begin)] stringByTrimmingCharactersInSet:ws];++p;
        BOOL end=[raw hasPrefix:@"/"];if(end)raw=[raw substringFromIndex:1];
        NSUInteger len=0;while(len<[raw length]&&![ws characterIsMember:[raw characterAtIndex:len]]&&[raw characterAtIndex:len]!='/')++len;
        NSString *tag=[[raw substringToIndex:len] lowercaseString];if(![tag length]||[tag hasPrefix:@"!"]||[tag hasPrefix:@"?"])continue;
        if(end) {
            for(NSInteger i=(NSInteger)[stack count]-1;i>=0;--i)if([stack[i][@"tag"] isEqual:tag]) {
                NSDictionary *frame=stack[i];
                if(![frame[@"hidden"] boolValue]) {
                    if([frame[@"link"] intValue])hhh_append(text,[NSString stringWithFormat:@" [%@]",frame[@"link"]]);
                    if([blocks containsObject:tag] && ![text hasSuffix:@"\n\n"])hhh_append(text,[text hasSuffix:@"\n"]?@"\n":@"\n\n");
                }
                [stack removeObjectsInRange:NSMakeRange(i,[stack count]-i)];break;
            }
            continue;
        }
        NSDictionary *attrs=hhh_attributes(raw);NSString *style=[attrs[@"style"] lowercaseString]?:@"";
        style=[[style componentsSeparatedByCharactersInSet:ws] componentsJoinedByString:@""];
        BOOL hidden=([stack count]&&[stack.lastObject[@"hidden"] boolValue])||[hiddenTags containsObject:tag]||attrs[@"hidden"]||[[attrs[@"aria-hidden"] lowercaseString] isEqual:@"true"]||[style containsString:@"display:none"]||[style containsString:@"visibility:hidden"];
        if([tag isEqual:@"base"]&&attrs[@"href"]) {NSString *candidate=hhh_url(attrs[@"href"],url);if(candidate)base=candidate;}
        int number=0;
        if(!hidden) {
            if([blocks containsObject:tag]&&[text length]&&![text hasSuffix:@"\n\n"])hhh_append(text,([tag isEqual:@"br"]||[text hasSuffix:@"\n"])?@"\n":@"\n\n");
            if([tag isEqual:@"li"])hhh_append(text,@"• ");
            if([tag isEqual:@"td"])hhh_append(text,@"  ");
            if([tag isEqual:@"img"]&&[attrs[@"alt"] length])hhh_append(text,[NSString stringWithFormat:@"[Image: %@] ",attrs[@"alt"]]);
            if([tag isEqual:@"a"]&&attrs[@"href"]&&[links count]<HHH_LINK_MAX) {
                NSString *target=hhh_url(attrs[@"href"],base);
                if(target) {NSUInteger index=[links indexOfObject:target];if(index==NSNotFound){index=[links count];[links addObject:target];}number=(int)index+1;}
            }
        }
        if(![voidTags containsObject:tag]&&![raw hasSuffix:@"/"]) {
            if([stack count]>=256)break;
            [stack addObject:@{@"tag":tag,@"hidden":@(hidden),@"link":@(number)}];
        }
    }
    if([text length]>=HHH_TEXT_MAX)[text appendString:@"\n[Page truncated at text limit]"];
    NSString *heading=[title stringByTrimmingCharactersInSet:ws];
    if([heading length]>200){NSUInteger end=200;if(CFStringIsSurrogateHighCharacter([heading characterAtIndex:end-1]))--end;heading=[heading substringToIndex:end];}
    return @{@"url":url,@"title":[heading length]?heading:([[NSURL URLWithString:url] host]?:@"Web"),@"text":[text stringByTrimmingCharactersInSet:ws],@"links":links};
}

@interface HCHereFetch : NSObject <NSURLSessionDataDelegate> {
@public NSMutableData *body; NSString *error,*url,*encoding,*mime; BOOL done; int redirects;
    NSURLSession *session; NSURLSessionDataTask *task; CFAbsoluteTime started;
}
- (id)initWithURL:(NSString*)url;
- (void)cancel;
@end
@implementation HCHereFetch
- (id)initWithURL:(NSString*)value {
    if((self=[super init])) {
        body=[[NSMutableData alloc] init];url=[value copy];started=CFAbsoluteTimeGetCurrent();
        NSURLSessionConfiguration *cfg=[NSURLSessionConfiguration ephemeralSessionConfiguration];
        cfg.timeoutIntervalForRequest=5;cfg.timeoutIntervalForResource=15;cfg.HTTPShouldSetCookies=NO;cfg.HTTPCookieStorage=nil;cfg.URLCache=nil;cfg.URLCredentialStorage=nil;
        NSOperationQueue *queue=[[[NSOperationQueue alloc] init] autorelease];queue.maxConcurrentOperationCount=1;
        session=[[NSURLSession sessionWithConfiguration:cfg delegate:self delegateQueue:queue] retain];
        NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:value]];
        [request setValue:@"Hardcore-text-reader/2.0" forHTTPHeaderField:@"User-Agent"];
        [request setValue:@"text/html,application/xhtml+xml,text/plain;q=0.9" forHTTPHeaderField:@"Accept"];
        task=[[session dataTaskWithRequest:request] retain];[task resume];
    }
    return self;
}
- (void)cancel { [task cancel];[session invalidateAndCancel]; }
- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t willPerformHTTPRedirection:(NSHTTPURLResponse*)r newRequest:(NSURLRequest*)request completionHandler:(void (^)(NSURLRequest*))completion {
    NSString *safe=hhh_url([[request URL] absoluteString],nil);
    @synchronized(self) {
        if(++redirects>5||!safe) {if(!error)error=[@"Unsafe or excessive redirects" copy];done=YES;completion(nil);return;}
        [url release];url=[safe copy];
    }
    completion(request);
}
- (void)URLSession:(NSURLSession*)s dataTask:(NSURLSessionDataTask*)t didReceiveResponse:(NSURLResponse*)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    @synchronized(self) {
        if(error){done=YES;completion(NSURLSessionResponseCancel);return;}
        NSInteger status=[response isKindOfClass:[NSHTTPURLResponse class]]?[(NSHTTPURLResponse*)response statusCode]:0;
        NSString *type=[[response MIMEType] lowercaseString];
        if(status<200||status>=300)error=[[NSString stringWithFormat:@"HTTP %ld",(long)status] copy];
        else if(response.expectedContentLength>HHH_BODY_MAX)error=[@"Page exceeds the 2 MiB limit" copy];
        else if(!([type isEqual:@"text/html"]||[type isEqual:@"application/xhtml+xml"]||[type isEqual:@"text/plain"]))error=[@"This content needs a full browser; press o" copy];
        mime=[type copy];encoding=[[response textEncodingName] copy];
        if(error){done=YES;completion(NSURLSessionResponseCancel);}else completion(NSURLSessionResponseAllow);
    }
}
- (void)URLSession:(NSURLSession*)s dataTask:(NSURLSessionDataTask*)t didReceiveData:(NSData*)data {
    @synchronized(self) {
        if(done)return;
        if([body length]+[data length]>HHH_BODY_MAX) {error=[@"Page exceeds the 2 MiB limit" copy];done=YES;[task cancel];}
        else [body appendData:data];
    }
}
- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t didCompleteWithError:(NSError*)err {
    @synchronized(self) {if(err&&!error)error=[hhh_safe([err localizedDescription]) copy];done=YES;}
    [session finishTasksAndInvalidate];
}
- (void)dealloc { [body release];[error release];[url release];[encoding release];[mime release];[task release];[session release];[super dealloc]; }
@end

static int hc_here_web_main(int argc,char **argv) {
    @autoreleasepool {
        NSString *url=[hhh_url(argc>1?[NSString stringWithUTF8String:argv[1]]:@"https://www.wikipedia.org/",nil) retain];
        if(!url){fputs("Web: enter an HTTP or HTTPS URL without embedded credentials.\n",stderr);return 2;}
        HHHTerm t;if(!hhh_start(&t)){[url release];return 2;}
        NSMutableArray *history=[[NSMutableArray alloc] init];NSDictionary *page=nil;HCHereFetch *fetch=nil;
        NSString *message=[@"Loading page…" retain];NSMutableString *input=[[NSMutableString alloc] init];
        HHHLines lines={0};int offset=0,oldcols=0,dirty=1,prompt=0,navigate=1;
        while(!hhh_stop) {
          @autoreleasepool {
            if(navigate) {
                if(fetch){[fetch cancel];[fetch release];fetch=nil;}
                [page release];page=nil;hhh_lines_free(&lines);offset=0;
                fetch=[[HCHereFetch alloc] initWithURL:url];hhh_string_set(&message,@"Loading page…");navigate=0;dirty=1;
            }
            if(fetch) {
                BOOL completed=NO;
                @synchronized(fetch) {
                    if(!fetch->done && CFAbsoluteTimeGetCurrent()-fetch->started>=15) {fetch->error=[@"Page request timed out" copy];fetch->done=YES;[fetch cancel];}
                    if(fetch->done) {
                        completed=YES;
                        if(fetch->error)hhh_string_set(&message,fetch->error);
                        else {
                            NSStringEncoding enc=NSUTF8StringEncoding;
                            if(fetch->encoding) {CFStringEncoding cf=CFStringConvertIANACharSetNameToEncoding((CFStringRef)fetch->encoding);if(cf!=kCFStringEncodingInvalidId)enc=CFStringConvertEncodingToNSStringEncoding(cf);}
                            NSString *html=[[[NSString alloc] initWithData:fetch->body encoding:enc] autorelease];
                            if(!html)html=[[[NSString alloc] initWithData:fetch->body encoding:NSISOLatin1StringEncoding] autorelease];
                            page=[hhh_html(html?:@"",fetch->url,[fetch->mime isEqual:@"text/plain"]) retain];hhh_string_set(&url,page[@"url"]);
                            hhh_string_set(&message,[NSString stringWithFormat:@"%lu links · text view; JavaScript and forms are not supported",(unsigned long)[page[@"links"] count]]);oldcols=0;
                        }
                        dirty=1;
                    }
                }
                if(completed){[fetch cancel];[fetch release];fetch=nil;}
            }
            hhh_size(&t);int bodyrows=t.rows-4;
            if(oldcols!=t.cols){hhh_wrap(&lines,[page[@"text"] UTF8String]?:"",t.cols);oldcols=t.cols;dirty=1;}
            if(offset>lines.count-bodyrows)offset=lines.count-bodyrows;if(offset<0)offset=0;
            if(dirty||hhh_resize) {
                hhh_line(1,t.cols,"\033[1;7m",[[NSString stringWithFormat:@"Hardcore web · %@",page[@"title"]?:@"Web"] UTF8String]);
                hhh_line(2,t.cols,"\033[0;36m",[url UTF8String]);
                for(int r=0;r<bodyrows;++r)hhh_line(r+3,t.cols,"\033[0m",offset+r<lines.count?lines.line[offset+r]:"");
                hhh_line(t.rows-1,t.cols,"\033[0;2m",[message UTF8String]);
                NSString *bottom=prompt?[NSString stringWithFormat:@"%@: %@",prompt==1?@"URL":@"Link",input]:@"j/k scroll | Space page | number↵ link | b back | u URL | o browser | q close";
                hhh_line(t.rows,t.cols,"\033[0;7m",[bottom UTF8String]);fputs("\033[0m",stdout);fflush(stdout);dirty=hhh_resize=0;
            }
            int k=hhh_key(&t,prompt==1);if(!k)continue;dirty=1;
            if(k==3){hhh_stop=1;break;}
            if(prompt) {
                if(k==27){prompt=0;[input setString:@""];}
                else if(k==127||k==8){if([input length]){NSRange range=[input rangeOfComposedCharacterSequenceAtIndex:[input length]-1];[input deleteCharactersInRange:range];}}
                else if(k==21)[input setString:@""];
                else if(k=='\r'||k=='\n') {
                    NSString *target=nil;
                    if(prompt==1)target=hhh_url(input,nil);
                    else {NSInteger number=[input integerValue];if(number>0&&number<=(NSInteger)[page[@"links"] count])target=page[@"links"][number-1];}
                    if(target) {
                        if(page){[history addObject:@{@"page":page,@"offset":@(offset)}];if([history count]>32)[history removeObjectAtIndex:0];}
                        hhh_string_set(&url,target);navigate=1;
                    }else hhh_string_set(&message,prompt==1?@"Enter a valid HTTP(S) URL":@"Enter a link number from this page");
                    prompt=0;[input setString:@""];
                }else if(k<0x110000&&hhh_printable(k)&&[input length]<HHH_URL_MAX) {
                    char u[5]={0};utf8_encode(k,u);[input appendString:[NSString stringWithUTF8String:u]?:@""];
                }
                continue;
            }
            if(k=='q'||k==27)break;
            if(k=='j'||k==HHH_DOWN)++offset;
            if(k=='k'||k==HHH_UP)--offset;
            if(k==' '||k==HHH_PGDN)offset+=bodyrows;
            if(k==HHH_PGUP)offset-=bodyrows;
            if(k=='g'||k==HHH_HOME)offset=0;
            if(k=='G'||k==HHH_END)offset=lines.count;
            if(k=='u'){prompt=1;[input setString:@""];}
            if(k>='0'&&k<='9'){prompt=2;[input setString:[NSString stringWithFormat:@"%c",k]];}
            if(k=='r')navigate=1;
            if(k=='o')hhh_string_set(&message,hhh_open_external([url UTF8String])?@"Opened in system browser":@"Could not open system browser");
            if(k=='b'&&[history count]) {
                if(fetch){[fetch cancel];[fetch release];fetch=nil;}
                NSDictionary *last=[history lastObject];[page release];page=[last[@"page"] retain];offset=[last[@"offset"] intValue];hhh_string_set(&url,page[@"url"]);[history removeLastObject];oldcols=0;hhh_string_set(&message,@"Previous page");
            }
          }
        }
        if(fetch){[fetch cancel];[fetch release];} [page release];[url release];[message release];[history release];[input release];hhh_lines_free(&lines);hhh_finish(&t);return 0;
    }
}

/* Per-frontend, opt-in host font transaction. Never changes a shared Terminal
   profile. Restore before suspend/detach/exit; retain snapshot if restore fails.
   Calls may take up to two seconds and belong on a serialized frontend worker,
   not the PTY broker/render loop. No scripting executable is spawned. */
static void hc_here_host_font_init(HCHereHostFont *font) {
    memset(font,0,sizeof(*font));const char *tty=ttyname(0),*program=getenv("TERM_PROGRAM");
    if(!program||strcmp(program,"Apple_Terminal"))snprintf(font->error,sizeof(font->error),"Host font control is available in local macOS Terminal");
    else if(getenv("SSH_TTY")||getenv("SSH_CONNECTION")||getenv("TMUX")||getenv("STY"))snprintf(font->error,sizeof(font->error),"Host font changes are unavailable through SSH or a multiplexer");
    else if(!tty||strncmp(tty,"/dev/tty",8)||strlen(tty)>=sizeof(font->tty))snprintf(font->error,sizeof(font->error),"Could not identify the originating Terminal tab");
    else {font->available=1;snprintf(font->tty,sizeof(font->tty),"%s",tty);}
}
static NSString *hhh_script_quote(NSString *s) {
    s=[s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    s=[s stringByReplacingOccurrencesOfString:@"\"" withString:@"\\\""];
    return [NSString stringWithFormat:@"\"%@\"",s];
}
static int hhh_font_call(HCHereHostFont *font,const char *name,double size,int set,char outname[256],double *outsize,long *window) {
    if(!font->available)return 0;
    @autoreleasepool {
        NSString *tty=hhh_script_quote([NSString stringWithUTF8String:font->tty]);
        NSString *change=set?[NSString stringWithFormat:@"set font name of pickedTab to %@\nset font size of pickedTab to %.8g\n",hhh_script_quote([NSString stringWithUTF8String:name]),size]:@"";
        NSString *guard=font->window_id?[NSString stringWithFormat:@"if pickedWindow is not %ld then error \"Originating Terminal window changed\"\n",font->window_id]:@"";
        NSString *source=[NSString stringWithFormat:@"with timeout of 2 seconds\ntell application id \"com.apple.Terminal\"\nset matchCount to 0\nrepeat with w in windows\nrepeat with t in tabs of w\nif tty of t is %@ then\nset matchCount to matchCount + 1\nset pickedTab to t\nset pickedWindow to id of w\nend if\nend repeat\nend repeat\nif matchCount is not 1 then error \"Originating Terminal tab not found\"\n%@%@return {pickedWindow, font name of pickedTab, font size of pickedTab}\nend tell\nend timeout",tty,guard,change];
        NSAppleScript *script=[[[NSAppleScript alloc] initWithSource:source] autorelease];NSDictionary *error=nil;
        NSAppleEventDescriptor *result=[script executeAndReturnError:&error];
        NSString *actual=[[result descriptorAtIndex:2] stringValue];double actualsize=[[[result descriptorAtIndex:3] stringValue] doubleValue];long actualwindow=[[result descriptorAtIndex:1] int32Value];
        if(!result||result.numberOfItems!=3||![actual length]||[actual lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>=256||!isfinite(actualsize)||actualsize<1||actualsize>200||actualwindow<=0) {
            NSString *message=hhh_safe(error[NSAppleScriptErrorMessage]?:@"Terminal did not provide a restorable font state");
            snprintf(font->error,sizeof(font->error),"%s",[message UTF8String]);return 0;
        }
        snprintf(outname,256,"%s",[actual UTF8String]);*outsize=actualsize;*window=actualwindow;return 1;
    }
}
static int hc_here_host_font_restore(HCHereHostFont *font) {
    if(!font->active)return 1;
    char actual[256];double size;long window;
    if(!hhh_font_call(font,font->original_name,font->original_size,1,actual,&size,&window))return 0;
    if(strcmp(actual,font->original_name)||fabs(size-font->original_size)>.001||window!=font->window_id) {snprintf(font->error,sizeof(font->error),"Terminal did not restore the original font");return 0;}
    font->active=0;font->window_id=0;font->error[0]=0;return 1;
}
static int hc_here_host_font_apply(HCHereHostFont *font,const char *name,double size,int allowed) {
    if(!allowed){snprintf(font->error,sizeof(font->error),"Enable Allow host font changes first");return 0;}
    if(!name||!*name||strlen(name)>200||!isfinite(size)||size<6||size>48){snprintf(font->error,sizeof(font->error),"Choose a valid font name and size from 6 to 48");return 0;}
    @autoreleasepool {if(![NSString stringWithUTF8String:name]){snprintf(font->error,sizeof(font->error),"Font name must be valid UTF-8");return 0;}}
    for(const unsigned char *p=(const unsigned char*)name;*p;++p)if(*p<32||*p==127){snprintf(font->error,sizeof(font->error),"Font name contains control characters");return 0;}
    if(!font->active) {
        if(!hhh_font_call(font,NULL,0,0,font->original_name,&font->original_size,&font->window_id))return 0;
        font->active=1;
    }
    char actual[256];double actualsize;long window;
    if(!hhh_font_call(font,name,size,1,actual,&actualsize,&window)||strcmp(actual,name)||fabs(actualsize-size)>.001) {
        char error[256];snprintf(error,sizeof(error),"%s",font->error[0]?font->error:"Terminal did not apply the requested font");
        int restored=hc_here_host_font_restore(font);snprintf(font->error,sizeof(font->error),"%s%s",error,restored?"":"; restoration pending");return 0;
    }
    font->error[0]=0;return 1;
}
#else
static int hhh_settings_path(char *path,size_t cap) {
    const char *xdg=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");
    if(xdg&&*xdg)return snprintf(path,cap,"%s/hardcore/here-native.ini",xdg)<(int)cap;
    return home&&*home&&snprintf(path,cap,"%s/.config/hardcore/here-native.ini",home)<(int)cap;
}
static void here_settings_load(HereSettings *s) {
    memset(s,0,sizeof(*s));s->theme='g';s->font_size=14;snprintf(s->font_name,sizeof(s->font_name),"Menlo-Regular");
    char path[PATH_MAX];if(!hhh_settings_path(path,sizeof(path)))return;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return;struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||st.st_size<0||st.st_size>16384){close(fd);return;}
    FILE *f=fdopen(fd,"r");if(!f){close(fd);return;}char line[512];
    while(fgets(line,sizeof(line),f)) {
        line[strcspn(line,"\r\n")]=0;
        if(!strncmp(line,"theme=",6)&&line[6]&&!line[7]&&strchr("hdmngsockrGSOCKR",line[6]))s->theme=line[6];
        else if(!strncmp(line,"font_size=",10)){char *end=NULL;long n=strtol(line+10,&end,10);if(end!=line+10&&!*end&&n>=6&&n<=48)s->font_size=(int)n;}
        else if(!strcmp(line,"allow_host_font=1"))s->allow_host_font=1;
        else if(!strncmp(line,"font_name=",10)&&line[10]&&strlen(line+10)<sizeof(s->font_name)) {
            int valid=1;for(unsigned char *p=(unsigned char*)line+10;*p;++p)if(*p<32||*p==127)valid=0;
            if(valid)snprintf(s->font_name,sizeof(s->font_name),"%s",line+10);
        }
    }
    fclose(f);
}
static int here_settings_save(const HereSettings *s) {
    if(!s->theme||!strchr("hdmngsockrGSOCKR",s->theme)||s->font_size<6||s->font_size>48||!memchr(s->font_name,0,sizeof(s->font_name))||!s->font_name[0])return 0;
    for(const unsigned char *p=(const unsigned char*)s->font_name;*p;++p)if(*p<32||*p==127)return 0;
    char path[PATH_MAX],dir[PATH_MAX],tmp[PATH_MAX];if(!hhh_settings_path(path,sizeof(path)))return 0;
    snprintf(dir,sizeof(dir),"%s",path);char *slash=strrchr(dir,'/');if(!slash)return 0;*slash=0;
    for(char *p=dir+1;*p;++p)if(*p=='/'){*p=0;if(mkdir(dir,0700)&&errno!=EEXIST)return 0;*p='/';}
    if(mkdir(dir,0700)&&errno!=EEXIST)return 0;
    if(snprintf(tmp,sizeof(tmp),"%s/.here-XXXXXX",dir)>=(int)sizeof(tmp))return 0;
    int fd=mkstemp(tmp);if(fd<0)return 0;fchmod(fd,0600);FILE *f=fdopen(fd,"w");if(!f){close(fd);unlink(tmp);return 0;}
    int ok=fprintf(f,"theme=%c\nfont_size=%d\nallow_host_font=%d\nfont_name=%s\n",s->theme,s->font_size,s->allow_host_font!=0,s->font_name)>0;
    if(fflush(f)||fsync(fd))ok=0;if(fclose(f))ok=0;if(ok&&rename(tmp,path))ok=0;if(!ok)unlink(tmp);return ok;
}
static void hc_here_host_font_init(HCHereHostFont *font) {
    memset(font,0,sizeof(*font));snprintf(font->error,sizeof(font->error),"Host font control is available in local macOS Terminal");
}
static int hc_here_host_font_restore(HCHereHostFont *font) { (void)font;return 1; }
static int hc_here_host_font_apply(HCHereHostFont *font,const char *name,double size,int allowed) {
    (void)name;(void)size;(void)allowed;snprintf(font->error,sizeof(font->error),"Host font control is available in local macOS Terminal");return 0;
}
static int hc_here_web_main(int argc,char **argv) {
    (void)argc;(void)argv;fputs("The built-in text web reader is available on macOS.\n",stderr);return 2;
}
#endif
#endif /* !_WIN32 */
#endif
