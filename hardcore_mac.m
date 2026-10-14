/*
 * hardcore_mac.m — the macOS platform layer, #included at the end of
 * hardcore.c (one translation unit, built with clang -x objective-c).
 *
 * Same shape as dumbterm's Mac side: a Cocoa window with an NSOpenGLView,
 * forkpty for the shell, the pasteboard for copy and paste. What hardcore.c
 * does on Win32 with ConPTY, D3D11, WM_* messages and the clipboard, this
 * does with:
 *
 *   - forkpty: a real local pty; your login shell (zsh, bash or fish) with a
 *     few lines of hooks that report the folder (OSC 9;9) and mark prompts
 *     (OSC 133;A), the same two things rc.bash does under WSL.
 *   - dispatch sources: output, process exit, and a serial queue per tab for
 *     writes, so a big paste never blocks the window.
 *   - OpenGL 2.1 (legacy profile, one GLSL 1.20 shader pair): the same vertex
 *     array and glyph atlas as the Direct3D path, drawn with one glDrawArrays.
 *   - The standard traffic lights over the tab column instead of hardcore's
 *     own glass caption buttons.
 *
 * Mac keys follow Mac habits: Cmd+T / Cmd+W / Cmd+C / Cmd+V, Cmd+1…9,
 * Cmd+Shift+[ ], Cmd+J, Cmd+↑/↓. Left Option is Meta (ESC prefix, what Claude
 * Code and readline expect); right Option still types é, ü, @ and friends.
 */

#define GL_SILENCE_DEPRECATION
#import <Cocoa/Cocoa.h>
#include <OpenGL/gl.h>
#include <util.h>
#include <fcntl.h>
#include <libproc.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>

@class HCView;
static NSWindow *g_win;
static HCView *g_view;
static int g_gl_ready;
static int g_quitting;           /* skip the "programs are running" question: last tab closed */
static NSTimer *g_timers[4];

static const char *g_shell;      /* your login shell */

/* ── platform layer ─────────────────────────────────────────────────── */

static void plat_invalidate(void);

static void plat_client_size(int *w, int *h) {
    NSSize px;
    *w = *h = 0;
    if (!g_view) return;
    px = [(NSView *)g_view convertSizeToBacking:[(NSView *)g_view bounds].size];
    *w = (int)px.width;
    *h = (int)px.height;
}

static void plat_flash(void) {
    if (![NSApp isActive]) [NSApp requestUserAttention:NSInformationalRequest];   /* one Dock bounce */
}

static void make_spare(void);

static void timer_fired(int id) {
    if (id == TIMER_THROB) { update_busy(); return; }
    plat_kill_timer(id);
    if (id == TIMER_SPARE) make_spare();
    else plat_invalidate();
}

static void plat_timer(int id, int ms) {
    plat_kill_timer(id);
    g_timers[id] = [[NSTimer scheduledTimerWithTimeInterval:ms / 1000.0 repeats:(id == TIMER_THROB)
                                                       block:^(NSTimer *tm) { (void)tm; timer_fired(id); }] retain];
    [[NSRunLoop currentRunLoop] addTimer:g_timers[id] forMode:NSEventTrackingRunLoopMode];   /* keep ticking during a live resize */
}

static void plat_kill_timer(int id) {
    if (!g_timers[id]) return;
    [g_timers[id] invalidate];
    [g_timers[id] release];
    g_timers[id] = nil;
}

static void plat_set_title(const wchar_t *title) {
    NSString *s;
    if (!g_win) return;
    s = [[NSString alloc] initWithBytes:title length:wcslen(title) * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding];
    [g_win setTitle:s ? s : @"hardcore"];
    [s release];
}

static void plat_close_window(void) {
    g_quitting = 1;
    [NSApp terminate:nil];
}

/* ── renderer: OpenGL 2.1 ──────────────────────────────────────────── */

static GLuint g_prog, g_atlas_tex;

static const char GLSL_VS[] =
    "#version 120\n"
    "varying vec2 uv; varying vec4 col;\n"
    "void main() { gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); uv = gl_MultiTexCoord0.xy; col = gl_Color; }\n";
static const char GLSL_FS[] =
    "#version 120\n"
    "uniform sampler2D t; varying vec2 uv; varying vec4 col;\n"
    "void main() {\n"
    "  float a = uv.x < 0.0 ? 1.0 : texture2D(t, uv).a;\n"
    "  gl_FragColor = vec4(col.rgb, col.a * a); }\n";

static GLuint gl_shader(GLenum kind, const char *src) {
    GLuint s = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "hardcore: shader: %s\n", log);
    }
    return s;
}

/* needs the view's context current */
static int gl_init(void) {
    GLint ok = 0;
    g_prog = glCreateProgram();
    glAttachShader(g_prog, gl_shader(GL_VERTEX_SHADER, GLSL_VS));
    glAttachShader(g_prog, gl_shader(GL_FRAGMENT_SHADER, GLSL_FS));
    glLinkProgram(g_prog);
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) return 0;
    glGenTextures(1, &g_atlas_tex);
    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
    g_gl_ready = 1;
    return 1;
}

static void r_flush(void) {
    if (!g_nvtx || !g_gl_ready) { g_nvtx = 0; return; }
    glVertexPointer(2, GL_FLOAT, sizeof(Vtx), &g_vtx[0].x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(Vtx), &g_vtx[0].u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vtx), &g_vtx[0].rgba);
    glDrawArrays(GL_TRIANGLES, 0, g_nvtx);
    g_nvtx = 0;
}

static void r_upload(int x, int y, const unsigned char *bmp) {
    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 16, 16, GL_ALPHA, GL_UNSIGNED_BYTE, bmp);
}

static void r_begin(int w, int h) {
    g_rw = (float)w;
    g_rh = (float)h;
    g_nvtx = 0;
    glViewport(0, 0, w, h);
    glClearColor(((DEF_BG >> 16) & 255) / 255.0f, ((DEF_BG >> 8) & 255) / 255.0f, (DEF_BG & 255) / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "t"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_atlas_tex);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
}

static void r_present(void) {
    r_flush();
    [[(NSOpenGLView *)g_view openGLContext] flushBuffer];
}

/* test channel: the frame about to be presented, as a bottom-up 32-bit .bmp */
static void snap_backbuffer(void) {
    int w, h, y;
    unsigned char *px;
    FILE *f;
    uint32_t hdr[13];
    plat_client_size(&w, &h);
    if (w < 1 || h < 1 || !(f = fopen(g_snap_path, "wb"))) return;
    px = (unsigned char *)malloc((size_t)w * h * 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, px);
    memset(hdr, 0, sizeof(hdr));
    fwrite("BM", 1, 2, f);
    hdr[0] = 54 + w * h * 4; hdr[2] = 54;        /* file size, pixel offset */
    hdr[3] = 40; hdr[4] = (uint32_t)w; hdr[5] = (uint32_t)h;
    hdr[6] = 1 | (32 << 16);                     /* planes, bits per pixel */
    fwrite(hdr, 4, 13, f);
    for (y = 0; y < h; y++) fwrite(px + (size_t)y * w * 4, 4, (size_t)w, f);
    fclose(f);
    free(px);
}

/* ── shell integration ─────────────────────────────────────────────── */

/* bash: login-shell startup files, then report $PWD and mark each prompt.
   Written on every launch to the config folder; your dotfiles never change. */
static const char RC_BASH_MAC[] =
    "# written by hardcore on every launch; edits here are overwritten\n"
    "[ -r /etc/profile ] && . /etc/profile\n"
    "if [ -r ~/.bash_profile ]; then . ~/.bash_profile\n"
    "elif [ -r ~/.bash_login ]; then . ~/.bash_login\n"
    "elif [ -r ~/.profile ]; then . ~/.profile\n"
    "elif [ -r ~/.bashrc ]; then . ~/.bashrc\n"
    "fi\n"
    "__hardcore_cwd() { printf '\\033]9;9;%s\\033\\\\\\033]133;A\\033\\\\' \"$PWD\"; }\n"
    "# report the folder on every cd too, so \"cd x && claude\" labels the tab x right away\n"
    "if ! declare -F cd >/dev/null; then\n"
    "  cd() { builtin cd \"$@\" && printf '\\033]9;9;%s\\033\\\\' \"$PWD\"; }\n"
    "fi\n"
    "case \";${PROMPT_COMMAND};\" in *\";__hardcore_cwd;\"*) ;; *)\n"
    "  PROMPT_COMMAND=\"__hardcore_cwd${PROMPT_COMMAND:+;$PROMPT_COMMAND}\" ;;\n"
    "esac\n";

/* zsh: ZDOTDIR points here first. This .zshenv puts your ZDOTDIR back, so
   zsh goes on to read your own .zshenv, .zprofile, .zshrc and .zlogin, and
   adds two hooks for interactive shells. */
static const char ZSHENV[] =
    "# written by hardcore on every launch; edits here are overwritten\n"
    "if [[ -n ${HARDCORE_ZDOTDIR+x} ]]; then ZDOTDIR=$HARDCORE_ZDOTDIR; else unset ZDOTDIR; fi\n"
    "unset HARDCORE_ZDOTDIR\n"
    "[[ -r ${ZDOTDIR:-$HOME}/.zshenv ]] && builtin source ${ZDOTDIR:-$HOME}/.zshenv\n"
    "if [[ -o interactive ]]; then\n"
    "  __hardcore_cwd() { builtin printf '\\e]9;9;%s\\e\\\\\\e]133;A\\e\\\\' \"$PWD\" }\n"
    "  __hardcore_dir() { builtin printf '\\e]9;9;%s\\e\\\\' \"$PWD\" }\n"
    "  typeset -ag precmd_functions chpwd_functions\n"
    "  precmd_functions+=(__hardcore_cwd)\n"
    "  chpwd_functions+=(__hardcore_dir)\n"
    "fi\n";

/* fish: one --init-command */
static const char FISH_INIT[] =
    "function __hardcore_cwd --on-event fish_prompt; printf '\\e]9;9;%s\\e\\\\\\e]133;A\\e\\\\' $PWD; end; "
    "function __hardcore_dir --on-variable PWD; printf '\\e]9;9;%s\\e\\\\' $PWD; end";

static void write_file(const char *dir, const char *name, const char *text) {
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if ((f = fopen(path, "wb"))) {
        fputs(text, f);
        fclose(f);
    }
}

static const char *shell_base(void) {
    const char *b = strrchr(g_shell, '/');
    return b ? b + 1 : g_shell;
}

static void write_rc(void) {
    char zdir[MAX_PATH];
    struct passwd *pw = getpwuid(getuid());
    const char *sh = getenv("HARDCORE_SHELL");   /* tests */
    if (!sh || !sh[0]) sh = pw && pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : getenv("SHELL");
    g_shell = strdup(sh && sh[0] ? sh : "/bin/zsh");
    write_file(g_cfg_dir, "rc.bash", RC_BASH_MAC);
    snprintf(zdir, sizeof(zdir), "%s/zsh", g_cfg_dir);
    mkdir(zdir, 0755);
    write_file(zdir, ".zshenv", ZSHENV);
}

/* ── tabs: forkpty + dispatch sources ──────────────────────────────── */

static void on_output(void);
static void on_tab_exited(int id, int code);

static int g_output_pending;

static void schedule_output(void) {
    if (g_output_pending) return;
    g_output_pending = 1;
    dispatch_async(dispatch_get_main_queue(), ^{ g_output_pending = 0; on_output(); });
}

/* writes go through a serial queue per tab: a big paste into a busy program
   blocks that queue, never the window */
static void tab_write(Tab *tb, const void *s, int n) {
    char *copy;
    int fd = tb->fd;
    if (!tb->wq || fd < 0 || n <= 0) return;   /* no shell (a --replay terminal, or it's gone) */
    copy = (char *)malloc((size_t)n);
    memcpy(copy, s, (size_t)n);
    dispatch_async((dispatch_queue_t)tb->wq, ^{
        int off = 0;
        while (off < n) {
            ssize_t w = write(fd, copy + off, (size_t)(n - off));
            if (w > 0) off += (int)w;
            else if (w < 0 && errno == EINTR) continue;
            else break;
        }
        free(copy);
    });
}

static void tab_set_size(Tab *tb, int cols, int rows) {
    struct winsize ws;
    if (!tb->wq || tb->fd < 0) return;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ws.ws_xpixel = (unsigned short)(cols * CW);
    ws.ws_ypixel = (unsigned short)(rows * CH);
    ioctl(tb->fd, TIOCSWINSZ, &ws);
}

/* Stops reading; the fd closes once the writes already queued have gone out. */
static void stop_reading(Tab *tb) {
    int fd = tb->fd;
    dispatch_queue_t wq = (dispatch_queue_t)tb->wq;
    if (tb->rsrc) {
        dispatch_source_t r = (dispatch_source_t)tb->rsrc;
        dispatch_source_set_cancel_handler(r, ^{ dispatch_async(wq, ^{ close(fd); }); });
        dispatch_source_cancel(r);
        dispatch_release(r);
        tb->rsrc = NULL;
    } else if (fd >= 0) {
        dispatch_async(wq, ^{ close(fd); });
    }
    tb->fd = -1;
}

static void read_some(Tab *tb) {
    char buf[65536];
    ssize_t n = read(tb->fd, buf, sizeof(buf));
    if (n > 0) {
        if (tb->in_n + (int)n > tb->in_cap) {
            int cap = tb->in_cap ? tb->in_cap : 65536;
            while (cap < tb->in_n + (int)n) cap *= 2;
            tb->in = (char *)realloc(tb->in, cap);
            tb->in_cap = cap;
        }
        memcpy(tb->in + tb->in_n, buf, (size_t)n);
        tb->in_n += (int)n;
        schedule_output();
    } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        stop_reading(tb);        /* EIO: every copy of the slave side is closed */
    }
}

static int tab_drain(Tab *tb) {
    char *data = tb->in;
    int n = tb->in_n;
    tb->in = NULL;
    tb->in_n = tb->in_cap = 0;
    if (n) vt_feed(&tb->t, (unsigned char *)data, n);
    free(data);
    return n;
}

/* the environment every tab's shell starts with */
static void child_env(void) {
    static const char *drop[] = {"TERM_SESSION_ID", "ITERM_SESSION_ID", "ITERM_PROFILE", "LC_TERMINAL", "LC_TERMINAL_VERSION",
                                 "WT_SESSION", "VTE_VERSION", "TMUX", "TMUX_PANE", "STY",
                                 /* started from inside Claude Code: its session markers would make every
                                    claude in our tabs a "child session" (no transcripts, wrong session id) */
                                 "CLAUDECODE", "CLAUDE_PID", "CLAUDE_EFFORT", "AI_AGENT", "CLAUDE_CODE_ENTRYPOINT",
                                 "CLAUDE_CODE_SSE_PORT", "CLAUDE_CODE_CHILD_SESSION", "CLAUDE_CODE_EXECPATH",
                                 "CLAUDE_CODE_SESSION_ID", "CLAUDE_CODE_SESSION_ATTENDED", "CLAUDE_CODE_MESSAGING_SOCKET",
                                 "CLAUDE_CODE_MESSAGING_TOKEN", "OLDPWD",
                                 /* our own test knobs */
                                 "HARDCORE_CTL", "HARDCORE_NO_ACTIVATE", "HARDCORE_SHELL"};
    size_t i;
    for (i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) unsetenv(drop[i]);
    /* 24-bit color (chalk drops to 256 colors without it), OSC 8 links */
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "hardcore", 1);
    setenv("TERM_PROGRAM_VERSION", HARDCORE_VERSION, 1);
    setenv("FORCE_HYPERLINK", "1", 1);
    setenv("BASH_SILENCE_DEPRECATION_WARNING", "1", 1);   /* macOS bash 3.2's "now zsh" nag */
    /* started from Finder there's no locale, and zsh and bash then mangle UTF-8 */
    if (!getenv("LANG") && !getenv("LC_ALL") && !getenv("LC_CTYPE")) setenv("LANG", "en_US.UTF-8", 1);
}

static void expand_home(const char *in, char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home) snprintf(out, cap, "%s%s", home, in + 1);
    else snprintf(out, cap, "%s", in);
}

/* in the child, after forkpty: start the shell in dir */
static void exec_shell(const char *dir) {
    char cwd[MAX_PATH], zdir[MAX_PATH], argv0[256], rc[MAX_PATH];
    const char *base = shell_base();
    expand_home(dir, cwd, sizeof(cwd));
    if (chdir(cwd) != 0 && (!getenv("HOME") || chdir(getenv("HOME")) != 0)) chdir("/");
    child_env();
    signal(SIGPIPE, SIG_DFL);
    snprintf(argv0, sizeof(argv0), "-%s", base);   /* a login shell, like Terminal starts */
    if (!strcmp(base, "zsh")) {
        snprintf(zdir, sizeof(zdir), "%s/zsh", g_cfg_dir);
        if (getenv("ZDOTDIR")) setenv("HARDCORE_ZDOTDIR", getenv("ZDOTDIR"), 1);
        setenv("ZDOTDIR", zdir, 1);
        execl(g_shell, argv0, (char *)NULL);
    } else if (!strcmp(base, "bash")) {
        /* not a login shell: --rcfile only counts for a non-login one, and rc.bash reads the login files itself */
        snprintf(rc, sizeof(rc), "%s/rc.bash", g_cfg_dir);
        execl(g_shell, "bash", "--rcfile", rc, "-i", (char *)NULL);
    } else if (!strcmp(base, "fish")) {
        execl(g_shell, argv0, "--init-command", FISH_INIT, (char *)NULL);
    } else {
        execl(g_shell, argv0, (char *)NULL);
    }
    fprintf(stderr, "hardcore: can't run %s: %s\r\n", g_shell, strerror(errno));
    _exit(127);
}

static int tab_spawn(Tab *tb) {
    struct winsize ws;
    pid_t pid;
    int fd, id = tb->id;
    dispatch_source_t r, p;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)tb->t.cols;
    ws.ws_row = (unsigned short)tb->t.rows;
    ws.ws_xpixel = (unsigned short)(tb->t.cols * CW);
    ws.ws_ypixel = (unsigned short)(tb->t.rows * CH);
    pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "\x1b[31mhardcore: forkpty failed: %s\x1b[0m\r\n", strerror(errno));
        term_message(&tb->t, msg);
        return 0;
    }
    if (pid == 0) exec_shell(tb->start_dir);
    fcntl(fd, F_SETFD, FD_CLOEXEC);    /* later tabs' shells mustn't hold this one open */
    tb->fd = fd;
    tb->pid = pid;
    tb->born = GetTickCount();
    tb->wq = dispatch_queue_create("hardcore.tab.write", DISPATCH_QUEUE_SERIAL);
    r = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(r, ^{
        Tab *t = tab_by_id(id);
        if (!t && g_spare && g_spare->id == id) t = g_spare;
        if (t && t->fd >= 0) read_some(t);
    });
    dispatch_resume(r);
    tb->rsrc = r;
    /* exit: catches the shell even while a background job keeps the pty open;
       holds only the pid and id, so it reaps tabs that were already closed too */
    p = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
    dispatch_source_set_event_handler(p, ^{
        int st = 0, code;
        if (waitpid(pid, &st, WNOHANG) == 0) return;
        code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        dispatch_source_cancel(p);
        dispatch_release(p);
        on_tab_exited(id, code);
    });
    dispatch_resume(p);
    return 1;
}

static Tab *tab_create(const char *dir, int spare) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) {
        free(tb);
        return NULL;
    }
    tb->id = g_next_id++;
    tb->fd = -1;
    tb->spare = spare;
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", dir && *dir ? dir : g_start_dir);
    if (tb->start_dir[0] == '/') set_cwd(&tb->t, tb->start_dir);
    tab_spawn(tb);
    return tb;
}

static void tab_destroy_async(Tab *tb) {
    if (tb->pid > 0 && !tb->exited) {
        kill(-tb->pid, SIGHUP);  /* its process group; closing the pty hangs up the foreground job too */
        kill(tb->pid, SIGHUP);
    }
    stop_reading(tb);
    if (tb->wq) dispatch_release((dispatch_queue_t)tb->wq);
    free(tb->in);
    term_free(&tb->t);
    free(tb);
}

/* The program in the foreground of a tab's pty, if it isn't the shell itself
   (claude, vim, a build...). Returns 0 when it's just the shell at a prompt. */
static int tab_busy_program(Tab *tb, char *name, int cap) {
    pid_t fg;
    if (tb->fd < 0 || tb->exited) return 0;
    fg = tcgetpgrp(tb->fd);
    if (fg <= 0 || fg == tb->pid) return 0;
    if (proc_name(fg, name, (uint32_t)cap) <= 0) snprintf(name, cap, "a program");
    return 1;
}

/* the folder a shell is in, from the kernel: for shells without our hooks */
static void tab_proc_cwd(Tab *tb, char *out, int cap) {
    struct proc_vnodepathinfo vi;
    out[0] = 0;
    if (tb->pid > 0 && proc_pidinfo(tb->pid, PROC_PIDVNODEPATHINFO, 0, &vi, sizeof(vi)) == (int)sizeof(vi))
        snprintf(out, cap, "%s", vi.pvi_cdir.vip_path);
}

/* ── output, exit ──────────────────────────────────────────────────── */

static void paint_view_now(void);

static void on_output(void) {
    int i, changed = 0;
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tab_drain(tb) <= 0) continue;
        if (i == g_active) changed = 1;
        else tb->activity = 1;
        if (tb->t.bell) {
            tb->t.bell = 0;
            if (!g_focused || i != g_active) {
                tb->attention = 1;
                if (!g_focused) plat_flash();
            }
        }
        changed = 1;
    }
    update_busy();
    if (changed) {
        update_title();
        plat_invalidate();
        /* under a flood of output, still show a frame every ~30ms */
        if (GetTickCount() - g_last_paint > 30) paint_view_now();
    }
}

static void replace_tab(int i) {
    Tab *old = g_tabs[i], *tb = tab_create(old->t.cwd[0] ? old->t.cwd : old->start_dir, 0);
    if (!tb) return;
    g_tabs[i] = tb;
    tab_destroy_async(old);
    activate(i);
}

/* A shell that died within 3 s with an error is a setup problem, not you
   typing exit: keep the tab and say so, instead of the window vanishing. */
static void on_tab_exited(int id, int code) {
    Tab *tb = tab_by_id(id);
    int i;
    char buf[300];
    if (!tb) return;             /* already closed: this was just the reaping */
    if (tb->fd >= 0) {           /* whatever it said last */
        fcntl(tb->fd, F_SETFL, O_NONBLOCK);
        while (tb->fd >= 0) {
            int before = tb->in_n;
            read_some(tb);
            if (tb->in_n == before) break;
        }
        stop_reading(tb);
    }
    tab_drain(tb);
    tb->exited = 1;
    if (code != 0 && GetTickCount() - tb->born < 3000 && !tb->help) {
        tb->help = HELP_FAILED;
        tb->t.m_cursor_vis = 0;
        snprintf(buf, sizeof(buf), "\r\n\x1b[2m%s exited right away (code %d).\x1b[0m\r\n\r\n"
                                   "  \x1b[38;2;255;166;13mEnter\x1b[0m  try again\r\n", g_shell, code);
        term_message(&tb->t, buf);
        plat_invalidate();
        return;
    }
    for (i = 0; i < g_ntabs; i++)
        if (g_tabs[i] == tb) close_tab(i);
}

/* ── config ────────────────────────────────────────────────────────── */

static void config_load(void) {
    char path[MAX_PATH], line[1200];
    const char *home = getenv("HOME");
    FILE *f;
    snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s/Library/Application Support/hardcore", home ? home : "/tmp");
    mkdir(g_cfg_dir, 0755);
    snprintf(path, sizeof(path), "%s/config.ini", g_cfg_dir);
    if (!(f = fopen(path, "rb"))) return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        size_t n;
        if (!v || line[0] == '#') continue;
        *v++ = 0;
        n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
        if (!strcmp(line, "dir") && *v) expand_home(v, g_start_dir, sizeof(g_start_dir));
    }
    fclose(f);
}

static void config_save(void) {
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof(path), "%s/config.ini", g_cfg_dir);
    if (!(f = fopen(path, "wb"))) return;
    fprintf(f, "# hardcore settings\n");
    fprintf(f, "# starting folder for new windows; empty = your home folder\n");
    fprintf(f, "dir=%s\n", g_cfg_dir_line);
    fclose(f);
}

/* ── clipboard, drops, links ───────────────────────────────────────── */

static void set_clipboard_utf8(const char *s, int n) {
    NSString *str = [[NSString alloc] initWithBytes:s length:(NSUInteger)n encoding:NSUTF8StringEncoding];
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    if (!str) return;
    [pb clearContents];
    [pb setString:str forType:NSPasteboardTypeString];
    [str release];
}

/* a path as you'd type it, shell-quoted only when it needs to be */
static void quoted_path(const char *path, char *q, size_t cap) {
    static const char safe[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-+:@%,=";
    size_t k, j = 0;
    int needq = !path[0];
    for (k = 0; path[k]; k++)
        if (!strchr(safe, path[k])) needq = 1;
    if (needq) q[j++] = '\'';
    for (k = 0; path[k] && j + 6 < cap; k++) {
        if (path[k] == '\'') { memcpy(q + j, "'\\''", 4); j += 4; }
        else q[j++] = path[k];
    }
    if (needq) q[j++] = '\'';
    q[j] = 0;
}

/* files (dropped, or copied in Finder) type their paths, space separated */
static int paste_file_urls(NSPasteboard *pb) {
    NSArray *urls = [pb readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    NSMutableString *out;
    if (![urls count]) return 0;
    out = [NSMutableString string];
    for (NSURL *u in urls) {
        char q[MAX_PATH * 2 + 16];
        quoted_path([[u path] fileSystemRepresentation], q, sizeof(q));
        [out appendFormat:@"%s ", q];
    }
    paste_utf8([out UTF8String]);
    return 1;
}

/* An image on the pasteboard (a screenshot, "Copy Image") is saved as a PNG
   under the config folder's pasted/ and its path is typed instead. Claude
   Code's own Ctrl+V image paste still goes straight through. */
static int paste_image(NSPasteboard *pb) {
    NSImage *img = [[[NSImage alloc] initWithPasteboard:pb] autorelease];
    NSBitmapImageRep *rep;
    NSData *png;
    char dir[MAX_PATH], path[MAX_PATH], q[MAX_PATH * 2 + 16];
    struct timespec ts;
    struct tm tm;
    if (!img || ![pb availableTypeFromArray:@[NSPasteboardTypePNG, NSPasteboardTypeTIFF]]) return 0;
    rep = [NSBitmapImageRep imageRepWithData:[img TIFFRepresentation]];
    png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (!png) return 0;
    snprintf(dir, sizeof(dir), "%s/pasted", g_cfg_dir);
    mkdir(dir, 0755);
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(path, sizeof(path), "%s/%04d%02d%02d-%02d%02d%02d-%03d.png", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    if (![png writeToFile:[NSString stringWithUTF8String:path] atomically:YES]) return 0;
    quoted_path(path, q, sizeof(q));
    strcat(q, " ");
    paste_utf8(q);
    return 1;
}

/* Text pastes as text; files as their paths; an image as a saved PNG's path. */
static void paste(void) {
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *s;
    if (paste_file_urls(pb)) return;
    if ((s = [pb stringForType:NSPasteboardTypeString])) { paste_utf8([s UTF8String]); return; }
    paste_image(pb);
}

static void open_url(const char *url) {
    NSString *s = [NSString stringWithUTF8String:url];
    NSURL *u = s ? [NSURL URLWithString:s] : nil;
    if (!u && s && [s hasPrefix:@"file://"])    /* unescaped spaces and such */
        u = [NSURL fileURLWithPath:[[s substringFromIndex:7] stringByRemovingPercentEncoding] ?: [s substringFromIndex:7]];
    if (u) [[NSWorkspace sharedWorkspace] openURL:u];
}

/* ── tabs the Mac way ──────────────────────────────────────────────── */

/* Closing a tab (or the window) that's running something asks first; a tab
   sitting at a prompt just closes. Returns 1 to go ahead. */
static int confirm_close(int tab_index) {
    char name[256] = "", list[600] = "";
    int i, n = 0, ok;
    NSAlert *a;
    for (i = 0; i < g_ntabs; i++) {
        if (tab_index >= 0 && i != tab_index) continue;
        if (tab_busy_program(g_tabs[i], name, sizeof(name))) {
            if (n++ < 3) snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", n > 1 ? ", " : "", name);
        }
    }
    if (!n) return 1;
    a = [[NSAlert alloc] init];
    [a setMessageText:tab_index >= 0 ? @"Close this tab?" : @"Quit hardcore?"];
    [a setInformativeText:[NSString stringWithFormat:@"%s %s still running.", list, n == 1 ? "is" : "are"]];
    [a addButtonWithTitle:tab_index >= 0 ? @"Close" : @"Quit"];
    [a addButtonWithTitle:@"Cancel"];
    ok = [a runModal] == NSAlertFirstButtonReturn;
    [a release];
    return ok;
}

static void user_close_tab(int i) {
    if (i >= 0 && i < g_ntabs && confirm_close(i)) close_tab(i);
}

/* New tabs open where the current one is: the folder its shell last reported,
   or, for a shell without our hooks, where the kernel says it is. */
static void new_tab_mac(void) {
    Tab *cur = active_tab();
    if (cur && !cur->t.cwd[0]) {
        char dir[MAX_PATH];
        tab_proc_cwd(cur, dir, sizeof(dir));
        if (dir[0]) set_cwd(&cur->t, dir);
    }
    new_tab_here();
}

/* ── the view: drawing, keys, mouse, drops ─────────────────────────── */

@interface HCView : NSOpenGLView <NSTextInputClient> {
@public
    NSMutableAttributedString *marked;
    float scroll_acc;
    int handled;                 /* keyDown sent something itself; skip insertText */
}
@end

static void plat_invalidate(void) { [(NSView *)g_view setNeedsDisplay:YES]; }

static void paint_view_now(void) {
    if (!g_gl_ready) return;
    [[(NSOpenGLView *)g_view openGLContext] makeCurrentContext];
    paint_now();
}

/* point in the view → pixels from the top left, like the Win32 client area */
static void view_px(NSEvent *e, int *x, int *y) {
    NSPoint p = [(NSView *)g_view convertPoint:[e locationInWindow] fromView:nil];
    CGFloat s = [g_win backingScaleFactor];
    *x = (int)(p.x * s);
    *y = (int)(p.y * s);
}

static int ev_mods(NSEvent *e) {
    NSEventModifierFlags f = [e modifierFlags];
    /* Option drags select, the Mac habit; Shift does too, the hardcore one */
    return ((f & (NSEventModifierFlagShift | NSEventModifierFlagOption)) ? M_SHIFT : 0) |
           ((f & NSEventModifierFlagControl) ? M_CTRL : 0);
}

static void send_str(const char *s) {
    g_sel_on = 0;
    send_to_active(s, (int)strlen(s));
}

@implementation HCView

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (void)prepareOpenGL {
    GLint swap = 0;
    [super prepareOpenGL];
    [[self openGLContext] setValues:&swap forParameter:NSOpenGLContextParameterSwapInterval];
    if (!gl_init()) fprintf(stderr, "hardcore: OpenGL setup failed\n");
}

- (void)reshape {
    [super reshape];
    resize_all();
    paint_view_now();   /* during a live resize, draw now rather than show a stretched frame */
}

- (void)viewDidChangeBackingProperties {
    int s = (int)([g_win backingScaleFactor] + 0.5);
    [super viewDidChangeBackingProperties];
    g_scale = s < 1 ? 1 : s;
    resize_all();
    plat_invalidate();
}

- (void)drawRect:(NSRect)r {
    (void)r;
    paint_view_now();
}

- (void)resetCursorRects {
    NSRect b = [self bounds];
    CGFloat side = sidebar_w() / [g_win backingScaleFactor];
    [self addCursorRect:NSMakeRect(0, 0, side, b.size.height) cursor:[NSCursor arrowCursor]];
    [self addCursorRect:NSMakeRect(side, 0, b.size.width - side, b.size.height) cursor:[NSCursor IBeamCursor]];
}

- (void)updateTrackingAreas {
    for (NSTrackingArea *a in [self trackingAreas]) [self removeTrackingArea:a];
    [self addTrackingArea:[[[NSTrackingArea alloc] initWithRect:[self bounds]
                                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                                 NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                           owner:self userInfo:nil] autorelease]];
    [super updateTrackingAreas];
}

/* ── keys ── */

/* xterm's modifier parameter: 1 + shift(1) + meta(2) + ctrl(4), 0 = none */
static int key_mods(NSEventModifierFlags f, int meta) {
    int m = ((f & NSEventModifierFlagShift) ? 1 : 0) | (meta ? 2 : 0) | ((f & NSEventModifierFlagControl) ? 4 : 0);
    return m ? m + 1 : 0;
}

/* Keys hardcore turns into escape sequences itself. Returns 1 if sent. */
- (int)specialKey:(NSEvent *)e {
    NSEventModifierFlags f = [e modifierFlags];
    unsigned short kc = [e keyCode];
    Tab *tb = active_tab();
    int shift = (f & NSEventModifierFlagShift) != 0, ctrl = (f & NSEventModifierFlagControl) != 0;
    int cmd = (f & NSEventModifierFlagCommand) != 0;
    int meta = (f & 0x20) != 0;  /* NX_DEVICELALTKEYMASK: the left Option key */
    int m = key_mods(f, meta);
    char buf[32];
    const char *seq = NULL;
    int n = 0;
    if (!tb) return 0;
    switch (kc) {
    case 126: case 125: case 124: case 123: case 115: case 119: {   /* arrows, Home, End */
        char c = kc == 126 ? 'A' : kc == 125 ? 'B' : kc == 124 ? 'C' : kc == 123 ? 'D' : kc == 115 ? 'H' : 'F';
        if (cmd && (kc == 123 || kc == 124)) { seq = kc == 123 ? "\x01" : "\x05"; break; }   /* Cmd+←/→: line start / end */
        if (meta && !shift && !ctrl && (kc == 123 || kc == 124)) { seq = kc == 123 ? "\x1b" "b" : "\x1b" "f"; break; }   /* word */
        if (shift && !ctrl && !meta && (kc == 115 || kc == 119)) { scroll_view(kc == 115 ? tb->t.hcount : -tb->t.hcount); return 1; }
        if (m) n = snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, c);
        else n = snprintf(buf, sizeof(buf), tb->t.m_appcursor ? "\x1bO%c" : "\x1b[%c", c);
        break;
    }
    case 116: case 121:          /* PgUp / PgDn; with Shift, scrollback */
        if (shift && !ctrl && !meta) { scroll_view(kc == 116 ? tb->t.rows - 1 : -(tb->t.rows - 1)); return 1; }
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", kc == 116 ? 5 : 6, m) : snprintf(buf, sizeof(buf), "\x1b[%d~", kc == 116 ? 5 : 6);
        break;
    case 117: case 114:          /* forward delete, Help/Insert */
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", kc == 117 ? 3 : 2, m) : snprintf(buf, sizeof(buf), "\x1b[%d~", kc == 117 ? 3 : 2);
        break;
    case 122: case 120: case 99: case 118: {   /* F1-F4 */
        char c = kc == 122 ? 'P' : kc == 120 ? 'Q' : kc == 99 ? 'R' : 'S';
        n = m ? snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, c) : snprintf(buf, sizeof(buf), "\x1bO%c", c);
        break;
    }
    case 96: case 97: case 98: case 100: case 101: case 109: case 103: case 111: {   /* F5-F12 */
        static const unsigned short kcs[] = {96, 97, 98, 100, 101, 109, 103, 111};
        static const int codes[] = {15, 17, 18, 19, 20, 21, 23, 24};
        int i;
        for (i = 0; i < 8 && kcs[i] != kc; i++) {}
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", codes[i], m) : snprintf(buf, sizeof(buf), "\x1b[%d~", codes[i]);
        break;
    }
    case 36: case 76:            /* Return, keypad Enter: Shift/Option+Enter = ESC CR, Claude Code's newline */
        seq = (shift || (f & NSEventModifierFlagOption)) ? "\x1b\r" : "\r";
        break;
    case 51:                     /* Backspace */
        seq = cmd ? "\x15" : ctrl ? "\x17" : meta ? "\x1b\x7f" : "\x7f";
        break;
    case 48:                     /* Tab; Ctrl+Tab switches tabs */
        if (ctrl) { activate((g_active + (shift ? g_ntabs - 1 : 1)) % g_ntabs); return 1; }
        seq = shift ? "\x1b[Z" : "\t";
        break;
    case 53:
        seq = "\x1b";
        break;
    }
    if (cmd && !seq) return 0;
    if (seq) n = (int)strlen(seq), memcpy(buf, seq, (size_t)n);
    if (!n) return 0;
    g_sel_on = 0;
    send_to_active(buf, n);
    return 1;
}

/* Ctrl+key and Meta (left Option)+key, from the unmodified character */
- (int)controlKey:(NSEvent *)e {
    NSEventModifierFlags f = [e modifierFlags];
    NSString *chars = [e charactersIgnoringModifiers];
    int ctrl = (f & NSEventModifierFlagControl) != 0, meta = (f & 0x20) != 0;
    char buf[16];
    int n = 0;
    unichar c;
    if ((!ctrl && !meta) || [chars length] != 1) return 0;
    c = [chars characterAtIndex:0];
    if (meta) buf[n++] = 0x1B;
    if (ctrl) {
        if (c >= 'a' && c <= 'z') buf[n++] = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') buf[n++] = (char)(c - 'A' + 1);
        else if (c == ' ' || c == '@' || c == '2') buf[n++] = 0;
        else if (c == '[' || c == '3') buf[n++] = 0x1B;
        else if (c == '\\' || c == '4') buf[n++] = 0x1C;
        else if (c == ']' || c == '5') buf[n++] = 0x1D;
        else if (c == '^' || c == '6') buf[n++] = 0x1E;
        else if (c == '_' || c == '-' || c == '/' || c == '7') buf[n++] = 0x1F;
        else if (c == '?' || c == '8') buf[n++] = 0x7F;
        else return 0;
    } else {
        NSString *s = [chars precomposedStringWithCanonicalMapping];
        const char *u = [s UTF8String];
        size_t k = strlen(u);
        if (k + 1 >= sizeof(buf)) return 0;
        memcpy(buf + n, u, k);
        n += (int)k;
    }
    g_sel_on = 0;
    send_to_active(buf, n);
    return 1;
}

- (void)keyDown:(NSEvent *)e {
    NSEventModifierFlags f = [e modifierFlags];
    Tab *tb = active_tab();
    if (!tb) return;
    [NSCursor setHiddenUntilMouseMoves:YES];
    if (tb->help) {
        int at;
        if ([e keyCode] == 36 || [e keyCode] == 76)
            for (at = 0; at < g_ntabs; at++) if (g_tabs[at] == tb) { replace_tab(at); break; }
        return;
    }
    if (f & NSEventModifierFlagCommand) {
        NSString *c = [e charactersIgnoringModifiers];
        unichar ch = [c length] ? [c characterAtIndex:0] : 0;
        if (ch >= '1' && ch <= '9') { activate(ch == '9' ? g_ntabs - 1 : ch - '1'); return; }
        [self specialKey:e];     /* Cmd+←/→/Backspace; other Cmd keys belong to the menus or nobody */
        return;
    }
    if ([self specialKey:e] || [self controlKey:e]) return;
    handled = 0;
    [self interpretKeyEvents:@[e]];   /* text, dead keys, input methods → insertText: */
}

/* Ctrl+Tab and Ctrl+Shift+Tab never reach keyDown by default (the window
   uses them to move between controls) */
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    if ([e type] == NSEventTypeKeyDown && [e keyCode] == 48 && ([e modifierFlags] & NSEventModifierFlagControl) &&
        [[self window] firstResponder] == self) {
        [self specialKey:e];
        return YES;
    }
    return [super performKeyEquivalent:e];
}

- (void)flagsChanged:(NSEvent *)e {
    /* Cmd held over a link: the pointing hand */
    NSPoint p = [self convertPoint:[g_win mouseLocationOutsideOfEventStream] fromView:nil];
    CGFloat s = [g_win backingScaleFactor];
    int col, row;
    char url[2048];
    if (!NSPointInRect(p, [self bounds]) || p.x * s < sidebar_w()) return;
    mouse_cell((int)(p.x * s), (int)(p.y * s), &col, &row);
    if (([e modifierFlags] & NSEventModifierFlagCommand) && link_at(col, row, url, sizeof(url))) [[NSCursor pointingHandCursor] set];
    else [[NSCursor IBeamCursor] set];
}

/* NSTextInputClient: what text the keys (and input methods) produced */
- (void)insertText:(id)s replacementRange:(NSRange)r {
    NSString *str = [s isKindOfClass:[NSAttributedString class]] ? [s string] : s;
    (void)r;
    [marked release];
    marked = nil;
    if ([str length]) send_str([str UTF8String]);
}
- (void)doCommandBySelector:(SEL)sel { (void)sel; }
- (void)setMarkedText:(id)s selectedRange:(NSRange)a replacementRange:(NSRange)b {
    (void)a; (void)b;
    [marked release];
    marked = [s isKindOfClass:[NSAttributedString class]] ? [[NSMutableAttributedString alloc] initWithAttributedString:s]
                                                           : [[NSMutableAttributedString alloc] initWithString:s];
}
- (void)unmarkText { [marked release]; marked = nil; }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return marked ? NSMakeRange(0, [marked length]) : NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return marked && [marked length]; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a { (void)r; (void)a; return nil; }
- (NSArray *)validAttributesForMarkedText { return @[]; }
- (NSUInteger)characterIndexForPoint:(NSPoint)p { (void)p; return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a {
    /* where the input method's candidate window goes: at the cursor */
    Tab *tb = active_tab();
    CGFloat s = [g_win backingScaleFactor];
    NSRect cell = NSMakeRect(0, 0, CW / s, CH / s);
    (void)r; (void)a;
    if (tb) {
        cell.origin.x = (term_ox() + tb->t.cx * CW) / s;
        cell.origin.y = (term_oy() + (tb->t.cy + 1) * CH) / s;
    }
    return [g_win convertRectToScreen:[self convertRect:cell toView:nil]];
}

/* ── mouse ── */

- (void)mouseDown:(NSEvent *)e {
    int mx, my, idx = -1, col, row, hit, w, h, mods = ev_mods(e);
    Tab *tb = active_tab();
    view_px(e, &mx, &my);
    hit = hit_tabbar(mx, my, &idx);
    if (hit == HIT_TAB) { activate(idx); return; }
    if (hit == HIT_CLOSE) { user_close_tab(idx); return; }
    if (hit == HIT_PLUS) { new_tab_mac(); return; }
    if (hit == HIT_BAR) {        /* the tab column's empty part is the title bar */
        if ([e clickCount] == 2) [g_win performZoom:nil];
        else [g_win performWindowDragWithEvent:e];
        return;
    }
    plat_client_size(&w, &h);
    if (tb && mx >= w - SCROLLBAR_W - g_scale && tb->t.hcount && !tb->t.alt) {
        g_sb_drag = 1;
        scrollbar_set(my);
        return;
    }
    if (!tb) return;
    mouse_cell(mx, my, &col, &row);
    if ([e modifierFlags] & NSEventModifierFlagCommand) {   /* Cmd+click opens a link, even inside mouse-aware apps */
        char url[2048];
        if (link_at(col, row, url, sizeof(url))) { open_url(url); return; }
    }
    if (mouse_report(0, 1, 0, mx, my, mods)) return;
    if ([e clickCount] == 2) {
        select_word(col, row);
        plat_invalidate();
        return;
    }
    g_sel_on = 0;
    g_sel_drag = 1;
    g_sel_a_row = g_sel_b_row = term_base(&tb->t) + tb->t.hcount - tb->t.view + row;
    g_sel_a_col = g_sel_b_col = col;
    plat_invalidate();
}

- (void)mouseDragged:(NSEvent *)e {
    int mx, my, col, row;
    Tab *tb = active_tab();
    view_px(e, &mx, &my);
    g_mouse_x = mx;
    g_mouse_y = my;
    if (g_sb_drag) { scrollbar_set(my); return; }
    if (g_sel_drag && tb) {
        mouse_cell(mx, my, &col, &row);
        if (my < term_oy()) scroll_view(1);
        else if (my >= term_oy() + tb->t.rows * CH) scroll_view(-1);
        g_sel_b_row = term_base(&tb->t) + tb->t.hcount - tb->t.view + row;
        g_sel_b_col = col;
        g_sel_on = 1;
        plat_invalidate();
        return;
    }
    if (mx >= sidebar_w()) mouse_report(0, 1, 1, mx, my, ev_mods(e));
}

- (void)mouseUp:(NSEvent *)e {
    int mx, my;
    if (g_sb_drag || g_sel_drag) {
        g_sb_drag = g_sel_drag = 0;
        return;
    }
    view_px(e, &mx, &my);
    mouse_report(0, 0, 0, mx, my, ev_mods(e));
}

- (void)rightMouseDown:(NSEvent *)e {
    int mx, my;
    NSMenu *m;
    view_px(e, &mx, &my);
    if (mx < sidebar_w()) return;
    if (mouse_report(2, 1, 0, mx, my, ev_mods(e))) return;
    m = [[[NSMenu alloc] init] autorelease];
    [m setAutoenablesItems:NO];
    [[m addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@""] setEnabled:g_sel_on];
    [m addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"New Tab" action:@selector(newTab:) keyEquivalent:@""];
    [NSMenu popUpContextMenu:m withEvent:e forView:self];
}

- (void)rightMouseUp:(NSEvent *)e {
    int mx, my;
    view_px(e, &mx, &my);
    mouse_report(2, 0, 0, mx, my, ev_mods(e));
}

- (void)rightMouseDragged:(NSEvent *)e {
    int mx, my;
    view_px(e, &mx, &my);
    if (mx >= sidebar_w()) mouse_report(2, 1, 1, mx, my, ev_mods(e));
}

- (void)otherMouseDown:(NSEvent *)e {
    int mx, my, idx = -1, hit;
    view_px(e, &mx, &my);
    hit = hit_tabbar(mx, my, &idx);
    if (hit == HIT_TAB || hit == HIT_CLOSE) { user_close_tab(idx); return; }   /* middle-click closes a tab */
    mouse_report(1, 1, 0, mx, my, ev_mods(e));
}

- (void)otherMouseUp:(NSEvent *)e {
    int mx, my;
    view_px(e, &mx, &my);
    mouse_report(1, 0, 0, mx, my, ev_mods(e));
}

- (void)mouseMoved:(NSEvent *)e {
    int mx, my, was_bar = g_mouse_y >= 0 && g_mouse_x < sidebar_w(), on_bar;
    Tab *tb = active_tab();
    view_px(e, &mx, &my);
    on_bar = mx < sidebar_w();
    g_mouse_x = mx;
    g_mouse_y = my;
    if (was_bar || on_bar) plat_invalidate();
    if (!on_bar && tb && tb->t.m_mouse == 1003) mouse_report(3, 1, 1, mx, my, ev_mods(e));
    if (!on_bar) [self flagsChanged:e];
}

- (void)mouseExited:(NSEvent *)e {
    (void)e;
    g_mouse_x = g_mouse_y = -1;
    plat_invalidate();
}

- (void)scrollWheel:(NSEvent *)e {
    Tab *tb = active_tab();
    int mx, my, lines, i;
    CGFloat dy = [e scrollingDeltaY];
    if (!tb) return;
    view_px(e, &mx, &my);
    /* trackpads report points: a line per cell height; wheels report notches: three lines each */
    scroll_acc += [e hasPreciseScrollingDeltas] ? (float)(dy * [g_win backingScaleFactor] / CH) : (float)(dy * 3);
    lines = (int)scroll_acc;
    if (!lines) return;
    scroll_acc -= lines;
    if (tb->t.m_mouse && !(ev_mods(e) & M_SHIFT)) {
        for (i = 0; i < abs(lines); i++) mouse_report(lines > 0 ? 64 : 65, 1, 0, mx, my, ev_mods(e));
    } else if (tb->t.alt) {
        /* alternate scroll: less, man, git log scroll with the wheel */
        const char *seq = lines > 0 ? (tb->t.m_appcursor ? "\x1bOA" : "\x1b[A") : (tb->t.m_appcursor ? "\x1bOB" : "\x1b[B");
        for (i = 0; i < abs(lines); i++) tab_write(tb, seq, 3);
    } else {
        scroll_view(lines);
    }
}

/* ── drops: files type their paths ── */

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { (void)info; return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    [NSApp activateIgnoringOtherApps:YES];
    if (paste_file_urls([info draggingPasteboard])) return YES;
    {
        NSString *s = [[info draggingPasteboard] stringForType:NSPasteboardTypeString];
        if (s) paste_utf8([s UTF8String]);
        return s != nil;
    }
}

/* ── menu actions ── */

- (void)copy:(id)sender { (void)sender; sel_copy(); g_sel_on = 0; plat_invalidate(); }
- (void)paste:(id)sender { (void)sender; paste(); }
- (void)newTab:(id)sender { (void)sender; new_tab_mac(); }
- (void)closeTab:(id)sender { (void)sender; user_close_tab(g_active); }
- (void)nextTab:(id)sender { (void)sender; if (g_ntabs) activate((g_active + 1) % g_ntabs); }
- (void)prevTab:(id)sender { (void)sender; if (g_ntabs) activate((g_active + g_ntabs - 1) % g_ntabs); }
- (void)jumpAttention:(id)sender { (void)sender; jump_to_attention(); }
- (void)prevMark:(id)sender { (void)sender; jump_mark(-1); }
- (void)nextMark:(id)sender { (void)sender; jump_mark(1); }

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if ([item action] == @selector(copy:)) return g_sel_on;
    return YES;
}

@end

/* ── window and app ────────────────────────────────────────────────── */

static void focus_changed(int focused) {
    Tab *tb = active_tab();
    g_focused = focused;
    if (g_focused && tb) tb->attention = 0;
    if (tb && tb->t.m_focus) tab_write(tb, g_focused ? "\x1b[I" : "\x1b[O", 3);
    plat_invalidate();
}

@interface HCDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation HCDelegate
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; focus_changed(1); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n; focus_changed(0); }
- (BOOL)windowShouldClose:(NSWindow *)w {
    (void)w;
    if (!confirm_close(-1)) return NO;
    g_quitting = 1;
    [NSApp terminate:nil];
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    (void)app;
    if (!g_quitting && !confirm_close(-1)) return NSTerminateCancel;
    return NSTerminateNow;
}
- (void)applicationWillTerminate:(NSNotification *)n {
    int i;
    (void)n;
    config_save();
    for (i = 0; i < g_ntabs; i++)   /* hang up the shells: closing the ptys does it too, this is just sure */
        if (g_tabs[i]->pid > 0 && !g_tabs[i]->exited) kill(g_tabs[i]->pid, SIGHUP);
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)v {
    (void)app; (void)v;
    [g_win makeKeyAndOrderFront:nil];
    return YES;
}
/* a folder dropped on the Dock icon, or `open -a hardcore DIR`: a tab there */
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    (void)app;
    for (NSURL *u in urls) {
        BOOL dir = NO;
        NSString *p = [u path];
        if (![[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&dir]) continue;
        if (!dir) p = [p stringByDeletingLastPathComponent];
        if (g_ntabs && active_tab()) set_cwd(&active_tab()->t, [p fileSystemRepresentation]);
        else snprintf(g_start_dir, sizeof(g_start_dir), "%s", [p fileSystemRepresentation]);
        if (g_ntabs) new_tab_here();
    }
}
@end

static NSMenuItem *menu_item(NSMenu *m, NSString *title, SEL action, NSString *key, NSEventModifierFlags mods) {
    NSMenuItem *it = [m addItemWithTitle:title action:action keyEquivalent:key];
    [it setKeyEquivalentModifierMask:mods];
    if ([g_view respondsToSelector:action]) [it setTarget:g_view];   /* ours: the view, key window or not */
    return it;
}

static void build_menus(void) {
    NSMenu *bar = [[NSMenu alloc] init], *app = [[NSMenu alloc] initWithTitle:@"hardcore"];
    NSMenu *shell = [[NSMenu alloc] initWithTitle:@"Shell"], *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    NSEventModifierFlags C = NSEventModifierFlagCommand, CS = C | NSEventModifierFlagShift;
    NSString *up = [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey];
    NSString *down = [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey];
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
    menu_item(app, @"About hardcore", @selector(orderFrontStandardAboutPanel:), @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    menu_item(app, @"Hide hardcore", @selector(hide:), @"h", C);
    menu_item(app, @"Hide Others", @selector(hideOtherApplications:), @"h", C | NSEventModifierFlagOption);
    menu_item(app, @"Show All", @selector(unhideAllApplications:), @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    menu_item(app, @"Quit hardcore", @selector(terminate:), @"q", C);
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:shell];
    menu_item(shell, @"New Tab", @selector(newTab:), @"t", C);
    menu_item(shell, @"Close Tab", @selector(closeTab:), @"w", C);
    [shell addItem:[NSMenuItem separatorItem]];
    menu_item(shell, @"Next Tab", @selector(nextTab:), @"]", CS);
    menu_item(shell, @"Previous Tab", @selector(prevTab:), @"[", CS);
    menu_item(shell, @"Jump to the Tab That Needs You", @selector(jumpAttention:), @"j", C);
    [shell addItem:[NSMenuItem separatorItem]];
    menu_item(shell, @"Previous Prompt", @selector(prevMark:), up, C);
    menu_item(shell, @"Next Prompt", @selector(nextMark:), down, C);
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:edit];
    menu_item(edit, @"Copy", @selector(copy:), @"c", C);
    menu_item(edit, @"Paste", @selector(paste:), @"v", C);
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:win];
    menu_item(win, @"Minimize", @selector(performMiniaturize:), @"m", C);
    menu_item(win, @"Zoom", @selector(performZoom:), @"", 0);
    menu_item(win, @"Enter Full Screen", @selector(toggleFullScreen:), @"f", C | NSEventModifierFlagControl);
    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:win];
}

/* ── test channel ───────────────────────────────────────────────────
   HARDCORE_CTL=/path/fifo: lines of "type TEXT" (\n is Enter, \e is ESC),
   "cmd newtab|nexttab|closetab|jump|tab N|hover X,Y|resize W,H", "snap OUT.bmp", and
   "winid OUT" (the window number, for screencapture -l, which also gets the
   traffic lights), and "key KEYCODE FLAGS CHARS [UNMODIFIED]": a synthetic
   key event through the same keyDown: path as the keyboard (FLAGS in hex,
   NSEventModifierFlags plus the device bits, e.g. 0x80120 for left Option;
   CHARS may use \xHH and \uHHHH, "-" is none).
   It drives a window that isn't focused, so tests never take your keyboard
   (the Mac side of bench/termbench.exe's send / cmd / snap). */

static NSString *ctl_unescape(const char *s) {
    NSMutableString *out = [NSMutableString string];
    while (*s) {
        unsigned v;
        if (s[0] == '\\' && s[1] == 'x' && sscanf(s + 2, "%2x", &v) == 1) { [out appendFormat:@"%C", (unichar)v]; s += 4; }
        else if (s[0] == '\\' && s[1] == 'u' && sscanf(s + 2, "%4x", &v) == 1) { [out appendFormat:@"%C", (unichar)v]; s += 6; }
        else {
            NSString *one = [[[NSString alloc] initWithBytes:s length:1 + ((*s & 0xE0) == 0xC0) + 2 * ((*s & 0xF0) == 0xE0) + 3 * ((*s & 0xF8) == 0xF0)
                                                     encoding:NSUTF8StringEncoding] autorelease];
            [out appendString:one ? one : @"?"];
            s += [one lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ? [one lengthOfBytesUsingEncoding:NSUTF8StringEncoding] : 1;
        }
    }
    return out;
}

static void ctl_line(char *s) {
    Tab *tb = active_tab();
    if (!strncmp(s, "type ", 5) && tb) {
        char *o = s, *p;
        for (p = s + 5; *p; p++) {
            if (*p == '\\' && p[1] == 'n') { *o++ = '\r'; p++; }
            else if (*p == '\\' && p[1] == 'e') { *o++ = 0x1B; p++; }
            else if (*p == '\\' && p[1] == '\\') { *o++ = '\\'; p++; }
            else *o++ = *p;
        }
        if (tb->help) { int at; for (at = 0; at < g_ntabs; at++) if (g_tabs[at] == tb) { replace_tab(at); break; } }
        else tab_write(tb, s, (int)(o - s));
    } else if (!strncmp(s, "cmd ", 4)) {
        const char *c = s + 4;
        if (!strcmp(c, "newtab")) new_tab_mac();
        else if (!strcmp(c, "nexttab") && g_ntabs) activate((g_active + 1) % g_ntabs);
        else if (!strcmp(c, "closetab")) close_tab(g_active);
        else if (!strcmp(c, "jump")) jump_to_attention();
        else if (!strncmp(c, "tab ", 4)) activate(atoi(c + 4));
        else if (!strncmp(c, "resize ", 7)) {   /* content size in points, as a drag would leave it */
            NSRect fr = [g_win frameRectForContentRect:NSMakeRect(0, 0, atoi(c + 7), strchr(c + 7, ',') ? atoi(strchr(c + 7, ',') + 1) : 400)];
            fr.origin = [g_win frame].origin;
            [g_win setFrame:fr display:YES];
        } else if (!strncmp(c, "hover ", 6)) {
            g_mouse_x = atoi(c + 6);
            g_mouse_y = strchr(c + 6, ',') ? atoi(strchr(c + 6, ',') + 1) : -1;
        }
        plat_invalidate();
    } else if (!strncmp(s, "key ", 4)) {
        unsigned kc = 0, flags = 0;
        char chars[64] = "", plain[64] = "";
        NSEvent *e;
        if (sscanf(s + 4, "%u %x %63s %63s", &kc, &flags, chars, plain) < 2) return;
        if (!strcmp(chars, "-")) chars[0] = 0;
        e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
                         windowNumber:[g_win windowNumber] context:nil characters:ctl_unescape(chars)
          charactersIgnoringModifiers:ctl_unescape(plain[0] ? plain : chars) isARepeat:NO keyCode:(unsigned short)kc];
        if (!(flags & NSEventModifierFlagCommand) || ![[NSApp mainMenu] performKeyEquivalent:e]) [g_view keyDown:e];
    } else if (!strncmp(s, "winid ", 6)) {
        FILE *f = fopen(s + 6, "wb");
        if (f) { fprintf(f, "%ld\n", (long)[g_win windowNumber]); fclose(f); }
    } else if (!strncmp(s, "snap ", 5)) {
        snprintf(g_snap_path, sizeof(g_snap_path), "%s", s + 5);
        paint_view_now();
    }
}

static void ctl_start(const char *path) {
    static char line[8192];
    static int n;
    int fd;
    dispatch_source_t r;
    unlink(path);
    if (mkfifo(path, 0600) != 0 || (fd = open(path, O_RDWR | O_NONBLOCK)) < 0) return;   /* RDWR: never sees EOF */
    r = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(r, ^{
        char buf[4096];
        ssize_t k = read(fd, buf, sizeof(buf)), i;
        for (i = 0; i < k; i++) {
            if (buf[i] == '\n') { line[n] = 0; ctl_line(line); n = 0; }
            else if (n < (int)sizeof(line) - 1) line[n++] = buf[i];
        }
    });
    dispatch_resume(r);
}

/* ── tests ─────────────────────────────────────────────────────────── */

/* Headless check of the whole child path: forkpty → your shell → our hooks → parser. */
static int selftest(const char *out_path) {
    Tab *tb;
    FILE *f;
    int y, phase = 0;
    DWORD start = GetTickCount(), phase_t = 0;
    char line[4096], tmp[MAX_PATH], tmp_real[MAX_PATH];
    g_cols = 100;
    g_rows = 30;
    tb = tab_create(NULL, 0);
    if (!tb) return 2;
    g_tabs[g_ntabs++] = tb;
    snprintf(tmp, sizeof(tmp), "%s", "/tmp");
    if (!realpath(tmp, tmp_real)) snprintf(tmp_real, sizeof(tmp_real), "%s", tmp);
    while (GetTickCount() - start < 20000) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        tab_drain(tb);
        if (phase == 0 && strstr(tb->t.cwd, "/") && GetTickCount() - start > 1500) {
            const char *cmd = "cd /tmp && printf '\\033[1;32mSELFTEST\\033[0m %s\\n' \"$PWD\" && tput cols\r";
            tab_write(tb, cmd, (int)strlen(cmd));
            phase = 1;
            phase_t = GetTickCount();
        } else if (phase == 1 && (!strcmp(tb->t.cwd, "/tmp") || !strcmp(tb->t.cwd, tmp_real)) && GetTickCount() - phase_t > 500) {
            phase = 2;
            break;
        }
    }
    f = fopen(out_path, "wb");
    if (!f) return 3;
    fprintf(f, "shell=%s\n", g_shell);
    fprintf(f, "phase=%d cwd=%s cols=%d rows=%d hist=%d\n", phase, tb->t.cwd, tb->t.cols, tb->t.rows, tb->t.hcount);
    {
        char title[1024];
        size_t i, k = 0;
        for (i = 0; tb->t.title[i] && k < sizeof(title) - 4; i++) k += utf8_encode((uint32_t)tb->t.title[i], title + k);
        title[k] = 0;
        fprintf(f, "title=%s\n----\n", title);
    }
    for (y = 0; y < tb->t.rows; y++) {
        int x, n = 0;
        for (x = 0; x < tb->t.cols; x++) {
            Cell *c = &tb->t.row[tb->t.alt][y][x];
            if (c->wflag == W_CONT) continue;
            n += utf8_encode(c->ch ? c->ch : ' ', line + n);
        }
        while (n && line[n - 1] == ' ') n--;
        line[n] = 0;
        fprintf(f, "%s\n", line);
    }
    fclose(f);
    g_ntabs = 0;
    tab_destroy_async(tb);
    return phase == 2 ? 0 : 1;
}

/* --unit OUT: table tests for the pure helpers behind drops, links and the UTF-8 decoder */
static int unit_tests(const char *out_path) {
    static const struct { const char *in, *want; } paths[] = {
        {"/Users/you/notes.txt", "/Users/you/notes.txt"},
        {"/Users/you/My Stuff/a b.png", "'/Users/you/My Stuff/a b.png'"},
        {"/tmp/it's/here", "'/tmp/it'\\''s/here'"},
        {"/Volumes/USB/x(1).c", "'/Volumes/USB/x(1).c'"},
    };
    static const struct { const char *word; int at; const char *want; } urls[] = {
        {"https://example.com/a?b=c", 3, "https://example.com/a?b=c"},
        {"(see:https://x.io/y).", 10, "https://x.io/y"},
        {"(see:https://x.io/y).", 1, NULL},
        {"file:///Users/you", 0, "file:///Users/you"},
        {"http://", 2, NULL},
        {"nourl.here", 2, NULL},
    };
    static const struct { const char *in; const wchar_t *want; } wide[] = {
        {"abc", L"abc"}, {"caf\xc3\xa9", L"caf\x00e9"}, {"\xe2\x9c\xb3 x", L"\x2733 x"}, {"\xf0\x9f\x98\x80", L"\x1F600"},
        {"bad\xff!", L"bad\xFFFD!"},
    };
    FILE *f = fopen(out_path, "wb");
    int i, fails = 0;
    if (!f) return 3;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
        char got[1024];
        quoted_path(paths[i].in, got, sizeof(got));
        fprintf(f, "%s drop  %s\n", strcmp(got, paths[i].want) ? "FAIL" : "ok  ", got);
        fails += strcmp(got, paths[i].want) != 0;
    }
    for (i = 0; i < (int)(sizeof(urls) / sizeof(urls[0])); i++) {
        char got[256] = "";
        int hit = url_in_word(urls[i].word, urls[i].at, got, sizeof(got));
        int ok = urls[i].want ? (hit && !strcmp(got, urls[i].want)) : !hit;
        fprintf(f, "%s url   %s @%d -> %s\n", ok ? "ok  " : "FAIL", urls[i].word, urls[i].at, hit ? got : "(none)");
        fails += !ok;
    }
    for (i = 0; i < (int)(sizeof(wide) / sizeof(wide[0])); i++) {
        wchar_t got[64];
        int ok;
        utf8_to_wide(wide[i].in, got, 64);
        ok = !wcscmp(got, wide[i].want);
        fprintf(f, "%s utf8  #%d\n", ok ? "ok  " : "FAIL", i);
        fails += !ok;
    }
    fprintf(f, "%d failed\n", fails);
    fclose(f);
    return fails ? 1 : 0;
}

static int replay(const char *out_path, int cols, int rows, char **files, int nfiles) {
    FILE *out = fopen(out_path, "wb");
    int i;
    if (!out) return 3;
    for (i = 0; i < nfiles; i++) {
        FILE *f = fopen(files[i], "rb");
        fprintf(out, "=== %s\n", files[i]);
        replay_one(out, f, cols, rows);
        if (f) fclose(f);
    }
    fclose(out);
    return 0;
}

/* ── startup ───────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    int i, cols, rows;
    char selftest_out[MAX_PATH] = "";
    HCDelegate *dlg;
    NSRect frame;
    NSOpenGLPixelFormatAttribute attrs[] = {NSOpenGLPFADoubleBuffer, NSOpenGLPFAColorSize, 24, NSOpenGLPFAAlphaSize, 8, 0};
    NSOpenGLPixelFormat *pf;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    signal(SIGPIPE, SIG_IGN);    /* a write to a pty whose shell just died returns EIO instead */
    palette_init();
    drip_init();
    config_load();
    snprintf(g_cfg_dir_line, sizeof(g_cfg_dir_line), "%s", g_start_dir);
    for (i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "--cd") || !strcmp(argv[i], "-d")) && i + 1 < argc) {
            snprintf(g_start_dir, sizeof(g_start_dir), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--unit") && i + 1 < argc) {
            return unit_tests(argv[i + 1]);
        } else if (!strcmp(argv[i], "--replay") && i + 3 < argc) {
            /* --replay OUT COLS ROWS FILE... */
            return replay(argv[i + 1], atoi(argv[i + 2]), atoi(argv[i + 3]), argv + i + 4, argc - i - 4);
        } else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) {
            snprintf(selftest_out, sizeof(selftest_out), "%s", argv[++i]);
        } else if (!strncmp(argv[i], "-psn_", 5)) {
            /* Finder's process serial number, on older macOS */
        } else if (argv[i][0] != '-') {
            char real[MAX_PATH];
            snprintf(g_start_dir, sizeof(g_start_dir), "%s", realpath(argv[i], real) ? real : argv[i]);
        }
    }
    if (!g_start_dir[0]) snprintf(g_start_dir, sizeof(g_start_dir), "%s", getenv("HOME") ? getenv("HOME") : "/");
    write_rc();
    if (selftest_out[0]) return selftest(selftest_out);

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    dlg = [[HCDelegate alloc] init];
    [NSApp setDelegate:dlg];

    g_scale = (int)([[NSScreen mainScreen] backingScaleFactor] + 0.5);
    if (g_scale < 1) g_scale = 1;
    frame = NSMakeRect(0, 0, (sidebar_w() + 2 * PAD + SCROLLBAR_W + 120 * CW) / (CGFloat)g_scale,
                       (2 * PAD + 36 * CH) / (CGFloat)g_scale);
    g_win = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                                  NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
                                          backing:NSBackingStoreBuffered defer:NO];
    /* the tab column is the title bar: no title text, the traffic lights over the tabs */
    [g_win setTitlebarAppearsTransparent:YES];
    [g_win setTitleVisibility:NSWindowTitleHidden];
    [g_win setBackgroundColor:[NSColor colorWithSRGBRed:((BAR_BG >> 16) & 255) / 255.0 green:((BAR_BG >> 8) & 255) / 255.0
                                                   blue:(BAR_BG & 255) / 255.0 alpha:1]];
    [g_win setAppearance:[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]];
    [g_win setContentMinSize:NSMakeSize(300, 160)];
    [g_win setAnimationBehavior:NSWindowAnimationBehaviorNone];
    [g_win setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [g_win setDelegate:dlg];
    [g_win setAcceptsMouseMovedEvents:YES];
    [g_win setTitle:@"hardcore"];
    [g_win center];
    [g_win setFrameAutosaveName:@"hardcore"];   /* size and place come back next time */

    pf = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    g_view = [[HCView alloc] initWithFrame:frame pixelFormat:pf];
    [g_view setWantsBestResolutionOpenGLSurface:YES];
    [g_view registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSPasteboardTypeString]];
    [g_win setContentView:g_view];
    [g_win makeFirstResponder:g_view];
    build_menus();
    g_scale = (int)([g_win backingScaleFactor] + 0.5);
    if (g_scale < 1) g_scale = 1;
    {
        int w, h;
        plat_client_size(&w, &h);
        compute_grid(w, h, &cols, &rows);
        g_cols = cols;
        g_rows = rows;
    }
    if (getenv("HARDCORE_CTL")) ctl_start(getenv("HARDCORE_CTL"));
    new_tab_here();
    [g_win makeKeyAndOrderFront:nil];
    if (!getenv("HARDCORE_NO_ACTIVATE")) [NSApp activateIgnoringOtherApps:YES];   /* tests: open behind */
    [NSApp run];
    [pool release];
    return 0;
}
