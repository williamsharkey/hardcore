/*
 * hardcore.c — a tabbed terminal for WSL on Windows 11.
 *
 * Grown out of dumbterm (github.com/williamsharkey/dumbterm): same GNU Unifont
 * bitmap glyphs, same immediate-mode drawing, same amber cursor. What is
 * different:
 *
 *   - ConPTY instead of anonymous pipes + a JS shim, so the child is a real
 *     WSL shell with a real pty (isTTY, SIGWINCH, job control, all of it).
 *   - A much fuller VT parser: alt screen, scroll regions, insert/delete,
 *     DEC line drawing, mouse reporting, bracketed paste, sync output.
 *   - Tabs. The "+" button (and Ctrl+Shift+T) opens a new WSL shell in the
 *     directory the current tab is sitting in. The shell reports $PWD on every
 *     prompt through OSC 9;9 from a small rc file hardcore writes for itself.
 *   - New windows start in %USERPROFILE% (/mnt/c/Users/<you>), not ~.
 *
 * Build (from WSL or MSYS2):
 *   make            → hardcore.exe
 *
 * Usage:
 *   hardcore.exe [--cd DIR] [--distro NAME] [DIR]
 *   hardcore.exe --selftest OUT.txt     # headless ConPTY + parser check
 */

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
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

/* Windows Terminal's own ConPTY (vendor/conpty), extracted on first run. */
__asm__(
    ".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global conpty_dll\n"
    "conpty_dll:\n"
    ".incbin \"vendor/conpty/conpty.dll\"\n"
    ".global conpty_dll_end\n"
    "conpty_dll_end:\n"
    ".balign 16\n"
    ".global openconsole_exe\n"
    "openconsole_exe:\n"
    ".incbin \"vendor/conpty/OpenConsole.exe\"\n"
    ".global openconsole_exe_end\n"
    "openconsole_exe_end:\n"
    ".byte 0\n"
    ".text\n");
extern const unsigned char conpty_dll[], conpty_dll_end[], openconsole_exe[], openconsole_exe_end[];

#define FONT_W 8
#define FONT_H 16

/* Returns the glyph record (width byte + rows) or NULL. */
static const unsigned char *font_glyph(uint32_t cp) {
    uint32_t page, off;
    if (cp > 0x10FFFF) return NULL;
    memcpy(&page, font_blob + 4 + (cp >> 8) * 4, 4);
    if (!page) return NULL;
    memcpy(&off, font_blob + page + (cp & 255) * 4, 4);
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

#define HARDCORE_VERSION "0.3"
#define AMBER 0xFFA60D           /* dumbterm's phosphor cursor */
#define DEF_FG 0xCCCCCC
#define DEF_BG 0x0C0C0C
#define BAR_BG 0x1B1B1B
#define TAB_BG 0x262626
#define TAB_HOVER 0x303030
#define SEL_BG 0x264F78

static int g_scale = 1;          /* integer pixel scale, from the monitor DPI; one size, no zoom */
static int g_dpi = 96;
static char g_distro[128];
static char g_start_dir[1024];   /* Windows or Linux path */
static wchar_t g_cfg_dir[MAX_PATH];
static char g_rc_linux[1024];    /* rc.bash as seen from inside WSL */
static int g_win_w = 0, g_win_h = 0;
static int g_inbox_conpty;       /* --inbox-conpty: use Windows' own conhost */
static const char *g_conpty_kind = "inbox";

/* ── cells ─────────────────────────────────────────────────────────── */

/* color spec: 0 = default, 0x01000000|idx = palette, 0x02000000|rgb */
#define C_PAL(i) (0x01000000u | (uint32_t)(i))
#define C_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

enum { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_BLINK = 16, A_INV = 32, A_HIDDEN = 64, A_STRIKE = 128 };
enum { W_WIDE = 1, W_CONT = 2 };

typedef struct {
    uint32_t ch, fg, bg;
    uint8_t attr, wflag;
    uint16_t link;               /* OSC 8 hyperlink id, 0 = none */
} Cell;

#define MAX_LINKS 4096           /* per tab, reused round-robin */

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

/* wcwidth from Unicode 17 (tools/gen_width.py → build/width.inc): the same
classic rule ConPTY measures with, so our grid agrees with the console host */
#include "build/width.inc"

static int in_ranges(uint32_t cp, const uint32_t (*r)[2], int n) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < r[mid][0]) hi = mid - 1;
        else if (cp > r[mid][1]) lo = mid + 1;
        else return 1;
    }
    return 0;
}

static int cp_width(uint32_t cp) {
    if (cp < 0x300) return 1;
    if (in_ranges(cp, W0, (int)(sizeof(W0) / sizeof(W0[0])))) return 0;
    if (in_ranges(cp, W2, (int)(sizeof(W2) / sizeof(W2[0])))) return 2;
    return 1;
}

static const uint16_t DEC_GFX[31] = { /* ESC ( 0, chars 0x60..0x7e */
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518,
    0x2510, 0x250C, 0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524,
    0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7};

/* ── terminal state ────────────────────────────────────────────────── */

enum { S_GROUND, S_ESC, S_ESC_INTER, S_CSI, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC };

typedef struct {
    int cx, cy, wrapnext, g0dec, origin;
    Cell pen;
} SavedCursor;

typedef struct {
    int cols, rows;
    Cell **row[2];               /* per-screen row pointers: scrolling moves pointers, not cells */
    uint8_t *wrapf[2];           /* row ended in an autowrap (for copy and reflow) */
    int alt;
    int cx, cy, wrapnext;
    Cell pen;
    SavedCursor saved[2];
    int top, bot;
    int g0dec;
    uint32_t last_ch;            /* for REP; only valid right after a printed char */
    uint8_t *tabs;               /* per-column tab stop flags */
    char *links[MAX_LINKS];      /* OSC 8 targets; cell.link = index + 1 */
    int next_link;
    /* modes */
    int m_newline;
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
    int bell;                    /* BEL, or a desktop-notification OSC (9 / 777) */
    uint32_t spin_ch;            /* first char of the title, when it looks like a spinner */
    DWORD spin_at;               /* when it last changed */
    int spin_n;                  /* changes in a row, each within SPIN_HOLD ms of the last */
    int progress;                /* OSC 9;4 progress state: 1 normal, 2 error, 3 indeterminate, 4 paused */
    /* OSC 7501 program status (Claude Code): the main session's state */
    int ps_state;                /* PS_NONE, PS_IDLE, ... */
    char ps_kind[24];            /* for PS_BLOCKED: permission, question, auth, ... */
    wchar_t ps_msg[200];         /* what it's doing, or what it needs from you */
    int ps_pct;                  /* -1 = unknown */
    int ps_tasks;                /* background agents/tasks currently working */
    char ps_task_id[16][33];
    uint8_t ps_task_on[16];
    /* OSC 133 ; A marks: absolute line numbers of prompts / turn starts, oldest first */
    long long marks[512];
    int nmarks;
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
    int activity;                /* output while in the background */
    int busy;                    /* term_busy(), as of the last check: drives the throbber */
    int attention;               /* bell, notification, or a busy program went quiet: the bell mark */
    int ps_seen;                 /* t.ps_state as of the last update_busy() */
    int spare;                   /* pre-warmed, waiting on stdin for its directory */
    char start_dir[1024];
} Tab;

static HWND g_hwnd;

/* HARDCORE_TRACE=1 writes startup phase times to %TEMP%\\hardcore-trace.txt */
static FILE *g_trace;
static LARGE_INTEGER g_t0, g_qpf;
static void trace(const char *what) {
    LARGE_INTEGER t;
    if (!g_trace) return;
    QueryPerformanceCounter(&t);
    fprintf(g_trace, "%8.2f ms  %s\n", (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart, what);
    fflush(g_trace);
}

/* HARDCORE_LAT=1: per keypress, how long until the shell's echo arrived and
   until the frame showing it was handed to the compositor
   (%TEMP%\hardcore-lat.txt: key→output ms, key→render start, key→SwapBuffers returned) */
static FILE *g_lat;
static LARGE_INTEGER g_lat_key, g_lat_out, g_lat_render;
static double lat_ms(LARGE_INTEGER a, LARGE_INTEGER b) { return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_qpf.QuadPart; }

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

#define CELL(t, x, y) (&(t)->row[(t)->alt][y][x])

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

/* Rotates rows [top, bot] by n: positive moves content up (rows leaving at
   the top come back blank at the bottom), negative moves it down. */
static void rotate_rows(Term *t, int top, int bot, int n) {
    Cell *stack[64], **tmp;
    uint8_t wstack[64], *wtmp;
    Cell **R = t->row[t->alt];
    uint8_t *W = t->wrapf[t->alt];
    int h = bot - top + 1, k = n > 0 ? n : -n;
    tmp = k <= 64 ? stack : (Cell **)malloc(sizeof(Cell *) * k);
    wtmp = k <= 64 ? wstack : (uint8_t *)malloc((size_t)k);
    if (n > 0) {
        memcpy(tmp, R + top, sizeof(Cell *) * k);
        memmove(R + top, R + top + k, sizeof(Cell *) * (h - k));
        memcpy(R + bot - k + 1, tmp, sizeof(Cell *) * k);
        memmove(W + top, W + top + k, (size_t)(h - k));
    } else {
        memcpy(tmp, R + bot - k + 1, sizeof(Cell *) * k);
        memmove(R + top + k, R + top, sizeof(Cell *) * (h - k));
        memcpy(R + top, tmp, sizeof(Cell *) * k);
        memmove(W + top + k, W + top, (size_t)(h - k));
    }
    if (tmp != stack) free(tmp);
    if (wtmp != wstack) free(wtmp);
}

static void scroll_up(Term *t, int top, int bot, int n, int to_hist) {
    int y, h = bot - top + 1;
    if (n <= 0) return;
    if (n > h) n = h;
    if (to_hist && !t->alt && top == 0)
        for (y = 0; y < n; y++) hist_push(t, CELL(t, 0, y), t->wrapf[0][y]);
    rotate_rows(t, top, bot, n);
    clear_rows(t, bot - n + 1, bot + 1);
}

static void scroll_down(Term *t, int top, int bot, int n) {
    int h = bot - top + 1;
    if (n <= 0) return;
    if (n > h) n = h;
    rotate_rows(t, top, bot, -n);
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
    t->m_origin = t->m_insert = t->m_appcursor = t->m_bracketed = t->m_newline = 0;
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
        int y;
        t->row[s] = (Cell **)malloc(sizeof(Cell *) * rows);
        t->wrapf[s] = (uint8_t *)calloc((size_t)rows, 1);
        if (!t->row[s]) return 0;
        for (y = 0; y < rows; y++)
            if (!(t->row[s][y] = (Cell *)malloc(sizeof(Cell) * cols))) return 0;
    }
    t->tabs = (uint8_t *)malloc((size_t)cols);
    if (!t->hist || !t->tabs) return 0;
    for (s = 0; s < cols; s++) t->tabs[s] = (s % 8 == 0);
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
    {
        int s, y;
        for (s = 0; s < 2; s++) {
            for (y = 0; y < t->rows; y++) free(t->row[s][y]);
            free(t->row[s]);
        }
    }
    free(t->wrapf[0]);
    free(t->wrapf[1]);
    free(t->tabs);
    {
        int i;
        for (i = 0; i < MAX_LINKS; i++) free(t->links[i]);
    }
}

/* growable list of physical lines, used while reflowing */
typedef struct { HLine *v; int n, cap; } LineList;

static void ll_add(LineList *l, const Cell *c, int n, int wrapped) {
    HLine *h;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 256;
        l->v = (HLine *)realloc(l->v, sizeof(HLine) * l->cap);
    }
    h = &l->v[l->n++];
    h->n = n;
    h->wrapped = wrapped;
    h->c = n ? (Cell *)malloc(sizeof(Cell) * n) : NULL;
    if (n) memcpy(h->c, c, sizeof(Cell) * n);
}

/* Rewraps the main screen and its scrollback to a new width, the way Windows
   Terminal does: wrapped rows are joined into logical lines and split again.
   The cursor keeps its place in the text. */
static void reflow_main(Term *t, int cols, int rows) {
    int oc = t->cols, orows = t->rows, y, L, i;
    int ccx = t->alt ? t->saved[0].cx : t->cx, ccy = t->alt ? t->saved[0].cy : t->cy;
    int last = ccy, cur_line = -1, cur_off = 0, cur_prow = 0, start, keep;
    Cell *acc = NULL;
    int acc_n = 0, acc_cap = 0;
    LineList out = {0};
    Cell blank;
    memset(&blank, 0, sizeof(blank));
    blank.ch = ' ';
    /* last screen row worth keeping: the cursor row or the last with content */
    for (y = orows - 1; y > last; y--) {
        int x;
        for (x = 0; x < oc && row_is_blank_default(&t->row[0][y][x]); x++) {}
        if (x < oc) { last = y; break; }
    }
    for (L = 0; L < t->hcount + last + 1; L++) {
        const Cell *c;
        int n, wrapped;
        if (L < t->hcount) {
            const HLine *h = &t->hist[(t->hstart + L) % HIST_LINES];
            c = h->c; n = h->n; wrapped = h->wrapped;
        } else {
            y = L - t->hcount;
            c = t->row[0][y]; n = oc; wrapped = t->wrapf[0][y];
            if (y == ccy) { cur_line = 1; cur_off = acc_n + ccx; }
        }
        if (acc_n + oc > acc_cap) {
            acc_cap = (acc_n + oc) * 2;
            acc = (Cell *)realloc(acc, sizeof(Cell) * acc_cap);
        }
        memcpy(acc + acc_n, c, sizeof(Cell) * n);
        acc_n += n;
        if (wrapped) {           /* a wrapped row's trailing blanks are real text */
            for (; n < oc; n++) acc[acc_n++] = blank;
            continue;
        }
        /* end of a logical line: trim and split it at the new width */
        {
            int len = acc_n, pos = 0;
            while (len > 0 && row_is_blank_default(&acc[len - 1])) len--;
            if (cur_line == 1 && len < cur_off) len = len;   /* cursor may sit past the text */
            do {
                int take = len - pos > cols ? cols : len - pos;
                if (take == cols && acc[pos + take - 1].wflag == W_WIDE) take--;  /* don't split a wide char */
                if (cur_line == 1 && cur_off >= pos && (cur_off < pos + cols || pos + take >= len)) {
                    cur_prow = out.n + (cur_off - pos) / cols;
                    ccx = (cur_off - pos) % cols;
                    cur_line = 2;
                }
                ll_add(&out, acc + pos, take, pos + take < len);
                pos += take;
            } while (pos < len);
            if (cur_line == 2 && cur_prow >= out.n) {  /* cursor beyond the text, on rows of its own */
                while (out.n <= cur_prow) ll_add(&out, NULL, 0, 0);
            }
            if (cur_line == 2) cur_line = 3;
            acc_n = 0;
        }
    }
    free(acc);
    /* screen = the last `rows` lines, but never scroll the cursor off the top */
    start = out.n - rows;
    if (start > cur_prow) start = cur_prow;
    if (start < 0) start = 0;
    hist_clear(t);
    keep = start > HIST_LINES ? HIST_LINES : start;
    for (i = 0; i < start; i++) {
        if (i < start - keep) { free(out.v[i].c); continue; }
        t->hist[t->hcount++] = out.v[i];
    }
    t->hpushed = t->hcount;
    t->nmarks = 0;               /* line numbers changed under them; reflow doesn't track marks */
    for (y = 0; y < orows; y++) free(t->row[0][y]);
    free(t->row[0]);
    free(t->wrapf[0]);
    t->row[0] = (Cell **)malloc(sizeof(Cell *) * rows);
    t->wrapf[0] = (uint8_t *)calloc((size_t)rows, 1);
    for (y = 0; y < rows; y++) {
        int x, idx = start + y;
        t->row[0][y] = (Cell *)malloc(sizeof(Cell) * cols);
        for (x = 0; x < cols; x++) t->row[0][y][x] = blank;
        if (idx < out.n) {
            memcpy(t->row[0][y], out.v[idx].c, sizeof(Cell) * out.v[idx].n);
            t->wrapf[0][y] = (uint8_t)out.v[idx].wrapped;
            free(out.v[idx].c);
        }
    }
    free(out.v);
    ccy = cur_prow - start;
    if (ccy < 0) ccy = 0;
    if (ccy >= rows) ccy = rows - 1;
    if (ccx >= cols) ccx = cols - 1;
    if (t->alt) { t->saved[0].cx = ccx; t->saved[0].cy = ccy; }
    else { t->cx = ccx; t->cy = ccy; }
}

/* the alt screen (vim, less, htop) just gets cropped or padded; its owner redraws */
static void resize_alt(Term *t, int cols, int rows) {
    Cell **nr = (Cell **)malloc(sizeof(Cell *) * rows);
    uint8_t *nw = (uint8_t *)calloc((size_t)rows, 1);
    Cell blank;
    int y, x;
    memset(&blank, 0, sizeof(blank));
    blank.ch = ' ';
    for (y = 0; y < rows; y++) {
        nr[y] = (Cell *)malloc(sizeof(Cell) * cols);
        for (x = 0; x < cols; x++)
            nr[y][x] = (y < t->rows && x < t->cols) ? t->row[1][y][x] : blank;
        if (cols < t->cols && nr[y][cols - 1].wflag == W_WIDE) nr[y][cols - 1] = blank;
    }
    for (y = 0; y < t->rows; y++) free(t->row[1][y]);
    free(t->row[1]);
    free(t->wrapf[1]);
    t->row[1] = nr;
    t->wrapf[1] = nw;
    if (t->alt) {
        if (t->cy >= rows) t->cy = rows - 1;
        if (t->cx >= cols) t->cx = cols - 1;
    }
}

static void term_resize(Term *t, int cols, int rows) {
    int s, x;
    if (cols == t->cols && rows == t->rows) return;
    reflow_main(t, cols, rows);
    resize_alt(t, cols, rows);
    {
        uint8_t *nt = (uint8_t *)malloc((size_t)cols);
        for (x = 0; x < cols; x++) nt[x] = x < t->cols ? t->tabs[x] : (x % 8 == 0);
        free(t->tabs);
        t->tabs = nt;
    }
    t->cols = cols;
    t->rows = rows;
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
    t->wrapnext = 0;             /* xterm: DECRC never restores a pending wrap */
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

static void tab_forward(Term *t, int n) {
    while (n-- > 0) {
        do t->cx++; while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
        if (t->cx >= t->cols - 1) { t->cx = t->cols - 1; break; }
    }
}

static void tab_back(Term *t, int n) {
    while (n-- > 0 && t->cx > 0)
        do t->cx--; while (t->cx > 0 && !t->tabs[t->cx]);
}

/* SL/SR, DECIC/DECDC: shift columns [x0, cols) of the scroll region by n
   (positive = right, inserting blanks at x0) */
static void shift_columns(Term *t, int x0, int n) {
    int y, x;
    Cell b = blank_cell(t);
    for (y = t->top; y <= t->bot; y++) {
        Cell *row = CELL(t, 0, y);
        if (n > 0) {
            for (x = t->cols - 1; x >= x0; x--) row[x] = (x - n >= x0) ? row[x - n] : b;
        } else {
            for (x = x0; x < t->cols; x++) row[x] = (x - n < t->cols) ? row[x - n] : b;
        }
    }
}

static void decaln(Term *t) {
    int y, x;
    for (y = 0; y < t->rows; y++)
        for (x = 0; x < t->cols; x++) {
            Cell *c = CELL(t, x, y);
            memset(c, 0, sizeof(*c));
            c->ch = 'E';
        }
    t->top = 0;
    t->bot = t->rows - 1;
    t->cx = t->cy = 0;
}

static void control(Term *t, int b) {
    if (b != 0x1B) t->last_ch = 0;
    switch (b) {
    case 0x07: t->bell = 1; break;
    case 0x08:
        t->wrapnext = 0;
        if (t->cx > 0) t->cx--;
        break;
    case 0x09:
        tab_forward(t, 1);
        break;
    case 0x0A: case 0x0B: case 0x0C:
        if (t->m_newline) t->cx = 0;  /* LNM */
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

static int decrqm(const Term *t, int p) {
    int on;
    switch (p) {
    case 1: on = t->m_appcursor; break;
    case 6: on = t->m_origin; break;
    case 7: on = t->m_autowrap; break;
    case 25: on = t->m_cursor_vis; break;
    case 47: case 1047: case 1049: on = t->alt; break;
    case 9: case 1000: case 1002: case 1003: on = t->m_mouse == p; break;
    case 1004: on = t->m_focus; break;
    case 1006: on = t->m_mouse_sgr; break;
    case 2004: on = t->m_bracketed; break;
    case 2026: on = t->m_sync; break;
    /* known DEC modes we keep permanently off: 132 cols, smooth scroll,
       reverse video, autorepeat, blink, NRCS, keypad, backarrow, LR margins... */
    case 3: case 4: case 5: case 8: case 12: case 18: case 19: case 42: case 45:
    case 60: case 61: case 64: case 66: case 67: case 69: case 95:
        on = 0; break;
    default: return 0;           /* not recognized */
    }
    return on ? 1 : 2;
}

static void csi_dispatch(Term *t, int f) {
    int n, i;
    char buf[64];
    if (t->priv == '?') {
        if (t->inter == '$' && f == 'p') {          /* DECRQM: is private mode n set? */
            snprintf(buf, sizeof(buf), "\x1b[?%d;%d$y", P(t, 0, 0), decrqm(t, P(t, 0, 0)));
            term_reply(t, buf);
        } else if (f == 'h' || f == 'l')
            for (i = 0; i < t->np; i++) decset(t, t->params[i], f == 'h');
        else if (f == 'J' || f == 'K') {
            t->priv = 0;
            csi_dispatch(t, f);
        }
        return;
    }
    if (t->priv == '>') {
        if (f == 'c') term_reply(t, "\x1b[>0;10;1c");
        /* XTVERSION: programs (Claude Code among them) ask which terminal this is */
        else if (f == 'q' && P(t, 0, 0) == 0) term_reply(t, "\x1bP>|hardcore(" HARDCORE_VERSION ")\x1b\\");
        return;
    }
    if (t->priv) return;
    if (t->inter == ' ' && f == 'q') {
        n = P(t, 0, 1);
        t->cursor_shape = n <= 2 ? 0 : n <= 4 ? 1 : 2;
        return;
    }
    if (t->inter == '!' && f == 'p') {               /* DECSTR soft reset */
        term_reset(t);
        memset(&t->saved[t->alt], 0, sizeof(t->saved[0]));
        return;
    }
    if (t->inter == '*' && f == 'y') {               /* DECRQCRA: checksum of a rectangle */
        int top = P(t, 2, 1) - 1, left = P(t, 3, 1) - 1, bot = P(t, 4, t->rows) - 1, right = P(t, 5, t->cols) - 1, y, x;
        unsigned sum = 0;
        if (t->m_origin) { top += t->top; bot += t->top; }
        if (bot >= t->rows) bot = t->rows - 1;
        if (right >= t->cols) right = t->cols - 1;
        for (y = top; y <= bot; y++)
            for (x = left; x <= right; x++) {
                const Cell *c = CELL(t, x, y);
                sum += c->wflag == W_CONT ? 0 : c->ch;
            }
        snprintf(buf, sizeof(buf), "\x1bP%d!~%04X\x1b\\", P(t, 0, 0), (0x10000 - (sum & 0xFFFF)) & 0xFFFF);
        term_reply(t, buf);
        return;
    }
    if (t->inter == '$' && f == 'p') {               /* ANSI DECRQM: only IRM is tracked */
        int m = P(t, 0, 0);
        snprintf(buf, sizeof(buf), "\x1b[%d;%d$y", m, m == 4 ? (t->m_insert ? 1 : 2) : m == 20 ? (t->m_newline ? 1 : 2)
                 : (m >= 1 && m <= 19) ? 2 : 0);     /* the other ISO 6429 modes exist but are fixed off */
        term_reply(t, buf);
        return;
    }
    if (t->inter == ' ' && (f == '@' || f == 'A')) {     /* SL / SR, only inside the region */
        if (t->cy >= t->top && t->cy <= t->bot) shift_columns(t, 0, f == 'A' ? P(t, 0, 1) : -P(t, 0, 1));
        return;
    }
    if (t->inter == '\'' && (f == '}' || f == '~')) {   /* DECIC / DECDC */
        if (t->cy >= t->top && t->cy <= t->bot) shift_columns(t, t->cx, f == '}' ? P(t, 0, 1) : -P(t, 0, 1));
        return;
    }
    if (t->inter) return;

    /* xterm keeps a pending wrap across scrolls, SGR and mode changes */
    if (!strchr("STbmhlnctqpgI", f)) t->wrapnext = 0;
    if (f != 'b') t->last_ch = 0;
    switch (f) {
    case '@': insert_blanks(t, P(t, 0, 1)); break;
    case 'A': {               /* stops at the top margin if starting inside it */
        int lim = t->cy >= t->top ? t->top : 0;
        t->cy -= P(t, 0, 1);
        if (t->cy < lim) t->cy = lim;
        break;
    }
    case 'e': t->cy += P(t, 0, 1); if (t->cy >= t->rows) t->cy = t->rows - 1; break;
    case 'B': {
        int lim = t->cy <= t->bot ? t->bot : t->rows - 1;
        t->cy += P(t, 0, 1);
        if (t->cy > lim) t->cy = lim;
        break;
    }
    case 'C': case 'a': t->cx += P(t, 0, 1); if (t->cx >= t->cols) t->cx = t->cols - 1; break;
    case 'D': t->cx -= P(t, 0, 1); if (t->cx < 0) t->cx = 0; break;
    case 'E': {               /* CNL/CPL stop at the margins like CUD/CUU */
        int lim = t->cy <= t->bot ? t->bot : t->rows - 1;
        t->cy += P(t, 0, 1);
        if (t->cy > lim) t->cy = lim;
        t->cx = 0;
        break;
    }
    case 'F': {
        int lim = t->cy >= t->top ? t->top : 0;
        t->cy -= P(t, 0, 1);
        if (t->cy < lim) t->cy = lim;
        t->cx = 0;
        break;
    }
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
    case 'I': tab_forward(t, P(t, 0, 1)); break;
    case 'Z': tab_back(t, P(t, 0, 1)); break;
    case 'g':
        if (P(t, 0, 0) == 0) t->tabs[t->cx] = 0;
        else if (P(t, 0, 0) == 3) memset(t->tabs, 0, (size_t)t->cols);
        break;
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
        if (top >= bot) break;   /* xterm ignores an empty/inverted region outright */
        t->top = top;
        t->bot = bot;
        t->cx = 0;
        t->cy = t->m_origin ? t->top : 0;
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    case 'h': case 'l':
        for (i = 0; i < t->np; i++)
            if (t->params[i] == 4) t->m_insert = (f == 'h');
            else if (t->params[i] == 20) t->m_newline = (f == 'h');
        break;
    case 't':
        if (P(t, 0, 0) == 18) {
            snprintf(buf, sizeof(buf), "\x1b[8;%d;%dt", t->rows, t->cols);
            term_reply(t, buf);
        } else if (P(t, 0, 0) == 16) {   /* cell size in pixels (Claude Code asks, for images) */
            snprintf(buf, sizeof(buf), "\x1b[6;%d;%dt", FONT_H * g_scale, FONT_W * g_scale);
            term_reply(t, buf);
        } else if (P(t, 0, 0) == 14) {   /* text area in pixels */
            snprintf(buf, sizeof(buf), "\x1b[4;%d;%dt", t->rows * FONT_H * g_scale, t->cols * FONT_W * g_scale);
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

enum { PS_NONE, PS_IDLE, PS_WORKING, PS_BLOCKED, PS_DONE, PS_ERROR };

static int b64val(int c);

/* base64 → UTF-8 → wide, for OSC 7501 title= / msg= */
static void b64_to_wide(const char *in, size_t n, wchar_t *out, int cap) {
    char buf[1600];
    int bits = 0, k = 0, v;
    uint32_t acc = 0;
    size_t i;
    for (i = 0; i < n && k < (int)sizeof(buf) - 1; i++) {
        if ((v = b64val((unsigned char)in[i])) < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; buf[k++] = (char)(acc >> bits); }
    }
    buf[k] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, cap)) out[0] = 0;
    out[cap - 1] = 0;
    for (i = 0; out[i]; i++) if (out[i] < 32) out[i] = L' ';   /* one line */
}

/* OSC 7501 ; state=S:app=A[:id=I][:kind=K][:progress=N][:title=B64][:msg=B64]
   Claude Code sends this once the terminal has answered "OSC 7501 ; ?".
   No id = the session itself; an id = one of its background tasks. */
static void program_status(Term *t, char *s) {
    char state[16] = "", id[33] = "", kind[24] = "", *f, *next;
    const char *msg = NULL;
    size_t msg_n = 0;
    int pct = -1, ps, i, free_i = -1;
    for (f = s; f && *f; f = next) {
        char *eq = strchr(f, '=');
        size_t vn;
        next = strchr(f, ':');
        vn = next ? (size_t)(next - (eq ? eq + 1 : f)) : strlen(eq ? eq + 1 : f);
        if (next) next++;
        if (!eq || (next && eq > next)) continue;
        if (!strncmp(f, "state=", 6)) snprintf(state, sizeof(state), "%.*s", (int)vn, eq + 1);
        else if (!strncmp(f, "id=", 3)) snprintf(id, sizeof(id), "%.*s", (int)vn, eq + 1);
        else if (!strncmp(f, "kind=", 5)) snprintf(kind, sizeof(kind), "%.*s", (int)vn, eq + 1);
        else if (!strncmp(f, "progress=", 9)) pct = atoi(eq + 1);
        else if (!strncmp(f, "msg=", 4)) { msg = eq + 1; msg_n = vn; }
    }
    ps = !strcmp(state, "working") ? PS_WORKING : !strcmp(state, "blocked") ? PS_BLOCKED :
         !strcmp(state, "done") ? PS_DONE : !strcmp(state, "error") || !strcmp(state, "failed") ? PS_ERROR :
         !strcmp(state, "idle") ? PS_IDLE : PS_NONE;
    if (id[0]) {                 /* a background task */
        for (i = 0; i < 16; i++) {
            if (t->ps_task_on[i] && !strcmp(t->ps_task_id[i], id)) break;
            if (!t->ps_task_on[i] && free_i < 0) free_i = i;
        }
        if (i == 16) i = free_i;
        if (i >= 0) {
            if (ps == PS_WORKING || ps == PS_BLOCKED) {
                snprintf(t->ps_task_id[i], sizeof(t->ps_task_id[i]), "%s", id);
                t->ps_task_on[i] = 1;
            } else t->ps_task_on[i] = 0;
        }
        for (t->ps_tasks = 0, i = 0; i < 16; i++) t->ps_tasks += t->ps_task_on[i];
        t->dirty = 1;
        return;
    }
    if (!strcmp(state, "clear")) {   /* the program exited, or wiped its status */
        t->ps_state = PS_NONE;
        t->ps_msg[0] = 0;
        t->ps_tasks = 0;
        memset(t->ps_task_on, 0, sizeof(t->ps_task_on));
        t->dirty = 1;
        return;
    }
    t->ps_state = ps;
    t->ps_pct = pct;
    snprintf(t->ps_kind, sizeof(t->ps_kind), "%s", kind);
    if (msg) b64_to_wide(msg, msg_n, t->ps_msg, 200);
    else t->ps_msg[0] = 0;
    t->dirty = 1;
}

static long long term_abs_line(const Term *t, int y);

static void add_mark(Term *t) {
    long long at = term_abs_line(t, t->cy);
    if (t->nmarks && t->marks[t->nmarks - 1] >= at) {   /* redrawn in place: keep one */
        t->marks[t->nmarks - 1] = at;
        return;
    }
    if (t->nmarks == 512) { memmove(t->marks, t->marks + 1, sizeof(t->marks[0]) * 511); t->nmarks--; }
    t->marks[t->nmarks++] = at;
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Programs that animate their title (Claude Code: "◐ task", "◑ task", ...;
   others use braille or asterisks) count as busy while the first character
   keeps changing. One change alone is a static marker such as Claude's idle "✳". */
#define SPIN_HOLD 1500

static int spinnerish(uint32_t c) {
    return (c >= 0x2800 && c <= 0x28FF) || (c >= 0x25D0 && c <= 0x25D3) || (c >= 0x25F4 && c <= 0x25F7) ||
           (c >= 0x2596 && c <= 0x259F) || (c >= 0x2722 && c <= 0x274B) || (c >= 0x25B0 && c <= 0x25C5) ||
           c == 0x00B7 || c == 0x2219 || c == 0x2022 || c == 0x22C6 || c == 0x2605 || c == 0x2606;
}

static void title_spin(Term *t) {
    uint32_t c = t->title[0];
    DWORD now = GetTickCount();
    if (!(spinnerish(c) && t->title[1] == L' ')) {
        t->spin_ch = 0;
        t->spin_n = 0;
        return;
    }
    if (c == t->spin_ch) return;
    t->spin_n = (t->spin_ch && now - t->spin_at < SPIN_HOLD) ? t->spin_n + 1 : 1;
    t->spin_ch = c;
    t->spin_at = now;
}

static int term_busy(const Term *t) {
    if (t->ps_state != PS_NONE) return t->ps_state == PS_WORKING;   /* Claude said so: believe it */
    if (t->progress == 1 || t->progress == 3) return 1;
    return t->spin_n >= 2 && GetTickCount() - t->spin_at < SPIN_HOLD;
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
        title_spin(t);
        t->dirty = 1;
        break;
    case 99:                     /* OSC 99 ; metadata ; payload (kitty notifications) */
        if (strchr(rest, ';')) t->bell = 1;
        break;
    case 7501:                   /* program status, see program_status() */
        if (!strcmp(rest, "?")) term_reply(t, "\x1b]7501;?\x1b\\");
        else program_status(t, rest);
        break;
    case 133:                    /* semantic prompts: A = a prompt (or a Claude turn) starts here */
        if (rest[0] == 'A' && !t->alt) add_mark(t);
        break;
    case 777:                    /* OSC 777 ; notify ; title ; body (urxvt, Ghostty) */
        if (!strncmp(rest, "notify;", 7)) t->bell = 1;
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
        } else if (!strncmp(rest, "4;", 2)) {
            t->progress = atoi(rest + 2);   /* ConEmu/Windows Terminal progress: 9;4;state;percent */
        } else if (!(rest[0] >= '0' && rest[0] <= '9' && rest[1] == ';')) {
            t->bell = 1;         /* OSC 9 ; message: an iTerm2-style notification */
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
    case 8: {                    /* OSC 8 ; params ; URI — hyperlink start, empty URI ends it */
        char *uri = strchr(rest, ';');
        if (!uri || !uri[1]) {
            t->pen.link = 0;
        } else {
            int id = t->next_link;
            t->next_link = (t->next_link + 1) % MAX_LINKS;
            free(t->links[id]);
            t->links[id] = _strdup(uri + 1);
            t->pen.link = (uint16_t)(id + 1);
        }
        break;
    }
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
    if (b != '[') t->last_ch = 0;   /* a CSI decides for itself (REP needs it) */
    if (b >= 0x20 && b <= 0x2F) {   /* intermediate: ESC ( 0, ESC # 8, ESC SP F ... */
        t->st = S_ESC_INTER;
        t->esc_which = b;
        return;
    }
    switch (b) {
    case 'H': t->tabs[t->cx] = 1; break;
    case '[':
        t->st = S_CSI;
        t->np = 0;
        t->params[0] = 0;
        t->priv = t->inter = 0;
        break;
    case ']': t->st = S_OSC; t->osc_n = 0; break;
    case 'P': case 'X': case '^': case '_': t->st = S_STR; break;
    case '7': save_cursor(t); break;
    case '8': restore_cursor(t); break;
    case 'D': term_lf(t); t->wrapnext = 0; break;
    case 'E': term_lf(t); t->cx = 0; t->wrapnext = 0; break;
    case 'M': term_ri(t); t->wrapnext = 0; break;
    case 'c': {                  /* RIS: everything back to power-on */
        int x;
        set_alt(t, 0, 0);
        term_reset(t);
        clear_rows(t, 0, t->rows);
        t->cx = t->cy = 0;
        memset(t->saved, 0, sizeof(t->saved));
        for (x = 0; x < t->cols; x++) t->tabs[x] = (x % 8 == 0);
        t->title[0] = 0;
        t->spin_ch = 0;
        t->spin_n = 0;
        t->progress = 0;
        break;
    }
    case 'Z': term_reply(t, "\x1b[?62;22c"); break;   /* DECID = DA */
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
    case S_ESC_INTER:
        if (b < 0x20) { control(t, b); return; }
        if (b <= 0x2F) return;   /* more intermediates; the first one decides */
        if (t->esc_which == '(') t->g0dec = (b == '0');
        else if (t->esc_which == '#' && b == '8') decaln(t);
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
    for (i = 0; i < n; i++) {
        unsigned char b = s[i];
        /* fast path: printable ASCII in the middle of a row, nothing special going on */
        if (b >= 0x20 && b < 0x7F && t->st == S_GROUND && !t->utf8_need && !t->wrapnext &&
            t->cx < t->cols - 1 && !t->g0dec && !t->m_insert) {
            Cell *c = &t->row[t->alt][t->cy][t->cx];
            if (!c->wflag) {
                *c = t->pen;
                c->ch = b;
                t->cx++;
                t->last_ch = b;
                continue;
            }
        }
        vt_byte(t, b);
    }
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
        *cells = t->row[t->alt][y];
        *n = t->cols;
        *wrapped = t->wrapf[t->alt][y];
    }
}

static long long term_base(const Term *t) { return t->hpushed - t->hcount; }
static long long term_abs_line(const Term *t, int y) { return t->hpushed + y; }

/* ── tabs: ConPTY + reader thread ─────────────────────────────────── */

#define WM_APP_OUTPUT (WM_APP + 1)
#define WM_APP_EXITED (WM_APP + 2)

static Tab *g_tabs[MAX_TABS];
static Tab *g_spare;             /* pre-warmed shell for the next new tab */
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
/* HARDCORE_VTLOG=1: everything ConPTY hands us, raw, to %TEMP%\\hardcore-vt.log */
static FILE *g_vtlog;

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
    if (n && g_vtlog) { fwrite(data, 1, (size_t)n, g_vtlog); fflush(g_vtlog); }
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
   Written by hardcore on each launch; nothing in your dotfiles changes. */
static const char RC_BASH[] =
    "# written by hardcore on every launch; edits here are overwritten\n"
    "[ -r /etc/profile ] && . /etc/profile\n"
    "if [ -r ~/.bash_profile ]; then . ~/.bash_profile\n"
    "elif [ -r ~/.bash_login ]; then . ~/.bash_login\n"
    "elif [ -r ~/.profile ]; then . ~/.profile\n"
    "elif [ -r ~/.bashrc ]; then . ~/.bashrc\n"
    "fi\n"
    "# tell programs what this terminal can do (WSL doesn't carry WT_SESSION-style hints):\n"
    "# 24-bit color (chalk/supports-color, so Claude Code's theme isn't cut to 256 colors)\n"
    "# and OSC 8 hyperlinks (supports-hyperlinks)\n"
    "export COLORTERM=truecolor TERM_PROGRAM=hardcore FORCE_HYPERLINK=1\n"
    "# a pre-warmed spare tab: profile already loaded, wait (silently) to be told where to go\n"
    "if [ -n \"$HARDCORE_SPARE\" ]; then\n"
    "  unset HARDCORE_SPARE\n"
    "  IFS= read -rs __hardcore_dir && cd -- \"$__hardcore_dir\" 2>/dev/null\n"
    "  unset __hardcore_dir\n"
    "fi\n"
    "__hardcore_cwd() { printf '\\033]9;9;%s\\033\\\\\\033]133;A\\033\\\\' \"$PWD\"; }\n"
    "# report the folder on every cd too, so \"cd x && claude\" labels the tab x right away\n"
    "if ! declare -F cd >/dev/null; then\n"
    "  cd() { builtin cd \"$@\" && __hardcore_cwd; }\n"
    "  pushd() { builtin pushd \"$@\" && __hardcore_cwd; }\n"
    "  popd() { builtin popd \"$@\" && __hardcore_cwd; }\n"
    "fi\n"
    "case \";${PROMPT_COMMAND[0]};\" in *\";__hardcore_cwd;\"*) ;; *)\n"
    "  PROMPT_COMMAND=\"__hardcore_cwd${PROMPT_COMMAND:+;$PROMPT_COMMAND}\" ;;\n"
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
    BOOL ok;

    if (!pCreatePC) {
        term_message(&tb->t, "\x1b[31mhardcore: this Windows has no ConPTY (needs Windows 10 1809+)\x1b[0m\r\n");
        return 0;
    }
    /* a roomy output pipe: with the 4 KB default, ConPTY stalls on every few KB */
    if (!CreatePipe(&in_r, &in_w, NULL, 0) || !CreatePipe(&out_r, &out_w, NULL, 1 << 20)) return 0;
    sz.X = (SHORT)tb->t.cols;
    sz.Y = (SHORT)tb->t.rows;
    /* OpenConsole can measure glyphs with wcswidth, which is what our grid uses */
    hr = pCreatePC(sz, in_r, out_w, g_conpty_kind[0] == 'O' ? 0x10 /* GLYPH_WIDTH_WCSWIDTH */ : 0, &tb->hpc);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (FAILED(hr)) {
        CloseHandle(in_w);
        CloseHandle(out_r);
        term_message(&tb->t, "\x1b[31mhardcore: CreatePseudoConsole failed\x1b[0m\r\n");
        return 0;
    }
    tb->pin_w = in_w;
    tb->pout_r = out_r;

    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    /* Without this a child of a console-attached hardcore inherits our std
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
    /* tabs are only spawned from the UI thread, so a temporary env var is safe;
       WSLENV carries it across into Linux */
    if (tb->spare) {
        wchar_t old[2048] = L"", wslenv[2100];
        DWORD n = GetEnvironmentVariableW(L"WSLENV", old, 2048);
        _snwprintf(wslenv, 2100, L"%ls%lsHARDCORE_SPARE", old, n ? L":" : L"");
        wslenv[2099] = 0;
        SetEnvironmentVariableW(L"HARDCORE_SPARE", L"1");
        SetEnvironmentVariableW(L"WSLENV", wslenv);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            NULL, NULL, &si.StartupInfo, &pi);
        SetEnvironmentVariableW(L"HARDCORE_SPARE", NULL);
        SetEnvironmentVariableW(L"WSLENV", n ? old : NULL);
    } else {
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            NULL, NULL, &si.StartupInfo, &pi);
    }
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "\x1b[31mhardcore: could not start wsl.exe (error %lu)\x1b[0m\r\n", GetLastError());
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

static Tab *tab_create(const char *dir, int spare) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) {
        free(tb);
        return NULL;
    }
    tb->id = g_next_id++;
    InitializeCriticalSection(&tb->lock);
    tb->spare = spare;
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
    _snwprintf(g_cfg_dir, MAX_PATH, L"%ls\\hardcore", la ? la : L".");
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
        if (!strcmp(line, "dir") && *v) snprintf(g_start_dir, sizeof(g_start_dir), "%s", v);
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
    fprintf(f, "# hardcore settings\n");
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

static int g_focused = 1;
static int g_mouse_x = -1, g_mouse_y = -1;
static DWORD g_last_paint;

static Tab *active_tab(void) { return g_ntabs ? g_tabs[g_active] : NULL; }

#define CW (FONT_W * g_scale)
#define CH (FONT_H * g_scale)
#define PAD (4 * g_scale)
#define SCROLLBAR_W (6 * g_scale)
/* The chrome: one column on the left with the tabs and "+". Min/max/close
   float over the terminal's top-right corner on smoked glass: the text runs
   underneath, only the mouse is theirs. No row is spent on a title bar. */
static int sidebar_w(void) { return 20 * CW; }
static int caption_h(void) { return CH + 8 * g_scale; }   /* the glass buttons' height */
static int term_ox(void) { return sidebar_w() + PAD; }
static int term_oy(void) { return PAD; }

static void compute_grid(int w, int h, int *cols, int *rows) {
    *cols = (w - sidebar_w() - 2 * PAD - SCROLLBAR_W) / CW;
    *rows = (h - 2 * PAD) / CH;
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
    for (i = 0; i <= g_ntabs; i++) {
        Tab *tb = i < g_ntabs ? g_tabs[i] : g_spare;
        if (!tb) continue;
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

/* Sends text the way a paste arrives: CRLF/LF become CR like a typed Enter,
   ESC is stripped so a paste can't end bracketed-paste mode early. */
static void paste_wide(const wchar_t *w) {
    Tab *tb = active_tab();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s, *o;
    const char *p;
    if (!tb || n <= 1) return;
    s = o = (char *)malloc(n + 16);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    for (p = s; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        if (*p == 0x1B) continue;
        *o++ = (*p == '\n') ? '\r' : *p;
    }
    if (tb->t.m_bracketed) send_to_active("\x1b[200~", 6);
    send_to_active(s, (int)(o - s));
    if (tb->t.m_bracketed) send_to_active("\x1b[201~", 6);
    free(s);
}

/* A Windows path as you'd type it in WSL, shell-quoted only when needed:
   C:\x y → '/mnt/c/x y', \\wsl.localhost\Debian\home → /home */
static void dropped_path(const wchar_t *path, wchar_t *q, size_t cap) {
    wchar_t lin[MAX_PATH * 4 + 16];
    const wchar_t *p, *safe = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-+:@%,=";
    size_t k, j = 0;
    int needq = 0;
    if (iswalpha(path[0]) && path[1] == L':') {
        _snwprintf(lin, MAX_PATH * 4 + 16, L"/mnt/%c%ls", towlower(path[0]), path + 2);
    } else if (!_wcsnicmp(path, L"\\\\wsl.localhost\\", 16) || !_wcsnicmp(path, L"\\\\wsl$\\", 7)) {
        p = wcschr(path + 2, L'\\');                 /* skip \\host */
        p = p ? wcschr(p + 1, L'\\') : NULL;           /* skip \distro */
        _snwprintf(lin, MAX_PATH * 4 + 16, L"%ls", p ? p : L"/");
    } else {
        _snwprintf(lin, MAX_PATH * 4 + 16, L"%ls", path);
    }
    lin[MAX_PATH * 4 + 15] = 0;
    for (k = 0; lin[k]; k++) {
        if (lin[k] == L'\\') lin[k] = L'/';
        if (!wcschr(safe, lin[k])) needq = 1;
    }
    if (needq) q[j++] = L'\'';
    for (k = 0; lin[k] && j + 6 < cap; k++) {
        if (lin[k] == L'\'') { wcscpy(q + j, L"'\\''"); j += 4; }
        else q[j++] = lin[k];
    }
    if (needq) q[j++] = L'\'';
    q[j] = 0;
}

/* Files dropped from Explorer are typed in as WSL paths, space-separated */
/* types the files' WSL paths, space separated */
static void paste_hdrop(HDROP drop) {
    UINT i, n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    wchar_t *out = (wchar_t *)calloc(1, sizeof(wchar_t));
    size_t len = 0;
    for (i = 0; i < n; i++) {
        wchar_t path[MAX_PATH * 4], q[MAX_PATH * 8 + 16];
        size_t j;
        DragQueryFileW(drop, i, path, MAX_PATH * 4);
        dropped_path(path, q, MAX_PATH * 8 + 16);
        j = wcslen(q);
        out = (wchar_t *)realloc(out, sizeof(wchar_t) * (len + j + 2));
        wcscpy(out + len, q);
        len += j;
        out[len++] = L' ';
        out[len] = 0;
    }
    paste_wide(out);
    free(out);
}

static void drop_files(HDROP drop) {
    paste_hdrop(drop);
    DragFinish(drop);
}

/* A bitmap on the clipboard (a screenshot, "copy image") is saved as a PNG
   under %LOCALAPPDATA%\hardcore\pasted and its path is typed instead.
   That's how Claude Code (and anything else) takes an image: as a file path. */
typedef int (WINAPI *PFN_GdiplusStartup)(ULONG_PTR *, const void *, void *);
typedef int (WINAPI *PFN_GdipCreateBitmapFromGdiDib)(const BITMAPINFO *, void *, void **);
typedef int (WINAPI *PFN_GdipSaveImageToFile)(void *, const WCHAR *, const CLSID *, const void *);
typedef int (WINAPI *PFN_GdipDisposeImage)(void *);

static int save_clipboard_png(HANDLE dib, wchar_t *path, size_t cap) {
    static const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    static ULONG_PTR token;
    struct { UINT32 ver; void *cb; BOOL no_thread, no_codecs; } in = {1, NULL, FALSE, FALSE};
    HMODULE gp = LoadLibraryW(L"gdiplus.dll");
    PFN_GdiplusStartup start = gp ? (PFN_GdiplusStartup)(void *)GetProcAddress(gp, "GdiplusStartup") : NULL;
    PFN_GdipCreateBitmapFromGdiDib mk = gp ? (PFN_GdipCreateBitmapFromGdiDib)(void *)GetProcAddress(gp, "GdipCreateBitmapFromGdiDib") : NULL;
    PFN_GdipSaveImageToFile save = gp ? (PFN_GdipSaveImageToFile)(void *)GetProcAddress(gp, "GdipSaveImageToFile") : NULL;
    PFN_GdipDisposeImage dispose = gp ? (PFN_GdipDisposeImage)(void *)GetProcAddress(gp, "GdipDisposeImage") : NULL;
    BITMAPINFO *bi = (BITMAPINFO *)GlobalLock(dib);
    void *img = NULL;
    SYSTEMTIME st;
    int ok = 0;
    if (!bi) return 0;
    if (start && mk && save && dispose && (token || start(&token, &in, NULL) == 0)) {
        /* pixels follow the header and any color table / BI_BITFIELDS masks */
        BITMAPINFOHEADER *h = &bi->bmiHeader;
        DWORD ncolors = h->biClrUsed ? h->biClrUsed : (h->biBitCount <= 8 ? 1u << h->biBitCount : 0);
        DWORD masks = (h->biCompression == BI_BITFIELDS && h->biSize == sizeof(BITMAPINFOHEADER)) ? 12 : 0;
        unsigned char *bits = (unsigned char *)bi + h->biSize + masks + ncolors * sizeof(RGBQUAD);
        wchar_t dir[MAX_PATH];
        _snwprintf(dir, MAX_PATH, L"%ls\\pasted", g_cfg_dir);
        dir[MAX_PATH - 1] = 0;
        CreateDirectoryW(dir, NULL);
        GetLocalTime(&st);
        _snwprintf(path, cap, L"%ls\\%04d%02d%02d-%02d%02d%02d-%03d.png", dir, st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        path[cap - 1] = 0;
        if (mk(bi, bits, &img) == 0 && img) {
            ok = save(img, path, &png, NULL) == 0;
            dispose(img);
        }
    }
    GlobalUnlock(dib);
    return ok;
}

/* Text pastes as text. Files copied in Explorer paste as their WSL paths,
   like a drop. An image pastes as the path of a PNG made from it. */
static void paste(void) {
    HANDLE h;
    if (!OpenClipboard(g_hwnd)) return;
    if ((h = GetClipboardData(CF_HDROP))) {
        paste_hdrop((HDROP)h);
    } else if ((h = GetClipboardData(CF_UNICODETEXT))) {
        const wchar_t *w = (const wchar_t *)GlobalLock(h);
        if (w) {
            paste_wide(w);
            GlobalUnlock(h);
        }
    } else if ((h = GetClipboardData(CF_DIB))) {
        wchar_t path[MAX_PATH], q[MAX_PATH * 2 + 16];
        if (save_clipboard_png(h, path, MAX_PATH)) {
            dropped_path(path, q, MAX_PATH * 2 + 16);
            wcscat(q, L" ");
            paste_wide(q);
        }
    }
    CloseClipboard();
}

/* ── glyph atlas: on-demand 16×16 slots in one texture ─────────────── */

#define ATLAS 2048
#define SLOTS_PER_ROW (ATLAS / 16)
#define MAX_SLOTS (SLOTS_PER_ROW * SLOTS_PER_ROW)
#define HASH_SIZE 65536
static uint32_t g_hkey[HASH_SIZE];   /* cp + 1, 0 = empty */
static uint16_t g_hslot[HASH_SIZE];
static uint8_t g_hwidth[HASH_SIZE];
static int g_nslots;

static void glyph_cache_reset(void);

/* ── renderer: Direct3D 11 on a flip-model swap chain ──────────────── */

/* Why not OpenGL any more: a WGL window is a "redirected" surface that DWM
   copies into its composition. Measured here, our whole key→echo→render path
   took 1.4 ms, then the frame waited 6 ms (p50) to 19 ms (p99) on that copy.
   A flip-model swap chain hands DWM the buffer itself, and with tearing
   allowed Windows can promote the window to an independent flip or a
   hardware overlay plane, skipping composition altogether.

   The drawing code is still immediate mode: r_color() sets the color,
   r_quad() / r_quad_uv() append two triangles to one CPU-side vertex array,
   and r_present() uploads it and draws everything with a single Draw. */

typedef struct { float x, y, u, v; uint32_t rgba; } Vtx;

/* not in every mingw libuuid */
static const GUID IID_IDXGIFactory5_ = {0x7632e1f5, 0xee65, 0x4dca, {0x87, 0xfd, 0x84, 0xcd, 0x75, 0xf8, 0x83, 0x8d}};
static const GUID IID_IDXGISwapChainMedia_ = {0xdd95b90b, 0xf05f, 0x4f6a, {0xbd, 0x65, 0x25, 0xbf, 0xb2, 0x64, 0xbd, 0x84}};

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_sc;
static ID3D11RenderTargetView *g_rtv;
static ID3D11Texture2D *g_atlas;
static ID3D11ShaderResourceView *g_srv;
static ID3D11Buffer *g_vb;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11InputLayout *g_il;
static ID3D11BlendState *g_blend;
static ID3D11SamplerState *g_samp;
static ID3D11RasterizerState *g_rs;
static int g_tearing;            /* DXGI_FEATURE_PRESENT_ALLOW_TEARING */
static int g_gfx_ready;          /* swap chain exists: GDI no longer paints */
static float g_rw = 1, g_rh = 1; /* current frame size, for pixel → clip space */
static uint32_t g_rgba = 0xFFFFFFFF;
#define VB_VERTS 65536           /* per Draw; a frame that needs more draws in pieces */
static Vtx g_vtx[VB_VERTS];
static int g_nvtx;

static const char HLSL[] =
    "struct VI { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "VO vs(VI i) { VO o; o.p = float4(i.p, 0, 1); o.uv = i.uv; o.c = i.c; return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(VO i) : SV_Target {\n"
    "  float a = i.uv.x < 0 ? 1 : t.SampleLevel(s, i.uv, 0).a;\n"
    "  return float4(i.c.rgb, i.c.a * a); }\n";

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                         LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static void r_color(uint32_t rgb, float a) {
    g_rgba = ((rgb >> 16) & 255) | (rgb & 0xFF00) | ((rgb & 255) << 16) | ((uint32_t)(a * 255.0f + 0.5f) << 24);
}

static void r_flush(void) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (!g_nvtx || !g_ctx) return;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, g_vtx, sizeof(Vtx) * g_nvtx);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
        ID3D11DeviceContext_Draw(g_ctx, (UINT)g_nvtx, 0);
    }
    g_nvtx = 0;
}

static void r_vert(float x, float y, float u, float v) {
    Vtx *p = &g_vtx[g_nvtx++];
    p->x = x * 2.0f / g_rw - 1.0f;
    p->y = 1.0f - y * 2.0f / g_rh;
    p->u = u;
    p->v = v;
    p->rgba = g_rgba;
}

/* textured quad; u0 < 0 means a solid one */
static void r_quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    if (g_nvtx + 6 > VB_VERTS) r_flush();
    r_vert(x0, y0, u0, v0); r_vert(x1, y0, u1, v0); r_vert(x1, y1, u1, v1);
    r_vert(x0, y0, u0, v0); r_vert(x1, y1, u1, v1); r_vert(x0, y1, u0, v1);
}

static void quad(float x0, float y0, float x1, float y1) { r_quad_uv(x0, y0, x1, y1, -1, -1, -1, -1); }

/* one 16×16 alpha glyph into the atlas */
static void r_upload(int x, int y, const unsigned char *bmp) {
    D3D11_BOX b = {(UINT)x, (UINT)y, 0, (UINT)x + 16, (UINT)y + 16, 1};
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_atlas, 0, &b, bmp, 16, 0);
}

static void r_make_rtv(void) {
    ID3D11Texture2D *back = NULL;
    if (SUCCEEDED(IDXGISwapChain1_GetBuffer(g_sc, 0, &IID_ID3D11Texture2D, (void **)&back))) {
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)back, NULL, &g_rtv);
        ID3D11Texture2D_Release(back);
    }
}

static void r_resize(void) {
    if (!g_sc) return;
    RECT r;
    GetClientRect(g_hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return;   /* minimized */
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    if (g_rtv) { ID3D11RenderTargetView_Release(g_rtv); g_rtv = NULL; }
    IDXGISwapChain1_ResizeBuffers(g_sc, 0, 0, 0, DXGI_FORMAT_UNKNOWN, g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    r_make_rtv();
}

static void r_begin(int w, int h) {
    static const float bg[4] = {((DEF_BG >> 16) & 255) / 255.0f, ((DEF_BG >> 8) & 255) / 255.0f, (DEF_BG & 255) / 255.0f, 1};
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    UINT stride = sizeof(Vtx), off = 0;
    g_rw = (float)w;
    g_rh = (float)h;
    g_nvtx = 0;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, bg);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_il);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_srv);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, NULL, 0xFFFFFFFF);
}

static void gfx_lost(void);

static void r_present(void) {
    HRESULT hr;
    r_flush();
    hr = IDXGISwapChain1_Present(g_sc, 0, g_tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) gfx_lost();
}

/* Device, shaders and atlas: no window needed, so this runs on a worker
   thread while the main thread shows the window and starts wsl.exe. */
static int gfx_init_device(void) {
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompile compile = dc ? (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile") : NULL;
    ID3DBlob *vsb = NULL, *psb = NULL;
    HRESULT hr;
    if (!compile) return 0;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_SINGLETHREADED, levels, 3,
                           D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_SINGLETHREADED, levels, 3,
                               D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
    if (FAILED(hr)) return 0;
    if (FAILED(compile(HLSL, sizeof(HLSL) - 1, NULL, NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, NULL)) ||
        FAILED(compile(HLSL, sizeof(HLSL) - 1, NULL, NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, NULL)))
        return 0;
    ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs);
    ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps);
    {
        D3D11_INPUT_ELEMENT_DESC el[3] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        ID3D11Device_CreateInputLayout(g_dev, el, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_il);
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    {
        D3D11_BUFFER_DESC bd = {sizeof(g_vtx), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vb);
    }
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof(td));
        td.Width = td.Height = ATLAS;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_atlas);
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_atlas, NULL, &g_srv);
    }
    {
        D3D11_BLEND_DESC b;
        memset(&b, 0, sizeof(b));
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ID3D11Device_CreateBlendState(g_dev, &b, &g_blend);
    }
    {
        D3D11_SAMPLER_DESC sd;
        memset(&sd, 0, sizeof(sd));
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp);
    }
    {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rs);
    }
    {   /* don't queue frames: each present should be the newest one */
        IDXGIDevice1 *xd = NULL;
        if (SUCCEEDED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice1, (void **)&xd))) {
            IDXGIDevice1_SetMaximumFrameLatency(xd, 1);
            IDXGIDevice1_Release(xd);
        }
    }
    return 1;
}

static DWORD WINAPI gfx_device_thread(LPVOID unused) {
    (void)unused;
    return (DWORD)gfx_init_device();
}

/* The swap chain belongs to the window's thread. */
static int gfx_init_swapchain(HWND hwnd) {
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    IDXGIFactory2 *f = NULL;
    DXGI_SWAP_CHAIN_DESC1 d;
    HRESULT hr;
    if (!g_dev) return 0;
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&xd);
    IDXGIDevice_GetAdapter(xd, &ad);
    IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&f);
    IDXGIAdapter_Release(ad);
    IDXGIDevice_Release(xd);
    if (!f) return 0;
    {
        IDXGIFactory5 *f5 = NULL;
        BOOL allow = FALSE;
        if (SUCCEEDED(IDXGIFactory2_QueryInterface(f, &IID_IDXGIFactory5_, (void **)&f5))) {
            if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
                g_tearing = allow ? 1 : 0;
            IDXGIFactory5_Release(f5);
        }
    }
    if (_wgetenv(L"HARDCORE_NO_TEARING")) g_tearing = 0;
    memset(&d, 0, sizeof(d));
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_NONE;          /* during a live resize, don't stretch the old frame */
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = g_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(f, (IUnknown *)g_dev, hwnd, &d, NULL, NULL, &g_sc);
    if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(f, hwnd, DXGI_MWA_NO_ALT_ENTER);   /* Alt+Enter is the shell's */
    IDXGIFactory2_Release(f);
    if (FAILED(hr)) return 0;
    r_make_rtv();
    g_gfx_ready = 1;
    return 1;
}

#define REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

/* driver update, TDR, GPU switch: start over and redraw every glyph */
static void gfx_lost(void) {
    g_gfx_ready = 0;
    REL(g_rtv); REL(g_sc); REL(g_rs); REL(g_samp); REL(g_blend); REL(g_srv); REL(g_atlas);
    REL(g_vb); REL(g_il); REL(g_ps); REL(g_vs); REL(g_ctx); REL(g_dev);
    glyph_cache_reset();
    if (gfx_init_device()) gfx_init_swapchain(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void glyph_cache_reset(void) {
    r_flush();                   /* quads already queued still point at the old slots */
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
    r_upload((slot % SLOTS_PER_ROW) * 16, (slot / SLOTS_PER_ROW) * 16, bmp);
    g_hkey[h] = cp + 1;
    g_hslot[h] = (uint16_t)slot;
    g_hwidth[h] = (uint8_t)w;
    *gw = w;
    return slot;
}

static void fill_rect(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
    r_color(rgb, a);
    quad(x0, y0, x1, y1);
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
    r_quad_uv(x0, y0, x1, y1, u0, v0, u1, v1);
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
/* UTF-16 → code points with cell widths (zero-width dropped); returns count */
static int text_cells(const wchar_t *s, uint32_t *cps, uint8_t *ws, int cap) {
    int n = 0;
    for (; *s && n < cap; s++) {
        uint32_t cp = *s;
        int w;
        if (cp >= 0xD800 && cp <= 0xDBFF && s[1] >= 0xDC00 && s[1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[1] - 0xDC00);
            s++;
        }
        if ((w = cp_width(cp)) == 0) continue;
        cps[n] = cp;
        ws[n++] = (uint8_t)w;
    }
    return n;
}

/* Lays text into up to max_lines lines of max_w pixels, breaking anywhere;
   the last line ends in "…" if it doesn't all fit. Draws when draw is set.
   Returns the lines used. */
static int layout_text(const wchar_t *s, float x, float y, float max_w, int max_lines, uint32_t rgb, int draw) {
    uint32_t cps[512];
    uint8_t ws[512];
    int n = text_cells(s, cps, ws, 512), per = (int)(max_w / CW), i = 0, line, k;
    if (per < 2) return 1;
    if (draw) r_color(rgb, 1);
    for (line = 0; line < max_lines && i < n; line++) {
        int last = line == max_lines - 1, cols = 0, end = i, brk = -1, rest = 0, col;
        for (k = i; k < n; k++) rest += ws[k];
        if (rest <= per) end = n;                      /* the rest fits on this line */
        else {
            int room = last ? per - 1 : per;           /* the last line keeps a cell for "…" */
            while (end < n && cols + ws[end] <= room) {
                if (cps[end] == ' ') brk = end;
                cols += ws[end++];
            }
            if (!last && brk > i) end = brk + 1;       /* wrap at a space when there is one */
        }
        for (col = 0, k = i; k < end; k++) {
            if (draw && !(col == 0 && cps[k] == ' ')) draw_glyph(cps[k], x + col * CW, y + line * CH, ws[k], g_scale);
            if (!(col == 0 && cps[k] == ' ')) col += ws[k];
        }
        if (last && end < n && draw) draw_glyph(0x2026, x + col * CW, y + line * CH, 1, g_scale);
        i = end;
        while (i < n && cps[i] == ' ' && !last) i++;  /* no leading space on the next line */
    }
    return line ? line : 1;
}

static int draw_label(const wchar_t *s, float x, float y, float max_w, uint32_t rgb) {
    return layout_text(s, x, y, max_w, 1, rgb, 1);
}

/* tab label: the folder's name and nothing else. Programs' titles are
   mostly noise ("✳ Claude Code — ...", "user@host: ~/x"); the only thing we
   take from them is whether they're animating a spinner (see title_spin). */
static void tab_label(Tab *tb, wchar_t *out, int cap) {
    const char *dir = tb->t.cwd[0] ? tb->t.cwd : tb->start_dir, *b = NULL, *p;
    for (p = dir; *p; p++)
        if ((*p == '/' || *p == '\\') && p[1]) b = p + 1;
    if (!dir[0]) _snwprintf(out, cap, L"wsl");
    else MultiByteToWideChar(CP_UTF8, 0, b ? b : dir, -1, out, cap);
    out[cap - 1] = 0;
}

/* The throbber: braille drops fall one at a time and stack up in a single
   cell, left and right in turn, then the full cell drains out the bottom. */
static uint16_t g_drip[40];
static int g_ndrip;
#define DRIP_MS 70

static void drip_init(void) {
    /* braille dot bits by column, top to bottom */
    static const uint8_t col[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
    uint8_t pile = 0;
    int level, side, y, k;
    for (level = 3; level >= 0; level--)
        for (side = 0; side < 2; side++) {
            for (y = 0; y <= level; y++) g_drip[g_ndrip++] = (uint16_t)(0x2800 | pile | col[side][y]);
            pile |= col[side][level];
        }
    for (k = 1; k <= 3; k++) {   /* drain: the pile slides down and out */
        uint8_t m = 0;
        for (side = 0; side < 2; side++)
            for (y = 0; y < 4; y++)
                if (y - k >= 0 && (pile & col[side][y - k])) m |= col[side][y];
        g_drip[g_ndrip++] = (uint16_t)(0x2800 | m);
    }
    g_drip[g_ndrip++] = 0x2800;
}

#define TIMER_THROB 3
#define BELL_GLYPH 0xF8F0        /* hand-drawn, font/hardcore-extra.hex */

static int caption_btn_w(void) { return 46 * g_scale; }

/* sidebar geometry, top to bottom:
     [ drag ][ – ][ □ ][ × ]     caption_h()
     ▌⣿ folder-name        ×     one row per tab
       +                         new tab
       ... drag space (it's the title bar) ...                         */
enum { HIT_NONE, HIT_TAB, HIT_CLOSE, HIT_PLUS, HIT_BAR };

static int tabs_top(void) { return 8 * g_scale; }   /* the gap above the tabs drags the window */

/* the dim second line under a tab's name: what Claude is doing, asking, or did */
static const wchar_t *tab_detail(const Tab *tb, wchar_t *buf, int cap) {
    const Term *t = &tb->t;
    if (t->ps_state == PS_NONE || t->ps_state == PS_IDLE) return NULL;
    if (t->ps_msg[0]) {
        if (t->ps_tasks) _snwprintf(buf, cap, L"%ls · %d bg", t->ps_msg, t->ps_tasks);
        else _snwprintf(buf, cap, L"%ls", t->ps_msg);
    } else if (t->ps_state == PS_BLOCKED) {
        _snwprintf(buf, cap, L"%hs", t->ps_kind[0] ? t->ps_kind : "waiting for you");
    } else if (t->ps_tasks) {
        _snwprintf(buf, cap, L"%d background task%ls", t->ps_tasks, t->ps_tasks == 1 ? L"" : L"s");
    } else return NULL;
    buf[cap - 1] = 0;
    return buf;
}

/* Row layout, recomputed per use (64 tabs at most). A tab with a detail line
   is two text lines tall. When they don't all fit, details go first, then
   rows squeeze. */
static int g_row_y[MAX_TABS + 2], g_row_two[MAX_TABS + 1];   /* g_row_two: detail lines, 0..2 */
static float detail_w(void) { return (float)(sidebar_w() - 10 * g_scale - 2 * CW - 8 * g_scale); }
static void tab_layout(void) {
    RECT r;
    int base = CH + 10 * g_scale, i, need = 0, two = 1, h, y, avail;
    wchar_t tmp[220];
    GetClientRect(g_hwnd, &r);
    avail = r.bottom - tabs_top() - 4 * g_scale;
    for (i = 0; i < g_ntabs; i++) {
        const wchar_t *d = tab_detail(g_tabs[i], tmp, 220);
        /* what Claude is asking you gets two lines; the rest get one */
        g_row_two[i] = !d ? 0 : g_tabs[i]->t.ps_state == PS_BLOCKED ? layout_text(d, 0, 0, detail_w(), 2, 0, 0) : 1;
        need += base + g_row_two[i] * CH;
    }
    need += base;                                        /* the + row */
    if (need > avail) two = 0;
    h = base;
    if (!two && (g_ntabs + 1) * base > avail) h = avail / (g_ntabs + 1);
    if (h < CH) h = CH;
    for (y = tabs_top(), i = 0; i <= g_ntabs; i++) {
        if (i < g_ntabs && !two) g_row_two[i] = 0;
        g_row_y[i] = y;
        y += h + (i < g_ntabs ? g_row_two[i] * CH : 0);
    }
    g_row_y[g_ntabs + 1] = y;
}

static int hit_tabbar(int mx, int my, int *idx) {
    int i;
    if (mx < 0 || mx >= sidebar_w() || my < 0) return HIT_NONE;
    tab_layout();
    for (i = 0; i <= g_ntabs; i++) {
        if (my < g_row_y[i] || my >= g_row_y[i + 1]) continue;
        if (i == g_ntabs) return HIT_PLUS;
        *idx = i;
        if (mx >= sidebar_w() - CW - 10 * g_scale) return HIT_CLOSE;
        return HIT_TAB;
    }
    return HIT_BAR;
}

/* caption buttons: HTMINBUTTON / HTMAXBUTTON / HTCLOSE, or 0 */
static int caption_hit(int mx, int my) {
    RECT r;
    int bw = caption_btn_w(), right;
    GetClientRect(g_hwnd, &r);
    right = r.right;
    if (my < 0 || my >= caption_h() || mx < right - 3 * bw || mx >= right) return 0;
    if (mx >= right - bw) return HTCLOSE;
    if (mx >= right - 2 * bw) return HTMAXBUTTON;
    return HTMINBUTTON;
}

static int g_nc_hover, g_nc_press, g_win_active = 1;

/* one-pixel-wide strokes, scaled: the caption glyphs stay as sharp as the font */
static void px_line(float x0, float y0, int n, int dx, int dy, uint32_t rgb) {
    int i;
    float s = (float)g_scale;
    r_color(rgb, 1);
    for (i = 0; i < n; i++) quad(x0 + i * dx * s, y0 + i * dy * s, x0 + i * dx * s + s, y0 + i * dy * s + s);
}

static void render_caption_buttons(int win_w) {
    int bw = caption_btn_w(), bh = caption_h(), k, s = g_scale;
    for (k = 0; k < 3; k++) {
        int code = k == 0 ? HTMINBUTTON : k == 1 ? HTMAXBUTTON : HTCLOSE;
        float x = (float)(win_w - (3 - k) * bw), cx = x + bw / 2 - 5 * s, cy = (float)(bh / 2 - 5 * s);
        int hov = g_nc_hover == code, prs = g_nc_press == code && hov;
        uint32_t fg = g_win_active ? 0xCCCCCC : 0x7A7A7A;
        fill_rect(x, 0, x + bw, (float)bh, BAR_BG, 0.62f);       /* smoked glass: text shows through */
        if (hov && code == HTCLOSE) {
            fill_rect(x, 0, x + bw, (float)bh, prs ? 0x94241A : 0xC42B1C, 1);
            fg = 0xFFFFFF;
        } else if (hov) {
            fill_rect(x, 0, x + bw, (float)bh, prs ? 0x262626 : 0x2D2D2D, 1);
        }
        if (code == HTMINBUTTON) {
            px_line(cx, cy + 5 * s, 10, 1, 0, fg);
        } else if (code == HTMAXBUTTON && !IsZoomed(g_hwnd)) {
            px_line(cx, cy, 10, 1, 0, fg);
            px_line(cx, cy + 9 * s, 10, 1, 0, fg);
            px_line(cx, cy, 10, 0, 1, fg);
            px_line(cx + 9 * s, cy, 10, 0, 1, fg);
        } else if (code == HTMAXBUTTON) {       /* restore: two stacked squares */
            px_line(cx, cy + 2 * s, 8, 1, 0, fg);
            px_line(cx, cy + 9 * s, 8, 1, 0, fg);
            px_line(cx, cy + 2 * s, 8, 0, 1, fg);
            px_line(cx + 7 * s, cy + 2 * s, 8, 0, 1, fg);
            px_line(cx + 2 * s, cy, 8, 1, 0, fg);
            px_line(cx + 9 * s, cy, 8, 0, 1, fg);
        } else {
            px_line(cx, cy, 10, 1, 1, fg);
            px_line(cx + 9 * s, cy, 10, -1, 1, fg);
        }
    }
}

static void render_tabbar(int win_w, int win_h) {
    int i, idx = -1, hover = HIT_NONE, sw = sidebar_w();
    float lx = (float)(10 * g_scale), textw = (float)(sw - 10 * g_scale - 3 * CW - 12 * g_scale);
    uint16_t drip = g_ndrip ? g_drip[(GetTickCount() / DRIP_MS) % g_ndrip] : 0x2800;
    if (g_mouse_y >= 0) hover = hit_tabbar(g_mouse_x, g_mouse_y, &idx);
    tab_layout();
    fill_rect(0, 0, (float)sw, (float)win_h, BAR_BG, 1);
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        wchar_t label[256], detail[220];
        float y0 = (float)g_row_y[i], y1 = (float)g_row_y[i + 1];
        float ty = y0 + (y1 - y0 - (1 + g_row_two[i]) * CH) / 2;
        int act = (i == g_active), hov = (idx == i && (hover == HIT_TAB || hover == HIT_CLOSE));
        uint32_t fg = act ? 0xF2F2F2 : 0x9A9A9A;
        /* the active tab takes the terminal's background, so it reads as attached to it */
        if (act || hov) fill_rect(0, y0, (float)sw, y1, act ? DEF_BG : TAB_HOVER, 1);
        if (act) fill_rect(0, y0, (float)(2 * g_scale), y1, AMBER, 1);
        tab_label(tb, label, 256);
        /* one status cell before the name, strongest first:
           ? Claude needs you   ! it failed   throbber: working   bell: finished or rang   dot: output */
        if (tb->t.ps_state == PS_BLOCKED || tb->t.ps_state == PS_ERROR) {
            uint32_t ch = tb->t.ps_state == PS_BLOCKED ? '?' : '!';
            r_color(tb->t.ps_state == PS_BLOCKED ? AMBER : 0xE74856, 1);
            draw_glyph(ch, lx, ty, 1, g_scale);
            draw_glyph(ch, lx + g_scale, ty, 1, g_scale);       /* bold, the way cells do it */
        } else if (tb->busy) {
            r_color(act ? 0xF2F2F2 : 0xBDBDBD, 1);
            draw_glyph(drip, lx, ty, 1, g_scale);
        } else if (tb->attention) {
            r_color(AMBER, 1);
            draw_glyph(BELL_GLYPH, lx, ty, 1, g_scale);
        } else if (tb->activity && !act) {
            fill_rect(lx + 3 * g_scale, ty + CH / 2 - g_scale, lx + 5 * g_scale, ty + CH / 2 + g_scale, 0x8A8A8A, 1);
        }
        draw_label(label, lx + 2 * CW, ty, textw, fg);
        if (g_row_two[i] && tab_detail(tb, detail, 220))
            layout_text(detail, lx + 2 * CW, ty + CH, detail_w(), g_row_two[i],
                        tb->t.ps_state == PS_BLOCKED ? 0xC9A15A : 0x767676, 1);
        if (act || hov) {
            r_color((hov && hover == HIT_CLOSE) ? 0xFF6B5E : 0x8A8A8A, 1);
            draw_glyph(0x00D7, (float)(sw - CW - 8 * g_scale), ty, 1, g_scale);
        }
        if (tb->exited) fill_rect(lx + 2 * CW, ty + CH / 2, (float)(sw - CW - 12 * g_scale), ty + CH / 2 + g_scale, 0x8A8A8A, 1);
    }
    {   /* "+": a plain plus in the status column, drawn rather than taken from the font */
        float y0 = (float)g_row_y[g_ntabs], rh = (float)(g_row_y[g_ntabs + 1] - g_row_y[g_ntabs]);
        float cx = lx + CW / 2, cy = y0 + rh / 2.0f, arm = 4.0f * g_scale, th = (float)g_scale;
        int hov = hover == HIT_PLUS;
        uint32_t col = hov ? 0xF2F2F2 : 0x9A9A9A;
        if (hov) fill_rect(0, y0, (float)sw, y0 + rh, TAB_HOVER, 1);
        fill_rect(cx - arm, cy - th, cx + arm + th, cy + th, col, 1);
        fill_rect(cx - th, cy - arm, cx + th, cy + arm + th, col, 1);
    }
}

static void render(void) {
    RECT r;
    Tab *tb = active_tab();
    Term *t;
    int y, x, ox = term_ox(), oy = term_oy();
    long long base;
    GetClientRect(g_hwnd, &r);
    r_begin(r.right, r.bottom);
    render_tabbar(r.right, r.bottom);
    if (!tb) { render_caption_buttons(r.right); return; }
    t = &tb->t;
    base = term_base(t);

    /* backgrounds + selection */
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
            r_color(bg, 1);
            quad((float)(ox + x * CW), (float)(oy + y * CH), (float)(ox + (x + 1) * CW), (float)(oy + (y + 1) * CH));
        }
    }

    /* glyphs */
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
            r_color(fg, 1);
            draw_glyph(c->ch, (float)(ox + x * CW), (float)(oy + y * CH), w, g_scale);
            /* Unifont has no bold; smear one pixel right, except box drawing */
            if ((c->attr & A_BOLD) && !(c->ch >= 0x2500 && c->ch <= 0x259F))
                draw_glyph(c->ch, (float)(ox + x * CW + g_scale), (float)(oy + y * CH), w, g_scale);
        }
    }

    /* underline / strike */
    for (y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n, wrapped, L = t->hcount - t->view + y;
        term_line(t, L, &cells, &n, &wrapped);
        for (x = 0; x < n && x < t->cols; x++) {
            const Cell *c = &cells[x];
            uint32_t fg, bg;
            float x0 = (float)(ox + x * CW), y0 = (float)(oy + y * CH);
            if (!(c->attr & (A_UNDER | A_STRIKE)) && !c->link) continue;
            cell_colors(c, &fg, &bg);
            if (c->link && !(c->attr & A_UNDER)) {   /* OSC 8 link: dotted underline */
                int d;
                r_color(fg, 0.6f);
                for (d = 0; d < CW; d += 2 * g_scale) quad(x0 + d, y0 + CH - g_scale, x0 + d + g_scale, y0 + CH);
            }
            r_color(fg, 1);
            if (c->attr & A_UNDER) quad(x0, y0 + CH - g_scale, x0 + CW, y0 + CH);
            if (c->attr & A_STRIKE) quad(x0, y0 + CH / 2, x0 + CW, y0 + CH / 2 + g_scale);
        }
    }

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
                r_color(DEF_BG, 1);
                draw_glyph(c->ch, cx, cy, w, g_scale);
            }
        }
    }

    /* scrollbar, dumbterm-style thin amber thumb */
    if (t->hcount > 0 && !t->alt) {
        float track_top = (float)caption_h(), track_h = (float)(oy + t->rows * CH - caption_h());   /* starts under the glass */
        float total = (float)(t->hcount + t->rows);
        float bar_h = track_h * t->rows / total, pos, bx = (float)(r.right - SCROLLBAR_W + g_scale);
        if (bar_h < 12 * g_scale) bar_h = (float)(12 * g_scale);
        pos = 1.0f - (float)t->view / t->hcount;
        fill_rect(bx, track_top, bx + SCROLLBAR_W - 2 * g_scale, track_top + track_h, 0x303030, 0.5f);
        fill_rect(bx, track_top + pos * (track_h - bar_h), bx + SCROLLBAR_W - 2 * g_scale,
                  track_top + pos * (track_h - bar_h) + bar_h, AMBER, t->view ? 0.8f : 0.3f);
    }
    render_caption_buttons(r.right);   /* last: glass over whatever is under it */
}

static void update_title(void) {
    Tab *tb = active_tab();
    wchar_t label[256], title[300];
    if (!tb) return;
    tab_label(tb, label, 256);
    _snwprintf(title, 300, L"%ls — hardcore", label);
    title[299] = 0;
    SetWindowTextW(g_hwnd, title);
}

/* Completes the previous keypress's line once its frame has had time to reach
   the display: DXGI reports the vblank it was scanned out on (SyncQPCTime) and
   whether DWM composed it (0) or it went straight to a hardware plane (1). */
static char g_lat_line[128];
static LARGE_INTEGER g_lat_pkey;
static UINT g_lat_pid;
static void lat_finish(void) {
    IDXGISwapChainMedia *m = NULL;
    DXGI_FRAME_STATISTICS_MEDIA st;
    double shown = -1;
    int mode = -1;
    if (!g_lat || !g_lat_line[0]) return;
    memset(&st, 0, sizeof(st));
    if (g_sc && SUCCEEDED(IDXGISwapChain1_QueryInterface(g_sc, &IID_IDXGISwapChainMedia_, (void **)&m))) {
        if (SUCCEEDED(IDXGISwapChainMedia_GetFrameStatisticsMedia(m, &st)) && st.PresentCount == g_lat_pid) {
            shown = lat_ms(g_lat_pkey, st.SyncQPCTime);
            mode = (int)st.CompositionMode;
        }
        IDXGISwapChainMedia_Release(m);
    }
    fprintf(g_lat, "%s %.2f %d\n", g_lat_line, shown, mode);
    fflush(g_lat);
    g_lat_line[0] = 0;
}

/* WM_COPYDATA test channel: a pending snapshot of the next frame */
static wchar_t g_snap_path[MAX_PATH];

static void snap_backbuffer(void) {
    ID3D11Texture2D *back = NULL, *stage = NULL;
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f;
    if (FAILED(IDXGISwapChain1_GetBuffer(g_sc, 0, &IID_ID3D11Texture2D, (void **)&back))) return;
    ID3D11Texture2D_GetDesc(back, &d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &stage))) {
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)stage, (ID3D11Resource *)back);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m))) {
            if ((f = _wfopen(g_snap_path, L"wb"))) {
                BITMAPFILEHEADER fh;
                BITMAPINFOHEADER ih;
                UINT y;
                memset(&fh, 0, sizeof(fh));
                memset(&ih, 0, sizeof(ih));
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                ih.biSize = sizeof(ih);
                ih.biWidth = (LONG)d.Width;
                ih.biHeight = -(LONG)d.Height;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (y = 0; y < d.Height; y++) fwrite((char *)m.pData + y * m.RowPitch, 4, d.Width, f);
                fclose(f);
            }
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)stage, 0);
        }
        ID3D11Texture2D_Release(stage);
    }
    ID3D11Texture2D_Release(back);
}

static void paint_now(void) {
    Tab *tb = active_tab();
    if (tb && tb->t.m_sync && GetTickCount() - tb->t.sync_since < 200) {
        SetTimer(g_hwnd, 1, 20, NULL);
        return;
    }
    if (g_lat && g_lat_out.QuadPart) QueryPerformanceCounter(&g_lat_render);
    render();
    if (g_snap_path[0]) {
        r_flush();
        snap_backbuffer();
        g_snap_path[0] = 0;
    }
    r_present();
    g_last_paint = GetTickCount();
    if (g_lat && g_lat_out.QuadPart) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        lat_finish();
        snprintf(g_lat_line, sizeof(g_lat_line), "%.2f %.2f %.2f", lat_ms(g_lat_key, g_lat_out), lat_ms(g_lat_key, g_lat_render), lat_ms(g_lat_key, t));
        g_lat_pkey = g_lat_key;
        IDXGISwapChain1_GetLastPresentCount(g_sc, &g_lat_pid);
        g_lat_key.QuadPart = g_lat_out.QuadPart = 0;
    }
}

/* Re-checks which tabs are animating a spinner title. A tab that stops while
   you're looking elsewhere gets the bell mark: that's "Claude is done". */
static int g_throb_on;
static void update_busy(void) {
    int i, any = 0, changed = 0;
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        int b = term_busy(&tb->t);
        if (tb->t.ps_state != tb->ps_seen) {
            /* Claude stopped to ask you something: say so if you aren't looking */
            if ((tb->t.ps_state == PS_BLOCKED || tb->t.ps_state == PS_ERROR) && !g_focused) {
                FLASHWINFO fi = {sizeof(fi), g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
                FlashWindowEx(&fi);
            }
            tb->ps_seen = tb->t.ps_state;
            changed = 1;
        }
        if (b != tb->busy) {
            changed = 1;
            if (!b && (i != g_active || !g_focused)) {
                tb->attention = 1;
                if (!g_focused) {        /* finished while you were in another window */
                    FLASHWINFO fi = {sizeof(fi), g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
                    FlashWindowEx(&fi);
                }
            }
            tb->busy = b;
        }
        any |= b;
    }
    if (any && !g_throb_on) SetTimer(g_hwnd, TIMER_THROB, DRIP_MS, NULL);
    if (!any && g_throb_on) KillTimer(g_hwnd, TIMER_THROB);
    g_throb_on = any;
    if (any || changed) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ── tab management ───────────────────────────────────────────────── */

static void activate(int i) {
    if (i < 0 || i >= g_ntabs) return;
    g_active = i;
    g_tabs[i]->activity = 0;
    g_tabs[i]->attention = 0;
    g_sel_on = 0;
    update_title();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

#define TIMER_SPARE 2

static void schedule_spare(void) { SetTimer(g_hwnd, TIMER_SPARE, 1000, NULL); }

static void make_spare(void) {
    if (g_spare || !g_ntabs) return;
    g_spare = tab_create(g_start_dir, 1);
}

static void new_tab_here(void) {
    Tab *cur = active_tab(), *tb = NULL;
    int i;
    const char *dir = (cur && cur->t.cwd[0]) ? cur->t.cwd : cur ? cur->start_dir : g_start_dir;
    if (g_ntabs >= MAX_TABS) return;
    /* the spare already has wsl.exe + bash + your profile up; it only needs a cd */
    if (g_spare && !g_spare->exited && g_spare->proc) {
        char lin[1100];
        tb = g_spare;
        g_spare = NULL;
        win_to_linux_path(dir, lin, sizeof(lin) - 1);
        snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", lin);
        set_cwd(&tb->t, lin);
        strcat(lin, "\n");
        tab_write(tb, lin, (int)strlen(lin));
        tab_drain(tb);
        tb->spare = 0;
    } else if (g_spare) {
        tab_destroy_async(g_spare);
        g_spare = NULL;
    }
    if (!tb) tb = tab_create(dir, 0);
    if (!tb) return;
    schedule_spare();
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

static int auto_scale(void) { return g_dpi >= 144 ? (g_dpi + 48) / 96 : 1; }

/* Returns 1 if the key was consumed (its WM_CHAR is then dropped). */
/* Ctrl+Shift+J: the next tab that wants you. Claude blocked on a question
   first, then failures, then finished/rang (the bell). */
static void jump_to_attention(void) {
    int pass, k;
    for (pass = 0; pass < 3; pass++)
        for (k = 1; k <= g_ntabs; k++) {
            Tab *tb = g_tabs[(g_active + k) % g_ntabs];
            int hit = pass == 0 ? tb->t.ps_state == PS_BLOCKED : pass == 1 ? tb->t.ps_state == PS_ERROR : tb->attention;
            if (hit) { activate((g_active + k) % g_ntabs); return; }
        }
}

/* Ctrl+Shift+Up/Down: scroll to the previous/next OSC 133 mark (a shell
   prompt, or the start of a Claude turn), placing it at the top. */
static void jump_mark(int dir) {
    Tab *tb = active_tab();
    Term *t;
    long long top, target = -1, base;
    int i;
    if (!tb) return;
    t = &tb->t;
    base = term_base(t);
    top = t->hpushed - t->view;
    if (dir < 0) {
        for (i = t->nmarks - 1; i >= 0; i--)
            if (t->marks[i] < top && t->marks[i] >= base) { target = t->marks[i]; break; }
    } else {
        for (i = 0; i < t->nmarks; i++)
            if (t->marks[i] > top) { target = t->marks[i]; break; }
    }
    if (target < 0) { if (dir > 0) t->view = 0; }
    else {
        long long v = t->hpushed - target;
        t->view = v < 0 ? 0 : v > t->hcount ? t->hcount : (int)v;
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

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

    /* hardcore's own shortcuts; everything else goes to the shell */
    if (ctrl && shift && !alt) {
        switch (vk) {
        case 'T': new_tab_here(); return 1;
        case 'W': close_tab(g_active); return 1;
        case 'C': sel_copy(); g_sel_on = 0; InvalidateRect(g_hwnd, NULL, FALSE); return 1;
        case 'V': paste(); return 1;
        case VK_TAB: activate((g_active + g_ntabs - 1) % g_ntabs); return 1;
        case 'J': jump_to_attention(); return 1;
        case VK_UP: jump_mark(-1); return 1;
        case VK_DOWN: jump_mark(1); return 1;
        }
    }
    if (ctrl && !shift && !alt && vk == VK_TAB) { activate((g_active + 1) % g_ntabs); return 1; }
    if (ctrl && alt && vk >= '1' && vk <= '9') { activate(vk == '9' ? g_ntabs - 1 : (int)(vk - '1')); return 1; }
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

/* A URL inside one whitespace-delimited word, if `at` (an index into it)
   lands on the URL itself; trailing sentence punctuation isn't part of it. */
static int url_in_word(const char *text, int at, char *out, int cap) {
    static const char *schemes[] = {"https://", "http://", "file://", "ftp://"};
    int i;
    for (i = 0; i < 4; i++) {
        const char *u = strstr(text, schemes[i]);
        size_t len;
        if (!u) continue;
        len = strlen(u);
        while (len && strchr(".,;:!?)]}", u[len - 1])) len--;
        if (len <= strlen(schemes[i])) continue;
        if (at < (int)(u - text) || at >= (int)(u - text) + (int)len) continue;
        snprintf(out, cap, "%.*s", (int)len, u);
        return 1;
    }
    return 0;
}

/* The link under a cell: an OSC 8 target if the app gave one, else a URL
   spelled out in the text around it. */
static int link_at(int col, int row, char *out, int cap) {
    Tab *tb = active_tab();
    const Cell *cells;
    int n, wrapped, a, b, k = 0, L;
    char text[4096];
    if (!tb) return 0;
    L = tb->t.hcount - tb->t.view + row;
    term_line(&tb->t, L, &cells, &n, &wrapped);
    if (!cells || col >= n) return 0;
    if (cells[col].link && tb->t.links[cells[col].link - 1]) {
        snprintf(out, cap, "%s", tb->t.links[cells[col].link - 1]);
        return 1;
    }
    /* plain text: widen to the surrounding run of URL-ish ASCII */
    for (a = col; a > 0 && cells[a - 1].ch > ' ' && cells[a - 1].ch < 127 && !strchr("\"'<>`", (int)cells[a - 1].ch); a--) {}
    for (b = col; b < n && cells[b].ch > ' ' && cells[b].ch < 127 && !strchr("\"'<>`", (int)cells[b].ch); b++) {}
    if (b - a < 8 || b - a >= (int)sizeof(text)) return 0;
    {
        int start = a;
        for (; a < b; a++) text[k++] = (char)cells[a].ch;
        text[k] = 0;
        return url_in_word(text, col - start, out, cap);
    }
}

/* file:///mnt/c/x opens C:\x; anything else goes to the default handler */
static void open_url(const char *url) {
    wchar_t w[2048];
    char buf[2048];
    const char *p = url;
    if (!strncmp(p, "file://", 7)) {
        p = strchr(p + 7, '/');
        if (p && !strncmp(p, "/mnt/", 5) && p[5] && (p[6] == '/' || !p[6])) {
            size_t i;
            snprintf(buf, sizeof(buf), "%c:%s", p[5] & ~32, p[6] ? p + 6 : "\\");
            for (i = 0; buf[i]; i++) if (buf[i] == '/') buf[i] = '\\';
            percent_decode(buf);
            url = buf;
        }
    }
    MultiByteToWideChar(CP_UTF8, 0, url, -1, w, 2048);
    ShellExecuteW(g_hwnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
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
    track_h = (float)(term_oy() + tb->t.rows * CH - caption_h());
    f = (float)(my - caption_h()) / track_h;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    tb->t.view = (int)((1.0f - f) * tb->t.hcount);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

/* ── window procedure ─────────────────────────────────────────────── */

static void save_window_size(void) {
    RECT r;
    if (!IsIconic(g_hwnd) && !IsZoomed(g_hwnd) && GetWindowRect(g_hwnd, &r)) {
        g_win_w = r.right - r.left;
        g_win_h = r.bottom - r.top;
    }
}

static int window_dpi(HWND h);

static int metric_for(HWND h, int index) {
    typedef int (WINAPI *PFN)(int, UINT);
    static PFN f;
    static int tried;
    if (!tried) { tried = 1; f = (PFN)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi"); }
    return f ? f(index, (UINT)window_dpi(h)) : GetSystemMetrics(index);
}

/* resize frame height: how far a maximized window hangs over the screen edge */
static int frame_thickness(HWND h) {
    return metric_for(h, SM_CYFRAME) + metric_for(h, 92 /* SM_CXPADDEDBORDER */);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        trace("WM_CREATE");
        DragAcceptFiles(hwnd, TRUE);
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
    case WM_COPYDATA: {
        /* Test channel (bench/termbench.exe send / cmd / snap): drives a window
           that isn't focused, so tests never take the keyboard from you. */
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        char buf[4096];
        size_t n = cd->cbData < sizeof(buf) - 1 ? cd->cbData : sizeof(buf) - 1;
        Tab *tb = active_tab();
        memcpy(buf, cd->lpData, n);
        buf[n] = 0;
        if (cd->dwData == 0x48430001 && tb) {          /* type: \n becomes Enter */
            size_t i;
            for (i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '\r';
            tab_write(tb, buf, (int)n);
        } else if (cd->dwData == 0x48430002) {          /* a hardcore command */
            if (!strcmp(buf, "newtab")) new_tab_here();
            else if (!strcmp(buf, "nexttab")) activate((g_active + 1) % g_ntabs);
            else if (!strcmp(buf, "closetab")) close_tab(g_active);
            else if (!strcmp(buf, "jump")) jump_to_attention();
            else if (!strncmp(buf, "tab ", 4)) activate(atoi(buf + 4));
            else if (!strncmp(buf, "hover ", 6)) {
                g_mouse_x = atoi(buf + 6);
                g_mouse_y = strchr(buf + 6, ',') ? atoi(strchr(buf + 6, ',') + 1) : -1;
            } else if (!strncmp(buf, "nchover ", 8)) g_nc_hover = atoi(buf + 8);
            else return FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (cd->dwData == 0x48430003 && g_gfx_ready) {   /* snapshot the next frame to a .bmp */
            MultiByteToWideChar(CP_UTF8, 0, buf, -1, g_snap_path, MAX_PATH);
            g_snap_path[MAX_PATH - 1] = 0;
            paint_now();
        } else return FALSE;
        return TRUE;
    }
    case WM_NCCALCSIZE:
        /* The tab strip is the title bar: keep the standard side and bottom
           frame (resize borders, shadow, snap), drop the caption. Maximized,
           the window hangs off-screen by the frame width, so inset by it. */
        {
            RECT *rc = wp ? &((NCCALCSIZE_PARAMS *)lp)->rgrc[0] : (RECT *)lp;
            int top = rc->top;
            DefWindowProcW(hwnd, msg, wp, lp);
            rc->top = top + (IsZoomed(hwnd) ? frame_thickness(hwnd) : 0);
            return 0;
        }
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT r;
        int idx, cap;
        if (hit != HTCLIENT) return hit;
        ScreenToClient(hwnd, &pt);
        GetClientRect(hwnd, &r);
        if (!IsZoomed(hwnd) && pt.y < 5 * g_scale) {
            int corner = 16 * g_scale;
            return pt.x < corner ? HTTOPLEFT : pt.x >= r.right - corner ? HTTOPRIGHT : HTTOP;
        }
        if ((cap = caption_hit(pt.x, pt.y))) return cap;
        if (hit_tabbar(pt.x, pt.y, &idx) == HIT_BAR) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
        int h = (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) ? (int)wp : 0;
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
        TrackMouseEvent(&tme);
        if (g_mouse_y >= 0) { g_mouse_x = g_mouse_y = -1; InvalidateRect(hwnd, NULL, FALSE); }
        if (h != g_nc_hover) { g_nc_hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        break;                   /* DefWindowProc still runs: Win11 snap layouts hang off HTMAXBUTTON */
    }
    case WM_NCMOUSELEAVE:
        if (g_nc_hover || g_nc_press) { g_nc_hover = g_nc_press = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
        /* our caption buttons: DefWindowProc would track and paint the classic ones */
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            g_nc_press = (int)wp;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            int was = g_nc_press;
            g_nc_press = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            if (was != (int)wp) return 0;
            if (wp == HTCLOSE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            else if (wp == HTMINBUTTON) ShowWindow(hwnd, SW_MINIMIZE);
            else ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        break;
    case WM_NCRBUTTONUP:
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) wp = HTCAPTION;   /* system menu */
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_NCACTIVATE:
        g_win_active = (int)wp;
        InvalidateRect(hwnd, NULL, FALSE);
        return DefWindowProcW(hwnd, msg, wp, -1);   /* -1: no classic frame repaint */
    case WM_ERASEBKGND:
        if (!g_gfx_ready) break; /* before the swap chain: let the class brush paint it dark */
        return 1;
    case WM_DROPFILES:
        SetForegroundWindow(hwnd);
        drop_files((HDROP)wp);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            int col, row;
            char url[2048];
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            mouse_cell(pt.x, pt.y, &col, &row);
            if (pt.x < sidebar_w()) SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW));
            else if ((GetKeyState(VK_CONTROL) & 0x8000) && link_at(col, row, url, sizeof(url)))
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
            else SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM));
            return TRUE;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        if (g_gfx_ready) paint_now();
        else {
            /* the swap chain isn't up yet (device creation is on a worker): show the
               window's colors right away so it appears instantly */
            RECT r, bar;
            HBRUSH bg = CreateSolidBrush(RGB(DEF_BG >> 16, (DEF_BG >> 8) & 255, DEF_BG & 255));
            HBRUSH bb = CreateSolidBrush(RGB(BAR_BG >> 16, (BAR_BG >> 8) & 255, BAR_BG & 255));
            GetClientRect(hwnd, &r);
            bar = r;
            bar.right = sidebar_w();
            FillRect(ps.hdc, &r, bg);
            FillRect(ps.hdc, &bar, bb);
            DeleteObject(bg);
            DeleteObject(bb);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_THROB) { update_busy(); return 0; }
        KillTimer(hwnd, wp);
        if (wp == TIMER_SPARE) make_spare();
        else InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            r_resize();
            resize_all();
            /* draw now, not on the next WM_PAINT: during a live resize the
               compositor shows the swap chain's last frame until we do */
            if (g_gfx_ready) paint_now();
            else InvalidateRect(hwnd, NULL, FALSE);
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
        g_scale = auto_scale();
        SetWindowPos(hwnd, NULL, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        resize_all();
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        Tab *tb = active_tab();
        g_focused = (msg == WM_SETFOCUS);
        if (g_focused && tb) tb->attention = 0;
        if (tb && tb->t.m_focus) tab_write(tb, g_focused ? "\x1b[I" : "\x1b[O", 3);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_APP_OUTPUT: {
        int i, changed = 0;
        if (g_lat && g_lat_key.QuadPart && !g_lat_out.QuadPart) QueryPerformanceCounter(&g_lat_out);
        for (i = 0; i < g_ntabs; i++) {
            Tab *tb = g_tabs[i];
            if (tab_drain(tb) <= 0) continue;
            if (i == g_active) changed = 1;
            else tb->activity = 1;
            if (tb->t.bell) {
                tb->t.bell = 0;
                if (!g_focused || i != g_active) {
                    FLASHWINFO fi = {sizeof(fi), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
                    tb->attention = 1;
                    if (!g_focused) FlashWindowEx(&fi);
                }
            }
            changed = 1;
        }
        update_busy();
        if (changed) {
            static int first = 1;
            if (first) { first = 0; trace("first child output"); }
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
        if (g_lat && !g_lat_key.QuadPart) {
            lat_finish();
            QueryPerformanceCounter(&g_lat_key);
        }
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
        int was_bar = g_mouse_y >= 0 && g_mouse_x < sidebar_w(), on_bar = mx < sidebar_w();
        Tab *tb = active_tab();
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        g_mouse_x = mx;
        g_mouse_y = my;
        if (g_nc_hover) { g_nc_hover = 0; InvalidateRect(hwnd, NULL, FALSE); }
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
        if (hit == HIT_BAR) return 0;
        GetClientRect(hwnd, &r);
        if (tb && mx >= r.right - SCROLLBAR_W - g_scale && tb->t.hcount && !tb->t.alt) {
            g_sb_drag = 1;
            SetCapture(hwnd);
            scrollbar_set(my);
            return 0;
        }
        if (!tb) return 0;
        mouse_cell(mx, my, &col, &row);
        if (wp & MK_CONTROL) {   /* Ctrl+click opens a link, even inside mouse-aware apps */
            char url[2048];
            if (link_at(col, row, url, sizeof(url))) { open_url(url); return 0; }
        }
        if (mouse_report(0, 1, 0, mx, my, wp)) return 0;
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
        if (GET_X_LPARAM(lp) < sidebar_w()) return 0;
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


/* Writes an embedded file unless an identical-size copy is already there.
   Goes through a temp name so a second instance never sees half a file. */
static int extract(const wchar_t *path, const unsigned char *data, size_t n) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    wchar_t tmp[MAX_PATH];
    HANDLE h;
    DWORD w;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow == n) return 1;
    _snwprintf(tmp, MAX_PATH, L"%ls.%lu.tmp", path, GetCurrentProcessId());
    tmp[MAX_PATH - 1] = 0;
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(h, data, (DWORD)n, &w, NULL) || w != n) { CloseHandle(h); DeleteFileW(tmp); return 0; }
    CloseHandle(h);
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
    return 1;
}

static void load_conpty(int inbox_only) {
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    if (!inbox_only) {
        wchar_t dir[MAX_PATH], dll[MAX_PATH], oc[MAX_PATH];
        HMODULE m;
        _snwprintf(dir, MAX_PATH, L"%ls\\conpty-" CONPTY_VER, g_cfg_dir);
        _snwprintf(dll, MAX_PATH, L"%ls\\conpty.dll", dir);
        _snwprintf(oc, MAX_PATH, L"%ls\\OpenConsole.exe", dir);
        CreateDirectoryW(dir, NULL);
        if (extract(oc, openconsole_exe, (size_t)(openconsole_exe_end - openconsole_exe)) &&
            extract(dll, conpty_dll, (size_t)(conpty_dll_end - conpty_dll)) &&
            (m = LoadLibraryW(dll)) != NULL) {
            pCreatePC = (PFN_CreatePC)(void *)GetProcAddress(m, "ConptyCreatePseudoConsole");
            pResizePC = (PFN_ResizePC)(void *)GetProcAddress(m, "ConptyResizePseudoConsole");
            pClosePC = (PFN_ClosePC)(void *)GetProcAddress(m, "ConptyClosePseudoConsole");
            if (pCreatePC && pResizePC && pClosePC) {
                g_conpty_kind = "OpenConsole " CONPTY_VER_A;
                return;
            }
        }
    }
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
    f(h, 3, &on, sizeof(on));        /* DWMWA_TRANSITIONS_FORCEDISABLED: skip the ~170 ms open/close
                                        animation, so the window is there the moment it exists */
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
    tb = tab_create(NULL, 0);
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
    fprintf(f, "conpty=%s\n", g_conpty_kind);
    fprintf(f, "phase=%d cwd=%s cols=%d rows=%d hist=%d\n", phase, tb->t.cwd, tb->t.cols, tb->t.rows, tb->t.hcount);
    WideCharToMultiByte(CP_UTF8, 0, tb->t.title, -1, line, sizeof(line), NULL, NULL);
    fprintf(f, "title=%s\n----\n", line);
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
    tab_teardown(tb);
    return phase == 2 ? 0 : 1;
}

/* --unit OUT: table tests for the pure helpers behind drag & drop and links */
static int unit_tests(const char *out_path) {
    static const struct { const wchar_t *in, *want; } paths[] = {
        {L"C:\\Users\\willi\\notes.txt", L"/mnt/c/Users/willi/notes.txt"},
        {L"D:\\My Stuff\\a b.png", L"'/mnt/d/My Stuff/a b.png'"},
        {L"C:\\it's\\here", L"'/mnt/c/it'\\''s/here'"},
        {L"\\\\wsl.localhost\\Debian\\home\\wm\\x.c", L"/home/wm/x.c"},
        {L"\\\\wsl$\\Ubuntu\\etc\\hosts", L"/etc/hosts"},
        {L"\\\\server\\share\\f", L"//server/share/f"},
    };
    static const struct { const char *word; int at; const char *want; } urls[] = {
        {"https://example.com/a?b=c", 3, "https://example.com/a?b=c"},
        {"(see:https://x.io/y).", 10, "https://x.io/y"},
        {"(see:https://x.io/y).", 1, NULL},
        {"file:///mnt/c/Users/willi", 0, "file:///mnt/c/Users/willi"},
        {"http://", 2, NULL},
        {"nourl.here", 2, NULL},
    };
    FILE *f = fopen(out_path, "wb");
    int i, fails = 0;
    if (!f) return 3;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
        wchar_t got[1024];
        char a[1024], b[1024];
        dropped_path(paths[i].in, got, 1024);
        WideCharToMultiByte(CP_UTF8, 0, got, -1, a, sizeof(a), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, paths[i].want, -1, b, sizeof(b), NULL, NULL);
        fprintf(f, "%s drop  %s\n", strcmp(a, b) ? "FAIL" : "ok  ", a);
        fails += strcmp(a, b) != 0;
    }
    for (i = 0; i < (int)(sizeof(urls) / sizeof(urls[0])); i++) {
        char got[256] = "";
        int hit = url_in_word(urls[i].word, urls[i].at, got, sizeof(got));
        int ok = urls[i].want ? (hit && !strcmp(got, urls[i].want)) : !hit;
        fprintf(f, "%s url   %s @%d -> %s\n", ok ? "ok  " : "FAIL", urls[i].word, urls[i].at, hit ? got : "(none)");
        fails += !ok;
    }
    fprintf(f, "%d failed\n", fails);
    fclose(f);
    return fails ? 1 : 0;
}

/* Parser-only replay for conformance corpora: each input file goes through a
   fresh terminal the way a pty would deliver it (ONLCR: \n → \r\n), then the
   screen is written out, one right-trimmed line per row. */
static int replay(const char *out_path, int cols, int rows, wchar_t **files, int nfiles) {
    FILE *out = fopen(out_path, "wb");
    int i;
    if (!out) return 3;
    for (i = 0; i < nfiles; i++) {
        Tab *tb = (Tab *)calloc(1, sizeof(Tab));
        FILE *f = _wfopen(files[i], L"rb");
        char name[MAX_PATH * 3], line[8192];
        unsigned char buf[65536];
        size_t n, k;
        int y;
        WideCharToMultiByte(CP_UTF8, 0, files[i], -1, name, sizeof(name), NULL, NULL);
        fprintf(out, "=== %s\n", name);
        if (!f || !tb || !term_init(&tb->t, cols, rows)) {
            fprintf(out, "(cannot open)\n");
            continue;
        }
        vt_feed(&tb->t, (const unsigned char *)"\r\n\x1b" "c\x1b[H", 7);
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            size_t s = 0;
            for (k = 0; k < n; k++)
                if (buf[k] == '\n') {
                    vt_feed(&tb->t, buf + s, (int)(k - s));
                    vt_feed(&tb->t, (const unsigned char *)"\r\n", 2);
                    s = k + 1;
                }
            vt_feed(&tb->t, buf + s, (int)(n - s));
        }
        fclose(f);
        for (y = 0; y < rows; y++) {
            int x, len = 0;
            for (x = 0; x < cols; x++) {
                Cell *c = &tb->t.row[tb->t.alt][y][x];
                if (c->wflag == W_CONT) continue;
                len += utf8_encode(c->ch ? c->ch : ' ', line + len);
            }
            while (len && (line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
            line[len] = 0;
            fprintf(out, "%s\n", line);
        }
        term_free(&tb->t);
        free(tb);
    }
    fclose(out);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    int argc, i, cols, rows;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WNDCLASSW wc;
    RECT r;
    MSG msg;
    char selftest_out[MAX_PATH] = "";
    HWND h;
    HANDLE gfx_thread = NULL;
    (void)prev; (void)cmdline;

    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_t0);
    if (_wgetenv(L"HARDCORE_TRACE")) {
        wchar_t tp[MAX_PATH];
        _snwprintf(tp, MAX_PATH, L"%ls\\hardcore-trace.txt", _wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L".");
        g_trace = _wfopen(tp, L"wb");
    }
    trace("wWinMain");
    if (_wgetenv(L"HARDCORE_VTLOG")) {
        wchar_t tp[MAX_PATH];
        _snwprintf(tp, MAX_PATH, L"%ls\\hardcore-vt.log", _wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L".");
        g_vtlog = _wfopen(tp, L"wb");
    }
    if (_wgetenv(L"HARDCORE_LAT")) {
        wchar_t tp[MAX_PATH];
        _snwprintf(tp, MAX_PATH, L"%ls\\hardcore-lat.txt", _wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L".");
        g_lat = _wfopen(tp, L"wb");
    }
    dpi_awareness();
    palette_init();
    drip_init();
    config_load();
    snprintf(g_cfg_dir_line, sizeof(g_cfg_dir_line), "%s", g_start_dir);
    for (i = 1; i < argc; i++) {
        char a[1024];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, a, sizeof(a), NULL, NULL);
        if ((!strcmp(a, "--cd") || !strcmp(a, "-d")) && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_start_dir, sizeof(g_start_dir), NULL, NULL);
        } else if (!strcmp(a, "--distro") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_distro, sizeof(g_distro), NULL, NULL);
        } else if (!strcmp(a, "--unit") && i + 1 < argc) {
            char out[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, out, sizeof(out), NULL, NULL);
            return unit_tests(out);
        } else if (!strcmp(a, "--no-activate")) {
            show = SW_SHOWNOACTIVATE;       /* tests: open behind, never take the keyboard */
        } else if (!strcmp(a, "--inbox-conpty")) {
            g_inbox_conpty = 1;
        } else if (!strcmp(a, "--replay") && i + 3 < argc) {
            /* --replay OUT COLS ROWS FILE... */
            char out[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, out, sizeof(out), NULL, NULL);
            return replay(out, _wtoi(argv[i + 2]), _wtoi(argv[i + 3]), argv + i + 4, argc - i - 4);
        } else if (!strcmp(a, "--selftest") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, selftest_out, sizeof(selftest_out), NULL, NULL);
        } else if (a[0] != '-') {
            snprintf(g_start_dir, sizeof(g_start_dir), "%s", a);
        }
    }
    trace("config");
    load_conpty(g_inbox_conpty);
    trace("conpty loaded");
    default_start_dir();
    write_rc();
    trace("rc written");
    if (selftest_out[0]) return selftest(selftest_out);
    gfx_thread = CreateThread(NULL, 0, gfx_device_thread, NULL, 0, NULL);

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
    wc.hbrBackground = CreateSolidBrush(RGB(DEF_BG >> 16, (DEF_BG >> 8) & 255, DEF_BG & 255));  /* no white flash */
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"hardcore";
    RegisterClassW(&wc);

    g_dpi = window_dpi(NULL);
    g_scale = auto_scale();
    if (g_win_w < 300 || g_win_h < 160) {
        r.left = r.top = 0;
        r.right = sidebar_w() + 2 * PAD + SCROLLBAR_W + 120 * CW;
        r.bottom = 2 * PAD + 36 * CH;
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        g_win_w = r.right - r.left;
        g_win_h = r.bottom - r.top - GetSystemMetrics(SM_CYCAPTION) - frame_thickness(NULL);   /* no caption: see WM_NCCALCSIZE */
    }
    trace("class registered");
    h = CreateWindowExW(0, L"hardcore", L"hardcore", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        g_win_w, g_win_h, NULL, NULL, inst, NULL);
    if (!h) return 1;
    SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    trace("window created");
    g_dpi = window_dpi(h);
    g_scale = auto_scale();
    dark_titlebar(h);
    GetClientRect(h, &r);
    compute_grid(r.right, r.bottom, &cols, &rows);
    g_cols = cols;
    g_rows = rows;
    ShowWindow(h, show);
    UpdateWindow(h);
    trace("window shown");
    new_tab_here();              /* wsl.exe boots while the D3D device is created */
    trace("first tab spawned");
    if (gfx_thread) { WaitForSingleObject(gfx_thread, 5000); CloseHandle(gfx_thread); }
    trace("D3D device ready");
    if (!gfx_init_swapchain(h)) {
        MessageBoxW(h, L"Couldn't start Direct3D 11.", L"hardcore", MB_ICONERROR);
        return 1;
    }
    trace("swap chain ready");
    InvalidateRect(h, NULL, FALSE);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LocalFree(argv);
    return 0;
}
