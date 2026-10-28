/* Cmd+hover and Cmd+click on file paths in the text (not only OSC 8 links).
 *
 * Holding Cmd over a path that exists underlines it and shows a hint by the
 * pointer: the resolved path, what Cmd+click does and what Cmd+Option+click
 * does instead. A file with a handler (hardcore_open.h: Markdown → hcmd, text
 * and source → hcmd -p at the line, images → the character image view,
 * folders → Files) opens in a new tab in split view beside the current one;
 * anything else opens in its default macOS app. Option swaps the two. URLs
 * keep their own Cmd+click (link_at runs first). */
#include "hardcore_open.h"

static HcOpenConf g_open_conf;
static struct timespec g_open_conf_mtime;
static int g_open_conf_loaded;

/* Built-in defaults, then ~/.config/hardcore/open.conf (reread when it changes). */
static const HcOpenConf *mac_open_conf(void) {
    char path[MAX_PATH];
    const char *home = getenv("HOME");
    struct stat st;
    snprintf(path, sizeof(path), "%s/.config/hardcore/open.conf", home ? home : "/");
    int have = stat(path, &st) == 0;
    struct timespec m = have ? st.st_mtimespec : (struct timespec){0, 0};
    if (g_open_conf_loaded && m.tv_sec == g_open_conf_mtime.tv_sec && m.tv_nsec == g_open_conf_mtime.tv_nsec) return &g_open_conf;
    hc_open_defaults(&g_open_conf);
    if (have) {
        NSString *text = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
        if (text) hc_open_parse(&g_open_conf, text.UTF8String);
    }
    g_open_conf_mtime = m; g_open_conf_loaded = 1;
    return &g_open_conf;
}

/* Where the tab's relative paths start: the foreground program's folder (an
   editor or Claude may have moved), else what the shell reported (OSC 7),
   else where the kernel says the shell is. */
static void mac_open_cwd(Tab *tb, char *out, int cap) {
    struct proc_vnodepathinfo vi;
    out[0] = 0;
    if (tb->fd >= 0) {
        pid_t fg = tcgetpgrp(tb->fd);
        if (fg > 0 && fg != tb->pid && proc_pidinfo(fg, PROC_PIDVNODEPATHINFO, 0, &vi, sizeof(vi)) == (int)sizeof(vi))
            snprintf(out, cap, "%s", vi.pvi_cdir.vip_path);
    }
    if (!out[0] && tb->t.cwd[0] == '/') snprintf(out, cap, "%s", tb->t.cwd);
    if (!out[0]) tab_proc_cwd(tb, out, cap);
    if (!out[0] && tb->start_dir[0] == '/') snprintf(out, cap, "%s", tb->start_dir);
}

static int mac_open_is_text(const char *path) {
    char buf[1024];
    int fd = open(path, O_RDONLY | O_NONBLOCK), n;
    if (fd < 0) return 0;
    n = (int)read(fd, buf, sizeof(buf));
    close(fd);
    return n >= 0 && !memchr(buf, 0, (size_t)n);
}

/* What a click does: 1 a hardcore tab running handler, 2 the default app,
   3 show it in Finder (a file that would run when opened). */
typedef struct { int kind; char handler[232]; char app[128]; } MacOpenPlan;
static void mac_open_plan(const HcPathHit *h, int alt, MacOpenPlan *p) {
    int text = !h->dir && mac_open_is_text(h->path);
    const char *handler = hc_open_lookup(mac_open_conf(), h->path, h->dir, text);
    NSURL *u = [NSURL fileURLWithPath:@(h->path)];
    int runs = !h->dir && (link_action(u.absoluteString.UTF8String) == LINK_REVEAL ||
                           [NSFileManager.defaultManager isExecutableFileAtPath:@(h->path)]);
    int in = hc_open_in_hardcore(handler);
    memset(p, 0, sizeof(*p));
    if (in != alt) { p->kind = 1; snprintf(p->handler, sizeof(p->handler), "%s", handler); }
    else if (!in) p->kind = runs ? 3 : 2;   /* the alternative to the default app */
    else if (runs) p->kind = 3;
    else p->kind = 2;
    if (p->kind == 1 && !in) {   /* Option on an unregistered file: hardcore's text view, if it is text */
        const char *t = hc_open_find(mac_open_conf(), "text");
        if (text && t && hc_open_in_hardcore(t)) snprintf(p->handler, sizeof(p->handler), "%s", t);
        else p->kind = 3;
    }
    if (p->kind == 2) {
        NSURL *app = h->dir ? nil : [NSWorkspace.sharedWorkspace URLForApplicationToOpenURL:u];
        NSString *name = app ? [NSFileManager.defaultManager displayNameAtPath:app.path] : (h->dir ? @"Finder" : nil);
        if ([name hasSuffix:@".app"]) name = [name substringToIndex:name.length - 4];
        snprintf(p->app, sizeof(p->app), "%s", name ? name.UTF8String : "");
        if (!app && !h->dir) p->kind = 3;   /* nothing would open it */
    }
}
static void mac_open_plan_label(const MacOpenPlan *p, char *out, int cap) {
    if (p->kind == 1) snprintf(out, cap, "open in %s", g_grid_on && !g_split_main ? "a hardcore pane" : "hardcore split");
    else if (p->kind == 2) snprintf(out, cap, "open in %s", p->app);
    else snprintf(out, cap, "show in Finder");
}

/* A new tab running the handler for the file, beside the current tab. */
static void mac_open_in_hardcore(const HcPathHit *h, const char *handler) {
    Tab *cur = active_tab();
    if (g_ntabs >= MAX_TABS || !cur) { NSBeep(); return; }
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s", h->path);
    if (!h->dir) { char *slash = strrchr(dir, '/'); if (slash && slash != dir) *slash = 0; else snprintf(dir, sizeof(dir), "/"); }
    int main_id = cur->id;
    if (!strcmp(handler, "files")) mac_files_at(dir);
    else {
        char script[600];
        if (!hc_open_script(handler, script, (int)sizeof(script) - 6)) { NSBeep(); return; }
        Tab *tb = calloc(1, sizeof(Tab));
        if (!tb || !term_init(&tb->t, g_cols, g_rows)) { free(tb); NSBeep(); return; }
        tb->id = g_next_id++; tb->fd = -1;
        snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", dir);
        snprintf(tb->run, sizeof(tb->run), "open:%s", script);
        snprintf(tb->open_file, sizeof(tb->open_file), "%s", h->path);
        tb->open_line = h->line > 0 ? h->line : 1;
        set_cwd(&tb->t, dir);
        tab_spawn(tb);
        g_tabs[g_ntabs++] = tb;
    }
    Tab *opened = g_tabs[g_ntabs - 1];
    if (opened->id == main_id) return;
    grid_split_enter(main_id, opened->id);
    activate(g_ntabs - 1);
}
/* In the child: run the handler script with the file, line and folder. */
static void mac_open_exec(Tab *tb) {
    char line[16];
    snprintf(line, sizeof(line), "%d", tb->open_line > 0 ? tb->open_line : 1);
    execl("/bin/sh", "sh", "-c", tb->run + 5, "hardcore-open", tb->open_file, line, tb->start_dir, (char *)NULL);
}

static void mac_open_path(const HcPathHit *h, int alt) {
    MacOpenPlan p;
    mac_open_plan(h, alt, &p);
    NSURL *u = [NSURL fileURLWithPath:@(h->path)];
    if (p.kind == 1) mac_open_in_hardcore(h, p.handler);
    else if (p.kind == 2) [NSWorkspace.sharedWorkspace openURL:u];
    else [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[u]];
}

/* ── hover ────────────────────────────────────────────────────────── */

static struct {
    int on, tab_id, L, c0, c1;   /* the underlined cells: absolute line L, columns [c0, c1) */
    int mx, my;                  /* the pointer, for the hint (a Cmd press alone moves no mouse) */
    HcPathHit hit;
    wchar_t text[3][300];
} g_open_hover;

/* The path under a pixel in the pane or tab there, if it exists. */
static int mac_open_path_at(int mx, int my, HcPathHit *hit, Tab **tab, int *L, int *c0, int *c1) {
    Tab *tb = active_tab();
    if (g_grid_on) { int i = grid_pane_at(mx, my); tb = i >= 0 ? g_tabs[i] : NULL; }
    if (!tb || tb->kind == TK_BROWSER || over_chrome(mx, my) || chrome_any_tv_contains(mx, my)) return 0;
    Tab *saved = g_pointer_tab;
    int col, row;
    g_pointer_tab = tb;
    mouse_cell(mx, my, &col, &row);
    g_pointer_tab = saved;
    if (g_grid_on) { HcGridRect r = grid_rect(tb->id); if (my < (r.y + 1) * CH) return 0; }
    const Cell *cells; int n, wrapped;
    int line = tb->t.hcount - tb->t.view + row;
    term_line(&tb->t, line, &cells, &n, &wrapped);
    if (!cells || col < 0 || col >= n) return 0;
    char text[4096]; int off[1025], k = 0;
    if (n > 1024) n = 1024;
    for (int i = 0; i < n; i++) {
        off[i] = k;
        if (cells[i].wflag == W_CONT) continue;
        uint32_t ch = cells[i].ch < ' ' ? ' ' : cells[i].ch;
        char u[8]; int ul = 0;
        if (ch < 0x80) u[ul++] = (char)ch;
        else if (ch < 0x800) { u[ul++] = (char)(0xC0 | ch >> 6); u[ul++] = (char)(0x80 | (ch & 63)); }
        else if (ch < 0x10000) { u[ul++] = (char)(0xE0 | ch >> 12); u[ul++] = (char)(0x80 | (ch >> 6 & 63)); u[ul++] = (char)(0x80 | (ch & 63)); }
        else { u[ul++] = (char)(0xF0 | ch >> 18); u[ul++] = (char)(0x80 | (ch >> 12 & 63)); u[ul++] = (char)(0x80 | (ch >> 6 & 63)); u[ul++] = (char)(0x80 | (ch & 63)); }
        if (k + ul >= (int)sizeof(text) - 1) { n = i; break; }
        memcpy(text + k, u, (size_t)ul); k += ul;
    }
    off[n] = k; text[k] = 0;
    if (col >= n) return 0;
    char cwd[MAX_PATH];
    mac_open_cwd(tb, cwd, sizeof(cwd));
    if (!hc_path_at(text, off[col], cwd, getenv("HOME"), hit)) return 0;
    int a = 0, b;
    while (a < n && off[a] < hit->start) a++;
    b = a; while (b < n && off[b] < hit->end) b++;
    *tab = tb; *L = line; *c0 = a; *c1 = b;
    return 1;
}

static void mac_open_wide(const char *s, wchar_t *out, int cap) {
    NSString *t = @(s) ?: @"";
    NSData *d = [t dataUsingEncoding:NSUTF32LittleEndianStringEncoding];
    int n = (int)(d.length / 4); if (n > cap - 1) n = cap - 1;
    memcpy(out, d.bytes, (size_t)n * 4); out[n] = 0;
}

static void mac_open_hover_clear(void) {
    if (g_open_hover.on) { g_open_hover.on = 0; plat_invalidate(); }
}
/* Pointer motion and modifier changes: Cmd (with or without Option) over a
   path shows the hint. Returns 1 when a path is under the pointer. */
static int mac_open_hover_update(int mx, int my, NSEventModifierFlags f) {
    NSEventModifierFlags m = f & (NSEventModifierFlagCommand | NSEventModifierFlagOption | NSEventModifierFlagShift | NSEventModifierFlagControl);
    char url[2048];
    if (!(m & NSEventModifierFlagCommand) || (m & (NSEventModifierFlagShift | NSEventModifierFlagControl)) || g_sel_drag) { mac_open_hover_clear(); return 0; }
    HcPathHit hit; Tab *tb; int L, c0, c1, col, row;
    if (!g_grid_on) { mouse_cell(mx, my, &col, &row); if (link_at(col, row, url, sizeof(url))) { mac_open_hover_clear(); return 0; } }
    if (!mac_open_path_at(mx, my, &hit, &tb, &L, &c0, &c1)) { mac_open_hover_clear(); return 0; }
    int alt = (m & NSEventModifierFlagOption) != 0;
    MacOpenPlan now, other;
    mac_open_plan(&hit, 0, &now);
    mac_open_plan(&hit, 1, &other);
    char shown[4200], a[200], b[200], l0[4300], l1[260], l2[260];
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(hit.path, home, hl) && (hit.path[hl] == '/' || !hit.path[hl])) snprintf(shown, sizeof(shown), "~%s", hit.path + hl);
    else snprintf(shown, sizeof(shown), "%s", hit.path);
    if (hit.line) snprintf(l0, sizeof(l0), "%s:%d", shown, hit.line); else snprintf(l0, sizeof(l0), "%s%s", shown, hit.dir ? "/" : "");
    mac_open_plan_label(&now, a, sizeof(a)); mac_open_plan_label(&other, b, sizeof(b));
    /* the line for the modifiers held now comes first */
    snprintf(l1, sizeof(l1), "%s %s", alt ? "\xe2\x8c\xa5\xe2\x8c\x98-click:" : "\xe2\x8c\x98-click:", alt ? b : a);
    snprintf(l2, sizeof(l2), "%s %s", alt ? "\xe2\x8c\x98-click:" : "\xe2\x8c\xa5\xe2\x8c\x98-click:", alt ? a : b);
    g_open_hover.mx = mx; g_open_hover.my = my;
    g_open_hover.on = 1; g_open_hover.tab_id = tb->id; g_open_hover.L = L; g_open_hover.c0 = c0; g_open_hover.c1 = c1; g_open_hover.hit = hit;
    mac_open_wide(l0, g_open_hover.text[0], 300);
    mac_open_wide(l1, g_open_hover.text[1], 300);
    mac_open_wide(l2, g_open_hover.text[2], 300);
    plat_invalidate();
    return 1;
}
/* Cmd+click (Option: the other choice). Returns 1 when it opened a path. */
static int mac_open_click(int mx, int my, NSEventModifierFlags f) {
    HcPathHit hit; Tab *tb; int L, c0, c1;
    if (!(f & NSEventModifierFlagCommand) || !mac_open_path_at(mx, my, &hit, &tb, &L, &c0, &c1)) return 0;
    mac_open_hover_clear();
    mac_open_path(&hit, (f & NSEventModifierFlagOption) != 0);
    return 1;
}

static void mac_open_hover_draw(void) {
    if (!g_open_hover.on) return;
    Tab *tb = tab_by_id(g_open_hover.tab_id);
    if (!tb) { g_open_hover.on = 0; return; }
    Term *t = &tb->t;
    int row = g_open_hover.L - (t->hcount - t->view);
    float thick = (float)(g_scale > 1 ? g_scale : 1) * 2;
    if (row >= 0 && row < t->rows) {
        if (g_grid_on) {
            HcGridRect r = grid_rect(tb->id);
            if (r.w > 0 && row < r.h - 1) {
                float x0 = (float)((r.x + g_open_hover.c0) * CW), x1 = (float)((r.x + chrome_min(g_open_hover.c1, r.w)) * CW);
                float y1 = (float)((r.y + 1 + row + 1) * CH);
                fill_rect(x0, y1 - thick, x1, y1, AMBER, 1);
            }
        } else if (tb == active_tab()) {
            /* the display may reflow around the menus: underline where each source cell landed */
            for (int y = 0; y < t->rows; y++) for (int x = 0; x < t->cols; x++) {
                int s = display_source(t, x, y), sr = s / t->cols, sc = s % t->cols;
                if (sr == row && sc >= g_open_hover.c0 && sc < g_open_hover.c1)
                    fill_rect((float)(x * CW), (float)((y + 1) * CH) - thick, (float)((x + 1) * CW), (float)((y + 1) * CH), AMBER, 1);
            }
        }
    }
    /* the hint: below and right of the pointer, kept inside the window */
    int w, h, cols = 0;
    plat_client_size(&w, &h);
    for (int i = 0; i < 3; i++) { int n = (int)wcslen(g_open_hover.text[i]); if (n > cols) cols = n; }
    if (cols > w / CW - 2) cols = w / CW - 2;
    float bw = (float)((cols + 2) * CW), bh = (float)(3 * CH + CH / 2);
    float x = (float)g_open_hover.mx + CW, y = (float)g_open_hover.my + CH + CH / 2;
    if (x + bw > w) x = (float)w - bw;
    if (y + bh > h) y = (float)g_open_hover.my - bh - CH / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    fill_rect(x - 1, y - 1, x + bw + 1, y + bh + 1, AMBER, 1);
    fill_rect(x, y, x + bw, y + bh, BAR_BG, 1);
    draw_label(g_open_hover.text[0], x + CW, y + CH / 4, (float)(cols * CW), g_look.detail);
    draw_label(g_open_hover.text[1], x + CW, y + CH / 4 + CH, (float)(cols * CW), DEF_FG);
    draw_label(g_open_hover.text[2], x + CW, y + CH / 4 + 2 * CH, (float)(cols * CW), g_look.detail);
}
