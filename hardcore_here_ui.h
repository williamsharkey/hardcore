/* ANSI presentation of the same native terminal cells used by Metal/D3D.
 * The session owner keeps this state across terminal attachments. Nothing here
 * parses child output: vt_feed(), term_line(), and term_resize() do that work. */
#ifndef HARDCORE_HERE_UI_H
#define HARDCORE_HERE_UI_H

enum { HU_NONE, HU_CHOOSE, HU_TV, HU_TV_INPUT, HU_CLOSE, HU_NEW,
       HU_CONFIG, HU_FILES, HU_WEB, HU_CAF, HU_DISMISS, HU_THEME,
       HU_HOSTFONT, HU_FONT, HU_FONT_SIZE, HU_FONT_APPLY };
typedef struct { int x, y, w, h, op, value; } HereHit;
typedef struct { Cell c; const Term *owner; } HerePixel;
struct HereUI {
    HerePixel *grid, *previous;
    size_t capacity;
    int force, menu, menu_width, menu_offset, preview, tv_focus;
    int tvs[4], ntvs, visible_tvs, tv_cx, tv_cy;
    int overlay, overlay_offset, focused, prefix, confirm, confirm_id;
    int prompt, prompt_n, search_n, search_direction, search_next, search_left;
    char prompt_text[2049], search[2049], message[320];
    uint32_t search_cp[512];
    int search_count;
    char input[4096]; size_t input_n;
    int paste, paste_wrapped;
    uint64_t input_at;
    HereHit hits[256], pressed;
    int nhits, have_pressed;
    int hostfont, font_size;
    char font_name[128];
};

static int hu_min(int a, int b) { return a < b ? a : b; }
static int hu_max(int a, int b) { return a > b ? a : b; }
static int hu_index(Here *h, int id) {
    for (int i = 0; i < h->ntabs; i++) if (h->tabs[i].id == id) return i;
    return -1;
}
static int hu_current(Here *h) {
    return h->ntabs ? hu_max(0, hu_min(h->active, h->ntabs - 1)) : -1;
}
static int hu_input(Here *h) {
    int i = hu_index(h, h->ui->tv_focus);
    return i >= 0 ? i : hu_current(h);
}
static int hu_has_tv(HereUI *u, int id) {
    for (int i = 0; i < u->ntvs; i++) if (u->tvs[i] == id) return 1;
    return 0;
}
/* Strict, bounded UTF-8 decoding for chrome labels and search. Invalid bytes
 * become replacement glyphs; terminal output remains the core parser's job. */
static uint32_t hu_cp(const char *s, size_t n, size_t *used) {
    unsigned char a = n ? (unsigned char)s[0] : 0;
    uint32_t c; int k;
    *used = n ? 1 : 0;
    if (a < 128) return a;
    if (a >= 0xc2 && a <= 0xdf) { c = a & 31; k = 2; }
    else if (a >= 0xe0 && a <= 0xef) { c = a & 15; k = 3; }
    else if (a >= 0xf0 && a <= 0xf4) { c = a & 7; k = 4; }
    else return 0xfffd;
    if (n < (size_t)k) return 0xfffd;
    for (int i = 1; i < k; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xc0) != 0x80) return 0xfffd;
        c = (c << 6) | (b & 63);
    }
    if ((k == 2 && c < 128) || (k == 3 && c < 2048) ||
        (k == 4 && c < 65536) || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0xfffd;
    *used = (size_t)k; return c;
}
static void hu_label(HereTab *tab, char *out, size_t cap) {
    size_t at = 0;
    for (int i = 0; tab->t.title[i] && i < 255; i++) {
        uint32_t cp = (uint32_t)tab->t.title[i]; char b[8];
        if (cp < 32 || (cp >= 127 && cp < 160)) continue;
        int n = utf8_encode(cp, b);
        if (at + (size_t)n >= cap) break;
        memcpy(out + at, b, (size_t)n); at += (size_t)n;
    }
    if (at) { out[at] = 0; return; }
    const char *cwd = tab->t.cwd[0] ? tab->t.cwd : tab->cwd;
    const char *base = strrchr(cwd, '/'); base = base ? base + 1 : cwd;
    const char *program = strrchr(tab->program, '/'); program = program ? program + 1 : tab->program;
    while (*program == '-') program++;
    if (!strcmp(program, "sh") || !strcmp(program, "zsh") || !strcmp(program, "bash") ||
        !strcmp(program, "fish") || !strcmp(program, "dash") || !strcmp(program, "ksh") ||
        !strcmp(program, "tcsh") || !strcmp(program, "nu"))
        program = tab->t.had_activity ? "term" : "<new>";
    if (!*program) program = "term";
    snprintf(out, cap, "%s%s%s", base, *base ? ":" : "", program);
}
static Cell hu_style(int kind) {
    Cell c = {0};
    c.ch = ' '; c.fg = 0x02000000u | (kind == 1 ? g_scheme->cursor : kind == 2 ? g_scheme->pal[8] : g_scheme->fg);
    c.bg = 0x02000000u | g_scheme->bg;
    if (kind == 1) c.attr = A_BOLD;
    return c;
}
static void hu_hit(Here *h, int x, int y, int w, int ht, int op, int value) {
    HereUI *u = h->ui;
    if (op && w > 0 && ht > 0 && u->nhits < (int)(sizeof(u->hits) / sizeof(u->hits[0])))
        u->hits[u->nhits++] = (HereHit){x, y, w, ht, op, value};
}
static void hu_put(Here *h, int x, int y, const char *text, int width, int style, int op, int value) {
    HereUI *u = h->ui;
    if (x < 0 || y < 0 || y >= h->rows || x >= h->cols || width <= 0) return;
    width = hu_min(width, h->cols - x);
    HerePixel *p = u->grid + (size_t)y * h->cols + x;
    Cell blank = hu_style(style);
    for (int i = 0; i < width; i++) p[i] = (HerePixel){blank, NULL};
    size_t at = 0, n = strlen(text); int col = 0;
    while (at < n && col < width) {
        size_t used; uint32_t cp = hu_cp(text + at, n - at, &used); at += used;
        if (cp < 32 || (cp >= 127 && cp < 160)) continue;
        int w = cp_width(cp); if (w <= 0) continue;
        if (col + w > width) break;
        p[col].c.ch = cp; p[col].c.wflag = w == 2 ? W_WIDE : 0;
        if (w == 2) { p[col + 1].c.ch = 0; p[col + 1].c.wflag = W_CONT; }
        col += w;
    }
    hu_hit(h, x, y, width, 1, op, value);
}
static void hu_glyph(Here *h, int x, int y, uint32_t cp, int style) {
    if (x < 0 || y < 0 || x >= h->cols || y >= h->rows) return;
    HerePixel *p = &h->ui->grid[(size_t)y * h->cols + x];
    *p = (HerePixel){hu_style(style), NULL}; p->c.ch = cp;
}
static void hu_edge(Here *h, int x, int y, int w, uint32_t left, uint32_t mid, uint32_t right, int style) {
    if (w < 2) return;
    hu_glyph(h, x, y, left, style);
    for (int col = 1; col < w - 1; col++) hu_glyph(h, x + col, y, mid, style);
    hu_glyph(h, x + w - 1, y, right, style);
}
static void hu_box(Here *h, int x, int y, int w, int ht, const char *title) {
    for (int row = y; row < y + ht; row++) hu_put(h, x, row, "", w, 0, 0, 0);
    hu_edge(h, x, y, w, 0x256d, 0x2500, 0x256e, 0);
    hu_edge(h, x, y + ht - 1, w, 0x2570, 0x2500, 0x256f, 0);
    for (int row = y + 1; row < y + ht - 1; row++) {
        hu_glyph(h, x, row, 0x2502, 0); hu_glyph(h, x + w - 1, row, 0x2502, 0);
    }
    hu_put(h, x + 2, y, title, w - 5, 1, 0, 0);
    hu_put(h, x + w - 3, y, "−", 1, 0, HU_DISMISS, 0);
    /* A panel consumes clicks even in its empty interior. */
}
static int hu_row_match(HereUI *u, const Cell *cells, int n) {
    if (!u->search_count || !cells) return 0;
    for (int x = 0; x < n; x++) {
        if (cells[x].wflag == W_CONT || cells[x].wflag == W_PAD) continue;
        int pos = x, q = 0;
        while (pos < n && q < u->search_count) {
            if (cells[pos].wflag == W_CONT || cells[pos].wflag == W_PAD) { pos++; continue; }
            uint32_t cp = cells[pos].ch ? cells[pos].ch : ' ';
            if ((uint32_t)towlower((wint_t)cp) != u->search_cp[q]) break;
            q++; pos++;
        }
        if (q == u->search_count) return 1;
    }
    return 0;
}
static void hu_copy(Here *h, int dx, int dy, const Term *t, const Cell *c, int n, int left, int width, int highlight) {
    if (dy < 0 || dy >= h->rows || dx < 0 || dx >= h->cols || !c) return;
    width = hu_min(width, h->cols - dx);
    for (int x = 0; x < width && left + x < n; x++) {
        Cell v = c[left + x];
        if ((x == 0 && v.wflag == W_CONT) || v.wflag == W_PAD ||
            (v.wflag == W_WIDE && (x + 1 >= width || left + x + 1 >= n))) {
            v.ch = ' '; v.wflag = 0; v.link = 0;
        }
        if (highlight) v.bg = 0x02000000u | g_scheme->sel;
        h->ui->grid[(size_t)dy * h->cols + dx + x] = (HerePixel){v, t};
    }
}
static void hu_choose(Here *h, int index) {
    if (!h->ntabs) return;
    h->active = (index % h->ntabs + h->ntabs) % h->ntabs;
    h->tabs[h->active].unread = 0;
    h->ui->preview = h->ui->tv_focus = 0; h->ui->force = 1; h->dirty = 1;
}
static void hu_toggle_tv(Here *h, int id) {
    HereUI *u = h->ui;
    for (int i = 0; i < u->ntvs; i++) if (u->tvs[i] == id) {
        memmove(u->tvs + i, u->tvs + i + 1, (size_t)(--u->ntvs - i) * sizeof(int));
        if (u->tv_focus == id) u->tv_focus = 0;
        h->dirty = 1; return;
    }
    if (hu_index(h, id) < 0) return;
    if (u->ntvs < 4) u->tvs[u->ntvs++] = id;
    else snprintf(u->message, sizeof(u->message), "Four TVs are already open");
    h->dirty = 1;
}
static void hu_open_files(Here *h) {
    int i = hu_current(h);
    const char *cwd = i >= 0 ? (h->tabs[i].t.cwd[0] ? h->tabs[i].t.cwd : h->tabs[i].cwd) : h->cwd;
    char *argv[] = {h->exe, "--here-files", (char *)cwd, NULL};
    here_new(h, cwd, argv);
}
static void hu_open_web(Here *h, const char *url) {
    char *argv[] = {h->exe, "--here-web", (char *)url, NULL};
    if (*url) here_new(h, NULL, argv);
}
static void hu_persist(Here *h) {
    HereUI *u = h->ui;
    HereSettings s = {0}; s.theme = g_scheme->key;
    s.allow_host_font = u->hostfont; s.font_size = u->font_size;
    snprintf(s.font_name, sizeof(s.font_name), "%s", u->font_name);
    if (!here_settings_save(&s)) snprintf(u->message, sizeof(u->message), "Settings could not be saved");
}
static void here_ui_message(Here *h, const char *message) {
    if (h->ui) snprintf(h->ui->message, sizeof(h->ui->message), "%s", message ? message : "");
    h->dirty = 1;
}
static void hu_font(Here *h) {
    HereUI *u = h->ui;
    here_font(h, u->font_name, u->font_size, u->hostfont);
    hu_persist(h);
}
static void hu_theme(Here *h, int key) {
    const Scheme *s = scheme_by_key(key);
    if (!s) return;
    g_scheme = s; look_set(s); hu_persist(h); h->ui->force = 1; h->dirty = 1;
}
static void hu_search_start(Here *h, int direction, int fresh) {
    HereUI *u = h->ui; int i = hu_current(h);
    if (i < 0 || !u->search[0]) return;
    Term *t = &h->tabs[i].t; term_settle(t);
    u->search_count = 0;
    for (size_t at = 0, n = strlen(u->search); at < n && u->search_count < 512;) {
        size_t used; uint32_t cp = hu_cp(u->search + at, n - at, &used); at += used;
        u->search_cp[u->search_count++] = (uint32_t)towlower((wint_t)cp);
    }
    int total = t->hcount + t->rows;
    if (fresh) u->search_next = direction > 0 ? 0 : total - 1;
    else u->search_next = (u->search_next + direction + total) % total;
    u->search_direction = direction; u->search_left = total;
    snprintf(u->message, sizeof(u->message), "Searching history…"); h->dirty = 1;
}
static void hu_prompt(Here *h, int kind, const char *value) {
    HereUI *u = h->ui;
    u->prompt = kind; snprintf(u->prompt_text, sizeof(u->prompt_text), "%s", value);
    u->prompt_n = (int)strlen(u->prompt_text); h->dirty = 1;
}
static void hu_action(Here *h, int op, int value) {
    HereUI *u = h->ui; int i;
    switch (op) {
    case HU_CHOOSE: i = hu_index(h, value); if (i >= 0) hu_choose(h, i); break;
    case HU_TV: hu_toggle_tv(h, value); break;
    case HU_TV_INPUT: if (hu_index(h, value) >= 0) { u->tv_focus = value; u->preview = 0; } break;
    case HU_CLOSE: u->confirm = 1; u->confirm_id = value; break;
    case HU_NEW: here_new(h, NULL, NULL); break;
    case HU_CONFIG: u->overlay = u->overlay == 1 ? 0 : 1; u->overlay_offset = 0; break;
    case HU_FILES: hu_open_files(h); break;
    case HU_WEB: hu_prompt(h, 2, "https://"); break;
    case HU_CAF: here_caf(h, h->caf_pid <= 0); break;
    case HU_DISMISS: u->overlay = 0; break;
    case HU_THEME: hu_theme(h, value); break;
    case HU_HOSTFONT: u->hostfont = !u->hostfont; hu_font(h); break;
    case HU_FONT: hu_prompt(h, 3, u->font_name); break;
    case HU_FONT_SIZE: u->font_size = hu_max(6, hu_min(48, u->font_size + value)); if (u->hostfont) hu_font(h); else hu_persist(h); break;
    case HU_FONT_APPLY: if (u->hostfont) hu_font(h); else snprintf(u->message, sizeof(u->message), "Enable host font changes in config first"); break;
    }
    h->dirty = 1;
}
static void hu_control(Here *h, unsigned char key) {
    HereUI *u = h->ui; int i = hu_current(h);
    switch (key) {
    case 'c': hu_action(h, HU_NEW, 0); break;
    case 'n': hu_choose(h, h->active + 1); break;
    case 'p': hu_choose(h, h->active - 1); break;
    case 'x': if (i >= 0) hu_action(h, HU_CLOSE, h->tabs[i].id); break;
    case 'q': u->confirm = 2; break;
    case 'D': h->action = HERE_DETACH; break;
    case 'R': h->action = HERE_RELOAD; break;
    case 's': case ',': hu_action(h, HU_CONFIG, 0); break;
    case 'm': u->menu = !u->menu; here_ui_resize(h, h->cols, h->rows); break;
    case 'v': if (i >= 0) hu_toggle_tv(h, h->tabs[i].id); break;
    case 'F': hu_open_files(h); break;
    case 'w': hu_action(h, HU_WEB, 0); break;
    case '/': case 'f': hu_prompt(h, 1, u->search); break;
    case 'N': hu_search_start(h, -1, 0); break;
    case 'j': hu_search_start(h, 1, 0); break;
    case '?': u->overlay = u->overlay == 2 ? 0 : 2; break;
    case 'u': case '[': if (i >= 0) h->tabs[i].t.view = hu_min(h->tabs[i].t.hcount, h->tabs[i].t.view + h->child_rows); break;
    case 'd': if (i >= 0) h->tabs[i].t.view = hu_max(0, h->tabs[i].t.view - h->child_rows); break;
    case ']': if (i >= 0) h->tabs[i].t.view = 0; break;
    default: if (key >= '1' && key <= '9') hu_choose(h, key - '1'); break;
    }
    h->dirty = 1;
}
static int hu_seq(const char *data, size_t n, const char *s) { return strlen(s) == n && !memcmp(data, s, n); }
static void hu_send(Here *h, const char *data, size_t n) {
    HereUI *u = h->ui;
    if (!n || h->action) return;
    if (u->confirm) {
        int action = u->confirm, id = u->confirm_id; u->confirm = 0;
        if (data[0] == 'y' || data[0] == 'Y') {
            if (action == 2) h->action = HERE_STOP;
            else { int i = hu_index(h, id); if (i >= 0) here_close(h, i); }
        }
        h->dirty = 1; return;
    }
    if (u->prompt) {
        if (n >= 2 && data[0] == 27 && data[1] == '[') return;
        for (size_t pos = 0; pos < n; pos++) {
            unsigned char c = (unsigned char)data[pos];
            if (c == 27 || c == 3) { u->prompt = 0; break; }
            if (c == '\r' || c == '\n') {
                int kind = u->prompt; u->prompt = 0;
                if (kind == 1) { snprintf(u->search, sizeof(u->search), "%s", u->prompt_text); hu_search_start(h, 1, 1); }
                else if (kind == 2) hu_open_web(h, u->prompt_text);
                else if (kind == 3) { snprintf(u->font_name, sizeof(u->font_name), "%.*s", (int)sizeof(u->font_name) - 1, u->prompt_text); if (u->hostfont) hu_font(h); else hu_persist(h); }
                break;
            }
            if (c == 127 || c == 8) {
                if (u->prompt_n) { do { u->prompt_n--; } while (u->prompt_n && ((unsigned char)u->prompt_text[u->prompt_n] & 0xc0) == 0x80); }
            } else if (c >= 32 && u->prompt_n < (int)sizeof(u->prompt_text) - 1) u->prompt_text[u->prompt_n++] = (char)c;
            u->prompt_text[u->prompt_n] = 0;
        }
        h->dirty = 1; return;
    }
    if (u->overlay) {
        if (hu_seq(data, n, "\x1b") || hu_seq(data, n, "q")) u->overlay = 0;
        else if (u->overlay == 1) {
            if (hu_seq(data, n, "\x1b[B") || hu_seq(data, n, "j")) u->overlay_offset = hu_min(NSCHEMES - 1, u->overlay_offset + 1);
            else if (hu_seq(data, n, "\x1b[A") || hu_seq(data, n, "k")) u->overlay_offset = hu_max(0, u->overlay_offset - 1);
            else if (hu_seq(data, n, "\r") || hu_seq(data, n, "\n")) hu_theme(h, SCHEMES[u->overlay_offset].key);
            else if (hu_seq(data, n, "h")) hu_action(h, HU_HOSTFONT, 0);
            else if (hu_seq(data, n, "f")) hu_action(h, HU_FONT, 0);
            else if (hu_seq(data, n, "a")) hu_action(h, HU_FONT_APPLY, 0);
            else if (hu_seq(data, n, "+") || hu_seq(data, n, "-")) hu_action(h, HU_FONT_SIZE, data[0] == '+' ? 1 : -1);
        }
        h->dirty = 1; return;
    }
    if (u->tv_focus && hu_seq(data, n, "\x1b")) { u->tv_focus = 0; h->dirty = 1; return; }
    int i = hu_input(h); if (i < 0) return;
    Term *t = &h->tabs[i].t;
    if (!u->tv_focus && t->view) {
        if (hu_seq(data, n, "\x1b[5~") || hu_seq(data, n, "\x1b[A")) { t->view = hu_min(t->hcount, t->view + (n == 4 ? h->child_rows : 1)); h->dirty = 1; return; }
        if (hu_seq(data, n, "\x1b[6~") || hu_seq(data, n, "\x1b[B")) { t->view = hu_max(0, t->view - (n == 4 ? h->child_rows : 1)); h->dirty = 1; return; }
        t->view = 0;
    }
    t->had_activity = 1;
    if (!here_write(h, i, data, n)) snprintf(u->message, sizeof(u->message), "Input queue is full; wait for the program to read input");
    h->dirty = 1;
}
static HereHit hu_hit_at(HereUI *u, int x, int y) {
    for (int i = u->nhits - 1; i >= 0; i--) {
        HereHit a = u->hits[i];
        if (x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h) return a;
    }
    return (HereHit){0};
}
static void hu_mouse(Here *h, int button, int x, int y, int down) {
    HereUI *u = h->ui;
    if (!u->focused || x < 1 || y < 1 || x > h->cols || y > h->rows) { u->preview = 0; return; }
    x--; y--; HereHit hit = hu_hit_at(u, x, y);
    if (button & 32) {
        int preview = hit.op == HU_CHOOSE && !u->overlay ? hit.value : 0;
        if (u->preview != preview) { u->preview = preview; u->force = 1; h->dirty = 1; }
        if (hit.op || x >= h->child_cols) return;
    }
    if (hit.op) {
        if (button == 0) {
            if (down) { u->pressed = hit; u->have_pressed = 1; }
            else {
                if (u->have_pressed && u->pressed.op == hit.op && u->pressed.value == hit.value) hu_action(h, hit.op, hit.value);
                u->have_pressed = 0;
            }
        } else if (down && (button == 64 || button == 65)) {
            if (u->overlay == 1) u->overlay_offset = hu_max(0, hu_min(NSCHEMES - 1, u->overlay_offset + (button == 64 ? -1 : 1)));
            else u->menu_offset = hu_max(0, hu_min(h->ntabs - 1, u->menu_offset + (button == 64 ? -1 : 1)));
            h->dirty = 1;
        }
        return;
    }
    if (u->have_pressed) { if (!down) u->have_pressed = 0; return; }
    if (u->overlay || u->prompt || u->confirm) return;
    if (u->preview) { u->preview = 0; h->dirty = 1; }
    int i = hu_current(h);
    if (i < 0 || x >= h->child_cols || y >= h->child_rows) return;
    if (button == 0 && down) { u->tv_focus = 0; h->dirty = 1; }
    Term *t = &h->tabs[i].t;
    if (!t->m_mouse) {
        if (down && (button == 64 || button == 65)) { t->view = hu_max(0, hu_min(t->hcount, t->view + (button == 64 ? 3 : -3))); h->dirty = 1; }
        return;
    }
    if ((button & 32) && (t->m_mouse == 1000 || (t->m_mouse == 1002 && (button & 3) == 3))) return;
    char b[64]; int n = 0;
    if (t->m_mouse_sgr) n = snprintf(b, sizeof(b), "\x1b[<%d;%d;%d%c", button, x + 1, y + 1, down ? 'M' : 'm');
    else if (x < 223 && y < 223 && button <= 223) { memcpy(b, "\x1b[M", 3); b[3] = (char)(32 + (down ? button : 3)); b[4] = (char)(33 + x); b[5] = (char)(33 + y); n = 6; }
    if (n > 0) here_write(h, i, b, (size_t)n);
}
static void here_ui_focus(Here *h, int focused) {
    HereUI *u = h->ui; if (!u) return;
    u->focused = focused;
    if (!focused) { u->preview = 0; u->have_pressed = 0; u->force = 1; }
    else { int i = hu_current(h); if (i >= 0 && !h->tabs[i].t.view) h->tabs[i].unread = 0; }
    int i = hu_input(h);
    if (i >= 0 && h->tabs[i].t.m_focus) here_write(h, i, focused ? "\x1b[I" : "\x1b[O", 3);
    h->dirty = 1;
}
/* Parse only host input framing. Paste markers survive fragmented reads and
 * pasted Ctrl-B remains literal data, never a command to the session owner. */
static void hu_decode(Here *h) {
    HereUI *u = h->ui; size_t at = 0;
    while (at < u->input_n && !h->action) {
        char *p = u->input + at; size_t n = u->input_n - at;
        if (u->paste) {
            static const char end[] = "\x1b[201~";
            size_t take = 0;
            while (take < n && p[take] != 27) take++;
            if (take) { hu_send(h, p, take); at += take; continue; }
            if (n < sizeof(end) - 1 && !memcmp(p, end, n)) break;
            if (n >= sizeof(end) - 1 && !memcmp(p, end, sizeof(end) - 1)) {
                if (u->paste_wrapped) hu_send(h, end, sizeof(end) - 1);
                u->paste = 0; u->paste_wrapped = 0; at += sizeof(end) - 1; continue;
            }
            hu_send(h, p, 1); at++; continue;
        }
        if (u->prefix) { u->prefix = 0; if ((unsigned char)*p == 2) hu_send(h, p, 1); else hu_control(h, (unsigned char)*p); at++; continue; }
        if ((unsigned char)*p == 2) { u->prefix = 1; at++; h->dirty = 1; continue; }
        if ((unsigned char)*p == 27) {
            static const char begin[] = "\x1b[200~";
            if (n == 1) break;
            if (n < sizeof(begin) - 1 && !memcmp(p, begin, n)) break;
            if (n >= sizeof(begin) - 1 && !memcmp(p, begin, sizeof(begin) - 1)) {
                int i = hu_input(h); u->paste = 1;
                u->paste_wrapped = i >= 0 && !u->prompt && !u->overlay && h->tabs[i].t.m_bracketed;
                if (u->paste_wrapped) hu_send(h, begin, sizeof(begin) - 1);
                at += sizeof(begin) - 1; continue;
            }
            if (p[1] == '[') {
                size_t end = 2;
                while (end < n && !((unsigned char)p[end] >= 0x40 && (unsigned char)p[end] <= 0x7e)) end++;
                if (end == n && n < 128) break;
                if (end == n) { hu_send(h, p, n); at += n; continue; }
                size_t used = end + 1;
                if (hu_seq(p, used, "\x1b[I") || hu_seq(p, used, "\x1b[O")) here_ui_focus(h, p[2] == 'I');
                else if (used < 64 && used > 5 && p[2] == '<' && (p[end] == 'M' || p[end] == 'm')) {
                    char b[64], tail; int button, x, y, count = 0; memcpy(b, p, used); b[used] = 0;
                    if (sscanf(b, "\x1b[<%d;%d;%d%c%n", &button, &x, &y, &tail, &count) == 4 && count == (int)used && button >= 0 && button <= 255)
                        hu_mouse(h, button, x, y, tail == 'M');
                } else hu_send(h, p, used);
                at += used; continue;
            }
            if (p[1] == 'O' && n < 3) break; /* SS3 application-cursor keys */
            size_t used = p[1] == 'O' ? 3 : 2;
            hu_send(h, p, used); at += used; continue;
        }
        size_t take = 1;
        while (take < n && p[take] != 2 && p[take] != 27) take++;
        hu_send(h, p, take); at += take;
    }
    if (at) { memmove(u->input, u->input + at, u->input_n - at); u->input_n -= at; }
}
static void here_ui_input(Here *h, const char *data, size_t n) {
    HereUI *u = h->ui;
    if (!u) return;
    u->input_at = here_now_ms();
    while (n && !h->action) {
        size_t take = sizeof(u->input) - u->input_n; if (take > n) take = n;
        if (!take) { hu_send(h, u->input, u->input_n); u->input_n = 0; continue; }
        memcpy(u->input + u->input_n, data, take); u->input_n += take; data += take; n -= take;
        hu_decode(h);
    }
}
static void here_ui_tick(Here *h, uint64_t now) {
    HereUI *u = h->ui; if (!u) return;
    if (u->input_n && !u->paste && now - u->input_at >= 45) { hu_send(h, u->input, u->input_n); u->input_n = 0; }
    if (u->search_left) {
        int i = hu_current(h);
        if (i < 0) { u->search_left = 0; return; }
        Term *t = &h->tabs[i].t; int total = t->hcount + t->rows;
        for (int budget = 128; budget-- && u->search_left > 0;) {
            const Cell *c; int n, wrapped;
            u->search_next = (u->search_next % total + total) % total;
            term_line(t, u->search_next, &c, &n, &wrapped); u->search_left--;
            if (hu_row_match(u, c, n)) {
                t->view = hu_max(0, hu_min(t->hcount, t->hcount - u->search_next));
                snprintf(u->message, sizeof(u->message), "Match on row %d · ^B j next · ^B N previous", u->search_next + 1);
                u->search_left = 0; h->dirty = 1; break;
            }
            u->search_next += u->search_direction;
            if (!u->search_left) { snprintf(u->message, sizeof(u->message), "No matches"); h->dirty = 1; }
        }
    }
}
static void here_ui_init(Here *h) {
    h->ui = (HereUI *)calloc(1, sizeof(HereUI));
    if (!h->ui) { h->action = HERE_STOP; return; }
    h->ui->menu = h->ui->focused = h->ui->force = 1;
    HereSettings prefs; here_settings_load(&prefs);
    h->ui->font_size = prefs.font_size; h->ui->hostfont = prefs.allow_host_font;
    snprintf(h->ui->font_name, sizeof(h->ui->font_name), "%s", prefs.font_name);
    if (scheme_by_key(prefs.theme)) g_scheme = scheme_by_key(prefs.theme);
    palette_init(); h->dirty = 1;
}
static void here_ui_free(Here *h) {
    if (h->ui) { free(h->ui->grid); free(h->ui->previous); free(h->ui); h->ui = NULL; }
}
static void here_ui_resize(Here *h, int cols, int rows) {
    HereUI *u = h->ui; if (!u) return;
    cols = hu_max(2, hu_min(500, cols)); rows = hu_max(2, hu_min(201, rows));
    size_t count = (size_t)cols * rows;
    if (count > u->capacity) {
        HerePixel *grid = (HerePixel *)calloc(count, sizeof(HerePixel));
        HerePixel *previous = (HerePixel *)calloc(count, sizeof(HerePixel));
        if (!grid || !previous) { free(grid); free(previous); h->action = HERE_STOP; return; }
        free(u->grid); free(u->previous); u->grid = grid; u->previous = previous; u->capacity = count;
    }
    h->cols = cols; h->rows = rows; u->menu_width = u->menu && cols >= 76 ? 28 : 0;
    here_resize_tabs(h, hu_max(2, cols - u->menu_width - (u->menu_width ? 1 : 0)), rows - 1);
    u->preview = u->tv_focus = 0; u->have_pressed = 0; u->force = 1; h->dirty = 1;
}
static void hu_compose(Here *h) {
    HereUI *u = h->ui; Cell blank = hu_style(0);
    for (size_t i = 0; i < (size_t)h->cols * h->rows; i++) u->grid[i] = (HerePixel){blank, NULL};
    u->nhits = 0; u->tv_cx = u->tv_cy = -1; u->visible_tvs = 0;
    for (int i = u->ntvs - 1; i >= 0; i--) if (hu_index(h, u->tvs[i]) < 0) {
        memmove(u->tvs + i, u->tvs + i + 1, (size_t)(--u->ntvs - i) * sizeof(int));
    }
    int cur = hu_current(h), shown = hu_index(h, u->preview); if (shown < 0) { u->preview = 0; shown = cur; }
    if (shown >= 0) {
        Term *t = &h->tabs[shown].t; t->view = hu_max(0, hu_min(t->hcount, t->view));
        int start = t->hcount - (t->alt ? 0 : t->view);
        for (int y = 0; y < h->child_rows; y++) {
            const Cell *cells; int n, wrapped; term_line(t, start + y, &cells, &n, &wrapped);
            hu_copy(h, 0, y, t, cells, n, 0, h->child_cols, hu_row_match(u, cells, n));
        }
    }
    int tv_visible_focus = 0;
    if (u->menu_width) {
        int x = h->child_cols + 1, w = u->menu_width;
        hu_put(h, x, 0, "⚙config", 9, 1, HU_CONFIG, 0); hu_put(h, x + 10, 0, "+term", 7, 0, HU_NEW, 0);
        hu_put(h, x + 19, 0, "files", 7, 0, HU_FILES, 0);
        hu_put(h, x, 1, "+www", 7, 0, HU_WEB, 0); hu_put(h, x + 9, 1, h->caf_pid > 0 ? "CAF" : "caf", 5, 0, HU_CAF, 0);
        hu_put(h, x + 15, 1, "^B ? help", 12, 2, 0, 0);
        int available = hu_max(1, h->child_rows - 3);
        int offset = hu_min(u->menu_offset, hu_max(0, h->ntabs - available));
        int y = 3;
        for (int i = offset; i < h->ntabs && i < offset + available; i++, y++) {
            HereTab *tab = &h->tabs[i]; char label[1100], marked[1120]; hu_label(tab, label, sizeof(label));
            const char *mark = i == cur ? "●" : tab->exited ? "!" : tab->unread ? "·" : " ";
            snprintf(marked, sizeof(marked), "%s%s", mark, label);
            int style = i == cur ? 1 : 0;
            hu_put(h, x, y, marked, w - 5, style, HU_CHOOSE, tab->id);
            hu_put(h, x + w - 5, y, hu_has_tv(u, tab->id) ? "−" : "□", 2, style, HU_TV, tab->id);
            hu_put(h, x + w - 2, y, "×", 2, style, HU_CLOSE, tab->id);
        }
        for (int i = 0; i < h->ntabs; i++) {
            HereTab *tab = &h->tabs[i]; Term *t = &tab->t;
            if (!hu_has_tv(u, tab->id) || y + 5 >= h->child_rows) continue;
            u->visible_tvs++;
            int ht = hu_min(9, h->child_rows - y - 1); y++;
            int focused = tab->id == u->tv_focus;
            int strong = i == cur || tab->id == u->preview || focused;
            char label[1100]; hu_label(tab, label, sizeof(label));
            hu_edge(h, x, y, w, strong ? 0x2554 : 0x250c, strong ? 0x2550 : 0x2500, strong ? 0x2557 : 0x2510, strong);
            hu_put(h, x + 2, y, label, w - 7, strong, HU_CHOOSE, tab->id);
            hu_put(h, x + w - 5, y, "↗", 2, 0, HU_CHOOSE, tab->id);
            hu_put(h, x + w - 3, y, "−", 2, 0, HU_TV, tab->id);
            int body = ht - 2, start = hu_max(0, hu_min(t->rows, hu_max(body, t->cy + 1)) - body);
            int left = focused ? hu_max(0, t->cx - (w - 4)) : 0;
            for (int row = 0; row < body; row++) {
                hu_glyph(h, x, y + row + 1, strong ? 0x2551 : 0x2502, 0);
                hu_glyph(h, x + w - 1, y + row + 1, strong ? 0x2551 : 0x2502, 0);
                if (start + row < t->rows) hu_copy(h, x + 1, y + row + 1, t, t->row[t->alt][start + row], t->cols, left, w - 2, 0);
                hu_hit(h, x + 1, y + row + 1, w - 2, 1, HU_TV_INPUT, tab->id);
            }
            hu_edge(h, x, y + ht - 1, w, strong ? 0x255a : 0x2514, strong ? 0x2550 : 0x2500, strong ? 0x255d : 0x2518, 0);
            if (focused && t->cy >= start && t->cy < start + body) { u->tv_cx = x + 1 + t->cx - left; u->tv_cy = y + 1 + t->cy - start; tv_visible_focus = 1; }
            y += ht;
        }
    } else {
        int x = 0;
        for (int i = 0; i < h->ntabs && x < h->cols; i++) {
            char label[1100], marked[1120]; hu_label(&h->tabs[i], label, sizeof(label));
            snprintf(marked, sizeof(marked), " %d %s ", i + 1, label);
            int w = hu_min(24, hu_max(8, h->cols / hu_max(1, h->ntabs)));
            hu_put(h, x, h->child_rows, marked, w, i == cur, HU_CHOOSE, h->tabs[i].id); x += w;
        }
    }
    if (!tv_visible_focus) u->tv_focus = 0;
    char status[1500];
    snprintf(status, sizeof(status), "%s · ^B c new  D detach  s config  / search  ? help", h->session);
    if (u->prefix) snprintf(status, sizeof(status), "Ctrl-B: c new · n/p tabs · v TV · s config · F files · w web · D detach · R reload · q quit");
    if (u->message[0]) snprintf(status, sizeof(status), "%s", u->message);
    if (cur >= 0 && h->tabs[cur].t.view) snprintf(status, sizeof(status), "History ↑%d · ^B ] live · ^B / search", h->tabs[cur].t.view);
    if (u->preview) snprintf(status, sizeof(status), "Preview only — click to select · input stays in the selected tab");
    if (u->tv_focus) snprintf(status, sizeof(status), "Typing in TV · Esc returns to main tab · ↗ expands");
    if (u->visible_tvs < u->ntvs) snprintf(status, sizeof(status), "TV kept open; enlarge terminal or hide another TV to view");
    if (cur >= 0 && h->tabs[cur].exited) snprintf(status, sizeof(status), "Program exited (%d); ^B c new / x close / D detach", h->tabs[cur].status);
    if (u->menu_width || u->message[0] || u->prefix || u->preview || u->visible_tvs < u->ntvs || (cur >= 0 && h->tabs[cur].exited))
        hu_put(h, 0, h->child_rows, status, h->cols, 2, 0, 0);
    if (u->overlay == 2) {
        const char *lines[] = {"Ctrl-B then:", "c new tab     n/p next/previous    1–9 select", "s config      m menu on/off       v mini TV", "F files       w text web          / search", "j next match  N previous match    u/d history", "] live        D detach           R reload UI", "x close tab   q end session       ? help", "Click a tab to select; hover only previews.", "Click TV content to type; Esc returns; ↗ expands.", "Closing the host keeps your session available.", "Shift-drag uses host selection; Esc closes help."};
        int ht = hu_min((int)(sizeof(lines) / sizeof(lines[0])) + 2, h->child_rows), w = hu_min(72, h->cols);
        int x = (h->cols - w) / 2, y = hu_max(0, (h->child_rows - ht) / 2);
        hu_box(h, x, y, w, ht, "Hardcore here");
        for (int i = 0; i < ht - 2; i++) hu_put(h, x + 2, y + i + 1, lines[i], w - 4, 0, 0, 0);
    } else if (u->overlay == 1) {
        int w = hu_min(54, h->cols), ht = hu_min(23, h->child_rows), x = (h->cols - w) / 2, y = hu_max(0, (h->child_rows - ht) / 2);
        hu_box(h, x, y, w, ht, "⚙ config");
        int cap = hu_max(0, ht - (ht >= 12 ? 8 : 2));
        int start = hu_min(u->overlay_offset, hu_max(0, NSCHEMES - cap));
        for (int i = 0; i < cap && start + i < NSCHEMES; i++) {
            const Scheme *s = &SCHEMES[start + i]; char line[80];
            snprintf(line, sizeof(line), "%s%s%s", s == g_scheme ? "● " : "  ", s->name, start + i == u->overlay_offset ? " ‹" : "");
            hu_put(h, x + 2, y + i + 1, line, w - 4, s == g_scheme, HU_THEME, s->key);
        }
        if (ht >= 12) {
            int row = y + ht - 6; char line[180];
            snprintf(line, sizeof(line), "[%c] Host font changes (h)", u->hostfont ? 'x' : ' '); hu_put(h, x + 2, row, line, w - 4, 0, HU_HOSTFONT, 0);
            snprintf(line, sizeof(line), "Font (f): %s", u->font_name); hu_put(h, x + 2, row + 1, line, w - 4, 0, HU_FONT, 0);
            hu_put(h, x + 2, row + 2, "−", 3, 0, HU_FONT_SIZE, -1);
            snprintf(line, sizeof(line), "%d pt", u->font_size); hu_put(h, x + 6, row + 2, line, 8, 0, 0, 0);
            hu_put(h, x + 15, row + 2, "+", 3, 0, HU_FONT_SIZE, 1); hu_put(h, x + 20, row + 2, "Apply(a)", 8, 1, HU_FONT_APPLY, 0);
            hu_put(h, x + 2, row + 3, "Original host font restored on exit/suspend.", w - 4, 2, 0, 0);
            hu_put(h, x + 2, row + 4, h->font_available ? "Font control: this Terminal tab only" : h->font_error[0] ? h->font_error : "Host font unavailable; themes work here", w - 4, 2, 0, 0);
        }
    }
    if (u->confirm) hu_put(h, 0, h->child_rows, u->confirm == 2 ? "End all session processes? y/N" : "Close this tab and its programs? y/N", h->cols, 1, 0, 0);
    if (u->prompt) {
        snprintf(status, sizeof(status), "%s: %.*s▏", u->prompt == 1 ? "search" : u->prompt == 2 ? "url" : "font", (int)sizeof(status) - 20, u->prompt_text);
        hu_put(h, 0, h->child_rows, status, h->cols, 1, 0, 0);
    }
}
static void here_ui_render(Here *h, SBuf *out) {
    HereUI *u = h->ui;
    if (!u || !u->grid || h->cols < 2 || h->rows < 2) return;
    hu_compose(h);
    sb_put(out, "\x1b[?25l\x1b[?7l", sizeof("\x1b[?25l\x1b[?7l") - 1);
    SnapPen pen = {0}; const Term *owner = NULL;
    for (int y = 0; y < h->rows; y++) {
        HerePixel *line = u->grid + (size_t)y * h->cols;
        HerePixel *old = u->previous + (size_t)y * h->cols;
        if (!u->force && !memcmp(line, old, (size_t)h->cols * sizeof(*line))) continue;
        snap_cup(out, 0, y);
        for (int x = 0; x < h->cols; x++) {
            Cell *c = &line[x].c; if (c->wflag == W_CONT) continue;
            uint32_t fg, bg; cell_colors(c, &fg, &bg);
            /* Resolve indexed/default colors ourselves. The hosting terminal's
             * palette and theme never have to change for Hardcore's theme. */
            if (owner != line[x].owner && pen.link) { sb_put(out, "\x1b]8;;\x1b\\", 7); pen.link = 0; }
            owner = line[x].owner;
            snap_sgr(out, owner, &pen, 0x02000000u | fg, 0x02000000u | bg,
                     c->attr & ~(A_INV | A_DIM | A_HIDDEN), owner ? c->link : 0);
            char b[8]; uint32_t cp = c->ch ? c->ch : ' ';
            if (cp < 32 || (cp >= 127 && cp < 160) || cp > 0x10ffff) cp = ' ';
            sb_put(out, b, (size_t)utf8_encode(cp, b));
        }
        memcpy(old, line, (size_t)h->cols * sizeof(*line));
    }
    sb_put(out, "\x1b]8;;\x1b\\\x1b[0m", sizeof("\x1b]8;;\x1b\\\x1b[0m") - 1);
    int i = hu_input(h);
    if (i >= 0) {
        Term *t = &h->tabs[i].t;
        sb_printf(out, "\x1b[?1%c\x1b[%d q", t->m_appcursor ? 'h' : 'l', t->cursor_shape == 1 ? 4 : t->cursor_shape == 2 ? 6 : 2);
        if (!u->overlay && !u->prompt && !u->confirm && !u->preview && !t->view && t->m_cursor_vis) {
            int x = u->tv_focus ? u->tv_cx : t->cx, y = u->tv_focus ? u->tv_cy : t->cy;
            if (x >= 0 && y >= 0 && x < h->cols && y < h->child_rows) { snap_cup(out, x, y); sb_put(out, "\x1b[?25h", 6); }
        }
    }
    u->force = 0; h->dirty = 0;
}
#endif
