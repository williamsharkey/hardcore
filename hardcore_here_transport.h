/* Native terminal attachment and persistent PTY owner. The protocol transports
 * input and rendered ANSI, never a second terminal implementation. */
#ifndef HARDCORE_HERE_TRANSPORT_H
#define HARDCORE_HERE_TRANSPORT_H
#include <arpa/inet.h>
#include <pthread.h>

#define HERE_WIRE_MAGIC 0x48434832u
#define HERE_WIRE_MAX (8u * 1024u * 1024u)
#define HERE_CLIENT_MAX (128u * 1024u)
#define HERE_PEERS 8
/* Separate namespace from the retired Python protocol. */
#define HERE_SOCKET_SUFFIX ".native.sock"
enum { HW_HELLO=1, HW_INPUT, HW_SIZE, HW_STATUS, HW_STOP, HW_FOCUS, HW_FONT_REPLY,
       HW_OK=101, HW_OUTPUT, HW_LEAVE, HW_ERROR, HW_STATUS_REPLY, HW_FONT };
typedef struct { uint32_t magic, type, length; } HereWire;
typedef struct { int fd, controller, closing; uint64_t since; SBuf in, out; size_t off; } HerePeer;
static Here *here_owner;
static volatile sig_atomic_t here_signal_exit, here_signal_resize, here_signal_suspend, here_signal_continue;
static char here_error[256];

static uint64_t here_now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec*1000u + (uint64_t)t.tv_nsec/1000000u;
}
static void here_fd_flags(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC); int flags=fcntl(fd,F_GETFL,0);
    if(flags>=0) fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}
static void here_consume(SBuf *b,size_t n) {
    if(n>=b->n) b->n=0; else { memmove(b->p,b->p+n,b->n-n); b->n-=n; }
    if(b->p)b->p[b->n]=0;
}
static int here_append(SBuf *b,const void *p,size_t n,size_t limit) {
    if(n>limit||b->n>limit-n)return 0;
    size_t old=b->n; sb_put(b,p,n); return b->n==old+n;
}
static int here_frame(SBuf *b,int type,const void *p,size_t n) {
    if(n>HERE_WIRE_MAX-sizeof(HereWire)||b->n>HERE_WIRE_MAX-n-sizeof(HereWire))return 0;
    HereWire w={htonl(HERE_WIRE_MAGIC),htonl((uint32_t)type),htonl((uint32_t)n)};
    return here_append(b,&w,sizeof(w),HERE_WIRE_MAX)&&here_append(b,p,n,HERE_WIRE_MAX);
}
static int here_peek(SBuf *b,int *type,size_t *len,size_t limit) {
    if(b->n<sizeof(HereWire))return 0;
    HereWire w;memcpy(&w,b->p,sizeof(w));*len=ntohl(w.length);*type=(int)ntohl(w.type);
    if(ntohl(w.magic)!=HERE_WIRE_MAGIC||*len>limit)return -1;
    return b->n>=sizeof(w)+*len;
}
static int here_send_all(int fd,const void *p,size_t n,int timeout_ms) {
    const char *q=p;uint64_t end=here_now_ms()+timeout_ms;
    while(n) {
        ssize_t k=write(fd,q,n);if(k>0){q+=k;n-=(size_t)k;continue;}
        if(k<0&&errno==EINTR)continue;
        if(k<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) {
            int left=(int)(end-here_now_ms());if(left<=0)return 0;
            struct pollfd pf={fd,POLLOUT,0};if(poll(&pf,1,left)>0)continue;
        }
        return 0;
    }
    return 1;
}
static int here_send_frame(int fd,int type,const void *p,size_t n,int timeout_ms) {
    SBuf b={0};int ok=here_frame(&b,type,p,n)&&here_send_all(fd,b.p,b.n,timeout_ms);free(b.p);return ok;
}
static int here_read_frame(int fd,int *type,SBuf *data,int timeout_ms) {
    SBuf b={0};uint64_t end=here_now_ms()+timeout_ms;int ok=0;
    for(;;) {
        size_t n;int ready=here_peek(&b,type,&n,HERE_WIRE_MAX);
        if(ready<0)break;
        if(ready){ok=here_append(data,b.p+sizeof(HereWire),n,HERE_WIRE_MAX);break;}
        int left=(int)(end-here_now_ms());if(left<=0)break;
        struct pollfd pf={fd,POLLIN,0};int r=poll(&pf,1,left);
        if(r<0&&errno==EINTR)continue;if(r<=0)break;
        size_t want=sizeof(HereWire)-b.n;
        if(b.n>=sizeof(HereWire)){HereWire w;memcpy(&w,b.p,sizeof(w));want=sizeof(w)+(size_t)ntohl(w.length)-b.n;}
        char tmp[8192];if(want>sizeof(tmp))want=sizeof(tmp);ssize_t got=read(fd,tmp,want);
        if(got<0&&(errno==EINTR||errno==EAGAIN))continue;if(got<=0)break;
        if(!here_append(&b,tmp,(size_t)got,HERE_WIRE_MAX+sizeof(HereWire)))break;
    }
    free(b.p);return ok;
}
static int here_name_valid(const char *s) {
    size_t n=strlen(s);if(!n||n>48||!isalnum((unsigned char)*s))return 0;
    for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)s[i])&&s[i]!='_'&&s[i]!='-'&&s[i]!='.')return 0;
    return 1;
}
static int here_runtime(char *path,size_t cap) {
    snprintf(path,cap,"/tmp/hardcore-here-%lu",(unsigned long)getuid());
    if(mkdir(path,0700)<0&&errno!=EEXIST){snprintf(here_error,sizeof(here_error),"Cannot create session directory: %s",strerror(errno));return 0;}
    struct stat st;if(lstat(path,&st)<0||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)) {
        snprintf(here_error,sizeof(here_error),"Unsafe session directory; expected your directory with mode 0700");return 0;
    }
    return 1;
}
static int here_path(const char *name,char *path,size_t cap) {
    char dir[128];if(!here_name_valid(name)){snprintf(here_error,sizeof(here_error),"Session names need 1-48 letters, numbers, dots, underscores or hyphens");return 0;}
    if(!here_runtime(dir,sizeof(dir)))return 0;
    int n=snprintf(path,cap,"%s/%s%s",dir,name,HERE_SOCKET_SUFFIX);
    return n>0&&(size_t)n<cap;
}
static int here_same_user(int fd) {
#ifdef __APPLE__
    uid_t uid;gid_t gid;return getpeereid(fd,&uid,&gid)==0&&uid==getuid();
#elif defined(SO_PEERCRED)
    struct { pid_t pid; uid_t uid; gid_t gid; } cred;socklen_t n=sizeof(cred);
    return getsockopt(fd,SOL_SOCKET,SO_PEERCRED,&cred,&n)==0&&cred.uid==getuid();
#else
    return 1; /* owner-only directory and socket remain mandatory */
#endif
}
static int here_connect(const char *path) {
    struct stat st;if(lstat(path,&st)<0)return -1;
    if(!S_ISSOCK(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)){errno=EPERM;return -1;}
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;
    here_fd_flags(fd);struct sockaddr_un a;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;
    if(strlen(path)>=sizeof(a.sun_path)){close(fd);errno=ENAMETOOLONG;return -1;}strcpy(a.sun_path,path);
    if(connect(fd,(struct sockaddr*)&a,sizeof(a))<0) {
        if(errno!=EINPROGRESS){int e=errno;close(fd);errno=e;return -1;}
        struct pollfd pf={fd,POLLOUT,0};int e=0;socklen_t n=sizeof(e);
        if(poll(&pf,1,400)<=0||getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&n)<0||e){close(fd);errno=e?e:ETIMEDOUT;return -1;}
    }
    if(!here_same_user(fd)){close(fd);errno=EPERM;return -1;}return fd;
}
static void here_signal(int sig) {
    if(sig==SIGWINCH)here_signal_resize=1;
    else if(sig==SIGTSTP)here_signal_suspend=1;
    else if(sig==SIGCONT)here_signal_continue=1;
    else here_signal_exit=sig;
}
static int here_process_cwd(pid_t pid,char *out,size_t cap) {
#ifdef __APPLE__
    struct proc_vnodepathinfo info;
    if(proc_pidinfo(pid,PROC_PIDVNODEPATHINFO,0,&info,sizeof(info))==(int)sizeof(info)&&info.pvi_cdir.vip_path[0]) {
        snprintf(out,cap,"%s",info.pvi_cdir.vip_path);return 1;
    }
#else
    char path[80];snprintf(path,sizeof(path),"/proc/%ld/cwd",(long)pid);ssize_t n=readlink(path,out,cap-1);
    if(n>0){out[n]=0;return 1;}
#endif
    return 0;
}
static void here_program(HereTab *t) {
    if(t->fd<0)return;pid_t fg=tcgetpgrp(t->fd);if(fg<=0)return;
#ifdef __APPLE__
    char name[256];int n=proc_name(fg,name,sizeof(name));if(n>0)snprintf(t->program,sizeof(t->program),"%s",name);
#else
    char path[80],name[256];snprintf(path,sizeof(path),"/proc/%ld/comm",(long)fg);FILE *f=fopen(path,"r");
    if(f){if(fgets(name,sizeof(name),f)){name[strcspn(name,"\r\n")]=0;snprintf(t->program,sizeof(t->program),"%s",name);}fclose(f);}
#endif
}
static void here_child_close_fds(void) {
    long limit=sysconf(_SC_OPEN_MAX);if(limit<0||limit>65536)limit=65536;
    for(int fd=3;fd<limit;fd++)close(fd);
}
static void here_child_reset(void) {
    int signals[]={SIGINT,SIGTERM,SIGHUP,SIGPIPE,SIGTSTP,SIGCONT,SIGWINCH,SIGCHLD};
    for(size_t i=0;i<sizeof(signals)/sizeof(signals[0]);i++)signal(signals[i],SIG_DFL);
    sigset_t mask;sigemptyset(&mask);sigprocmask(SIG_SETMASK,&mask,NULL);
}
static int here_new(Here *h,const char *cwd,char *const *argv) {
    if(h->ntabs>=HERE_MAX_TABS)return -1;
    HereTab *t=&h->tabs[h->ntabs];memset(t,0,sizeof(*t));t->fd=-1;
    char current[1024];
    if(!cwd||!*cwd) {
        HereTab *active=h->ntabs?&h->tabs[h->active]:NULL;
        if(active&&active->t.cwd[0])cwd=active->t.cwd;
        else if(active&&here_process_cwd(active->pid,current,sizeof(current)))cwd=current;
        else cwd=active?active->cwd:h->cwd;
    }
    struct stat st;if(stat(cwd,&st)<0||!S_ISDIR(st.st_mode))return -1;
    snprintf(t->cwd,sizeof(t->cwd),"%s",cwd);
    if(!term_init(&t->t,h->child_cols>1?h->child_cols:80,h->child_rows>0?h->child_rows:23))return -1;
    struct winsize ws={0};ws.ws_col=(unsigned short)t->t.cols;ws.ws_row=(unsigned short)t->t.rows;
    pid_t pid=forkpty(&t->fd,NULL,NULL,&ws);
    if(pid<0){term_free(&t->t);return -1;}
    if(!pid) {
        here_child_reset();
        /* No listener, frontend or another tab PTY is inherited by a shell. */
        here_child_close_fds();
        if(chdir(t->cwd)<0){dprintf(2,"hardcore here: cannot enter directory: %s\r\n",strerror(errno));_exit(126);}
        setenv("TERM","xterm-256color",1);setenv("COLORTERM","truecolor",1);setenv("TERM_PROGRAM","hardcore-here",1);
        unsetenv("HARDCORE_CTL");unsetenv("HARDCORE_WINDOW_PID");unsetenv("HARDCORE_TAB_ID");
        char *shellargs[]={h->shell,"-i",NULL};char *const *command=argv&&argv[0]?argv:shellargs;
        execvp(command[0],command);dprintf(2,"hardcore here: %s: %s\r\n",command[0],strerror(errno));_exit(127);
    }
    t->pid=pid;t->id=++h->next_id;here_fd_flags(t->fd);
    const char *program=argv&&argv[0]?argv[0]:h->shell;const char *slash=strrchr(program,'/');
    snprintf(t->program,sizeof(t->program),"%s",slash?slash+1:program);
    h->active=h->ntabs++;h->dirty=1;return h->active;
}
static int here_write(Here *h,int index,const char *data,size_t n) {
    if(index<0||index>=h->ntabs)return 0;HereTab *t=&h->tabs[index];if(t->fd<0||t->exited)return 0;
    if(t->pending_off){here_consume(&t->pending,t->pending_off);t->pending_off=0;}
    if(!here_append(&t->pending,data,n,HERE_PENDING_MAX))return 0;
    t->t.had_activity=1;return 1;
}
static void here_reply(Term *term,const char *data,int n) {
    Here *h=here_owner;if(!h||n<=0)return;
    for(int i=0;i<h->ntabs;i++)if(&h->tabs[i].t==term){here_write(h,i,data,(size_t)n);return;}
}
static void here_resize_tabs(Here *h,int cols,int rows) {
    if(cols<2)cols=2;if(cols>500)cols=500;if(rows<1)rows=1;if(rows>200)rows=200;
    h->child_cols=cols;h->child_rows=rows;
    struct winsize ws={0};ws.ws_col=(unsigned short)cols;ws.ws_row=(unsigned short)rows;
    for(int i=0;i<h->ntabs;i++) {
        HereTab *t=&h->tabs[i];if(t->t.cols!=cols||t->t.rows!=rows)term_resize(&t->t,cols,rows);
        if(t->fd>=0)ioctl(t->fd,TIOCSWINSZ,&ws);
    }
    h->dirty=1;
}
/* Only process groups whose live session is the forkpty session we own. This
 * catches background shell jobs without touching the invoking terminal. */
static int here_owned_groups(HereTab *t,pid_t *groups,int cap) {
    int n=0;if(t->pid<=0)return 0;
    if(!t->exited&&getsid(t->pid)==t->pid)groups[n++]=t->pid;
#ifdef __APPLE__
    int bytes=proc_listallpids(NULL,0);if(bytes>0&&bytes<1000000) {
        int count=bytes+128;pid_t *pids=calloc((size_t)count,sizeof(pid_t));
        if(pids){int got=proc_listallpids(pids,count*(int)sizeof(pid_t));
            for(int i=0;i<got&&n<cap;i++)if(pids[i]>0&&getsid(pids[i])==t->pid){pid_t pg=getpgid(pids[i]);int found=0;for(int j=0;j<n;j++)if(groups[j]==pg)found=1;if(pg>0&&!found)groups[n++]=pg;}free(pids);}
    }
#else
    DIR *d=opendir("/proc");if(d){struct dirent *e;while((e=readdir(d))&&n<cap){pid_t p=(pid_t)strtol(e->d_name,NULL,10);if(p>0&&getsid(p)==t->pid){pid_t pg=getpgid(p);int found=0;for(int j=0;j<n;j++)if(groups[j]==pg)found=1;if(pg>0&&!found)groups[n++]=pg;}}closedir(d);}
#endif
    return n;
}
static void here_close(Here *h,int index) {
    if(index<0||index>=h->ntabs)return;HereTab *t=&h->tabs[index];pid_t groups[256];int n=here_owned_groups(t,groups,256);
    for(int i=0;i<n;i++)kill(-groups[i],SIGHUP);
    if(t->fd>=0){close(t->fd);t->fd=-1;}
    /* SIGTERM after HUP handles shell jobs that deliberately ignore hangup. */
    for(int i=0;i<n;i++)if(getsid(groups[i])==t->pid)kill(-groups[i],SIGTERM);
    /* Re-scan the owned session after a short grace period: group leaders can
     * exit before their children, so checking getsid(group_leader) alone misses
     * orphaned jobs. The scan authorizes every final kill by a live member. */
    uint64_t deadline=here_now_ms()+120;
    while(n&&here_now_ms()<deadline){struct timespec pause={0,10000000};nanosleep(&pause,NULL);n=here_owned_groups(t,groups,256);}
    for(int i=0;i<n;i++)kill(-groups[i],SIGKILL);
    if(!t->exited){int status;pid_t reaped=waitpid(t->pid,&status,WNOHANG);
        if(!reaped){uint64_t end=here_now_ms()+100;while(!reaped&&here_now_ms()<end){struct timespec pause={0,5000000};nanosleep(&pause,NULL);reaped=waitpid(t->pid,&status,WNOHANG);}}}
    term_free(&t->t);free(t->pending.p);
    memmove(h->tabs+index,h->tabs+index+1,(size_t)(h->ntabs-index-1)*sizeof(HereTab));h->ntabs--;
    if(h->active>index)h->active--;if(h->active>=h->ntabs)h->active=h->ntabs-1;
    if(!h->ntabs){h->active=0;h->action=HERE_STOP;}h->dirty=1;
}
static void here_caf(Here *h,int enabled) {
    if(!enabled){if(h->caf_pid>0){kill(h->caf_pid,SIGTERM);waitpid(h->caf_pid,NULL,WNOHANG);}h->caf_pid=0;h->dirty=1;return;}
#ifdef __APPLE__
    if(h->caf_pid>0)return;pid_t pid=fork();if(!pid){here_child_reset();here_child_close_fds();char parent[32];snprintf(parent,sizeof(parent),"%ld",(long)getppid());execl("/usr/bin/caffeinate","caffeinate","-di","-w",parent,(char*)NULL);_exit(127);}if(pid>0)h->caf_pid=pid;
#endif
    h->dirty=1;
}
static void here_font(Here *h,const char *name,int size,int allowed) {
    if(size<6||size>48||!name||!*name||strlen(name)>=sizeof(h->font_name))return;
    for(const unsigned char *p=(const unsigned char *)name;*p;p++)if(*p<32||*p==127)return;
    snprintf(h->font_name,sizeof(h->font_name),"%s",name);h->font_size=size;h->font_allowed=!!allowed;h->font_request=1;
}
static void here_peer_drop(Here *h,HerePeer *p) {
    if(p->controller){h->attached=0;here_ui_focus(h,0);here_caf(h,0);}
    if(p->fd>=0)close(p->fd);free(p->in.p);free(p->out.p);memset(p,0,sizeof(*p));p->fd=-1;
}
static void here_json_string(SBuf *b,const char *s) {
    sb_put(b,"\"",1);for(const unsigned char *p=(const unsigned char*)s;*p;p++) {
        if(*p=='"'||*p=='\\')sb_printf(b,"\\%c",*p);else if(*p<32)sb_printf(b,"\\u%04x",*p);else sb_put(b,(const char *)p,1);
    }sb_put(b,"\"",1);
}
static void here_status(Here *h,SBuf *b) {
    sb_put(b,"{\"name\":",8);here_json_string(b,h->session);
    sb_printf(b,",\"native\":true,\"version\":2,\"pid\":%ld,\"attached\":%s,\"active\":%d,\"cols\":%d,\"rows\":%d,\"tabs\":[",(long)getpid(),h->attached?"true":"false",h->active,h->child_cols,h->child_rows);
    for(int i=0;i<h->ntabs;i++) {
        HereTab *t=&h->tabs[i];if(i)sb_put(b,",",1);
        sb_printf(b,"{\"id\":%d,\"pid\":%ld,\"exited\":%s,\"status\":%d,\"cwd\":",t->id,(long)t->pid,t->exited?"true":"false",t->status);
        here_json_string(b,t->t.cwd[0]?t->t.cwd:t->cwd);sb_put(b,",\"program\":",11);here_json_string(b,t->program);
        sb_printf(b,",\"cols\":%d,\"rows\":%d,\"pending\":%zu}",t->t.cols,t->t.rows,t->pending.n-t->pending_off);
    }sb_put(b,"]}",2);
}
static int here_owner_request(Here *h,HerePeer *peers,int at,int type,const char *p,size_t n) {
    HerePeer *peer=&peers[at];
    if(type==HW_STATUS&&n==0){SBuf b={0};here_status(h,&b);here_frame(&peer->out,HW_STATUS_REPLY,b.p,b.n);free(b.p);peer->closing=1;return 1;}
    if(type==HW_STOP&&n==0){here_frame(&peer->out,HW_OK,"",0);h->action=HERE_STOP;return 1;}
    if(type==HW_HELLO&&n==16&&!peer->controller) {
        uint32_t value[4];memcpy(value,p,sizeof(value));int version=(int)ntohl(value[0]),cols=(int)ntohl(value[1]),rows=(int)ntohl(value[2]),takeover=(int)ntohl(value[3]);
        if(version!=2||cols<2||cols>500||rows<2||rows>200)return 0;
        if(h->attached&&!takeover){const char *msg="Session already attached; use --takeover to replace its attachment";here_frame(&peer->out,HW_ERROR,msg,strlen(msg));peer->closing=1;return 1;}
        for(int i=0;i<HERE_PEERS;i++)if(i!=at&&peers[i].controller){here_frame(&peers[i].out,HW_LEAVE,"takeover",8);peers[i].controller=0;peers[i].closing=1;}
        peer->controller=1;h->attached=1;h->action=0;
        here_ui_resize(h,cols,rows);here_ui_focus(h,1);h->dirty=1;
        here_frame(&peer->out,HW_OK,"",0);return 1;
    }
    if(!peer->controller)return 0;
    if(type==HW_FONT_REPLY&&n==268){uint32_t values[3];memcpy(values,p,12);h->font_available=!!ntohl(values[0]);h->font_active=!!ntohl(values[1]);memcpy(h->font_error,p+12,255);h->font_error[255]=0;if(ntohl(values[2]))here_ui_message(h,h->font_error[0]?h->font_error:h->font_active?"Host font changed (restored when leaving)":"Host font restored");h->dirty=1;return 1;}
    if(type==HW_INPUT&&n<=65536){here_ui_input(h,p,n);return 1;}
    if(type==HW_SIZE&&n==8){uint32_t values[2];memcpy(values,p,8);int cols=(int)ntohl(values[0]),rows=(int)ntohl(values[1]);if(cols<2||cols>500||rows<2||rows>200)return 0;here_ui_resize(h,cols,rows);return 1;}
    if(type==HW_FOCUS&&n==1){here_ui_focus(h,p[0]!=0);return 1;}
    return 0;
}
static void here_owner_loop(Here *h,int listener,const char *path,int lockfd) {
    HerePeer peers[HERE_PEERS];memset(peers,0,sizeof(peers));for(int i=0;i<HERE_PEERS;i++)peers[i].fd=-1;
    here_owner=h;g_term_reply_backend=here_reply;here_signal_exit=0;
    signal(SIGPIPE,SIG_IGN);signal(SIGHUP,SIG_IGN);signal(SIGTERM,here_signal);signal(SIGINT,here_signal);
    here_ui_init(h);here_ui_resize(h,h->cols,h->rows);
    if(here_new(h,h->cwd,h->command)<0){close(listener);unlink(path);close(lockfd);_exit(1);}
    uint64_t program_at=0,render_at=0;
    while(!here_signal_exit&&h->action!=HERE_STOP) {
        struct pollfd pf[1+HERE_PEERS+HERE_MAX_TABS];int index=0,peerpos[HERE_PEERS],tabpos[HERE_MAX_TABS];
        pf[index++]=(struct pollfd){listener,POLLIN,0};
        size_t pending=0;for(int i=0;i<h->ntabs;i++)pending+=h->tabs[i].pending.n-h->tabs[i].pending_off;
        for(int i=0;i<HERE_PEERS;i++){peerpos[i]=index;short ev=0;if(peers[i].fd>=0){if(!peers[i].closing&&(!peers[i].controller||pending<HERE_PENDING_MAX-131072))ev|=POLLIN;if(peers[i].out.n>peers[i].off)ev|=POLLOUT;}pf[index++]=(struct pollfd){peers[i].fd,ev,0};}
        int oldtabs=h->ntabs;for(int i=0;i<oldtabs;i++){HereTab *t=&h->tabs[i];tabpos[i]=index;pf[index++]=(struct pollfd){t->fd,(short)(POLLIN|(t->pending.n>t->pending_off?POLLOUT:0)),0};}
        int ready=poll(pf,(nfds_t)index,h->dirty?16:h->attached?80:500);if(ready<0&&errno!=EINTR)break;
        if(pf[0].revents&POLLIN){int fd=accept(listener,NULL,NULL);if(fd>=0){here_fd_flags(fd);int slot=-1;for(int i=0;i<HERE_PEERS;i++)if(peers[i].fd<0){slot=i;break;}if(slot<0||!here_same_user(fd))close(fd);else{peers[slot].fd=fd;peers[slot].since=here_now_ms();}}}
        /* Read PTYs before input because an input command may add/remove tabs. */
        for(int i=0;i<oldtabs;i++) {
            HereTab *t=&h->tabs[i];short ev=pf[tabpos[i]].revents;
            if(t->fd>=0&&(ev&(POLLIN|POLLHUP|POLLERR))) {
                char buf[32768];for(int batch=0;batch<8;batch++){ssize_t n=read(t->fd,buf,sizeof(buf));if(n>0){vt_feed(&t->t,(const unsigned char *)buf,(int)n);t->unread=i!=h->active;h->dirty=1;continue;}if(n==0||(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)){close(t->fd);t->fd=-1;}break;}
            }
            if(t->fd>=0&&(ev&POLLOUT)&&t->pending.n>t->pending_off){ssize_t n=write(t->fd,t->pending.p+t->pending_off,t->pending.n-t->pending_off);if(n>0){t->pending_off+=(size_t)n;if(t->pending_off==t->pending.n){t->pending.n=0;t->pending_off=0;}}}
            if(!t->exited){int st;pid_t reaped=waitpid(t->pid,&st,WNOHANG);if(reaped==t->pid){t->exited=1;t->status=WIFEXITED(st)?WEXITSTATUS(st):WIFSIGNALED(st)?128+WTERMSIG(st):0;h->dirty=1;}}
        }
        for(int i=0;i<HERE_PEERS;i++) {
            HerePeer *p=&peers[i];if(p->fd<0)continue;short ev=pf[peerpos[i]].revents;int drop=0;
            if(ev&POLLIN){char buf[65536];ssize_t n=read(p->fd,buf,sizeof(buf));if(n>0){if(!here_append(&p->in,buf,(size_t)n,HERE_CLIENT_MAX))drop=1;}else if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR))drop=1;}
            while(!drop&&p->in.n){int type;size_t n;int got=here_peek(&p->in,&type,&n,65536);if(got<0){drop=1;break;}if(!got)break;if(!here_owner_request(h,peers,i,type,p->in.p+sizeof(HereWire),n)){drop=1;break;}here_consume(&p->in,sizeof(HereWire)+n);if(h->action)break;}
            if(!drop&&(ev&POLLOUT)&&p->out.n>p->off){ssize_t n=write(p->fd,p->out.p+p->off,p->out.n-p->off);if(n>0){p->off+=(size_t)n;if(p->off==p->out.n){p->out.n=0;p->off=0;}}else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)drop=1;}
            if(ev&(POLLERR|POLLNVAL))drop=1;if((ev&POLLHUP)&&!(ev&POLLIN))drop=1;
            if(p->closing&&p->out.n==p->off)drop=1;if(!p->controller&&!p->closing&&here_now_ms()-p->since>5000)drop=1;
            if(drop)here_peer_drop(h,p);
        }
        /* Reap every owned child, including recently closed tabs and caf. A child
         * may exit between its individual wait above and this catch-up loop. */
        int child_status;pid_t reaped;
        while((reaped=waitpid(-1,&child_status,WNOHANG))>0){
            if(h->caf_pid==reaped){h->caf_pid=0;h->dirty=1;}
            for(int i=0;i<h->ntabs;i++)if(h->tabs[i].pid==reaped){HereTab *t=&h->tabs[i];t->exited=1;t->status=WIFEXITED(child_status)?WEXITSTATUS(child_status):WIFSIGNALED(child_status)?128+WTERMSIG(child_status):0;h->dirty=1;}
        }
        uint64_t now=here_now_ms();here_ui_tick(h,now);
        if(now-program_at>=500){for(int i=0;i<h->ntabs;i++)here_program(&h->tabs[i]);program_at=now;}
        if(h->font_request&&h->attached){for(int i=0;i<HERE_PEERS;i++)if(peers[i].controller&&!peers[i].closing){unsigned char request[136]={0};uint32_t values[2]={htonl((uint32_t)h->font_allowed),htonl((uint32_t)h->font_size)};memcpy(request,values,8);memcpy(request+8,h->font_name,strlen(h->font_name));here_frame(&peers[i].out,HW_FONT,request,sizeof(request));h->font_request=0;break;}}
        if(h->action==HERE_DETACH||h->action==HERE_RELOAD){for(int i=0;i<HERE_PEERS;i++)if(peers[i].controller){uint32_t action=htonl((uint32_t)h->action);here_frame(&peers[i].out,HW_LEAVE,&action,sizeof(action));peers[i].closing=1;peers[i].controller=0;}h->attached=0;here_ui_focus(h,0);here_caf(h,0);h->action=0;}
        if(h->dirty&&h->attached&&now-render_at>=16) {
            for(int i=0;i<HERE_PEERS;i++)if(peers[i].controller&&!peers[i].closing&&peers[i].out.n-peers[i].off<65536) {
                SBuf out={0};here_ui_render(h,&out);
                if(out.n&&!here_frame(&peers[i].out,HW_OUTPUT,out.p,out.n))here_peer_drop(h,&peers[i]);
                free(out.p);h->dirty=0;render_at=now;break;
            }
        }
    }
    /* Give explicit stop its acknowledgment before tearing down owned PTYs. */
    for(int i=0;i<HERE_PEERS;i++)if(peers[i].fd>=0){HerePeer *p=&peers[i];here_frame(&p->out,HW_LEAVE,"stopped",7);here_send_all(p->fd,p->out.p+p->off,p->out.n-p->off,100);here_peer_drop(h,p);}
    here_caf(h,0);while(h->ntabs)here_close(h,h->ntabs-1);here_ui_free(h);g_term_reply_backend=NULL;here_owner=NULL;
    close(listener);unlink(path);close(lockfd);
}
static int here_start(Here *h,const char *path) {
    char lockpath[256];snprintf(lockpath,sizeof(lockpath),"%s.lock",path);
    int lockfd=open(lockpath,O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);if(lockfd<0)return 0;
    struct stat st;if(fstat(lockfd,&st)<0||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)||flock(lockfd,LOCK_EX|LOCK_NB)<0){close(lockfd);return 0;}
    /* With the lock held, only an owner-created stale socket may be replaced. */
    if(lstat(path,&st)==0){if(!S_ISSOCK(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&0077)){close(lockfd);errno=EPERM;return 0;}int fd=here_connect(path);if(fd>=0){close(fd);close(lockfd);errno=EEXIST;return 0;}if(errno!=ECONNREFUSED&&errno!=ENOENT){close(lockfd);return 0;}if(unlink(path)<0){close(lockfd);return 0;}}
    int listener=socket(AF_UNIX,SOCK_STREAM,0);if(listener<0){close(lockfd);return 0;}here_fd_flags(listener);
    struct sockaddr_un a;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;snprintf(a.sun_path,sizeof(a.sun_path),"%s",path);
    mode_t mask=umask(0077);int ok=bind(listener,(struct sockaddr*)&a,sizeof(a))==0;umask(mask);
    if(!ok||chmod(path,0600)<0||listen(listener,HERE_PEERS)<0){close(listener);close(lockfd);if(ok)unlink(path);return 0;}
    pid_t child=fork();if(child<0){close(listener);unlink(path);close(lockfd);return 0;}
    if(!child){setsid();int nullfd=open("/dev/null",O_RDWR);if(nullfd>=0){dup2(nullfd,0);dup2(nullfd,1);dup2(nullfd,2);if(nullfd>2)close(nullfd);}here_owner_loop(h,listener,path,lockfd);_exit(0);}
    close(listener);close(lockfd);return 1;
}
static void here_host_size(int *cols,int *rows) {
    struct winsize ws={0};ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws);*cols=ws.ws_col?ws.ws_col:80;*rows=ws.ws_row?ws.ws_row:24;
    if(*cols<2)*cols=2;if(*cols>500)*cols=500;if(*rows<2)*rows=2;if(*rows>200)*rows=200;
}
static const char here_host_enter[]="\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1003h\x1b[?1006h\x1b[?1004h\x1b[?2004h";
static const char here_host_leave[]="\x1b[0m\x1b]8;;\x1b\\\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l\x1b[?1004l\x1b[?2004l\x1b[?1l\x1b>\x1b[0 q\x1b[?7h\x1b[?25h\x1b[?1049l";
/* Host-specific mutations belong to the attachment, never the daemon. A
 * single serialized worker coalesces requests; joining it first on exit or
 * suspend guarantees a late Apple event cannot reapply a font after restore. */
typedef struct {
#ifdef __APPLE__
    HCHereHostFont font;
#endif
    pthread_t thread;pthread_mutex_t mutex;pthread_cond_t cond;
    int started,quit,pending,ready,available,active,allowed,size;
    char name[128],error[256];
} HereFontWorker;
static void *here_font_work(void *arg) {
    HereFontWorker *w=arg;
#ifdef __APPLE__
    for(;;){pthread_mutex_lock(&w->mutex);while(!w->quit&&!w->pending)pthread_cond_wait(&w->cond,&w->mutex);
        if(w->quit){pthread_mutex_unlock(&w->mutex);break;}
        int allowed=w->allowed,size=w->size;char name[128];memcpy(name,w->name,sizeof(name));w->pending=0;pthread_mutex_unlock(&w->mutex);
        if(allowed)hc_here_host_font_apply(&w->font,name,size,1);else hc_here_host_font_restore(&w->font);
        pthread_mutex_lock(&w->mutex);w->active=w->font.active;snprintf(w->error,sizeof(w->error),"%s",w->font.error);w->ready=1;pthread_mutex_unlock(&w->mutex);
    }
    /* Retry once after a failed restore without discarding the original state. */
    if(!hc_here_host_font_restore(&w->font))hc_here_host_font_restore(&w->font);
    w->active=w->font.active;snprintf(w->error,sizeof(w->error),"%s",w->font.error);
#else
    (void)w;
#endif
    return NULL;
}
static void here_font_worker_init(HereFontWorker *w) {
    memset(w,0,sizeof(*w));pthread_mutex_init(&w->mutex,NULL);pthread_cond_init(&w->cond,NULL);
#ifdef __APPLE__
    hc_here_host_font_init(&w->font);w->available=w->font.available;snprintf(w->error,sizeof(w->error),"%s",w->font.error);
#else
    snprintf(w->error,sizeof(w->error),"Host font control is available in local macOS Terminal");
#endif
}
static void here_font_worker_request(HereFontWorker *w,const char *payload,size_t n) {
    if(n!=136)return;uint32_t values[2];memcpy(values,payload,8);int allowed=(int)ntohl(values[0]),size=(int)ntohl(values[1]);
    if((allowed!=0&&allowed!=1)||size<6||size>48||!memchr(payload+8,0,128))return;
    const char *name=payload+8;if(!*name)return;for(const unsigned char *p=(const unsigned char*)name;*p;p++)if(*p<32||*p==127)return;
    pthread_mutex_lock(&w->mutex);w->allowed=allowed;w->size=size;snprintf(w->name,sizeof(w->name),"%s",name);
    if(!w->available){w->ready=1;pthread_mutex_unlock(&w->mutex);return;}
    w->pending=1;
    if(!w->started){if(pthread_create(&w->thread,NULL,here_font_work,w)==0)w->started=1;else{w->pending=0;w->ready=1;snprintf(w->error,sizeof(w->error),"Could not start font worker");}}
    pthread_cond_signal(&w->cond);pthread_mutex_unlock(&w->mutex);
}
static void here_font_worker_reply(HereFontWorker *w,SBuf *out,int initial) {
    pthread_mutex_lock(&w->mutex);if(initial||w->ready){unsigned char payload[268]={0};uint32_t values[3]={htonl((uint32_t)w->available),htonl((uint32_t)w->active),htonl((uint32_t)!initial)};memcpy(payload,values,12);memcpy(payload+12,w->error,strlen(w->error));here_frame(out,HW_FONT_REPLY,payload,sizeof(payload));w->ready=0;}pthread_mutex_unlock(&w->mutex);
}
static void here_font_worker_stop(HereFontWorker *w) {
    pthread_mutex_lock(&w->mutex);w->quit=1;w->pending=0;pthread_cond_signal(&w->cond);pthread_mutex_unlock(&w->mutex);
    if(w->started)pthread_join(w->thread,NULL);
    pthread_cond_destroy(&w->cond);pthread_mutex_destroy(&w->mutex);
}
static int here_attach(Here *h,const char *path,int takeover) {
    int fd=here_connect(path);if(fd<0){snprintf(here_error,sizeof(here_error),"Cannot attach: %s",strerror(errno));return 1;}
    uint32_t hello[4]={htonl(2),htonl((uint32_t)h->cols),htonl((uint32_t)h->rows),htonl((uint32_t)takeover)};
    SBuf reply={0};int type=0;
    if(!here_send_frame(fd,HW_HELLO,hello,sizeof(hello),2000)||!here_read_frame(fd,&type,&reply,4000)||type!=HW_OK) {
        snprintf(here_error,sizeof(here_error),"%s",type==HW_ERROR&&reply.p?reply.p:"Session did not accept attachment");free(reply.p);close(fd);return 1;
    }free(reply.p);
    struct termios saved,raw;if(tcgetattr(0,&saved)<0){close(fd);snprintf(here_error,sizeof(here_error),"Cannot read host terminal settings");return 1;}
    raw=saved;cfmakeraw(&raw);raw.c_cc[VMIN]=1;raw.c_cc[VTIME]=0;
    int in_flags=fcntl(0,F_GETFL),out_flags=fcntl(1,F_GETFL);fcntl(0,F_SETFL,in_flags|O_NONBLOCK);fcntl(1,F_SETFL,out_flags|O_NONBLOCK);
    struct sigaction old[7],act;int sigs[]={SIGINT,SIGTERM,SIGHUP,SIGWINCH,SIGTSTP,SIGCONT,SIGPIPE};memset(&act,0,sizeof(act));sigemptyset(&act.sa_mask);act.sa_handler=here_signal;
    here_signal_exit=here_signal_resize=here_signal_suspend=here_signal_continue=0;
    for(int i=0;i<7;i++){act.sa_handler=sigs[i]==SIGPIPE?SIG_IGN:here_signal;sigaction(sigs[i],&act,&old[i]);}
    int entered=tcsetattr(0,TCSADRAIN,&raw)==0;if(entered)here_send_all(1,here_host_enter,sizeof(here_host_enter)-1,1000);
    SBuf incoming={0},outgoing={0},screen={0};size_t outoff=0,screenoff=0;int result=0,leaving=0,reload=0;
    HereFontWorker font;here_font_worker_init(&font);here_font_worker_reply(&font,&outgoing,1);
    unsigned char resume_font[136]={0};int resume_font_valid=0;
    while(entered&&!here_signal_exit&&!leaving) {
        if(here_signal_suspend){here_signal_suspend=0;
            pthread_mutex_lock(&font.mutex);resume_font_valid=font.allowed&&font.name[0];if(resume_font_valid){uint32_t values[2]={htonl(1),htonl((uint32_t)font.size)};memset(resume_font,0,sizeof(resume_font));memcpy(resume_font,values,8);memcpy(resume_font+8,font.name,strlen(font.name));}pthread_mutex_unlock(&font.mutex);
            here_font_worker_stop(&font);
#ifdef __APPLE__
            HCHereHostFont retained=font.font;
#endif
            here_font_worker_init(&font);
#ifdef __APPLE__
            if(retained.active){font.font=retained;font.active=1;}
#endif
            here_send_all(1,here_host_leave,sizeof(here_host_leave)-1,300);tcsetattr(0,TCSANOW,&saved);char focus=0;here_frame(&outgoing,HW_FOCUS,&focus,1);here_send_all(fd,outgoing.p+outoff,outgoing.n-outoff,200);outgoing.n=outoff=0;kill(getpid(),SIGSTOP);here_signal_continue=1;}
        if(here_signal_continue){here_signal_continue=0;if(resume_font_valid)here_font_worker_request(&font,(const char*)resume_font,sizeof(resume_font));tcsetattr(0,TCSANOW,&raw);here_send_all(1,here_host_enter,sizeof(here_host_enter)-1,300);char focus=1;here_frame(&outgoing,HW_FOCUS,&focus,1);here_signal_resize=1;}
        here_font_worker_reply(&font,&outgoing,0);
        if(here_signal_resize){here_signal_resize=0;here_host_size(&h->cols,&h->rows);uint32_t size[2]={htonl((uint32_t)h->cols),htonl((uint32_t)h->rows)};here_frame(&outgoing,HW_SIZE,size,sizeof(size));}
        struct pollfd pf[3]={{0,(short)(outgoing.n-outoff<HERE_CLIENT_MAX-8192?POLLIN:0),0},{fd,(short)((screen.n-screenoff<1024*1024?POLLIN:0)|(outgoing.n>outoff?POLLOUT:0)),0},{1,(short)(screen.n>screenoff?POLLOUT:0),0}};
        int ready=poll(pf,3,100);if(ready<0&&errno!=EINTR){result=1;break;}
        if(pf[0].revents&POLLIN){char buf[4096];ssize_t n=read(0,buf,sizeof(buf));if(n>0){if(outoff){here_consume(&outgoing,outoff);outoff=0;}if(!here_frame(&outgoing,HW_INPUT,buf,(size_t)n)){result=1;break;}}else if(n==0){leaving=1;}else if(errno!=EINTR&&errno!=EAGAIN)leaving=1;}
        if(pf[0].revents&(POLLHUP|POLLERR))leaving=1;
        if((pf[1].revents&POLLOUT)&&outgoing.n>outoff){ssize_t n=write(fd,outgoing.p+outoff,outgoing.n-outoff);if(n>0){outoff+=(size_t)n;if(outoff==outgoing.n)outgoing.n=outoff=0;}else if(n<0&&errno!=EINTR&&errno!=EAGAIN){leaving=1;}}
        if(pf[1].revents&POLLIN){char buf[65536];ssize_t n=read(fd,buf,sizeof(buf));if(n>0){if(!here_append(&incoming,buf,(size_t)n,HERE_WIRE_MAX+65536)){result=1;break;}}else if(n==0)leaving=1;else if(errno!=EINTR&&errno!=EAGAIN)leaving=1;}
        if(pf[1].revents&(POLLHUP|POLLERR|POLLNVAL))leaving=1;
        while(incoming.n){size_t n;int msg;int got=here_peek(&incoming,&msg,&n,HERE_WIRE_MAX);if(got<0){result=1;leaving=1;break;}if(!got)break;char *p=incoming.p+sizeof(HereWire);
            if(msg==HW_OUTPUT){if(screenoff){here_consume(&screen,screenoff);screenoff=0;}if(!here_append(&screen,p,n,HERE_WIRE_MAX)){result=1;leaving=1;break;}}
            else if(msg==HW_FONT){here_font_worker_request(&font,p,n);}
            else if(msg==HW_LEAVE){if(n==4){uint32_t a;memcpy(&a,p,4);reload=ntohl(a)==HERE_RELOAD;}leaving=1;}
            else if(msg==HW_ERROR){snprintf(here_error,sizeof(here_error),"%.*s",(int)n,p);result=1;leaving=1;}
            here_consume(&incoming,sizeof(HereWire)+n);
        }
        if((pf[2].revents&POLLOUT)&&screen.n>screenoff){ssize_t n=write(1,screen.p+screenoff,screen.n-screenoff);if(n>0){screenoff+=(size_t)n;if(screenoff==screen.n)screen.n=screenoff=0;}else if(n<0&&errno!=EINTR&&errno!=EAGAIN){leaving=1;}}
    }
    close(fd);here_font_worker_stop(&font);if(entered){here_send_all(1,here_host_leave,sizeof(here_host_leave)-1,500);tcsetattr(0,TCSANOW,&saved);}
    fcntl(0,F_SETFL,in_flags);fcntl(1,F_SETFL,out_flags);for(int i=0;i<7;i++)sigaction(sigs[i],&old[i],NULL);
    free(incoming.p);free(outgoing.p);free(screen.p);
    if(font.active)fprintf(stderr,"hardcore here: host font restoration needs attention: %s\n",font.error);
    if(reload){char *args[]={h->exe,"here","--attach",h->session,NULL};execv(h->exe,args);snprintf(here_error,sizeof(here_error),"Cannot reload attachment: %s",strerror(errno));return 1;}
    if(!result&&!here_signal_exit)fprintf(stdout,"Detached from %s. Reattach: hardcore here --attach %s\n",h->session,h->session);
    return result;
}
static int here_admin(const char *name,int stop,SBuf *reply) {
    char path[256];if(!here_path(name,path,sizeof(path)))return 0;int fd=here_connect(path);if(fd<0){snprintf(here_error,sizeof(here_error),"Session %s: %s",name,strerror(errno));return 0;}
    int type;int ok=here_send_frame(fd,stop?HW_STOP:HW_STATUS,"",0,400)&&here_read_frame(fd,&type,reply,1000)&&type==(stop?HW_OK:HW_STATUS_REPLY);close(fd);return ok;
}
static int here_list(void) {
    char dir[128];if(!here_runtime(dir,sizeof(dir)))return 1;DIR *d=opendir(dir);if(!d)return 1;struct dirent *e;int count=0;size_t suffix=strlen(HERE_SOCKET_SUFFIX);
    while((e=readdir(d))){size_t n=strlen(e->d_name);if(n<=suffix||strcmp(e->d_name+n-suffix,HERE_SOCKET_SUFFIX))continue;char name[49];size_t len=n-suffix;if(len>=sizeof(name))continue;memcpy(name,e->d_name,len);name[len]=0;SBuf reply={0};if(here_admin(name,0,&reply)){printf("%s\n",reply.p?reply.p:"");count++;}free(reply.p);}
    closedir(d);if(!count)puts("No native terminal-hosted Hardcore sessions.");return 0;
}
static int here_main(int argc,char **argv) {
    Here h;memset(&h,0,sizeof(h));const char *shell=getenv("SHELL");snprintf(h.shell,sizeof(h.shell),"%s",shell&&*shell?shell:"/bin/sh");
    if(!getcwd(h.cwd,sizeof(h.cwd)))strcpy(h.cwd,"/");
#ifdef __APPLE__
    uint32_t exe_n=sizeof(h.exe);if(_NSGetExecutablePath(h.exe,&exe_n)!=0)return 1;char resolved[4096];if(realpath(h.exe,resolved))snprintf(h.exe,sizeof(h.exe),"%s",resolved);
#else
    ssize_t exe_n=readlink("/proc/self/exe",h.exe,sizeof(h.exe)-1);if(exe_n>0)h.exe[exe_n]=0;
#endif
    int attach=0,takeover=0,list=0,stop=0,status=0;here_error[0]=0;
    for(int i=0;i<argc;i++) {
        const char *a=argv[i];
        if(!strcmp(a,"--help")||!strcmp(a,"-h")) {
            puts("hardcore here — native terminal mode, using Hardcore's C engine\n"
                 "Usage: hardcore here [--cwd DIR] [--shell SHELL] [--session NAME |\n"
                 "       --attach NAME | --list | --stop NAME] [--takeover] [-- command ...]\n"
                 "Ctrl-B then ?: help; c: new tab; D: detach; R: reload attachment.\n"
                 "Sessions and their shells survive detaching. No Python/Lua runtime.\n"
                 "--status NAME prints native session details as JSON.");return 0;
        }
        if(!strcmp(a,"--takeover")){takeover=1;continue;}if(!strcmp(a,"--list")){list=1;continue;}
        if(!strcmp(a,"--")){h.command=argv+i+1;break;}
        if(!strcmp(a,"--cwd")||!strcmp(a,"--shell")||!strcmp(a,"--session")||!strcmp(a,"--attach")||!strcmp(a,"--stop")||!strcmp(a,"--status")) {
            if(++i>=argc){fprintf(stderr,"hardcore here: %s needs a value\n",a);return 2;}
            if(!strcmp(a,"--cwd")){if(strlen(argv[i])>=sizeof(h.cwd))return 2;strcpy(h.cwd,argv[i]);}
            else if(!strcmp(a,"--shell")){if(strlen(argv[i])>=sizeof(h.shell))return 2;strcpy(h.shell,argv[i]);}
            else {if(h.session[0]||list){fprintf(stderr,"hardcore here: choose one session operation\n");return 2;}if(!here_name_valid(argv[i])){fprintf(stderr,"hardcore here: invalid session name\n");return 2;}strcpy(h.session,argv[i]);attach=!strcmp(a,"--attach");stop=!strcmp(a,"--stop");status=!strcmp(a,"--status");}
            continue;
        }
        if(a[0]=='-'){fprintf(stderr,"hardcore here: unknown option: %s\n",a);return 2;}h.command=argv+i;break;
    }
    if(list){if(h.session[0]){fprintf(stderr,"hardcore here: choose one session operation\n");return 2;}int r=here_list();if(r)fprintf(stderr,"hardcore here: %s\n",here_error);return r;}
    if(stop||status){SBuf reply={0};int ok=here_admin(h.session,stop,&reply);if(ok&&status)printf("%s\n",reply.p?reply.p:"");free(reply.p);if(!ok)fprintf(stderr,"hardcore here: %s\n",here_error[0]?here_error:"Session did not respond");return ok?0:1;}
    if(!isatty(0)||!isatty(1)){fprintf(stderr,"hardcore here needs an interactive terminal on stdin and stdout.\n");return 2;}
    char cwd[4096];if(!realpath(h.cwd,cwd)){fprintf(stderr,"hardcore here: invalid directory: %s\n",h.cwd);return 2;}struct stat st;if(stat(cwd,&st)<0||!S_ISDIR(st.st_mode)||strlen(cwd)>=sizeof(h.cwd)){fprintf(stderr,"hardcore here: invalid directory\n");return 2;}strcpy(h.cwd,cwd);
    if(!h.session[0])snprintf(h.session,sizeof(h.session),"here-%lx-%lx",(unsigned long)getpid(),(unsigned long)here_now_ms());
    here_host_size(&h.cols,&h.rows);char path[256];if(!here_path(h.session,path,sizeof(path))){fprintf(stderr,"hardcore here: %s\n",here_error);return 1;}
    int fd=here_connect(path);
    if(fd>=0)close(fd);else {
        int e=errno;if(attach){fprintf(stderr,"hardcore here: session %s does not exist or cannot be opened: %s\n",h.session,strerror(e));return 1;}
        if(e!=ENOENT&&e!=ECONNREFUSED){fprintf(stderr,"hardcore here: cannot open session: %s\n",strerror(e));return 1;}
        if(!here_start(&h,path)){/* Another creator may have won the lock. */fd=here_connect(path);if(fd<0){fprintf(stderr,"hardcore here: cannot start session: %s\n",strerror(errno));return 1;}close(fd);}
    }
    int result=here_attach(&h,path,takeover);if(result)fprintf(stderr,"hardcore here: %s\n",here_error);return result;
}
#endif
