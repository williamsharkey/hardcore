/*
 * wsl-easy.c — a tabbed terminal for WSL on Windows 11.
 *
 * Grown out of dumbterm (github.com/williamsharkey/dumbterm): same GNU Unifont
 * bitmap glyphs, same immediate-mode GL renderer, same amber cursor. What is
 * different:
 *
 *   - ConPTY instead of anonymous pipes + a JS shim, so the child is a real
 *     WSL shell with a real pty (isTTY, SIGWINCH, job control, all of it).
 *   - A much fuller VT parser: alt screen, scroll regions, insert/delete,
 *     DEC line drawing, mouse reporting, bracketed paste, sync output.
 *   - Tabs. The "+" button (and Ctrl+Shift+T) opens a new WSL shell in the
 *     directory the current tab is sitting in. The shell reports $PWD on every
 *     prompt through OSC 9;9 from a small rc file wsl-easy writes for itself.
 *   - New windows start in %USERPROFILE% (/mnt/c/Users/<you>), not ~.
 *
 * Build (from WSL or MSYS2):
 *   make            → wsl-easy.exe
 *
 * Usage:
 *   wsl-easy.exe [--cd DIR] [--distro NAME] [DIR]
 *   wsl-easy.exe --selftest OUT.txt     # headless ConPTY + parser check
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── embedded font (tools/gen_font.py output) ──────────────────────── */

__asm__(
    ".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global font_blob\n"
    "font_blob:\n"
    ".incbin \"" FONT_BIN "\"\n"
    ".global font_blob_end\n"
    "font_blob_end:\n"
    ".byte 0\n"
    ".text\n");
extern const unsigned char font_blob[], font_blob_end[];

#define FONT_W 8
#define FONT_H 16

/* Returns the glyph record (width byte + rows) or NULL. */
static const unsigned char *font_glyph(uint32_t cp) {
    uint32_t off;
    if (cp > 0xFFFF) return NULL;
    memcpy(&off, font_blob + 4 + cp * 4, 4);
    if (!off || font_blob + off >= font_blob_end) return NULL;
    return font_blob + off;
}

/* ── ConPTY (loaded at runtime so old mingw headers don't matter) ─── */

typedef void *HPCON_;
typedef HRESULT (WINAPI *PFN_CreatePC)(COORD, HANDLE, HANDLE, DWORD, HPCON_ *);
typedef HRESULT (WINAPI *PFN_ResizePC)(HPCON_, COORD);
typedef void    (WINAPI *PFN_ClosePC)(HPCON_);
static PFN_CreatePC pCreatePC;
static PFN_ResizePC pResizePC;
static PFN_ClosePC  pClosePC;
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

/* ── configuration ─────────────────────────────────────────────────── */

#define HIST_LINES   20000
#define MAX_TABS     64
#define MAX_PARAMS   32
#define OSC_MAX      8192

#define AMBER 0xFFA60D           /* dumbterm's phosphor cursor */
#define DEF_FG 0xCCCCCC
#define DEF_BG 0x0C0C0C
#define BAR_BG 0x1B1B1B
#define TAB_BG 0x262626
#define TAB_HOVER 0x303030
#define SEL_BG 0x264F78

static int g_scale_cfg;          /* 0 = follow DPI */
static int g_scale = 1;
static int g_dpi = 96;
static char g_distro[128];
static char g_start_dir[1024];   /* Windows or Linux path */
static wchar_t g_cfg_dir[MAX_PATH];
static char g_rc_linux[1024];    /* rc.bash as seen from inside WSL */
static int g_win_w = 0, g_win_h = 0;

/* ── cells ─────────────────────────────────────────────────────────── */

/* color spec: 0 = default, 0x01000000|idx = palette, 0x02000000|rgb */
#define C_PAL(i) (0x01000000u | (uint32_t)(i))
#define C_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

enum { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_BLINK = 16, A_INV = 32, A_HIDDEN = 64, A_STRIKE = 128 };
enum { W_WIDE = 1, W_CONT = 2 };

typedef struct {
    uint32_t ch, fg, bg;
    uint16_t attr;
    uint8_t wflag, pad;
} Cell;

typedef struct {
    Cell *c;
    int n, wrapped;
} HLine;

static uint32_t PAL[256];

static void palette_init(void) {
    /* Campbell, the Windows Terminal default, so colors look familiar */
    static const uint32_t base[16] = {
        0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
        0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2};
    static const int cube[6] = {0, 95, 135, 175, 215, 255};
    int i;
    for (i = 0; i < 16; i++) PAL[i] = base[i];
    for (i = 0; i < 216; i++)
        PAL[16 + i] = ((uint32_t)cube[i / 36] << 16) | ((uint32_t)cube[(i / 6) % 6] << 8) | (uint32_t)cube[i % 6];
    for (i = 0; i < 24; i++) {
        uint32_t g = 8 + i * 10;
        PAL[232 + i] = (g << 16) | (g << 8) | g;
    }
}

/* wcwidth, coarse: enough to agree with ConPTY on the common cases */
static int cp_width(uint32_t cp) {
    static const uint32_t zero[][2] = {
        {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A}, {0x064B, 0x065F},
        {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
        {0x200B, 0x200F}, {0x2028, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
        {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0x1F3FB, 0x1F3FF}, {0xE0000, 0xE0FFF}};
    static const uint32_t wide[][2] = {
        {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0},
        {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
        {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5},
        {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
        {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728},
        {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
        {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
        {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF},
        {0xA960, 0xA97F}, {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
        {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
        {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
        {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x3FFFD}};
    size_t i;
    if (cp < 0x300) return 1;
    for (i = 0; i < sizeof(zero) / sizeof(zero[0]); i++)
        if (cp >= zero[i][0] && cp <= zero[i][1]) return 0;
    for (i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
        if (cp >= wide[i][0] && cp <= wide[i][1]) return 2;
    return 1;
}

static const uint16_t DEC_GFX[31] = { /* ESC ( 0, chars 0x60..0x7e */
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518,
    0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524,
    0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7};

/* ── terminal state ────────────────────────────────────────────────── */

enum { S_GROUND, S_ESC, S_ESC_CHARSET, S_ESC_SKIP, S_CSI, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC };

typedef struct {
    int cx, cy, wrapnext, g0dec, origin;
    Cell pen;
} SavedCursor;

typedef struct {
    int cols, rows;
    Cell *buf[2];
    uint8_t *wrapf[2];
    int alt;
    int cx, cy, wrapnext;
    Cell pen;
    SavedCursor saved[2];
    int top, bot;
    int g0dec;
    uint32_t last_ch;
    /* modes */
    int m_autowrap, m_origin, m_insert, m_cursor_vis, m_appcursor, m_bracketed;
    int m_mouse, m_mouse_sgr, m_focus, m_sync;
    DWORD sync_since;
    int cursor_shape;            /* 0 block, 1 underline, 2 bar */
    /* parser */
    int st, esc_which;
    int params[MAX_PARAMS], np, priv, inter;
    int utf8_need;
    uint32_t utf8_acc;
    char osc[OSC_MAX];
    int osc_n;
    /* scrollback */
    HLine *hist;
    int hcount, hstart;
    long long hpushed;
    int view;                    /* lines scrolled back from live */
    /* reported by the shell / apps */
    wchar_t title[256];
    char cwd[1024];
    int bell;
    int dirty;
} Term;

/* Term is the first member so the parser can find its tab for replies. */
typedef struct Tab {
    Term t;
    int id;
    HPCON_ hpc;
    HANDLE pin_w, pout_r, proc, reader, wait;
    CRITICAL_SECTION lock;
    char *in;
    int in_n, in_cap;
    volatile LONG posted;
    volatile LONG exited;
    int activity;
    char start_dir[1024];
} Tab;

static HWND g_hwnd;

static void tab_write(Tab *tb, const void *s, int n) {
    DWORD w;
    if (tb->pin_w && n > 0) WriteFile(tb->pin_w, s, (DWORD)n, &w, NULL);
}

static void term_reply(Term *t, const char *s) { tab_write((Tab *)t, s, (int)strlen(s)); }

static Cell blank_cell(const Term *t) {
    Cell c;
    memset(&c, 0, sizeof(c));
    c.ch = ' ';
    c.bg = t->pen.bg;            /* xterm-style background color erase */
    return c;
}

#define CELL(t, x, y) (&(t)->buf[(t)->alt][(size_t)(y) * (t)->cols + (x)])

static void clear_span(Term *t, int y, int x0, int x1) {
    Cell b = blank_cell(t);
    int x;
    if (x0 < 0) x0 = 0;
    if (x1 > t->cols) x1 = t->cols;
    for (x = x0; x < x1; x++) *CELL(t, x, y) = b;
}

static void clear_rows(Term *t, int y0, int y1) {
    int y;
    for (y = y0; y < y1; y++) {
        clear_span(t, y, 0, t->cols);
        t->wrapf[t->alt][y] = 0;
    }
}

static int row_is_blank_default(const Cell *c) {
    return c->ch == ' ' && !c->bg && !c->fg && !(c->attr & (A_INV | A_UNDER | A_STRIKE));
}

static void hist_clear(Term *t) {
    int i;
    for (i = 0; i < t->hcount; i++) free(t->hist[(t->hstart + i) % HIST_LINES].c);
    t->hcount = t->hstart = 0;
    t->view = 0;
}

static void hist_push(Term *t, const Cell *row, int wrapped) {
    int n = t->cols, idx;
    HLine *h;
    while (n > 0 && row_is_blank_default(&row[n - 1])) n--;
    if (t->hcount < HIST_LINES) {
        idx = (t->hstart + t->hcount) % HIST_LINES;
        t->hcount++;
    } else {
        idx = t->hstart;
        free(t->hist[idx].c);
        t->hstart = (t->hstart + 1) % HIST_LINES;
        if (t->view > 0) t->view--;     /* oldest line fell off */
    }
    h = &t->hist[idx];
    h->n = n;
    h->wrapped = wrapped;
    h->c = n ? (Cell *)malloc(sizeof(Cell) * n) : NULL;
    if (n) memcpy(h->c, row, sizeof(Cell) * n);
    t->hpushed++;
    if (t->view > 0 && t->view < t->hcount) t->view++;  /* keep the viewport still */
}

static void scroll_up(Term *t, int top, int bot, int n, int to_hist) {
    int y, h = bot - top + 1;
    if (n <= 0) return;
    if (n > h) n = h;
    if (to_hist && !t->alt && top == 0)
        for (y = 0; y < n; y++) hist_push(t, CELL(t, 0, y), t->wrapf[0][y]);
    memmove(CELL(t, 0, top), CELL(t, 0, top + n), sizeof(Cell) * (size_t)t->cols * (h - n));
    memmove(&t->wrapf[t->alt][top], &t->wrapf[t->alt][top + n], (size_t)(h - n));
    clear_rows(t, bot - n + 1, bot + 1);
}

static void scroll_down(Term *t, int top, int bot, int n) {
    int h = bot - top + 1;
    if (n <= 0) return;
    if (n > h) n = h;
    memmove(CELL(t, 0, top + n), CELL(t, 0, top), sizeof(Cell) * (size_t)t->cols * (h - n));
    memmove(&t->wrapf[t->alt][top + n], &t->wrapf[t->alt][top], (size_t)(h - n));
    clear_rows(t, top, top + n);
}

static void term_lf(Term *t) {
    if (t->cy == t->bot) scroll_up(t, t->top, t->bot, 1, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void term_ri(Term *t) {
    if (t->cy == t->top) scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0) t->cy--;
}

static void term_reset(Term *t) {
    memset(&t->pen, 0, sizeof(t->pen));
    t->pen.ch = ' ';
    t->top = 0;
    t->bot = t->rows - 1;
    t->m_autowrap = 1;
    t->m_cursor_vis = 1;
    t->m_origin = t->m_insert = t->m_appcursor = t->m_bracketed = 0;
    t->m_mouse = t->m_mouse_sgr = t->m_focus = t->m_sync = 0;
    t->g0dec = 0;
    t->cursor_shape = 0;
    t->wrapnext = 0;
}

static int term_init(Term *t, int cols, int rows) {
    int s;
    memset(t, 0, sizeof(*t));
    t->cols = cols;
    t->rows = rows;
    t->hist = (HLine *)calloc(HIST_LINES, sizeof(HLine));
    for (s = 0; s < 2; s++) {
        t->buf[s] = (Cell *)malloc(sizeof(Cell) * (size_t)cols * rows);
        t->wrapf[s] = (uint8_t *)calloc((size_t)rows, 1);
    }
    if (!t->hist || !t->buf[0] || !t->buf[1]) return 0;
    term_reset(t);
    for (s = 1; s >= 0; s--) {
        t->alt = s;
        clear_rows(t, 0, rows);
    }
    return 1;
}

static void term_free(Term *t) {
    hist_clear(t);
    free(t->hist);
    free(t->buf[0]);
    free(t->buf[1]);
    free(t->wrapf[0]);
    free(t->wrapf[1]);
}

static void term_resize(Term *t, int cols, int rows) {
    int s, y, x;
    if (cols == t->cols && rows == t->rows) return;
    for (s = 0; s < 2; s++) {
        Cell *nb = (Cell *)malloc(sizeof(Cell) * (size_t)cols * rows);
        uint8_t *nw = (uint8_t *)calloc((size_t)rows, 1);
        int cy = (s == t->alt) ? t->cy : t->saved[s].cy;
        int shift = 0, cur_alt = t->alt;
        Cell b;
        memset(&b, 0, sizeof(b));
        b.ch = ' ';
        /* Shrinking: keep the cursor row on screen, push the rest into history. */
        if (rows < t->rows && cy >= rows) shift = cy - rows + 1;
        if (s == 0 && shift) {
            t->alt = 0;
            for (y = 0; y < shift; y++) hist_push(t, CELL(t, 0, y), t->wrapf[0][y]);
            t->alt = cur_alt;
        }
        for (y = 0; y < rows; y++) {
            int sy = y + shift;
            nw[y] = (sy < t->rows) ? t->wrapf[s][sy] : 0;
            for (x = 0; x < cols; x++)
                nb[(size_t)y * cols + x] = (sy < t->rows && x < t->cols) ? t->buf[s][(size_t)sy * t->cols + x] : b;
            if (cols < t->cols && nb[(size_t)y * cols + cols - 1].wflag == W_WIDE) nb[(size_t)y * cols + cols - 1] = b;
        }
        free(t->buf[s]);
        free(t->wrapf[s]);
        t->buf[s] = nb;
        t->wrapf[s] = nw;
        if (s == t->alt) t->cy = cy - shift;
        else t->saved[s].cy = cy - shift;
    }
    t->cols = cols;
    t->rows = rows;
    if (t->cx >= cols) t->cx = cols - 1;
    if (t->cy >= rows) t->cy = rows - 1;
    if (t->cy < 0) t->cy = 0;
    for (s = 0; s < 2; s++) {
        if (t->saved[s].cx >= cols) t->saved[s].cx = cols - 1;
        if (t->saved[s].cy >= rows) t->saved[s].cy = rows - 1;
        if (t->saved[s].cy < 0) t->saved[s].cy = 0;
    }
    t->top = 0;
    t->bot = rows - 1;
    t->wrapnext = 0;
    t->view = 0;
    t->dirty = 1;
}

/* Writing over half of a wide char leaves the other half orphaned. */
static void fix_wide(Term *t, int x, int w) {
    Cell b = blank_cell(t);
    int p;
    for (p = x; p < x + w && p < t->cols; p++) {
        Cell *c = CELL(t, p, t->cy);
        if (c->wflag == W_CONT && p > 0 && p == x) *CELL(t, p - 1, t->cy) = b;
        if (c->wflag == W_WIDE && p + 1 < t->cols && p + 1 >= x + w) *CELL(t, p + 1, t->cy) = b;
    }
}

static void insert_blanks(Term *t, int n) {
    int x;
    Cell b = blank_cell(t);
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    for (x = t->cols - 1; x >= t->cx + n; x--) *CELL(t, x, t->cy) = *CELL(t, x - n, t->cy);
    for (x = t->cx; x < t->cx + n; x++) *CELL(t, x, t->cy) = b;
}

static void term_put(Term *t, uint32_t cp) {
    int w;
    Cell *c;
    if (t->g0dec && cp >= 0x60 && cp <= 0x7E) cp = DEC_GFX[cp - 0x60];
    w = cp_width(cp);
    if (w == 0) return;          /* combining marks: no cell of their own */
    if (t->wrapnext) {
        if (t->m_autowrap) {
            t->wrapf[t->alt][t->cy] = 1;
            t->cx = 0;
            term_lf(t);
        }
        t->wrapnext = 0;
    }
    if (w == 2 && t->cx == t->cols - 1) {
        if (!t->m_autowrap) return;
        clear_span(t, t->cy, t->cx, t->cols);
        t->wrapf[t->alt][t->cy] = 1;
        t->cx = 0;
        term_lf(t);
    }
    if (t->m_insert) insert_blanks(t, w);
    fix_wide(t, t->cx, w);
    c = CELL(t, t->cx, t->cy);
    *c = t->pen;
    c->ch = cp;
    c->wflag = (w == 2) ? W_WIDE : 0;
    if (w == 2) {
        c[1] = t->pen;
        c[1].ch = 0;
        c[1].wflag = W_CONT;
    }
    t->last_ch = cp;
    t->cx += w;
    if (t->cx >= t->cols) {
        t->cx = t->cols - 1;
        t->wrapnext = 1;
    }
}

static void save_cursor(Term *t) {
    SavedCursor *s = &t->saved[t->alt];
    s->cx = t->cx;
    s->cy = t->cy;
    s->wrapnext = t->wrapnext;
    s->g0dec = t->g0dec;
    s->origin = t->m_origin;
    s->pen = t->pen;
}

static void restore_cursor(Term *t) {
    SavedCursor *s = &t->saved[t->alt];
    t->cx = s->cx < t->cols ? s->cx : t->cols - 1;
    t->cy = s->cy < t->rows ? s->cy : t->rows - 1;
    t->wrapnext = s->wrapnext;
    t->g0dec = s->g0dec;
    t->m_origin = s->origin;
    t->pen = s->pen;
    if (!t->pen.ch) t->pen.ch = ' ';
}

static void set_alt(Term *t, int on, int clear) {
    if (on && !t->alt) {
        t->alt = 1;
        if (clear) clear_rows(t, 0, t->rows);
    } else if (!on && t->alt) {
        if (clear) clear_rows(t, 0, t->rows);
        t->alt = 0;
    }
    t->view = 0;
}

static void control(Term *t, int b) {
    switch (b) {
    case 0x07: t->bell = 1; break;
    case 0x08:
        if (t->wrapnext) t->wrapnext = 0;
        else if (t->cx > 0) t->cx--;
        break;
    case 0x09: {
        int nx = (t->cx + 8) & ~7;
        t->cx = nx < t->cols ? nx : t->cols - 1;
        break;
    }
    case 0x0A: case 0x0B: case 0x0C:
        term_lf(t);
        t->wrapnext = 0;
        break;
    case 0x0D: t->cx = 0; t->wrapnext = 0; break;
    case 0x18: case 0x1A: t->st = S_GROUND; break;
    case 0x1B: t->st = S_ESC; break;
    }
}

static int P(Term *t, int i, int def) { return (i < t->np && t->params[i]) ? t->params[i] : def; }

static void sgr(Term *t) {
    int i;
    for (i = 0; i < t->np; i++) {
        int p = t->params[i];
        Cell *pen = &t->pen;
        switch (p) {
        case 0: pen->fg = pen->bg = 0; pen->attr = 0; break;
        case 1: pen->attr |= A_BOLD; break;
        case 2: pen->attr |= A_DIM; break;
        case 3: pen->attr |= A_ITALIC; break;
        case 4: case 21: pen->attr |= A_UNDER; break;
        case 5: case 6: pen->attr |= A_BLINK; break;
        case 7: pen->attr |= A_INV; break;
        case 8: pen->attr |= A_HIDDEN; break;
        case 9: pen->attr |= A_STRIKE; break;
        case 22: pen->attr &= ~(A_BOLD | A_DIM); break;
        case 23: pen->attr &= ~A_ITALIC; break;
        case 24: pen->attr &= ~A_UNDER; break;
        case 25: pen->attr &= ~A_BLINK; break;
        case 27: pen->attr &= ~A_INV; break;
        case 28: pen->attr &= ~A_HIDDEN; break;
        case 29: pen->attr &= ~A_STRIKE; break;
        case 39: pen->fg = 0; break;
        case 49: pen->bg = 0; break;
        case 38: case 48: case 58: {
            uint32_t col = 0;
            if (i + 1 < t->np && t->params[i + 1] == 5 && i + 2 < t->np) {
                col = C_PAL(t->params[i + 2] & 255);
                i += 2;
            } else if (i + 1 < t->np && t->params[i + 1] == 2 && i + 4 < t->np) {
                col = C_RGB(t->params[i + 2] & 255, t->params[i + 3] & 255, t->params[i + 4] & 255);
                i += 4;
            } else {
                i = t->np;
                break;
            }
            if (p == 38) pen->fg = col;
            else if (p == 48) pen->bg = col;
            break;
        }
        default:
            if (p >= 30 && p <= 37) pen->fg = C_PAL(p - 30);
            else if (p >= 40 && p <= 47) pen->bg = C_PAL(p - 40);
            else if (p >= 90 && p <= 97) pen->fg = C_PAL(p - 90 + 8);
            else if (p >= 100 && p <= 107) pen->bg = C_PAL(p - 100 + 8);
        }
    }
}

static void decset(Term *t, int p, int on) {
    switch (p) {
    case 1: t->m_appcursor = on; break;
    case 6: t->m_origin = on; t->cx = 0; t->cy = on ? t->top : 0; break;
    case 7: t->m_autowrap = on; break;
    case 25: t->m_cursor_vis = on; break;
    case 47: set_alt(t, on, 0); break;
    case 1047: set_alt(t, on, !on); break;
    case 1048: if (on) save_cursor(t); else restore_cursor(t); break;
    case 1049:
        if (on) {
            if (!t->alt) {
                save_cursor(t);
                set_alt(t, 1, 1);
                t->saved[1] = t->saved[0];
            }
        } else if (t->alt) {
            set_alt(t, 0, 0);
            restore_cursor(t);
        }
        break;
    case 9: case 1000: case 1002: case 1003: t->m_mouse = on ? p : 0; break;
    case 1006: t->m_mouse_sgr = on; break;
    case 1004: t->m_focus = on; break;
    case 2004: t->m_bracketed = on; break;
    case 2026:
        t->m_sync = on;
        t->sync_since = GetTickCount();
        break;
    }
}

static void csi_dispatch(Term *t, int f) {
    int n, i;
    char buf[64];
    if (t->priv == '?') {
        if (f == 'h' || f == 'l')
            for (i = 0; i < t->np; i++) decset(t, t->params[i], f == 'h');
        else if (f == 'J' || f == 'K') {
            t->priv = 0;
            csi_dispatch(t, f);
        }
        return;
    }
    if (t->priv == '>') {
        if (f == 'c') term_reply(t, "\x1b[>0;10;1c");
        return;
    }
    if (t->priv) return;
    if (t->inter == ' ' && f == 'q') {
        n = P(t, 0, 1);
        t->cursor_shape = n <= 2 ? 0 : n <= 4 ? 1 : 2;
        return;
    }
    if (t->inter == '!' && f == 'p') {
        term_reset(t);
        return;
    }
    if (t->inter) return;

    if (f != 'b') t->wrapnext = 0;
    switch (f) {
    case '@': insert_blanks(t, P(t, 0, 1)); break;
    case 'A': {               /* stops at the top margin if starting inside it */
        int lim = t->cy >= t->top ? t->top : 0;
        t->cy -= P(t, 0, 1);
        if (t->cy < lim) t->cy = lim;
        break;
    }
    case 'B': case 'e': {
        int lim = t->cy <= t->bot ? t->bot : t->rows - 1;
        t->cy += P(t, 0, 1);
        if (t->cy > lim) t->cy = lim;
        break;
    }
    case 'C': case 'a': t->cx += P(t, 0, 1); if (t->cx >= t->cols) t->cx = t->cols - 1; break;
    case 'D': t->cx -= P(t, 0, 1); if (t->cx < 0) t->cx = 0; break;
    case 'E': t->cy += P(t, 0, 1); if (t->cy >= t->rows) t->cy = t->rows - 1; t->cx = 0; break;
    case 'F': t->cy -= P(t, 0, 1); if (t->cy < 0) t->cy = 0; t->cx = 0; break;
    case 'G': case '`': t->cx = P(t, 0, 1) - 1; if (t->cx >= t->cols) t->cx = t->cols - 1; break;
    case 'H': case 'f': {
        int off = t->m_origin ? t->top : 0;
        t->cy = P(t, 0, 1) - 1 + off;
        t->cx = P(t, 1, 1) - 1;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
        if (t->m_origin && t->cy > t->bot) t->cy = t->bot;
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        break;
    }
    case 'd': {
        int off = t->m_origin ? t->top : 0;
        t->cy = P(t, 0, 1) - 1 + off;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
        break;
    }
    case 'I': for (n = P(t, 0, 1); n > 0; n--) { t->cx = (t->cx + 8) & ~7; if (t->cx >= t->cols) { t->cx = t->cols - 1; break; } } break;
    case 'Z': for (n = P(t, 0, 1); n > 0 && t->cx > 0; n--) t->cx = (t->cx - 1) & ~7; break;
    case 'J':
        switch (P(t, 0, 0)) {
        case 0: clear_span(t, t->cy, t->cx, t->cols); clear_rows(t, t->cy + 1, t->rows); break;
        case 1: clear_rows(t, 0, t->cy); clear_span(t, t->cy, 0, t->cx + 1); break;
        case 2: clear_rows(t, 0, t->rows); break;
        case 3: if (!t->alt) hist_clear(t); break;
        }
        break;
    case 'K':
        switch (P(t, 0, 0)) {
        case 0: clear_span(t, t->cy, t->cx, t->cols); break;
        case 1: clear_span(t, t->cy, 0, t->cx + 1); break;
        case 2: clear_span(t, t->cy, 0, t->cols); break;
        }
        break;
    case 'L': if (t->cy >= t->top && t->cy <= t->bot) { scroll_down(t, t->cy, t->bot, P(t, 0, 1)); t->cx = 0; } break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bot) { scroll_up(t, t->cy, t->bot, P(t, 0, 1), 0); t->cx = 0; } break;
    case 'P': {
        int x;
        Cell b = blank_cell(t);
        n = P(t, 0, 1);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        for (x = t->cx; x < t->cols - n; x++) *CELL(t, x, t->cy) = *CELL(t, x + n, t->cy);
        for (x = t->cols - n; x < t->cols; x++) *CELL(t, x, t->cy) = b;
        break;
    }
    case 'X': clear_span(t, t->cy, t->cx, t->cx + P(t, 0, 1)); break;
    case 'S': scroll_up(t, t->top, t->bot, P(t, 0, 1), 1); break;
    case 'T': scroll_down(t, t->top, t->bot, P(t, 0, 1)); break;
    case 'b': for (n = P(t, 0, 1); n > 0 && t->last_ch; n--) term_put(t, t->last_ch); break;
    case 'c': if (P(t, 0, 0) == 0) term_reply(t, "\x1b[?62;22c"); break;
    case 'm': sgr(t); break;
    case 'n':
        if (P(t, 0, 0) == 5) term_reply(t, "\x1b[0n");
        else if (P(t, 0, 0) == 6) {
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR", t->cy + 1 - (t->m_origin ? t->top : 0), t->cx + 1);
            term_reply(t, buf);
        }
        break;
    case 'r': {
        int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) {
            t->top = top;
            t->bot = bot;
        } else {
            t->top = 0;
            t->bot = t->rows - 1;
        }
        t->cx = 0;
        t->cy = t->m_origin ? t->top : 0;
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    case 'h': case 'l':
        for (i = 0; i < t->np; i++)
            if (t->params[i] == 4) t->m_insert = (f == 'h');
        break;
    case 't':
        if (P(t, 0, 0) == 18) {
            snprintf(buf, sizeof(buf), "\x1b[8;%d;%dt", t->rows, t->cols);
            term_reply(t, buf);
        }
        break;
    }
}

/* ── OSC: titles, cwd, clipboard, color queries ───────────────────── */

static void set_clipboard_utf8(const char *s, int n);

static void percent_decode(char *s) {
    char *o = s;
    while (*s) {
        if (s[0] == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else *o++ = *s++;
    }
    *o = 0;
}

static void set_cwd(Term *t, const char *p) {
    char tmp[1024];
    size_t i;
    if (!p || !*p) return;
    /* a Windows path (wslpath -w style) becomes /mnt/<drive>/... */
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':' && (p[2] == '\\' || p[2] == '/' || !p[2])) {
        snprintf(tmp, sizeof(tmp), "/mnt/%c%s", p[0] | 32, p + 2);
        for (i = 0; tmp[i]; i++) if (tmp[i] == '\\') tmp[i] = '/';
        p = tmp;
    }
    if (strcmp(t->cwd, p) != 0) {
        snprintf(t->cwd, sizeof(t->cwd), "%s", p);
        t->dirty = 1;
    }
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void osc_dispatch(Term *t) {
    char *s = t->osc, *rest;
    int code;
    t->osc[t->osc_n] = 0;
    code = (int)strtol(s, &rest, 10);
    if (rest == s) return;
    if (*rest == ';') rest++;
    switch (code) {
    case 0: case 2:
        MultiByteToWideChar(CP_UTF8, 0, rest, -1, t->title, 255);
        t->title[255] = 0;
        t->dirty = 1;
        break;
    case 7:
        if (!strncmp(rest, "file://", 7)) {
            char *path = strchr(rest + 7, '/');
            if (path) {
                percent_decode(path);
                set_cwd(t, path);
            }
        }
        break;
    case 9:
        if (!strncmp(rest, "9;", 2)) {
            char *p = rest + 2;
            size_t n = strlen(p);
            if (n >= 2 && p[0] == '"' && p[n - 1] == '"') {
                p[n - 1] = 0;
                p++;
            }
            set_cwd(t, p);
        }
        break;
    case 10: case 11:
        if (!strcmp(rest, "?")) {
            char buf[64];
            uint32_t c = code == 10 ? DEF_FG : DEF_BG;
            snprintf(buf, sizeof(buf), "\x1b]%d;rgb:%02x%02x/%02x%02x/%02x%02x\x1b\\", code,
                     c >> 16, c >> 16, (c >> 8) & 255, (c >> 8) & 255, c & 255, c & 255);
            term_reply(t, buf);
        }
        break;
    case 52: {
        char *data = strchr(rest, ';');
        if (data && strcmp(data + 1, "?") != 0) {
            unsigned char *out = (unsigned char *)malloc(strlen(data) + 1);
            int bits = 0, n = 0, v;
            uint32_t acc = 0;
            for (data++; *data; data++) {
                if ((v = b64val((unsigned char)*data)) < 0) continue;
                acc = (acc << 6) | (uint32_t)v;
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out[n++] = (unsigned char)(acc >> bits);
                }
            }
            if (out) {
                set_clipboard_utf8((char *)out, n);
                free(out);
            }
        }
        break;
    }
    }
}

/* ── the parser ────────────────────────────────────────────────────── */

static void esc_dispatch(Term *t, int b) {
    t->st = S_GROUND;
    switch (b) {
    case '[':
        t->st = S_CSI;
        t->np = 0;
        t->params[0] = 0;
        t->priv = t->inter = 0;
        break;
    case ']': t->st = S_OSC; t->osc_n = 0; break;
    case 'P': case 'X': case '^': case '_': t->st = S_STR; break;
    case '(': case ')': case '*': case '+': t->st = S_ESC_CHARSET; t->esc_which = b; break;
    case '#': case ' ': case '%': t->st = S_ESC_SKIP; break;
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'D': term_lf(t); t->wrapnext = 0; break;
    case 'E': term_lf(t); t->cx = 0; t->wrapnext = 0; break;
    case 'M': term_ri(t); t->wrapnext = 0; break;
    case 'c':
        term_reset(t);
        set_alt(t, 0, 0);
        clear_rows(t, 0, t->rows);
        t->cx = t->cy = 0;
        break;
    }
}

static void vt_byte(Term *t, unsigned char b) {
    switch (t->st) {
    case S_GROUND:
        if (t->utf8_need) {
            if ((b & 0xC0) == 0x80) {
                t->utf8_acc = (t->utf8_acc << 6) | (b & 0x3F);
                if (--t->utf8_need == 0) term_put(t, t->utf8_acc);
                return;
            }
            t->utf8_need = 0;
            term_put(t, 0xFFFD);
        }
        if (b < 0x20 || b == 0x7F) { control(t, b); return; }
        if (b < 0x80) { term_put(t, b); return; }
        if ((b & 0xE0) == 0xC0) { t->utf8_acc = b & 0x1F; t->utf8_need = 1; }
        else if ((b & 0xF0) == 0xE0) { t->utf8_acc = b & 0x0F; t->utf8_need = 2; }
        else if ((b & 0xF8) == 0xF0) { t->utf8_acc = b & 0x07; t->utf8_need = 3; }
        else term_put(t, 0xFFFD);
        return;
    case S_ESC:
        if (b < 0x20) { control(t, b); return; }
        esc_dispatch(t, b);
        return;
    case S_ESC_CHARSET:
        if (t->esc_which == '(') t->g0dec = (b == '0');
        t->st = S_GROUND;
        return;
    case S_ESC_SKIP:
        t->st = S_GROUND;
        return;
    case S_CSI:
        if (b >= '0' && b <= '9') {
            int *p = &t->params[t->np];
            if (*p < 100000) *p = *p * 10 + (b - '0');
        } else if (b == ';' || b == ':') {
            if (t->np < MAX_PARAMS - 1) t->params[++t->np] = 0;
        } else if (b >= '<' && b <= '?') {
            t->priv = b;
        } else if (b >= 0x20 && b <= 0x2F) {
            t->inter = b;
        } else if (b >= 0x40 && b <= 0x7E) {
            t->np++;
            t->st = S_GROUND;
            csi_dispatch(t, b);
        } else if (b < 0x20) {
            control(t, b);       /* C0 inside CSI executes in place */
        } else {
            t->st = S_GROUND;
        }
        return;
    case S_OSC:
        if (b == 0x07) { osc_dispatch(t); t->st = S_GROUND; }
        else if (b == 0x1B) t->st = S_OSC_ESC;
        else if (t->osc_n < OSC_MAX - 1) t->osc[t->osc_n++] = (char)b;
        return;
    case S_OSC_ESC:
        osc_dispatch(t);
        t->st = S_GROUND;
        if (b != '\\') esc_dispatch(t, b);
        return;
    case S_STR:
        if (b == 0x1B) t->st = S_STR_ESC;
        else if (b == 0x07) t->st = S_GROUND;
        return;
    case S_STR_ESC:
        t->st = (b == '\\') ? S_GROUND : S_STR;
        return;
    }
}

static void vt_feed(Term *t, const unsigned char *s, int n) {
    int i;
    for (i = 0; i < n; i++) vt_byte(t, s[i]);
    t->dirty = 1;
}

/* ── history / selection addressing ───────────────────────────────── */

/* L counts from the oldest kept history line; screen rows follow history. */
static void term_line(const Term *t, int L, const Cell **cells, int *n, int *wrapped) {
    if (L < t->hcount && !t->alt) {
        const HLine *h = &t->hist[(t->hstart + L) % HIST_LINES];
        *cells = h->c;
        *n = h->n;
        *wrapped = h->wrapped;
    } else {
        int y = L - t->hcount;
        if (y < 0 || y >= t->rows) { *cells = NULL; *n = 0; *wrapped = 0; return; }
        *cells = &t->buf[t->alt][(size_t)y * t->cols];
        *n = t->cols;
        *wrapped = t->wrapf[t->alt][y];
    }
}

static long long term_base(const Term *t) { return t->hpushed - t->hcount; }

/* ── tabs: ConPTY + reader thread ─────────────────────────────────── */

#define WM_APP_OUTPUT (WM_APP + 1)
#define WM_APP_EXITED (WM_APP + 2)

static Tab *g_tabs[MAX_TABS];
static int g_ntabs, g_active;
static int g_next_id = 1;
static int g_cols = 120, g_rows = 34;

static Tab *tab_by_id(int id) {
    int i;
    for (i = 0; i < g_ntabs; i++)
        if (g_tabs[i]->id == id) return g_tabs[i];
    return NULL;
}

static DWORD WINAPI reader_thread(LPVOID arg) {
    Tab *tb = (Tab *)arg;
    char buf[65536];
    DWORD n;
    while (ReadFile(tb->pout_r, buf, sizeof(buf), &n, NULL) && n > 0) {
        EnterCriticalSection(&tb->lock);
        if (tb->in_n + (int)n > tb->in_cap) {
            int cap = tb->in_cap ? tb->in_cap : 65536;
            while (cap < tb->in_n + (int)n) cap *= 2;
            tb->in = (char *)realloc(tb->in, cap);
            tb->in_cap = cap;
        }
        memcpy(tb->in + tb->in_n, buf, n);
        tb->in_n += n;
        LeaveCriticalSection(&tb->lock);
        if (g_hwnd && InterlockedExchange(&tb->posted, 1) == 0)
            PostMessageW(g_hwnd, WM_APP_OUTPUT, (WPARAM)tb->id, 0);
    }
    return 0;
}

static VOID CALLBACK on_child_exit(PVOID ctx, BOOLEAN timed_out) {
    Tab *tb = (Tab *)ctx;
    (void)timed_out;
    InterlockedExchange(&tb->exited, 1);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_EXITED, (WPARAM)tb->id, 0);
}

/* Moves pending output into the parser. Returns bytes consumed. */
static int tab_drain(Tab *tb) {
    char *data;
    int n;
    EnterCriticalSection(&tb->lock);
    data = tb->in;
    n = tb->in_n;
    tb->in = NULL;
    tb->in_n = tb->in_cap = 0;
    InterlockedExchange(&tb->posted, 0);
    LeaveCriticalSection(&tb->lock);
    if (n) vt_feed(&tb->t, (unsigned char *)data, n);
    free(data);
    return n;
}

static void win_to_linux_path(const char *w, char *out, size_t cap) {
    size_t i;
    if (((w[0] | 32) >= 'a' && (w[0] | 32) <= 'z') && w[1] == ':') {
        snprintf(out, cap, "/mnt/%c%s", w[0] | 32, w + 2);
        for (i = 0; out[i]; i++) if (out[i] == '\\') out[i] = '/';
    } else snprintf(out, cap, "%s", w);
}

/* Login-shell startup files, then report $PWD on every prompt (OSC 9;9).
   Written by wsl-easy on each launch; nothing in your dotfiles changes. */
static const char RC_BASH[] =
    "# written by wsl-easy on every launch; edits here are overwritten\n"
    "[ -r /etc/profile ] && . /etc/profile\n"
    "if [ -r ~/.bash_profile ]; then . ~/.bash_profile\n"
    "elif [ -r ~/.bash_login ]; then . ~/.bash_login\n"
    "elif [ -r ~/.profile ]; then . ~/.profile\n"
    "elif [ -r ~/.bashrc ]; then . ~/.bashrc\n"
    "fi\n"
    "__wsl_easy_cwd() { printf '\\033]9;9;%s\\033\\\\' \"$PWD\"; }\n"
    "case \";${PROMPT_COMMAND[0]};\" in *\";__wsl_easy_cwd;\"*) ;; *)\n"
    "  PROMPT_COMMAND=\"__wsl_easy_cwd${PROMPT_COMMAND:+;$PROMPT_COMMAND}\" ;;\n"
    "esac\n";

static void write_rc(void) {
    wchar_t path[MAX_PATH];
    char wpath[MAX_PATH * 3];
    FILE *f;
    _snwprintf(path, MAX_PATH, L"%ls\\rc.bash", g_cfg_dir);
    path[MAX_PATH - 1] = 0;
    f = _wfopen(path, L"wb");
    if (f) {
        fwrite(RC_BASH, 1, sizeof(RC_BASH) - 1, f);
        fclose(f);
    }
    WideCharToMultiByte(CP_UTF8, 0, path, -1, wpath, sizeof(wpath), NULL, NULL);
    win_to_linux_path(wpath, g_rc_linux, sizeof(g_rc_linux));
}

static void term_message(Term *t, const char *msg) {
    vt_feed(t, (const unsigned char *)msg, (int)strlen(msg));
}

static int tab_spawn(Tab *tb) {
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    COORD sz;
    HRESULT hr;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T attr_sz = 0;
    char cmd[4096];
    wchar_t wcmd[4096];

    if (!pCreatePC) {
        term_message(&tb->t, "\x1b[31mwsl-easy: this Windows has no ConPTY (needs Windows 10 1809+)\x1b[0m\r\n");
        return 0;
    }
    if (!CreatePipe(&in_r, &in_w, NULL, 0) || !CreatePipe(&out_r, &out_w, NULL, 0)) return 0;
    sz.X = (SHORT)tb->t.cols;
    sz.Y = (SHORT)tb->t.rows;
    hr = pCreatePC(sz, in_r, out_w, 0, &tb->hpc);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (FAILED(hr)) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        term_message(&tb->t, "\x1b[31mwsl-easy: CreatePseudoConsole failed\x1b[0m\r\n");
        return 0;
    }
    tb->pin_w = in_w;
    tb->pout_r = out_r;

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    /* Without this a child of a console-attached wsl-easy inherits our std
       handles instead of talking to the pseudoconsole. */
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_sz);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_sz);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, tb->hpc, sizeof(HPCON_), NULL, NULL);

    snprintf(cmd, sizeof(cmd), "wsl.exe%s%s%s --cd \"%s\" -e bash --rcfile \"%s\" -i",
             g_distro[0] ? " -d \"" : "", g_distro, g_distro[0] ? "\"" : "",
             tb->start_dir, g_rc_linux);
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 4096);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        NULL, NULL, &si.StartupInfo, &pi)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "\x1b[31mwsl-easy: could not start wsl.exe (error %lu)\x1b[0m\r\n", GetLastError());
        term_message(&tb->t, msg);
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        return 0;
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    CloseHandle(pi.hThread);
    tb->proc = pi.hProcess;
    tb->reader = CreateThread(NULL, 0, reader_thread, tb, 0, NULL);
    RegisterWaitForSingleObject(&tb->wait, tb->proc, on_child_exit, tb, INFINITE, WT_EXECUTEONLYONCE);
    return 1;
}

static Tab *tab_create(const char *dir) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) {
        free(tb);
        return NULL;
    }
    tb->id = g_next_id++;
    InitializeCriticalSection(&tb->lock);
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", dir && *dir ? dir : g_start_dir);
    if (tb->start_dir[1] == ':' || tb->start_dir[0] == '/') set_cwd(&tb->t, tb->start_dir);
    tab_spawn(tb);
    return tb;
}

/* Closing ConPTY can block until conhost drains, so tear down off the UI thread. */
static DWORD WINAPI tab_teardown(LPVOID arg) {
    Tab *tb = (Tab *)arg;
    if (tb->wait) UnregisterWaitEx(tb->wait, INVALID_HANDLE_VALUE);
    if (tb->hpc) pClosePC(tb->hpc);
    if (tb->reader) {
        if (WaitForSingleObject(tb->reader, 5000) == WAIT_TIMEOUT) {
            CancelSynchronousIo(tb->reader);
            WaitForSingleObject(tb->reader, 1000);
        }
        CloseHandle(tb->reader);
    }
    if (tb->proc) {
        if (WaitForSingleObject(tb->proc, 0) == WAIT_TIMEOUT) TerminateProcess(tb->proc, 1);
        CloseHandle(tb->proc);
    }
    if (tb->pin_w) CloseHandle(tb->pin_w);
    if (tb->pout_r) CloseHandle(tb->pout_r);
    DeleteCriticalSection(&tb->lock);
    free(tb->in);
    term_free(&tb->t);
    free(tb);
    return 0;
}

static void tab_destroy_async(Tab *tb) {
    HANDLE h = CreateThread(NULL, 0, tab_teardown, tb, 0, NULL);
    if (h) CloseHandle(h);
    else tab_teardown(tb);
}

/* ── config ────────────────────────────────────────────────────────── */

static void config_path(wchar_t *out) { _snwprintf(out, MAX_PATH, L"%ls\\config.ini", g_cfg_dir); out[MAX_PATH - 1] = 0; }

static void config_load(void) {
    wchar_t path[MAX_PATH], *la = _wgetenv(L"LOCALAPPDATA");
    char line[1200];
    FILE *f;
    _snwprintf(g_cfg_dir, MAX_PATH, L"%ls\\wsl-easy", la ? la : L".");
    g_cfg_dir[MAX_PATH - 1] = 0;
    CreateDirectoryW(g_cfg_dir, NULL);
    config_path(path);
    f = _wfopen(path, L"rb");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        size_t n;
        if (!v || line[0] == '#') continue;
        *v++ = 0;
        n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
        if (!strcmp(line, "scale")) g_scale_cfg = atoi(v);
        else if (!strcmp(line, "dir") && *v) snprintf(g_start_dir, sizeof(g_start_dir), "%s", v);
        else if (!strcmp(line, "distro")) snprintf(g_distro, sizeof(g_distro), "%s", v);
        else if (!strcmp(line, "width")) g_win_w = atoi(v);
        else if (!strcmp(line, "height")) g_win_h = atoi(v);
    }
    fclose(f);
}

static char g_cfg_dir_line[1100];

static void config_save(void) {
    wchar_t path[MAX_PATH];
    FILE *f;
    config_path(path);
    f = _wfopen(path, L"wb");
    if (!f) return;
    fprintf(f, "# wsl-easy settings. scale=0 follows the monitor DPI.\n");
    fprintf(f, "scale=%d\n", g_scale_cfg);
    fprintf(f, "# starting folder for new windows (Windows or Linux path); empty = %%USERPROFILE%%\n");
    fprintf(f, "dir=%s\n", g_cfg_dir_line);
    fprintf(f, "# WSL distro name; empty = your default distro\n");
    fprintf(f, "distro=%s\n", g_distro);
    fprintf(f, "width=%d\nheight=%d\n", g_win_w, g_win_h);
    fclose(f);
}

/* ── clipboard ─────────────────────────────────────────────────────── */

static void set_clipboard_utf8(const char *s, int n) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(wn + 1) * sizeof(wchar_t));
    wchar_t *w;
    if (!h) return;
    w = (wchar_t *)GlobalLock(h);
    MultiByteToWideChar(CP_UTF8, 0, s, n, w, wn);
    w[wn] = 0;
    GlobalUnlock(h);
    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, h);
        CloseClipboard();
    } else GlobalFree(h);
}

static int utf8_encode(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}

/* ── window, input, rendering ─────────────────────────────────────── */

static HDC g_hdc;
static HGLRC g_glrc;
static int g_focused = 1;
static int g_mouse_x = -1, g_mouse_y = -1;
static DWORD g_last_paint;

static Tab *active_tab(void) { return g_ntabs ? g_tabs[g_active] : NULL; }

#define CW (FONT_W * g_scale)
#define CH (FONT_H * g_scale)
#define PAD (4 * g_scale)
#define SCROLLBAR_W (6 * g_scale)
static int tabbar_h(void) { return CH + 10 * g_scale; }
static int term_ox(void) { return PAD; }
static int term_oy(void) { return tabbar_h() + PAD; }

static void compute_grid(int w, int h, int *cols, int *rows) {
    *cols = (w - 2 * PAD - SCROLLBAR_W) / CW;
    *rows = (h - tabbar_h() - 2 * PAD) / CH;
    if (*cols < 10) *cols = 10;
    if (*rows < 3) *rows = 3;
    if (*cols > 1000) *cols = 1000;
    if (*rows > 500) *rows = 500;
}

/* selection, in absolute line numbers so it survives scrolling */
static int g_sel_on, g_sel_drag;
static long long g_sel_a_row, g_sel_b_row;
static int g_sel_a_col, g_sel_b_col;
static int g_sb_drag;

static void resize_all(void) {
    RECT r;
    int cols, rows, i;
    COORD sz;
    if (!g_hwnd) return;
    GetClientRect(g_hwnd, &r);
    if (r.right < 1 || r.bottom < 1) return;
    compute_grid(r.right, r.bottom, &cols, &rows);
    if (cols == g_cols && rows == g_rows) return;
    g_cols = cols;
    g_rows = rows;
    g_sel_on = 0;
    sz.X = (SHORT)cols;
    sz.Y = (SHORT)rows;
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        tab_drain(tb);
        term_resize(&tb->t, cols, rows);
        if (tb->hpc) pResizePC(tb->hpc, sz);
    }
}

static void sel_norm(long long *r0, int *c0, long long *r1, int *c1) {
    if (g_sel_a_row < g_sel_b_row || (g_sel_a_row == g_sel_b_row && g_sel_a_col <= g_sel_b_col)) {
        *r0 = g_sel_a_row; *c0 = g_sel_a_col; *r1 = g_sel_b_row; *c1 = g_sel_b_col;
    } else {
        *r0 = g_sel_b_row; *c0 = g_sel_b_col; *r1 = g_sel_a_row; *c1 = g_sel_a_col;
    }
}

static int sel_contains(long long row, int col) {
    long long r0, r1;
    int c0, c1;
    if (!g_sel_on) return 0;
    sel_norm(&r0, &c0, &r1, &c1);
    if (row < r0 || row > r1) return 0;
    if (row == r0 && col < c0) return 0;
    if (row == r1 && col > c1) return 0;
    return 1;
}

static void sel_copy(void) {
    Tab *tb = active_tab();
    long long r0, r1, r;
    int c0, c1;
    char *out;
    size_t n = 0, cap = 4096;
    if (!tb || !g_sel_on) return;
    sel_norm(&r0, &c0, &r1, &c1);
    out = (char *)malloc(cap);
    for (r = r0; r <= r1; r++) {
        const Cell *cells;
        int len, wrapped, x, x0 = (r == r0) ? c0 : 0, x1 = (r == r1) ? c1 : 1 << 30;
        size_t line_start = n;
        long long L = r - term_base(&tb->t);
        term_line(&tb->t, (int)L, &cells, &len, &wrapped);
        if (L < 0) continue;
        for (x = x0; x <= x1 && x < len; x++) {
            if (cells[x].wflag == W_CONT) continue;
            if (n + 8 >= cap) out = (char *)realloc(out, cap *= 2);
            n += utf8_encode(cells[x].ch ? cells[x].ch : ' ', out + n);
        }
        if (!(wrapped && r < r1) || x1 < len - 1) {
            while (n > line_start && out[n - 1] == ' ') n--;
            if (r < r1) {
                if (n + 4 >= cap) out = (char *)realloc(out, cap *= 2);
                out[n++] = '\r';
                out[n++] = '\n';
            }
        }
    }
    set_clipboard_utf8(out, (int)n);
    free(out);
}

static void send_to_active(const char *s, int n) {
    Tab *tb = active_tab();
    if (!tb) return;
    if (tb->t.view) {
        tb->t.view = 0;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
    tab_write(tb, s, n);
}

static void paste(void) {
    Tab *tb = active_tab();
    HANDLE h;
    if (!tb || !OpenClipboard(g_hwnd)) return;
    h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
            char *s = (char *)malloc(n + 16), *o = s;
            const char *p;
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
            /* CRLF/LF → CR, like a typed Enter; strip ESC so a paste can't
               end bracketed-paste mode early */
            for (p = s; *p; p++) {
                if (*p == '\r' && p[1] == '\n') continue;
                if (*p == 0x1B) continue;
                *o++ = (*p == '\n') ? '\r' : *p;
            }
            if (tb->t.m_bracketed) send_to_active("\x1b[200~", 6);
            send_to_active(s, (int)(o - s));
            if (tb->t.m_bracketed) send_to_active("\x1b[201~", 6);
            free(s);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

/* ── glyph atlas: on-demand 16×16 slots in one texture ─────────────── */

#define ATLAS 2048
#define SLOTS_PER_ROW (ATLAS / 16)
#define MAX_SLOTS (SLOTS_PER_ROW * SLOTS_PER_ROW)
#define HASH_SIZE 65536
static GLuint g_tex;
static uint32_t g_hkey[HASH_SIZE];   /* cp + 1, 0 = empty */
static uint16_t g_hslot[HASH_SIZE];
static uint8_t g_hwidth[HASH_SIZE];
static int g_nslots;
static int g_in_quads;

static void glyph_cache_reset(void) {
    memset(g_hkey, 0, sizeof(g_hkey));
    g_nslots = 0;
}

static int glyph_slot(uint32_t cp, int *gw) {
    uint32_t h = (cp * 2654435761u) & (HASH_SIZE - 1);
    const unsigned char *g;
    unsigned char bmp[16 * 16];
    int slot, y, x, w;
    while (g_hkey[h]) {
        if (g_hkey[h] == cp + 1) { *gw = g_hwidth[h]; return g_hslot[h]; }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    if (g_nslots >= MAX_SLOTS || g_nslots >= HASH_SIZE / 2) {
        glyph_cache_reset();
        return glyph_slot(cp, gw);
    }
    g = font_glyph(cp);
    if (!g) g = font_glyph(0xFFFD);
    memset(bmp, 0, sizeof(bmp));
    w = g ? g[0] : 8;
    if (g) {
        for (y = 0; y < 16; y++) {
            unsigned bits = (w == 8) ? (unsigned)g[1 + y] << 8 : ((unsigned)g[1 + y * 2] << 8) | g[2 + y * 2];
            for (x = 0; x < 16; x++)
                if (bits & (0x8000u >> x)) bmp[y * 16 + x] = 255;
        }
    }
    slot = g_nslots++;
    if (g_in_quads) glEnd();
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, (slot % SLOTS_PER_ROW) * 16, (slot / SLOTS_PER_ROW) * 16, 16, 16,
                    GL_ALPHA, GL_UNSIGNED_BYTE, bmp);
    if (g_in_quads) glBegin(GL_QUADS);
    g_hkey[h] = cp + 1;
    g_hslot[h] = (uint16_t)slot;
    g_hwidth[h] = (uint8_t)w;
    *gw = w;
    return slot;
}

static void gl_color(uint32_t rgb, float a) {
    glColor4ub((GLubyte)(rgb >> 16), (GLubyte)(rgb >> 8), (GLubyte)rgb, (GLubyte)(a * 255));
}

static void quad(float x0, float y0, float x1, float y1) {
    glVertex2f(x0, y0); glVertex2f(x1, y0); glVertex2f(x1, y1); glVertex2f(x0, y1);
}

static void fill_rect(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
    glDisable(GL_TEXTURE_2D);
    gl_color(rgb, a);
    glBegin(GL_QUADS);
    quad(x0, y0, x1, y1);
    glEnd();
}

/* Draws one glyph into a w_cells-wide box at (x, y). A 16px-wide Unifont glyph
   in a 1-cell slot is shrunk to fit, the way dumbterm did it. */
static void draw_glyph(uint32_t cp, float x, float y, int w_cells, int scale) {
    int gw, slot = glyph_slot(cp, &gw);
    float cw = (float)(FONT_W * scale), chh = (float)(FONT_H * scale);
    float u0 = (float)((slot % SLOTS_PER_ROW) * 16) / ATLAS, v0 = (float)((slot / SLOTS_PER_ROW) * 16) / ATLAS;
    float u1 = u0 + (float)gw / ATLAS, v1 = v0 + 16.0f / ATLAS;
    float x0 = x, y0 = y, x1, y1 = y + chh;
    if (gw == 16 && w_cells == 1) {
        float pad = (chh - cw) * 0.5f;
        y0 = y + pad;
        y1 = y0 + cw;
        x1 = x + cw;
    } else if (gw == 8 && w_cells == 2) {
        x0 = x + cw * 0.5f;
        x1 = x0 + cw;
    } else {
        x1 = x + cw * w_cells;
    }
    glTexCoord2f(u0, v0); glVertex2f(x0, y0);
    glTexCoord2f(u1, v0); glVertex2f(x1, y0);
    glTexCoord2f(u1, v1); glVertex2f(x1, y1);
    glTexCoord2f(u0, v1); glVertex2f(x0, y1);
}

static uint32_t resolve(uint32_t spec, int is_fg, int bold) {
    if (!spec) return is_fg ? (bold ? 0xF2F2F2 : DEF_FG) : DEF_BG;
    if (spec & 0x01000000u) {
        int i = spec & 255;
        if (is_fg && bold && i < 8) i += 8;
        return PAL[i];
    }
    return spec & 0xFFFFFF;
}

static void cell_colors(const Cell *c, uint32_t *fg, uint32_t *bg) {
    uint32_t f = resolve(c->fg, 1, c->attr & A_BOLD), b = resolve(c->bg, 0, 0);
    if (c->attr & A_INV) { uint32_t s = f; f = b; b = s; }
    if (c->attr & A_DIM) f = ((f >> 1) & 0x7F7F7F) + ((b >> 1) & 0x7F7F7F);
    if (c->attr & A_HIDDEN) f = b;
    *fg = f;
    *bg = b;
}

/* draws a UTF-16 label clipped to max_w pixels; returns width used */
static int draw_label(const wchar_t *s, float x, float y, float max_w, uint32_t rgb) {
    int n = 0;
    float cx = x;
    gl_color(rgb, 1);
    for (; *s; s++) {
        uint32_t cp = *s;
        int w;
        if (cp >= 0xD800 && cp <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[1] - 0xDC00);
            s++;
        }
        w = cp_width(cp);
        if (w == 0) continue;
        if (cx + w * CW > x + max_w) {
            if (n) draw_glyph(0x2026, cx - CW, y, 1, g_scale);
            break;
        }
        draw_glyph(cp, cx, y, w, g_scale);
        cx += w * CW;
        n++;
    }
    return (int)(cx - x);
}

/* tab label: a program's own title wins; the shell's "user@host: path"
   title loses to the folder name, which is what you actually want to see */
static void tab_label(Tab *tb, wchar_t *out, int cap) {
    const wchar_t *title = tb->t.title;
    int shellish = !title[0] || (wcschr(title, L'@') && wcschr(title, L':')) ||
                   (wcslen(title) > 4 && !_wcsicmp(title + wcslen(title) - 4, L".exe"));
    if (!shellish) {
        _snwprintf(out, cap, L"%ls", title);
    } else if (tb->t.cwd[0]) {
        const char *b = strrchr(tb->t.cwd, '/');
        b = (b && b[1]) ? b + 1 : tb->t.cwd;
        MultiByteToWideChar(CP_UTF8, 0, b, -1, out, cap);
    } else {
        _snwprintf(out, cap, L"wsl");
    }
    out[cap - 1] = 0;
}

/* tab bar geometry */
enum { HIT_NONE, HIT_TAB, HIT_CLOSE, HIT_PLUS, HIT_BAR };
static int tab_width(int win_w) {
    int avail = win_w - 8 * g_scale - tabbar_h() - 8 * g_scale;
    int w = g_ntabs ? avail / g_ntabs : avail;
    int maxw = 26 * CW, minw = 6 * CW;
    if (w > maxw) w = maxw;
    if (w < minw) w = minw;
    return w;
}

static int hit_tabbar(int mx, int my, int *idx) {
    RECT r;
    int tw, x0 = 6 * g_scale, i, bh = tabbar_h();
    GetClientRect(g_hwnd, &r);
    if (my < 0 || my >= bh) return HIT_NONE;
    tw = tab_width(r.right);
    for (i = 0; i < g_ntabs; i++) {
        int tx = x0 + i * tw;
        if (mx >= tx && mx < tx + tw - 2 * g_scale) {
            *idx = i;
            if (mx >= tx + tw - 2 * g_scale - CW - 6 * g_scale) return HIT_CLOSE;
            return HIT_TAB;
        }
    }
    {
        int px = x0 + g_ntabs * tw + 2 * g_scale;
        if (mx >= px && mx < px + bh - 6 * g_scale) return HIT_PLUS;
    }
    return HIT_BAR;
}

static void render_tabbar(int win_w) {
    int i, idx = -1, hover = HIT_NONE, tw = tab_width(win_w), x0 = 6 * g_scale, bh = tabbar_h();
    float ty = (float)(bh - CH) / 2 + g_scale;
    if (g_mouse_y >= 0) hover = hit_tabbar(g_mouse_x, g_mouse_y, &idx);
    fill_rect(0, 0, (float)win_w, (float)bh, BAR_BG, 1);
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        wchar_t label[256];
        float tx = (float)(x0 + i * tw), top = 4.0f * g_scale;
        int act = (i == g_active), hov = (idx == i && (hover == HIT_TAB || hover == HIT_CLOSE));
        uint32_t bg = act ? DEF_BG : hov ? TAB_HOVER : TAB_BG;
        fill_rect(tx, top, tx + tw - 2 * g_scale, (float)bh, bg, 1);
        if (act) fill_rect(tx, top, tx + tw - 2 * g_scale, top + 2 * g_scale, AMBER, 1);
        tab_label(tb, label, 256);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_tex);
        glBegin(GL_QUADS);
        g_in_quads = 1;
        draw_label(label, tx + 8 * g_scale, ty, (float)(tw - 2 * g_scale - 16 * g_scale - CW), act ? 0xF2F2F2 : 0x9A9A9A);
        if (act || hov) {
            float cx = tx + tw - 2 * g_scale - CW - 6 * g_scale;
            gl_color((hov && hover == HIT_CLOSE) ? 0xFF6B5E : 0x8A8A8A, 1);
            draw_glyph(0x00D7, cx, ty, 1, g_scale);
        }
        glEnd();
        g_in_quads = 0;
        if (tb->activity && !act) fill_rect(tx + 3 * g_scale, ty + CH / 2 - g_scale, tx + 5 * g_scale, ty + CH / 2 + g_scale, AMBER, 1);
        if (tb->exited) fill_rect(tx + 8 * g_scale, ty + CH / 2, tx + tw - 10 * g_scale - CW, ty + CH / 2 + g_scale, 0x8A8A8A, 1);
    }
    {
        float px = (float)(x0 + g_ntabs * tw + 2 * g_scale), sz = (float)(bh - 6 * g_scale), top = 4.0f * g_scale;
        int hov = hover == HIT_PLUS;
        fill_rect(px, top, px + sz, top + sz, hov ? TAB_HOVER : BAR_BG, 1);
        /* a plain plus, drawn rather than taken from the font */
        {
            float cx = px + sz / 2, cy = top + sz / 2, arm = sz * 0.28f, th = (float)g_scale;
            uint32_t col = hov ? 0xF2F2F2 : 0xB0B0B0;
            fill_rect(cx - arm, cy - th, cx + arm, cy + th, col, 1);
            fill_rect(cx - th, cy - arm, cx + th, cy + arm, col, 1);
        }
    }
}

static void render(void) {
    RECT r;
    Tab *tb = active_tab();
    Term *t;
    int y, x, ox = term_ox(), oy = term_oy();
    long long base;
    GetClientRect(g_hwnd, &r);
    glViewport(0, 0, r.right, r.bottom);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, r.right, r.bottom, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(((DEF_BG >> 16) & 255) / 255.0f, ((DEF_BG >> 8) & 255) / 255.0f, (DEF_BG & 255) / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    render_tabbar(r.right);
    if (!tb) return;
    t = &tb->t;
    base = term_base(t);

    /* backgrounds + selection */
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    for (y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n, wrapped, L = t->hcount - t->view + y;
        term_line(t, L, &cells, &n, &wrapped);
        for (x = 0; x < t->cols; x++) {
            uint32_t fg, bg;
            int sel = sel_contains(base + L, x);
            if (x < n) cell_colors(&cells[x], &fg, &bg);
            else bg = DEF_BG;
            if (sel) bg = SEL_BG;
            if (bg == DEF_BG) continue;
            gl_color(bg, 1);
            quad((float)(ox + x * CW), (float)(oy + y * CH), (float)(ox + (x + 1) * CW), (float)(oy + (y + 1) * CH));
        }
    }
    glEnd();

    /* glyphs */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glBegin(GL_QUADS);
    g_in_quads = 1;
    for (y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n, wrapped, L = t->hcount - t->view + y;
        term_line(t, L, &cells, &n, &wrapped);
        for (x = 0; x < n && x < t->cols; x++) {
            const Cell *c = &cells[x];
            uint32_t fg, bg;
            int w;
            if (c->ch <= ' ' || c->wflag == W_CONT) continue;
            cell_colors(c, &fg, &bg);
            if (fg == bg && !(c->attr & A_HIDDEN)) continue;
            w = (c->wflag == W_WIDE) ? 2 : 1;
            gl_color(fg, 1);
            draw_glyph(c->ch, (float)(ox + x * CW), (float)(oy + y * CH), w, g_scale);
            /* Unifont has no bold; smear one pixel right, except box drawing */
            if ((c->attr & A_BOLD) && !(c->ch >= 0x2500 && c->ch <= 0x259F))
                draw_glyph(c->ch, (float)(ox + x * CW + g_scale), (float)(oy + y * CH), w, g_scale);
        }
    }
    glEnd();
    g_in_quads = 0;

    /* underline / strike */
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    for (y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n, wrapped, L = t->hcount - t->view + y;
        term_line(t, L, &cells, &n, &wrapped);
        for (x = 0; x < n && x < t->cols; x++) {
            const Cell *c = &cells[x];
            uint32_t fg, bg;
            float x0 = (float)(ox + x * CW), y0 = (float)(oy + y * CH);
            if (!(c->attr & (A_UNDER | A_STRIKE))) continue;
            cell_colors(c, &fg, &bg);
            gl_color(fg, 1);
            if (c->attr & A_UNDER) quad(x0, y0 + CH - g_scale, x0 + CW, y0 + CH);
            if (c->attr & A_STRIKE) quad(x0, y0 + CH / 2, x0 + CW, y0 + CH / 2 + g_scale);
        }
    }
    glEnd();

    /* cursor: dumbterm amber */
    if (t->view == 0 && t->m_cursor_vis) {
        float cx = (float)(ox + t->cx * CW), cy = (float)(oy + t->cy * CH);
        int w = (t->cx < t->cols && CELL(t, t->cx, t->cy)->wflag == W_WIDE) ? 2 : 1;
        float cw = (float)(CW * w);
        if (!g_focused) {
            float th = (float)g_scale;
            fill_rect(cx, cy, cx + cw, cy + th, AMBER, 0.9f);
            fill_rect(cx, cy + CH - th, cx + cw, cy + CH, AMBER, 0.9f);
            fill_rect(cx, cy, cx + th, cy + CH, AMBER, 0.9f);
            fill_rect(cx + cw - th, cy, cx + cw, cy + CH, AMBER, 0.9f);
        } else if (t->cursor_shape == 1) {
            fill_rect(cx, cy + CH - 2 * g_scale, cx + cw, cy + CH, AMBER, 1);
        } else if (t->cursor_shape == 2) {
            fill_rect(cx, cy, cx + 2 * g_scale, cy + CH, AMBER, 1);
        } else {
            const Cell *c = CELL(t, t->cx, t->cy);
            fill_rect(cx, cy, cx + cw, cy + CH, AMBER, 0.9f);
            if (c->ch > ' ' && c->wflag != W_CONT) {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, g_tex);
                glBegin(GL_QUADS);
                g_in_quads = 1;
                gl_color(DEF_BG, 1);
                draw_glyph(c->ch, cx, cy, w, g_scale);
                glEnd();
                g_in_quads = 0;
            }
        }
    }

    /* scrollbar, dumbterm-style thin amber thumb */
    if (t->hcount > 0 && !t->alt) {
        float track_top = (float)oy, track_h = (float)(t->rows * CH);
        float total = (float)(t->hcount + t->rows);
        float bar_h = track_h * t->rows / total, pos, bx = (float)(r.right - SCROLLBAR_W + g_scale);
        if (bar_h < 12 * g_scale) bar_h = (float)(12 * g_scale);
        pos = 1.0f - (float)t->view / t->hcount;
        fill_rect(bx, track_top, bx + SCROLLBAR_W - 2 * g_scale, track_top + track_h, 0x303030, 0.5f);
        fill_rect(bx, track_top + pos * (track_h - bar_h), bx + SCROLLBAR_W - 2 * g_scale,
                  track_top + pos * (track_h - bar_h) + bar_h, AMBER, t->view ? 0.8f : 0.3f);
    }
}

static void update_title(void) {
    Tab *tb = active_tab();
    wchar_t label[256], title[300];
    if (!tb) return;
    tab_label(tb, label, 256);
    _snwprintf(title, 300, L"%ls — wsl-easy", label);
    title[299] = 0;
    SetWindowTextW(g_hwnd, title);
}

static void paint_now(void) {
    Tab *tb = active_tab();
    if (tb && tb->t.m_sync && GetTickCount() - tb->t.sync_since < 200) {
        SetTimer(g_hwnd, 1, 20, NULL);
        return;
    }
    render();
    SwapBuffers(g_hdc);
    g_last_paint = GetTickCount();
}

/* ── tab management ───────────────────────────────────────────────── */

static void activate(int i) {
    if (i < 0 || i >= g_ntabs) return;
    g_active = i;
    g_tabs[i]->activity = 0;
    g_sel_on = 0;
    update_title();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void new_tab_here(void) {
    Tab *cur = active_tab(), *tb;
    int i;
    const char *dir = (cur && cur->t.cwd[0]) ? cur->t.cwd : cur ? cur->start_dir : g_start_dir;
    if (g_ntabs >= MAX_TABS) return;
    tb = tab_create(dir);
    if (!tb) return;
    /* insert right after the current tab, like a browser */
    for (i = g_ntabs; i > g_active + 1; i--) g_tabs[i] = g_tabs[i - 1];
    g_tabs[g_ntabs ? g_active + 1 : 0] = tb;
    g_ntabs++;
    activate(g_ntabs == 1 ? 0 : g_active + 1);
}

static void close_tab(int i) {
    Tab *tb;
    if (i < 0 || i >= g_ntabs) return;
    tb = g_tabs[i];
    memmove(&g_tabs[i], &g_tabs[i + 1], sizeof(Tab *) * (g_ntabs - i - 1));
    g_ntabs--;
    tab_destroy_async(tb);
    if (!g_ntabs) {
        DestroyWindow(g_hwnd);
        return;
    }
    if (g_active > i || g_active >= g_ntabs) g_active--;
    if (g_active < 0) g_active = 0;
    activate(g_active);
}

/* ── keyboard ──────────────────────────────────────────────────────── */

static int mods_param(void) {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1;
    if (GetKeyState(VK_MENU) & 0x8000) m |= 2;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= 4;
    return m ? m + 1 : 0;
}

static void scroll_view(int lines) {
    Tab *tb = active_tab();
    if (!tb || tb->t.alt) return;
    tb->t.view += lines;
    if (tb->t.view > tb->t.hcount) tb->t.view = tb->t.hcount;
    if (tb->t.view < 0) tb->t.view = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void set_scale(int s) {
    if (s < 1) s = 1;
    if (s > 6) s = 6;
    if (s == g_scale) return;
    g_scale = s;
    resize_all();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static int auto_scale(void) { return g_dpi >= 144 ? (g_dpi + 48) / 96 : 1; }

/* Returns 1 if the key was consumed (its WM_CHAR is then dropped). */
static int handle_keydown(WPARAM vk) {
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    int m = mods_param();
    Tab *tb = active_tab();
    char buf[32];
    const char *seq = NULL;
    int n = 0;
    if (!tb) return 0;

    /* wsl-easy's own shortcuts; everything else goes to the shell */
    if (ctrl && shift && !alt) {
        switch (vk) {
        case 'T': new_tab_here(); return 1;
        case 'W': close_tab(g_active); return 1;
        case 'C': sel_copy(); g_sel_on = 0; InvalidateRect(g_hwnd, NULL, FALSE); return 1;
        case 'V': paste(); return 1;
        case VK_TAB: activate((g_active + g_ntabs - 1) % g_ntabs); return 1;
        }
    }
    if (ctrl && !shift && !alt && vk == VK_TAB) { activate((g_active + 1) % g_ntabs); return 1; }
    if (ctrl && alt && vk >= '1' && vk <= '9') { activate(vk == '9' ? g_ntabs - 1 : (int)(vk - '1')); return 1; }
    if (ctrl && !alt && !shift && (vk == VK_OEM_PLUS || vk == VK_ADD)) { g_scale_cfg = g_scale + 1; set_scale(g_scale_cfg); return 1; }
    if (ctrl && !alt && !shift && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) { g_scale_cfg = g_scale > 1 ? g_scale - 1 : 1; set_scale(g_scale_cfg); return 1; }
    if (ctrl && !alt && !shift && vk == '0') { g_scale_cfg = 0; set_scale(auto_scale()); return 1; }
    if (shift && !ctrl && !alt) {
        switch (vk) {
        case VK_PRIOR: scroll_view(tb->t.rows - 1); return 1;
        case VK_NEXT: scroll_view(-(tb->t.rows - 1)); return 1;
        case VK_INSERT: paste(); return 1;
        }
    }
    if (ctrl && !shift && vk == VK_INSERT) { sel_copy(); return 1; }

    switch (vk) {
    case VK_UP: case VK_DOWN: case VK_RIGHT: case VK_LEFT: case VK_HOME: case VK_END: {
        char f = vk == VK_UP ? 'A' : vk == VK_DOWN ? 'B' : vk == VK_RIGHT ? 'C' : vk == VK_LEFT ? 'D' : vk == VK_HOME ? 'H' : 'F';
        if (m) n = snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, f);
        else n = snprintf(buf, sizeof(buf), tb->t.m_appcursor ? "\x1bO%c" : "\x1b[%c", f);
        break;
    }
    case VK_INSERT: case VK_DELETE: case VK_PRIOR: case VK_NEXT: {
        int code = vk == VK_INSERT ? 2 : vk == VK_DELETE ? 3 : vk == VK_PRIOR ? 5 : 6;
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", code, m) : snprintf(buf, sizeof(buf), "\x1b[%d~", code);
        break;
    }
    case VK_F1: case VK_F2: case VK_F3: case VK_F4: {
        char f = (char)('P' + (vk - VK_F1));
        n = m ? snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, f) : snprintf(buf, sizeof(buf), "\x1bO%c", f);
        break;
    }
    case VK_F5: case VK_F6: case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12: {
        static const int codes[] = {15, 17, 18, 19, 20, 21, 23, 24};
        int code = codes[vk - VK_F5];
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", code, m) : snprintf(buf, sizeof(buf), "\x1b[%d~", code);
        break;
    }
    case VK_RETURN:
        /* Shift+Enter = ESC CR, which Claude Code reads as "newline, don't submit" */
        seq = (shift || alt) ? "\x1b\r" : "\r";
        break;
    case VK_BACK:
        seq = ctrl ? "\x17" : alt ? "\x1b\x7f" : "\x7f";
        break;
    case VK_TAB:
        seq = shift ? "\x1b[Z" : "\t";
        break;
    case VK_ESCAPE:
        seq = "\x1b";
        break;
    case VK_SPACE:
        if (ctrl) { buf[0] = 0; n = 1; }
        break;
    case VK_OEM_2:               /* Ctrl+/ → ^_ (undo in readline, emacs) */
        if (ctrl && !alt) seq = "\x1f";
        break;
    case '2':
        if (ctrl && !alt) { buf[0] = 0; n = 1; }
        break;
    case '6':
        if (ctrl && !alt) seq = "\x1e";
        break;
    }
    if (seq) n = (int)strlen(seq), memcpy(buf, seq, n);
    if (n) {
        g_sel_on = 0;
        send_to_active(buf, n);
        return 1;
    }
    return 0;
}

static wchar_t g_high_surrogate;

static void handle_char(WPARAM wc, int alt) {
    char buf[8];
    int n = 0;
    uint32_t cp = (uint32_t)wc;
    if (cp >= 0xD800 && cp <= 0xDBFF) { g_high_surrogate = (wchar_t)cp; return; }
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        if (!g_high_surrogate) return;
        cp = 0x10000 + (((uint32_t)g_high_surrogate - 0xD800) << 10) + (cp - 0xDC00);
        g_high_surrogate = 0;
    }
    if (alt) buf[n++] = 0x1B;
    n += utf8_encode(cp, buf + n);
    g_sel_on = 0;
    send_to_active(buf, n);
}

/* ── mouse ─────────────────────────────────────────────────────────── */

static void mouse_cell(int mx, int my, int *col, int *row) {
    Tab *tb = active_tab();
    int c = (mx - term_ox()) / CW, r = (my - term_oy()) / CH;
    if (mx < term_ox()) c = 0;
    if (my < term_oy()) r = 0;
    if (tb) {
        if (c >= tb->t.cols) c = tb->t.cols - 1;
        if (r >= tb->t.rows) r = tb->t.rows - 1;
    }
    *col = c;
    *row = r;
}

/* reports to an app that asked for the mouse (vim, htop, tmux...) */
static int mouse_report(int button, int press, int motion, int mx, int my, WPARAM wp) {
    Tab *tb = active_tab();
    char buf[64];
    int col, row, b = button, n;
    if (!tb || !tb->t.m_mouse || (wp & MK_SHIFT)) return 0;
    if (motion && tb->t.m_mouse != 1003 && !(tb->t.m_mouse == 1002 && button != 3)) return 1;
    if (motion && tb->t.m_mouse == 9) return 1;
    mouse_cell(mx, my, &col, &row);
    if (wp & MK_CONTROL) b |= 16;
    if (GetKeyState(VK_MENU) & 0x8000) b |= 8;
    if (motion) b |= 32;
    if (tb->t.m_mouse_sgr) {
        n = snprintf(buf, sizeof(buf), "\x1b[<%d;%d;%d%c", b, col + 1, row + 1, press ? 'M' : 'm');
    } else {
        if (!press) b = (b & ~3) | 3;
        if (col > 222) col = 222;
        if (row > 222) row = 222;
        n = snprintf(buf, sizeof(buf), "\x1b[M%c%c%c", 32 + b, 33 + col, 33 + row);
    }
    tab_write(tb, buf, n);
    return 1;
}

static int is_word_char(uint32_t ch) {
    return ch > ' ' && !strchr("\"'`()[]{}<>|;,", (int)(ch < 128 ? ch : 'a'));
}

static void select_word(int col, int row) {
    Tab *tb = active_tab();
    const Cell *cells;
    int n, wrapped, L, a, b;
    if (!tb) return;
    L = tb->t.hcount - tb->t.view + row;
    term_line(&tb->t, L, &cells, &n, &wrapped);
    if (!cells || col >= n || !is_word_char(cells[col].ch)) return;
    for (a = col; a > 0 && (is_word_char(cells[a - 1].ch) || cells[a - 1].wflag == W_CONT); a--) {}
    for (b = col; b + 1 < n && (is_word_char(cells[b + 1].ch) || cells[b + 1].wflag == W_CONT); b++) {}
    g_sel_on = 1;
    g_sel_a_row = g_sel_b_row = term_base(&tb->t) + L;
    g_sel_a_col = a;
    g_sel_b_col = b;
}

static void scrollbar_set(int my) {
    Tab *tb = active_tab();
    float track_h, f;
    if (!tb || !tb->t.hcount) return;
    track_h = (float)(tb->t.rows * CH);
    f = (float)(my - term_oy()) / track_h;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    tb->t.view = (int)((1.0f - f) * tb->t.hcount);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ── window procedure ─────────────────────────────────────────────── */

static void gl_init(void) {
    PIXELFORMATDESCRIPTOR pfd;
    int pf;
    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pf = ChoosePixelFormat(g_hdc, &pfd);
    SetPixelFormat(g_hdc, pf, &pfd);
    g_glrc = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, g_glrc);
    {   /* vsync off: we only draw when something changed */
        typedef BOOL (WINAPI *PFNSWAP)(int);
        PFNSWAP swap = (PFNSWAP)(void *)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap) swap(0);
    }
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, ATLAS, ATLAS, 0, GL_ALPHA, GL_UNSIGNED_BYTE, NULL);
    glyph_cache_reset();
}

static void save_window_size(void) {
    RECT r;
    if (!IsIconic(g_hwnd) && !IsZoomed(g_hwnd) && GetWindowRect(g_hwnd, &r)) {
        g_win_w = r.right - r.left;
        g_win_h = r.bottom - r.top;
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_hdc = GetDC(hwnd);
        gl_init();
        return 0;
    case WM_CLOSE:
        save_window_size();
        config_save();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        /* No per-tab teardown: when this process exits its pseudoconsoles
           close, and conhost takes the shells down with it. Closing stays instant. */
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint_now();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        KillTimer(hwnd, wp);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            resize_all();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_EXITSIZEMOVE:
        save_window_size();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = 300;
        mm->ptMinTrackSize.y = 160;
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *nr = (RECT *)lp;
        g_dpi = HIWORD(wp);
        if (!g_scale_cfg) g_scale = auto_scale();
        SetWindowPos(hwnd, NULL, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        resize_all();
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        Tab *tb = active_tab();
        g_focused = (msg == WM_SETFOCUS);
        if (tb && tb->t.m_focus) tab_write(tb, g_focused ? "\x1b[I" : "\x1b[O", 3);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_APP_OUTPUT: {
        int i, changed = 0;
        for (i = 0; i < g_ntabs; i++) {
            Tab *tb = g_tabs[i];
            if (tab_drain(tb) <= 0) continue;
            if (i == g_active) changed = 1;
            else tb->activity = 1;
            if (tb->t.bell) {
                tb->t.bell = 0;
                if (!g_focused || i != g_active) {
                    FLASHWINFO fi = {sizeof(fi), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
                    if (!g_focused) FlashWindowEx(&fi);
                }
            }
            changed = 1;
        }
        if (changed) {
            update_title();
            InvalidateRect(hwnd, NULL, FALSE);
            /* under a flood of output, still show a frame every ~30ms */
            if (GetTickCount() - g_last_paint > 30) UpdateWindow(hwnd);
        }
        return 0;
    }
    case WM_APP_EXITED: {
        Tab *tb = tab_by_id((int)wp);
        int i;
        if (!tb) return 0;
        tab_drain(tb);
        for (i = 0; i < g_ntabs; i++)
            if (g_tabs[i] == tb) close_tab(i);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && (wp == VK_F4 || wp == VK_SPACE) && !(GetKeyState(VK_CONTROL) & 0x8000)) break;
        if (handle_keydown(wp)) {
            MSG m;
            while (PeekMessageW(&m, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE)) {}
            while (PeekMessageW(&m, hwnd, WM_SYSCHAR, WM_SYSCHAR, PM_REMOVE)) {}
            return 0;
        }
        if (msg == WM_SYSKEYDOWN) return 0;   /* keep Alt+letter from opening the menu */
        break;
    case WM_CHAR:
        handle_char(wp, 0);
        return 0;
    case WM_SYSCHAR:
        handle_char(wp, 1);
        return 0;
    case WM_SYSKEYUP:
        if (wp == VK_MENU || wp == VK_F10) return 0;
        break;
    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp), col, row;
        int was_bar = g_mouse_y >= 0 && g_mouse_y < tabbar_h(), on_bar = my < tabbar_h();
        Tab *tb = active_tab();
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        g_mouse_x = mx;
        g_mouse_y = my;
        if (was_bar || on_bar) InvalidateRect(hwnd, NULL, FALSE);
        if (g_sb_drag) { scrollbar_set(my); return 0; }
        if (g_sel_drag && tb) {
            mouse_cell(mx, my, &col, &row);
            if (my < term_oy()) scroll_view(1);
            else if (my >= term_oy() + tb->t.rows * CH) scroll_view(-1);
            g_sel_b_row = term_base(&tb->t) + tb->t.hcount - tb->t.view + row;
            g_sel_b_col = col;
            g_sel_on = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (!on_bar && (wp & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)))
            mouse_report((wp & MK_LBUTTON) ? 0 : (wp & MK_MBUTTON) ? 1 : 2, 1, 1, mx, my, wp);
        else if (!on_bar && tb && tb->t.m_mouse == 1003)
            mouse_report(3, 1, 1, mx, my, wp);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mouse_x = g_mouse_y = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp), idx = -1, col, row, hit;
        RECT r;
        Tab *tb = active_tab();
        hit = hit_tabbar(mx, my, &idx);
        if (hit == HIT_TAB) { activate(idx); return 0; }
        if (hit == HIT_CLOSE) { close_tab(idx); return 0; }
        if (hit == HIT_PLUS) { new_tab_here(); return 0; }
        if (hit == HIT_BAR) {
            if (msg == WM_LBUTTONDBLCLK) new_tab_here();
            return 0;
        }
        GetClientRect(hwnd, &r);
        if (tb && mx >= r.right - SCROLLBAR_W - g_scale && tb->t.hcount && !tb->t.alt) {
            g_sb_drag = 1;
            SetCapture(hwnd);
            scrollbar_set(my);
            return 0;
        }
        if (mouse_report(0, 1, 0, mx, my, wp)) return 0;
        if (!tb) return 0;
        mouse_cell(mx, my, &col, &row);
        if (msg == WM_LBUTTONDBLCLK) {
            select_word(col, row);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        g_sel_on = 0;
        g_sel_drag = 1;
        g_sel_a_row = g_sel_b_row = term_base(&tb->t) + tb->t.hcount - tb->t.view + row;
        g_sel_a_col = g_sel_b_col = col;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sb_drag || g_sel_drag) {
            g_sb_drag = g_sel_drag = 0;
            ReleaseCapture();
            return 0;
        }
        mouse_report(0, 0, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
        return 0;
    case WM_MBUTTONDOWN: {
        int idx = -1;
        if (hit_tabbar(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx) == HIT_TAB ||
            hit_tabbar(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx) == HIT_CLOSE) {
            close_tab(idx);
            return 0;
        }
        mouse_report(1, 1, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
        return 0;
    }
    case WM_MBUTTONUP:
        mouse_report(1, 0, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
        return 0;
    case WM_RBUTTONDOWN:
        if (GET_Y_LPARAM(lp) < tabbar_h()) return 0;
        if (mouse_report(2, 1, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp)) return 0;
        /* conhost habit: right-click copies a selection, or pastes */
        if (g_sel_on) {
            sel_copy();
            g_sel_on = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        } else paste();
        return 0;
    case WM_RBUTTONUP:
        mouse_report(2, 0, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
        return 0;
    case WM_MOUSEWHEEL: {
        short delta = (short)HIWORD(wp);
        int notches = delta / 120, i;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Tab *tb = active_tab();
        if (!notches) notches = delta > 0 ? 1 : -1;
        ScreenToClient(hwnd, &pt);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            g_scale_cfg = g_scale + (notches > 0 ? 1 : -1);
            if (g_scale_cfg < 1) g_scale_cfg = 1;
            set_scale(g_scale_cfg);
            return 0;
        }
        if (!tb) return 0;
        if (tb->t.m_mouse && !(GET_KEYSTATE_WPARAM(wp) & MK_SHIFT)) {
            for (i = 0; i < abs(notches); i++)
                mouse_report(notches > 0 ? 64 : 65, 1, 0, pt.x, pt.y, GET_KEYSTATE_WPARAM(wp));
        } else if (tb->t.alt) {
            /* alternate scroll: less, man, git log scroll with the wheel */
            const char *seq = notches > 0 ? (tb->t.m_appcursor ? "\x1bOA" : "\x1b[A") : (tb->t.m_appcursor ? "\x1bOB" : "\x1b[B");
            for (i = 0; i < abs(notches) * 3; i++) tab_write(tb, seq, 3);
        } else {
            scroll_view(notches * 3);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ── startup ───────────────────────────────────────────────────────── */

static void load_conpty(void) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    pCreatePC = (PFN_CreatePC)(void *)GetProcAddress(k, "CreatePseudoConsole");
    pResizePC = (PFN_ResizePC)(void *)GetProcAddress(k, "ResizePseudoConsole");
    pClosePC = (PFN_ClosePC)(void *)GetProcAddress(k, "ClosePseudoConsole");
}

static void dpi_awareness(void) {
    typedef BOOL (WINAPI *PFN)(HANDLE);
    PFN f = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (f) f((HANDLE)(intptr_t)-4);   /* per-monitor v2 */
    else SetProcessDPIAware();
}

static int window_dpi(HWND h) {
    typedef UINT (WINAPI *PFN)(HWND);
    PFN f = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (f && h) return (int)f(h);
    {
        HDC dc = GetDC(NULL);
        int d = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
        return d;
    }
}

/* Dark title bar in the tab bar's color, so the window reads as one piece. */
static void dark_titlebar(HWND h) {
    typedef HRESULT (WINAPI *PFN)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    PFN f = dwm ? (PFN)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
    BOOL on = TRUE;
    COLORREF cap = RGB((BAR_BG >> 16) & 255, (BAR_BG >> 8) & 255, BAR_BG & 255);
    COLORREF txt = RGB(0xCC, 0xCC, 0xCC);
    if (!f) return;
    f(h, 20, &on, sizeof(on));       /* DWMWA_USE_IMMERSIVE_DARK_MODE */
    f(h, 35, &cap, sizeof(cap));     /* DWMWA_CAPTION_COLOR (Windows 11) */
    f(h, 36, &txt, sizeof(txt));     /* DWMWA_TEXT_COLOR */
}

static void default_start_dir(void) {
    wchar_t *up = _wgetenv(L"USERPROFILE");
    if (g_start_dir[0]) return;
    if (up) WideCharToMultiByte(CP_UTF8, 0, up, -1, g_start_dir, sizeof(g_start_dir), NULL, NULL);
    else snprintf(g_start_dir, sizeof(g_start_dir), "~");
}

/* Headless check of the whole child path: ConPTY → wsl → bash → rc → parser. */
static int selftest(const char *out_path) {
    Tab *tb;
    FILE *f;
    int y, phase = 0;
    DWORD start = GetTickCount(), phase_t = 0;
    char line[4096];
    g_cols = 100;
    g_rows = 30;
    tb = tab_create(NULL);
    if (!tb) return 2;
    while (GetTickCount() - start < 20000) {
        Sleep(50);
        tab_drain(tb);
        if (phase == 0 && strstr(tb->t.cwd, "/") && GetTickCount() - start > 1500) {
            const char *cmd = "cd /tmp && printf '\\033[1;32mSELFTEST\\033[0m %s\\n' \"$PWD\" && tput cols\r";
            tab_write(tb, cmd, (int)strlen(cmd));
            phase = 1;
            phase_t = GetTickCount();
        } else if (phase == 1 && !strcmp(tb->t.cwd, "/tmp") && GetTickCount() - phase_t > 500) {
            phase = 2;
            break;
        }
    }
    f = fopen(out_path, "wb");
    if (!f) return 3;
    fprintf(f, "phase=%d cwd=%s cols=%d rows=%d hist=%d\n", phase, tb->t.cwd, tb->t.cols, tb->t.rows, tb->t.hcount);
    WideCharToMultiByte(CP_UTF8, 0, tb->t.title, -1, line, sizeof(line), NULL, NULL);
    fprintf(f, "title=%s\n----\n", line);
    for (y = 0; y < tb->t.rows; y++) {
        int x, n = 0;
        for (x = 0; x < tb->t.cols; x++) {
            Cell *c = &tb->t.buf[tb->t.alt][(size_t)y * tb->t.cols + x];
            if (c->wflag == W_CONT) continue;
            n += utf8_encode(c->ch ? c->ch : ' ', line + n);
        }
        while (n && line[n - 1] == ' ') n--;
        line[n] = 0;
        fprintf(f, "%s\n", line);
    }
    fclose(f);
    tab_teardown(tb);
    return phase == 2 ? 0 : 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    int argc, i, cols, rows;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WNDCLASSW wc;
    RECT r;
    MSG msg;
    char selftest_out[MAX_PATH] = "";
    HWND h;
    (void)prev; (void)cmdline;

    dpi_awareness();
    palette_init();
    load_conpty();
    config_load();
    snprintf(g_cfg_dir_line, sizeof(g_cfg_dir_line), "%s", g_start_dir);
    for (i = 1; i < argc; i++) {
        char a[1024];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, a, sizeof(a), NULL, NULL);
        if ((!strcmp(a, "--cd") || !strcmp(a, "-d")) && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_start_dir, sizeof(g_start_dir), NULL, NULL);
        } else if (!strcmp(a, "--distro") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_distro, sizeof(g_distro), NULL, NULL);
        } else if (!strcmp(a, "--selftest") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, selftest_out, sizeof(selftest_out), NULL, NULL);
        } else if (a[0] != '-') {
            snprintf(g_start_dir, sizeof(g_start_dir), "%s", a);
        }
    }
    default_start_dir();
    write_rc();
    if (selftest_out[0]) return selftest(selftest_out);

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"wsl-easy";
    RegisterClassW(&wc);

    g_dpi = window_dpi(NULL);
    g_scale = g_scale_cfg ? g_scale_cfg : auto_scale();
    if (g_win_w < 300 || g_win_h < 160) {
        r.left = r.top = 0;
        r.right = 2 * PAD + SCROLLBAR_W + 120 * CW;
        r.bottom = tabbar_h() + 2 * PAD + 34 * CH;
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        g_win_w = r.right - r.left;
        g_win_h = r.bottom - r.top;
    }
    h = CreateWindowExW(0, L"wsl-easy", L"wsl-easy", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        g_win_w, g_win_h, NULL, NULL, inst, NULL);
    if (!h) return 1;
    g_dpi = window_dpi(h);
    if (!g_scale_cfg) g_scale = auto_scale();
    dark_titlebar(h);
    GetClientRect(h, &r);
    compute_grid(r.right, r.bottom, &cols, &rows);
    g_cols = cols;
    g_rows = rows;
    new_tab_here();
    ShowWindow(h, show);
    UpdateWindow(h);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LocalFree(argv);
    return 0;
}
