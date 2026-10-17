/*
 * hardcore.c — a tabbed terminal for WSL on Windows 11, and for macOS.
 *
 * Win32 (ConPTY, Direct3D 11) lives in this file; the macOS layer (Cocoa,
 * forkpty, OpenGL) is hardcore_mac.m, included at the end. The parser,
 * scrollback, tab column and drawing are shared, and call down through the
 * plat_* functions and the r_* backend.
 *
 * Grown out of dumbterm (an earlier, private terminal): same GNU Unifont
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
 *   - New windows start in the folder the last window closed in, or the
 *     first time in %USERPROFILE% (/mnt/c/Users/<you>), not ~.
 *
 * Build (from WSL or MSYS2):
 *   make            → hardcore.exe
 * Build (macOS):
 *   make            → hardcore.app
 *
 * Usage:
 *   hardcore.exe [--cd DIR] [--distro NAME] [DIR]
 *   hardcore.exe --selftest OUT.txt     # headless ConPTY + parser check
 */

#if defined(__linux__)
#define HARDCORE_TTY_ONLY 1
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <winhttp.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#define COBJMACROS
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#else
/* macOS: the same core, with Cocoa + forkpty + OpenGL underneath (hardcore_mac.m,
   included at the end). The few Win32 names the core uses are mapped here. */
#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>
#include <strings.h>
typedef uint32_t DWORD;
#define MAX_PATH 1024
#define _strdup strdup
#define _stricmp strcasecmp
#define _snwprintf swprintf      /* same argument order; wchar_t is UTF-32 here */
static DWORD GetTickCount(void) {
#ifdef __APPLE__
    return (DWORD)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000);
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (DWORD)((uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000);
#endif
}
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

/* ── embedded font (tools/gen_font.py output) ──────────────────────── */

#ifdef __APPLE__
#define BLOB_SECTION ".const_data\n"
#define BLOB_SYM(name) "_" #name
#elif defined(__linux__)
#define BLOB_SECTION ".section .rodata\n"
#define BLOB_SYM(name) #name
#else
#define BLOB_SECTION ".section .rdata,\"dr\"\n"
#define BLOB_SYM(name) #name
#endif

__asm__(
    BLOB_SECTION
    ".balign 16\n"
    ".global " BLOB_SYM(font_blob) "\n"
    BLOB_SYM(font_blob) ":\n"
    ".incbin \"" FONT_BIN "\"\n"
    ".global " BLOB_SYM(font_blob_end) "\n"
    BLOB_SYM(font_blob_end) ":\n"
    ".byte 0\n"
    ".balign 16\n"
    ".global " BLOB_SYM(fonts_blob) "\n"
    BLOB_SYM(fonts_blob) ":\n"
    ".incbin \"" FONTS_BIN "\"\n"
    ".byte 0\n"
    ".text\n");
extern const unsigned char font_blob[], font_blob_end[], fonts_blob[];

#ifdef _WIN32
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
#endif

/* ── fonts: the settings panel's upper grid ────────────────────────────
   Unifont is the font; the others (tools/gen_fonts.py → build/fonts.bin)
   carry only their own glyphs, and whatever one lacks comes from its
   fallback, then from Unifont squeezed into its cell. A font's cell is in
   its own pixels. Unifont's and the other small ones' are multiplied by the
   display scale, so on a Retina screen each of their pixels is 2×2; the
   big ones (dev) were drawn for the display's own pixels and go on 1:1. */
typedef struct {
    char key;                    /* its letter in the settings, and in config.ini */
    const char *name;
    int w, h;                    /* the cell, in the font's pixels */
    int dev;                     /* those pixels are the display's: drawn 1:1, whatever the scale */
    int thin;                    /* at scale 2, box drawing gets 1-pixel lines instead of doubled ones */
    char fallback;               /* the font a missing glyph comes from before Unifont, or 0 */
    long off;                    /* its glyph table in fonts.bin; -1: Unifont, font.bin */
} Font;

static const Font FONTS[] = {
#include "build/fonts.inc"
};
#define NFONTS ((int)(sizeof(FONTS) / sizeof(FONTS[0])))

static int g_scale = 1;          /* integer pixel scale, from the monitor DPI */
static const Font *g_font = FONTS;    /* the font you picked: every window, and the tab column */
static const Font *g_tfont = FONTS;   /* the same selected font as the chrome */
static const Font *g_cf = FONTS;      /* the font CW and CH measure: g_tfont, or g_font while the tab column draws */

static int cell_w(const Font *f) { return f->dev ? f->w : f->w * g_scale; }
static int cell_h(const Font *f) { return f->dev ? f->h : f->h * g_scale; }

/* A font's glyph record (width byte + rows, (width + 7) / 8 bytes each) or NULL. */
static const unsigned char *font_record(const Font *f, uint32_t cp) {
    const unsigned char *base = f->off < 0 ? font_blob : fonts_blob + f->off;
    uint32_t page, off;
    if (cp > 0x10FFFF) return NULL;
    memcpy(&page, base + 4 + (cp >> 8) * 4, 4);
    if (!page) return NULL;
    memcpy(&off, base + page + (cp & 255) * 4, 4);
    if (!off || (f->off < 0 && font_blob + off >= font_blob_end)) return NULL;
    return base + off;
}

static const Font *font_by_key(int key) {
    int i;
    for (i = 0; i < NFONTS; i++)
        if (FONTS[i].key == key) return &FONTS[i];
    return NULL;
}

#ifdef _WIN32
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
#endif

/* ── configuration ─────────────────────────────────────────────────── */

#define HIST_LINES   20000
#define MAX_TABS     64
#define MAX_PARAMS   32
#define OSC_MAX      (4 << 20)   /* longest OSC kept (an OSC 52 copy); longer ones are dropped whole */

/* the release: `hardcore update` compares it with the latest tag, and the Mac
   build reads it into Info.plist (keep this line exactly as it is) */
#define HARDCORE_VERSION "0.5"

/* ── color schemes: the settings panel's lower grid ────────────────────
   One is in effect for every window. Lowercase letters are dark schemes,
   uppercase the light twin of the same family. hardcore's own is Windows
   Terminal's Campbell with dumbterm's amber cursor; the others are as
   iTerm2-Color-Schemes (MIT, Mark Badolato) has them, except four
   selection colors that were the text color or nearly white, so selected
   text vanished: those are each scheme's own surface/bg2 shade. */
typedef struct {
    char key;
    const char *name;
    int dark;
    uint32_t bg, fg, cursor, sel, pal[16];
} Scheme;

static const Scheme SCHEMES[] = {
    {'h', "hardcore", 1, 0x0C0C0C, 0xCCCCCC, 0xFFA60D, 0x264F78, {0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC, 0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2}},
    {'d', "Dracula", 1, 0x282A36, 0xF8F8F2, 0xF8F8F2, 0x44475A, {0x21222C, 0xFF5555, 0x50FA7B, 0xF1FA8C, 0xBD93F9, 0xFF79C6, 0x8BE9FD, 0xF8F8F2, 0x6272A4, 0xFF6E6E, 0x69FF94, 0xFFFFA5, 0xD6ACFF, 0xFF92DF, 0xA4FFFF, 0xFFFFFF}},
    {'m', "Monokai", 1, 0x272822, 0xFDFFF1, 0xC0C1B5, 0x57584F, {0x272822, 0xF92672, 0xA6E22E, 0xE6DB74, 0xFD971F, 0xAE81FF, 0x66D9EF, 0xFDFFF1, 0x6E7066, 0xF92672, 0xA6E22E, 0xE6DB74, 0xFD971F, 0xAE81FF, 0x66D9EF, 0xFDFFF1}},
    {'n', "Nord", 1, 0x2E3440, 0xD8DEE9, 0xECEFF4, 0x434C5E, {0x3B4252, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x88C0D0, 0xE5E9F0, 0x596377, 0xBF616A, 0xA3BE8C, 0xEBCB8B, 0x81A1C1, 0xB48EAD, 0x8FBCBB, 0xECEFF4}},
    {'g', "Gruvbox Dark", 1, 0x282828, 0xEBDBB2, 0xEBDBB2, 0x665C54, {0x282828, 0xCC241D, 0x98971A, 0xD79921, 0x458588, 0xB16286, 0x689D6A, 0xA89984, 0x928374, 0xFB4934, 0xB8BB26, 0xFABD2F, 0x83A598, 0xD3869B, 0x8EC07C, 0xEBDBB2}},
    {'s', "Solarized Dark", 1, 0x002B36, 0x839496, 0x839496, 0x073642, {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xEEE8D5, 0x335E69, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3}},
    {'o', "One Dark", 1, 0x282C34, 0xDCDFE4, 0xA3B3CC, 0x474E5D, {0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4, 0x5D677A, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4}},
    {'c', "Catppuccin Mocha", 1, 0x1E1E2E, 0xCDD6F4, 0xF5E0DC, 0x585B70, {0x45475A, 0xF38BA8, 0xA6E3A1, 0xF9E2AF, 0x89B4FA, 0xF5C2E7, 0x94E2D5, 0xBAC2DE, 0x585B70, 0xF7AEC2, 0xC2ECBF, 0xFCD682, 0xAECCFC, 0xF398DA, 0xB1EAE1, 0xA6ADC8}},
    {'k', "Tokyo Night", 1, 0x1A1B26, 0xC0CAF5, 0xC0CAF5, 0x33467C, {0x15161E, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xA9B1D6, 0x414868, 0xF7768E, 0x9ECE6A, 0xE0AF68, 0x7AA2F7, 0xBB9AF7, 0x7DCFFF, 0xC0CAF5}},
    {'r', "Rose Pine", 1, 0x191724, 0xE0DEF4, 0xE0DEF4, 0x403D52, {0x26233A, 0xEB6F92, 0x31748F, 0xF6C177, 0x9CCFD8, 0xC4A7E7, 0xEBBCBA, 0xE0DEF4, 0x6E6A86, 0xEB6F92, 0x31748F, 0xF6C177, 0x9CCFD8, 0xC4A7E7, 0xEBBCBA, 0xE0DEF4}},
    {'G', "Gruvbox Light", 0, 0xFBF1C7, 0x3C3836, 0x3C3836, 0xD5C4A1, {0xFBF1C7, 0xCC241D, 0x98971A, 0xD79921, 0x458588, 0xB16286, 0x689D6A, 0x7C6F64, 0x928374, 0x9D0006, 0x79740E, 0xB57614, 0x076678, 0x8F3F71, 0x427B58, 0x3C3836}},
    {'S', "Solarized Light", 0, 0xFDF6E3, 0x657B83, 0x657B83, 0xEEE8D5, {0x073642, 0xDC322F, 0x859900, 0xB58900, 0x268BD2, 0xD33682, 0x2AA198, 0xBBB5A2, 0x002B36, 0xCB4B16, 0x586E75, 0x657B83, 0x839496, 0x6C71C4, 0x93A1A1, 0xFDF6E3}},
    {'O', "One Light", 0, 0xFAFAFA, 0x383A42, 0xA5B4E5, 0xBFCEFF, {0x383A42, 0xE45649, 0x50A14F, 0xC18401, 0x0184BC, 0xA626A4, 0x0997B3, 0xBABABA, 0x4F525E, 0xE06C75, 0x98C379, 0xD8B36E, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xFFFFFF}},
    {'C', "Catppuccin Latte", 0, 0xEFF1F5, 0x4C4F69, 0xDC8A78, 0xACB0BE, {0xBCC0CC, 0xD20F39, 0x40A02B, 0xDF8E1D, 0x1E66F5, 0xEA76CB, 0x179299, 0x5C5F77, 0xACB0BE, 0xE7103F, 0x46B02F, 0xE49931, 0x3878F6, 0xEF95D7, 0x19A1A8, 0x6C6F85}},
    {'K', "Tokyo Night Day", 0, 0xE1E2E7, 0x3760BF, 0x3760BF, 0x99A7DF, {0xE9E9ED, 0xF52A65, 0x587539, 0x8C6C3E, 0x2E7DE9, 0x9854F1, 0x007197, 0x6172B0, 0xA1A6C5, 0xF52A65, 0x587539, 0x8C6C3E, 0x2E7DE9, 0x9854F1, 0x007197, 0x3760BF}},
    {'R', "Rose Pine Dawn", 0, 0xFAF4ED, 0x575279, 0x575279, 0xDFDAD9, {0xF2E9E1, 0xB4637A, 0x286983, 0xEA9D34, 0x56949F, 0x907AA9, 0xD7827E, 0x575279, 0x9893A5, 0xB4637A, 0x286983, 0xEA9D34, 0x56949F, 0x907AA9, 0xD7827E, 0x575279}},
};
#define NSCHEMES ((int)(sizeof(SCHEMES) / sizeof(SCHEMES[0])))

/* What the drawing uses, worked out from the scheme in effect. The tab
   column's shades sit between the scheme's background and text; hardcore's
   own keeps the exact greys it always had. */
typedef struct {
    uint32_t bg, fg, bold, accent, sel, bar, hover;
    uint32_t text, text_lo, busy_lo, dim, detail, detail_ask, red;
} Look;
static Look g_look;
static const Scheme *g_scheme = &SCHEMES[4]; /* Gruvbox Dark until a saved theme= is read */
static const Scheme *g_shown = &SCHEMES[4];  /* the one on screen: g_scheme, or one you're pointing at */

#define AMBER (g_look.accent)    /* the cursor and the marks: dumbterm's phosphor amber in hardcore's scheme */
#define DEF_FG (g_look.fg)
#define DEF_BG (g_look.bg)
#define BAR_BG (g_look.bar)
#define TAB_HOVER (g_look.hover)
#define SEL_BG (g_look.sel)

static const Scheme *scheme_by_key(int key) {
    int i;
    for (i = 0; i < NSCHEMES; i++)
        if (SCHEMES[i].key == key) return &SCHEMES[i];
    return NULL;
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t o = 0;
    int s;
    for (s = 0; s < 24; s += 8) {
        float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        o |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
    }
    return o;
}

static char g_start_dir[1024];   /* Windows or Linux path */
static char g_last_dir[1024];    /* where the last window closed: the next one opens there */
#ifdef _WIN32
static int g_shell_ps;           /* shell=powershell in config.ini: tabs run PowerShell, not WSL */
static int g_no_bash;            /* the distro has no bash: run its default shell, without rc.bash */
static int g_have_distro = 1;    /* any WSL distro registered for this user (HKCU\...\Lxss) */
#endif
static char g_mnt_root[64] = "/mnt/";   /* where Windows drives appear in WSL (wsl.conf automount root) */
static char g_cfg_dir_line[1100];   /* dir= as config.ini had it, written back unchanged */
#ifdef _WIN32
static int g_dpi = 96;
static char g_distro[128];
static wchar_t g_cfg_dir[MAX_PATH];
static wchar_t g_default_distro[128];
static int g_win_w = 0, g_win_h = 0;
static int g_inbox_conpty;       /* --inbox-conpty: use Windows' own conhost */
static const char *g_conpty_kind = "inbox";
#else
static char g_cfg_dir[MAX_PATH]; /* ~/Library/Application Support/hardcore */
#endif

/* ── cells ─────────────────────────────────────────────────────────── */

/* color spec: 0 = default, 0x01000000|idx = palette, 0x02000000|rgb */
#define C_PAL(i) (0x01000000u | (uint32_t)(i))
#define C_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

enum { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_BLINK = 16, A_INV = 32, A_HIDDEN = 64, A_STRIKE = 128 };
/* W_PAD: the blank left at the end of a row when a wide char didn't fit and
   went to the next one. It isn't text: reflow mustn't turn it into a space
   (before it was marked, every resize added one before such characters). */
enum { W_WIDE = 1, W_CONT = 2, W_PAD = 4 };

typedef struct {
    uint32_t ch, fg, bg;
    uint8_t attr, wflag;
    uint16_t link;               /* OSC 8 hyperlink id, 0 = none */
} Cell;

#define MAX_LINKS 4096           /* per tab, reused round-robin */

typedef struct {
    Cell *c;
    int n, wrapped;              /* wrapped: the width it wrapped at (its trailing blanks are text), 0 = it didn't */
    int cap;                     /* cells allocated: a recycled line keeps its buffer */
} HLine;

static uint32_t PAL[256];

/* the scheme on screen → the 16 colors and the look; the cube and greys stay */
static void look_set(const Scheme *s) {
    Look *l = &g_look;
    int i;
    g_shown = s;
    for (i = 0; i < 16; i++) PAL[i] = s->pal[i];
    l->bg = s->bg;
    l->fg = s->fg;
    l->accent = s->cursor;
    l->sel = s->sel;
    l->red = s->pal[9];
    if (s == SCHEMES) {          /* hardcore's own, as it always was */
        l->bold = 0xF2F2F2;
        l->bar = 0x1B1B1B;
        l->hover = 0x303030;
        l->text = 0xF2F2F2;
        l->text_lo = 0x9A9A9A;
        l->busy_lo = 0xBDBDBD;
        l->dim = 0x8A8A8A;
        l->detail = 0x767676;
        l->detail_ask = 0xC9A15A;
        return;
    }
    l->bold = s->dark ? s->pal[15] : s->fg;   /* a light scheme's bright white would vanish */
    l->bar = mix(s->bg, s->fg, s->dark ? 0.06f : 0.05f);
    l->hover = mix(s->bg, s->fg, 0.16f);
    l->text = s->fg;
    l->text_lo = mix(s->bg, s->fg, 0.72f);
    l->busy_lo = mix(s->bg, s->fg, 0.85f);
    l->dim = mix(s->bg, s->fg, 0.6f);
    l->detail = mix(s->bg, s->fg, 0.5f);
    l->detail_ask = mix(s->bg, s->cursor, 0.75f);
}

static void palette_init(void) {
    static const int cube[6] = {0, 95, 135, 175, 215, 255};
    int i;
    look_set(g_scheme);
    for (i = 0; i < 216; i++)
        PAL[16 + i] = ((uint32_t)cube[i / 36] << 16) | ((uint32_t)cube[(i / 6) % 6] << 8) | (uint32_t)cube[i % 6];
    for (i = 0; i < 24; i++) {
        uint32_t g = 8 + i * 10;
        PAL[232 + i] = (g << 16) | (g << 8) | g;
    }
}

static int g_tab_underlay = 1; /* 1: right underlay, 2: left underlay */
static int g_chrome_reflow = 1, g_chrome_debug, g_dock_row;

/* config.ini's font= and theme=: a letter each */
static void config_look(const char *key, const char *v) {
    if (!strcmp(key, "font") && font_by_key(v[0])) g_font = g_tfont = g_cf = font_by_key(v[0]);
    else if (!strcmp(key, "tab_underlay")) g_tab_underlay = !strcmp(v, "2") || !strcmp(v, "0") ? 2 : 1;
    else if (!strcmp(key, "chrome_reflow")) g_chrome_reflow = 1; /* migrate old preferences */
    else if (!strcmp(key, "chrome_debug")) g_chrome_debug = 0;
    else if (!strcmp(key, "dock_row")) { g_dock_row = atoi(v); if (g_dock_row < 0) g_dock_row = 0; if (g_dock_row > 499) g_dock_row = 499; }
    else if (!strcmp(key, "theme") && scheme_by_key(v[0])) look_set(g_scheme = scheme_by_key(v[0]));
}

static void config_look_save(FILE *f) {
    int i;
    fprintf(f, "# the font and color scheme, a letter each (the settings, under the tabs, pick them):\n#");
    for (i = 0; i < NFONTS; i++) fprintf(f, " %c=%s %dx%d", FONTS[i].key, FONTS[i].name, cell_w(&FONTS[i]), cell_h(&FONTS[i]));
    fprintf(f, "\nfont=%c\n#", g_font->key);
    for (i = 0; i < NSCHEMES; i++) fprintf(f, " %c=%s", SCHEMES[i].key, SCHEMES[i].name);
    fprintf(f, "\ntheme=%c\n# 1=right underlay, 2=left underlay; drag the menu handle to place it\ntab_underlay=%d\n"
               "dock_row=%d\n", g_scheme->key, g_tab_underlay, g_dock_row);
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

static int cp_width_search(uint32_t cp) {
    if (in_ranges(cp, W0, (int)(sizeof(W0) / sizeof(W0[0])))) return 0;
    if (in_ranges(cp, W2, (int)(sizeof(W2) / sizeof(W2[0])))) return 2;
    return 1;
}

static int cp_width(uint32_t cp) {
    /* The BMP's widths, two bits each (16 KB), filled on first use: CJK and box
       drawing were two binary searches per character, and showed in throughput. */
    static uint8_t bmp[0x10000 / 4];
    static int ready;
    if (cp < 0x300) return 1;
    if (cp > 0xFFFF) return cp_width_search(cp);
    if (!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)) {   /* (a racing second filler writes the same bits) */
        uint32_t c;
        for (c = 0; c < 0x10000; c++) bmp[c >> 2] |= (uint8_t)(cp_width_search(c) << ((c & 3) * 2));
        __atomic_store_n(&ready, 1, __ATOMIC_RELEASE);
    }
    return bmp[cp >> 2] >> ((cp & 3) * 2) & 3;
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
    uint16_t *rlen[2];           /* per row: cells from here on are default blanks. An upper
                                    bound, so a line feed only touches the part in use */
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
    uint32_t sub;                /* bit i: params[i] came after a ':' (a sub-parameter, as in 38:2::r:g:b) */
    int csi_bad;                 /* a byte no CSI may hold: ignore the sequence up to its final byte */
    int utf8_need;
    uint32_t utf8_acc, utf8_min; /* utf8_min: smaller is an overlong encoding */
    char *osc;                   /* grows as needed, up to OSC_MAX */
    int osc_n, osc_cap, osc_over;
    /* scrollback */
    HLine *hist;
    int hcount, hstart;
    long long hpushed;
    int view;                    /* lines scrolled back from live */
    int hist_cols;               /* the width the scrollback is wrapped at... */
    int hist_stale;              /* ...or 1: older lines still wrap at an earlier width (term_settle) */
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
    int had_activity;           /* sticky: first prompt becomes a used terminal after input/commands */
    int dirty;
    int side;                    /* something the tab column shows changed: folder, program status */
    char claude[40];             /* Claude Code session running here (rc.bash's wrapper says), "?" = unknown */
    char claude_args[400];       /* the flags it was started with, shell-quoted, to start it with again */
    char cmd[400];               /* the command running in the shell (rc.bash's PS0), "" at a prompt */
} Term;

/* Term is the first member so the parser can find its tab for replies. */
typedef struct Tab {
    Term t;
    int id;
    uint64_t output_generation;  /* invalidate miniature input maps after each parsed batch */
    int parent_id;               /* browser children retain their owning terminal */
    int browser_native;          /* default 0: Hardcore grid; 1: live native view */
    int browser_recording;       /* explicit Clips host: 1 replay, 2 exported video */
    void *browser;               /* platform-owned live document, never a shell */
    char browser_url[4096];
    char browser_target[128];    /* Live Chrome target, reused across GUI exec reloads. */
    char browser_site[512];
    wchar_t browser_label[256];  /* fitted browser caption cache, never a process label */
    int browser_label_cols;
#ifdef _WIN32
    HPCON_ hpc;
    HANDLE pin_w, pout_r, proc, reader, wait;
    HWND slurp_hwnd;             /* live native console; closing this tab detaches */
    int slurp_ready, slurp_committed, slurp_wsl;
    int slurp_sel_on, slurp_sel_top, slurp_sel_bottom, slurp_sel_left, slurp_sel_right;
    wchar_t slurp_command[4096]; /* source-built Linux attachment, bypass shell startup */
    CRITICAL_SECTION lock;
    volatile LONG posted;
    volatile LONG exited;
#else
    int fd;                      /* pty master, -1 = none */
    pid_t pid;
    void *rsrc;                  /* dispatch source: output ready */
    void *pin;                   /* PtyIn: what the reader queue has read and the main queue hasn't parsed */
    void *psrc;                  /* dispatch source: shell exit */
    void *wq;                    /* serial queue for writes the pty can't take right away */
    int *wpending;               /* writes waiting on that queue (atomic; the queue frees it last) */
    unsigned long long write_seq; /* main-queue writes; maintenance refuses input during staging */
    int exited;
    int claude_guard, claude_released, claude_cursor_sent;
    DWORD claude_guard_at;       /* bounded input hold while the adapter restores its draft */
#endif
    char *in;
    int in_n, in_cap;
    int activity;                /* output while in the background */
    int busy;                    /* term_busy(), as of the last check: drives the throbber */
    int attention;               /* bell, notification, or a busy program went quiet: the bell mark */
    int ps_seen;                 /* t.ps_state as of the last update_busy() */
    int kind;                    /* TK_WSL or TK_PS */
    int help;                    /* a help screen instead of a shell: HELP_* */
    DWORD born;                  /* GetTickCount() at spawn: a quick death means a setup problem */
    int spare;                   /* pre-warmed, waiting on stdin for its directory */
    DWORD adopted;               /* when a spare became this tab: hold its first frame for the prompt */
    int traced;
    char run[600];               /* a command for the shell to run first (a restored claude --resume) */
    char hist[400];              /* ...or one only to offer: put in its history, not run */
    int adopt_marks;             /* reload: prompt marks to set once the snapshot is in */
    long long mark_off[512];
    int inherit;                 /* restored text above: ConPTY starts at our cursor, not at the top */
    char start_dir[1024];
    char program[64];             /* macOS foreground app, refreshed independently of output */
} Tab;

enum { TK_WSL, TK_PS, TK_CMD, TK_BROWSER };
enum { HELP_NONE, HELP_NO_DISTRO, HELP_FAILED, HELP_SLURP };

#ifdef _WIN32
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
static double lat_ms(LARGE_INTEGER a, LARGE_INTEGER b) { return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)g_qpf.QuadPart; }
static FILE *g_lat;
static LARGE_INTEGER g_lat_key, g_lat_out, g_lat_render;
/* HARDCORE_LAT, new tabs: from the "newtab" command to the first frame that
   shows the tab, and to the first frame with its prompt on it */
static LARGE_INTEGER g_nt_t0;
static double g_nt_shown;
static int g_nt_id;

/* HARDCORE_PERF=1: once a second, to %TEMP%\\hardcore-perf.txt, what the UI
   thread did: bytes parsed, frames drawn, and where the time went. */
static FILE *g_perf;
static struct { LARGE_INTEGER t0; long long bytes; int frames, drains; double parse, build, present; } g_pf;
static double g_pf_last_build, g_pf_last_present;   /* the latest frame's */
static void perf_tick(void) {
    LARGE_INTEGER now;
    double dt;
    if (!g_perf) return;
    QueryPerformanceCounter(&now);
    if (!g_pf.t0.QuadPart) { g_pf.t0 = now; return; }
    dt = lat_ms(g_pf.t0, now);
    if (dt < 1000) return;
    if (g_pf.bytes || g_pf.frames)
        fprintf(g_perf, "%7.0f ms  %9.2f MB/s  %5d reads  %4d fps  parse %6.1f ms  build %6.1f ms (%.2f/f)  present %6.1f ms (%.2f/f)\n",
                dt, g_pf.bytes / dt / 1000.0, g_pf.drains, (int)(g_pf.frames * 1000 / dt), g_pf.parse,
                g_pf.build, g_pf.frames ? g_pf.build / g_pf.frames : 0, g_pf.present, g_pf.frames ? g_pf.present / g_pf.frames : 0);
    fflush(g_perf);
    memset(&g_pf, 0, sizeof(g_pf));
    g_pf.t0 = now;
}

static void slurp_write(Tab *tb, const void *s, int n);
static int slurp_drain(Tab *tb);
static void slurp_detach(Tab *tb);
static void slurp_commit_all(void);
static int slurp_key(Tab *tb, WPARAM vk);
static int slurp_start(int list_only);
static void slurp_selection(Tab *tb);

static void tab_write(Tab *tb, const void *s, int n) {
    DWORD w;
    if (n > 0 && tb->t.nmarks && (memchr(s, '\r', (size_t)n) || memchr(s, '\n', (size_t)n))) tb->t.had_activity = 1;
    if (tb->slurp_hwnd) { slurp_write(tb, s, n); return; }
    if (tb->pin_w && n > 0) WriteFile(tb->pin_w, s, (DWORD)n, &w, NULL);
}
#else
static void tab_write(Tab *tb, const void *s, int n);
static void mac_browser_sync(void);
static void mac_browser_resize(Tab *tb);
static void mac_browser_destroy(Tab *tb);
static void mac_browser_prepare(Tab *tb);
static int mac_browser_cutie_project(Tab*,Cell*,int,int,int);
static int mac_browser_cutie_generation(Tab*);
static void mac_browser_cutie_action(Tab*,int);
static void mac_cutie_blur(void);
static void mac_tv_drag_end(void);
static void mac_browser_scroll(int lines);
static void mac_browser_draw_overlay(void);
static void mac_browser_draw_tile(Tab *tb,const Cell *cell,int x,int y);
static int mac_browser_hover_link(void);
static int mac_browser_page_rows(Tab *tb);
static void mac_browser_tab_label(Tab *tb,wchar_t *out,int cap,int cols);
static int mac_input_held(Tab *tb);
#endif

/* ── platform layer: Win32 in this file, macOS in hardcore_mac.m ───── */

static void plat_invalidate(void);               /* redraw soon */
static void plat_client_size(int *w, int *h);    /* drawable size, in pixels */
static void plat_flash(void);                    /* ask for attention: taskbar flash, Dock bounce */
static void plat_timer(int id, int ms);          /* one-shot (or, for TIMER_THROB, repeating) */

#define TIMER_SYNC 1
#define TIMER_SPARE 2
#define TIMER_THROB 3
#define TIMER_DISK 4              /* Win32: the low-disk check, every minute */
#define TIMER_SETTLE 5            /* resizing stopped: rewrap the rest of every tab's scrollback */
#define TIMER_TV 7                /* secondary panel idle countdown/fade */
#define TIMER_CHROME 6            /* adaptive menu search and its brief fade transition */
static DWORD g_chrome_input_at;   /* pause automatic menu motion while typing */
static void plat_kill_timer(int id);
static void plat_set_title(const wchar_t *title);
static void plat_close_window(void);
static void set_clipboard_utf8(const char *s, int n);
static void config_save(void);
static Tab *tab_create(const char *dir, int spare);
static void tab_destroy_async(Tab *tb);
static int tab_drain(Tab *tb);

/* UTF-8 → wide, NUL-terminated and clipped to cap */
static void utf8_to_wide(const char *s, wchar_t *out, int cap) {
#ifdef _WIN32
    /* too long: keep what fit (a title's first character drives the spinner check) */
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) out[0] = 0;
#else
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < cap - 1) {
        uint32_t cp = *p++;
        int need = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (cp >= 0x80 && !need) { out[n++] = 0xFFFD; continue; }
        cp &= need == 3 ? 0x07 : need == 2 ? 0x0F : need == 1 ? 0x1F : 0x7F;
        while (need-- > 0) {
            if ((*p & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        out[n++] = (wchar_t)cp;
    }
    out[n] = 0;
#endif
    out[cap - 1] = 0;
}

/* A renderer/backend can own a Term without pretending to be a GUI Tab. */
static void (*g_term_reply_backend)(Term *, const char *, int);
static void term_reply(Term *t, const char *s) {
    if (g_term_reply_backend) g_term_reply_backend(t, s, (int)strlen(s));
    else tab_write((Tab *)t, s, (int)strlen(s));
}

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
    uint16_t *len = &t->rlen[t->alt][y];
    int x;
    if (x0 < 0) x0 = 0;
    if (x1 > t->cols) x1 = t->cols;
    if (x0 >= x1) return;
    if (x1 == t->cols) t->wrapf[t->alt][y] = 0;   /* its end is gone: it no longer runs on to the next row */
    if (x0 > 0 && CELL(t, x0, y)->wflag == W_CONT) x0--;                  /* erase whole wide chars, */
    if (x1 < t->cols && CELL(t, x1, y)->wflag == W_CONT) x1++;            /* never half of one */
    if (!b.bg) {                 /* default blanks: past rlen there's nothing to clear */
        if (x1 >= *len) {
            x1 = *len;
            if (x0 < *len) *len = (uint16_t)x0;
        }
    } else if (x1 > *len) *len = (uint16_t)x1;
    for (x = x0; x < x1; x++) *CELL(t, x, y) = b;
}

/* a write at [0, x) of row y: rlen has to cover it */
static void row_used(Term *t, int y, int x) {
    if (x > t->rlen[t->alt][y]) t->rlen[t->alt][y] = (uint16_t)x;
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
    for (i = 0; i < HIST_LINES; i++) {      /* recycled buffers live past hcount too */
        free(t->hist[i].c);
        t->hist[i].c = NULL;
        t->hist[i].cap = t->hist[i].n = 0;
    }
    t->hcount = t->hstart = 0;
    t->view = 0;
}

/* the width a wrapped screen row really used: its end, less any W_PAD */
static int wrap_width(const Cell *row, int cols) {
    int w = cols;
    while (w > 1 && row[w - 1].wflag == W_PAD) w--;
    return w;
}

static void hist_push(Term *t, const Cell *row, int len, int wrapped) {
    int n = len, idx;
    HLine *h;
    if (wrapped) wrapped = wrap_width(row, t->cols);
    /* a row that wraps keeps its trailing blanks: they're text (the space
       between two words that fell at the edge), and selection reads them */
    if (wrapped) n = wrapped;
    else while (n > 0 && row_is_blank_default(&row[n - 1])) n--;
    if (t->hcount < HIST_LINES) {
        idx = (t->hstart + t->hcount) % HIST_LINES;
        t->hcount++;
    } else {                     /* the oldest line falls off; its buffer is reused */
        idx = t->hstart;
        t->hstart = (t->hstart + 1) % HIST_LINES;
    }
    h = &t->hist[idx];
    if (n > h->cap) {
        int cap = n < 16 ? 16 : n;
        free(h->c);
        h->c = (Cell *)malloc(sizeof(Cell) * cap);
        h->cap = h->c ? cap : 0;
        if (!h->c) n = 0;
    }
    h->n = n;
    h->wrapped = wrapped;
    if (n) memcpy(h->c, row, sizeof(Cell) * n);
    t->hpushed++;
    if (t->view > 0 && t->view < t->hcount) t->view++;  /* keep the viewport still (full or not: every line moved up one) */
}

/* Rotates rows [top, bot] by n: positive moves content up (rows leaving at
   the top come back blank at the bottom), negative moves it down. */
static void rotate_rows(Term *t, int top, int bot, int n) {
    Cell *stack[64], **tmp;
    uint8_t wstack[64], *wtmp;
    Cell **R = t->row[t->alt];
    uint8_t *W = t->wrapf[t->alt];
    uint16_t *Ln = t->rlen[t->alt], lstack[64], *ltmp;
    int h = bot - top + 1, k = n > 0 ? n : -n;
    tmp = k <= 64 ? stack : (Cell **)malloc(sizeof(Cell *) * k);
    wtmp = k <= 64 ? wstack : (uint8_t *)malloc((size_t)k);
    ltmp = k <= 64 ? lstack : (uint16_t *)malloc(sizeof(uint16_t) * k);
    /* the rows' lengths travel with them: the rows coming back still hold
       their old cells until clear_rows() clears them */
    if (n > 0) {
        memcpy(tmp, R + top, sizeof(Cell *) * k);
        memmove(R + top, R + top + k, sizeof(Cell *) * (h - k));
        memcpy(R + bot - k + 1, tmp, sizeof(Cell *) * k);
        memmove(W + top, W + top + k, (size_t)(h - k));
        memcpy(ltmp, Ln + top, sizeof(uint16_t) * k);
        memmove(Ln + top, Ln + top + k, sizeof(uint16_t) * (h - k));
        memcpy(Ln + bot - k + 1, ltmp, sizeof(uint16_t) * k);
    } else {
        memcpy(tmp, R + bot - k + 1, sizeof(Cell *) * k);
        memmove(R + top + k, R + top, sizeof(Cell *) * (h - k));
        memcpy(R + top, tmp, sizeof(Cell *) * k);
        memmove(W + top + k, W + top, (size_t)(h - k));
        memcpy(ltmp, Ln + bot - k + 1, sizeof(uint16_t) * k);
        memmove(Ln + top + k, Ln + top, sizeof(uint16_t) * (h - k));
        memcpy(Ln + top, ltmp, sizeof(uint16_t) * k);
    }
    if (tmp != stack) free(tmp);
    if (wtmp != wstack) free(wtmp);
    if (ltmp != lstack) free(ltmp);
}

static void scroll_up(Term *t, int top, int bot, int n, int to_hist) {
    int y, h = bot - top + 1;
    if (n <= 0) return;
    if (n > h) n = h;
    if (to_hist && !t->alt && top == 0)
        for (y = 0; y < n; y++) hist_push(t, CELL(t, 0, y), t->rlen[0][y], t->wrapf[0][y]);
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
    if (cols < 2) cols = 2;      /* as term_resize */
    if (rows < 1) rows = 1;
    t->cols = cols;
    t->rows = rows;
    t->hist_cols = cols;
    t->hist = (HLine *)calloc(HIST_LINES, sizeof(HLine));
    for (s = 0; s < 2; s++) {
        int y;
        t->row[s] = (Cell **)malloc(sizeof(Cell *) * rows);
        t->wrapf[s] = (uint8_t *)calloc((size_t)rows, 1);
        t->rlen[s] = (uint16_t *)malloc(sizeof(uint16_t) * rows);
        if (!t->row[s] || !t->rlen[s]) return 0;
        for (y = 0; y < rows; y++) {
            if (!(t->row[s][y] = (Cell *)malloc(sizeof(Cell) * cols))) return 0;
            t->rlen[s][y] = (uint16_t)cols;     /* garbage until the clear below */
        }
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
    free(t->rlen[0]);
    free(t->rlen[1]);
    free(t->tabs);
    {
        int i;
        for (i = 0; i < MAX_LINKS; i++) free(t->links[i]);
    }
    free(t->osc);
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
    h->n = h->cap = n;
    h->wrapped = wrapped;
    h->c = n ? (Cell *)malloc(sizeof(Cell) * n) : NULL;
    if (n) memcpy(h->c, c, sizeof(Cell) * n);
}

/* Appends a line to the scrollback ring, letting the oldest go when it's
   full. Returns 1 if one went. */
static int hist_append(Term *t, HLine h) {
    if (t->hcount < HIST_LINES) {
        HLine *slot = &t->hist[(t->hstart + t->hcount++) % HIST_LINES];
        free(slot->c);           /* a recycled buffer may still sit past hcount */
        *slot = h;
        return 0;
    }
    free(t->hist[t->hstart].c);
    t->hist[t->hstart] = h;
    t->hstart = (t->hstart + 1) % HIST_LINES;
    return 1;
}

/* Rewraps the main screen and its scrollback to a new width, the way Windows
   Terminal does: wrapped rows are joined into logical lines and split again.
   The cursor keeps its place in the text. Only history from line `from` (the
   start of a logical line) on is rewrapped; older lines keep the width they
   wrapped at until term_settle. */
static void reflow_main(Term *t, int cols, int rows, int from) {
    int oc = t->cols, orows = t->rows, y, L, i;
    /* the places that follow the text: [0] the cursor (saved by 1049 while the
       alt screen is up), [1] the cursor ESC 7 saved on the main screen */
    struct { int x, y, pend, line, off, prow; } m[2];
    int nm = t->alt ? 1 : 2, k, ccx, ccy;
    int last, start, evicted = 0;
    long long base = t->hpushed - t->hcount;   /* absolute number of the oldest line: stays put */
    Cell *acc = NULL;
    int acc_n = 0, acc_cap = 0;
    LineList out = {0};
    Cell blank;
    memset(&blank, 0, sizeof(blank));
    blank.ch = ' ';
    memset(m, 0, sizeof(m));
    m[0].x = t->alt ? t->saved[0].cx : t->cx;
    m[0].y = t->alt ? t->saved[0].cy : t->cy;
    m[0].pend = t->alt ? t->saved[0].wrapnext : t->wrapnext;   /* the next char goes on the next row */
    m[1].x = t->saved[0].cx; m[1].y = t->saved[0].cy; m[1].pend = t->saved[0].wrapnext;
    for (k = 0; k < 2; k++) m[k].line = -1;
    last = m[0].y;
    if (nm > 1 && m[1].y > last && m[1].y < orows) last = m[1].y;
    /* last screen row worth keeping: the cursor row or the last with content */
    for (y = orows - 1; y > last; y--) {
        int x;
        for (x = 0; x < oc && row_is_blank_default(&t->row[0][y][x]); x++) {}
        if (x < oc) { last = y; break; }
    }
    for (L = from; L < t->hcount + last + 1; L++) {
        const Cell *c;
        int n, wrapped;
        if (L < t->hcount) {
            const HLine *h = &t->hist[(t->hstart + L) % HIST_LINES];
            c = h->c; n = h->n; wrapped = h->wrapped;
        } else {
            y = L - t->hcount;
            c = t->row[0][y]; n = oc; wrapped = t->wrapf[0][y] ? wrap_width(c, oc) : 0;
            for (k = 0; k < nm; k++)
                if (y == m[k].y) { m[k].line = 1; m[k].off = acc_n + m[k].x; }
        }
        if (acc_n + (wrapped > n ? wrapped : n) > acc_cap) {
            acc_cap = (acc_n + (wrapped > n ? wrapped : n)) * 2;
            acc = (Cell *)realloc(acc, sizeof(Cell) * acc_cap);
        }
        if (wrapped && n > wrapped) n = wrapped;   /* not the padding */
        if (n) memcpy(acc + acc_n, c, sizeof(Cell) * n);   /* (an empty line has no cells, and acc may be none yet) */
        acc_n += n;
        if (wrapped) {           /* a wrapped row's trailing blanks are real text, up to the width it wrapped at */
            for (; n < wrapped; n++) acc[acc_n++] = blank;
            continue;
        }
        /* end of a logical line: trim and split it at the new width */
        {
            int len = acc_n, pos = 0;
            while (len > 0 && row_is_blank_default(&acc[len - 1])) len--;
            do {
                int take = len - pos > cols ? cols : len - pos;
                if (take == cols && acc[pos + take - 1].wflag == W_WIDE) take--;  /* don't split a wide char */
                for (k = 0; k < nm; k++)
                    if (m[k].line == 1 && m[k].off >= pos && (m[k].off < pos + cols || pos + take >= len)) {
                        m[k].prow = out.n + (m[k].off - pos) / cols;
                        m[k].x = (m[k].off - pos) % cols;
                        m[k].line = 2;
                    }
                ll_add(&out, take ? acc + pos : NULL, take, pos + take < len ? take : 0);   /* take < cols: a wide char went on */
                pos += take;
            } while (pos < len);
            if (m[0].line == 2 && m[0].prow >= out.n) {  /* cursor beyond the text, on rows of its own */
                while (out.n <= m[0].prow) ll_add(&out, NULL, 0, 0);
            }
            if (nm > 1 && m[1].line == 2 && m[1].prow >= out.n) { m[1].prow = out.n - 1; m[1].x = cols - 1; }   /* a saved place adds no rows */
            for (k = 0; k < nm; k++)
                if (m[k].line == 2) m[k].line = 3;
            acc_n = 0;
        }
    }
    free(acc);
    /* screen = the last `rows` lines, but never scroll the cursor off the top */
    start = out.n - rows;
    if (start > m[0].prow) start = m[0].prow;
    if (start < 0) start = 0;
    /* history: lines before `from` stay as they are; the rewrapped rest follows */
    for (L = from; L < t->hcount; L++) {   /* hist_clear frees every slot, used or not: leave none dangling */
        HLine *h = &t->hist[(t->hstart + L) % HIST_LINES];
        free(h->c);
        h->c = NULL;
        h->n = h->cap = 0;
    }
    t->hcount = from;
    for (i = 0; i < start; i++) evicted += hist_append(t, out.v[i]);
    base += evicted;
    t->hpushed = base + t->hcount;
    {   /* prompt marks above the rewrapped part keep their lines; the rest are gone */
        int k, n = 0;
        for (k = 0; k < t->nmarks; k++)
            if (t->marks[k] >= base && t->marks[k] < base + from - evicted) t->marks[n++] = t->marks[k];
        t->nmarks = n;
    }
    for (y = 0; y < orows; y++) free(t->row[0][y]);
    free(t->row[0]);
    free(t->wrapf[0]);
    free(t->rlen[0]);
    t->row[0] = (Cell **)malloc(sizeof(Cell *) * rows);
    t->wrapf[0] = (uint8_t *)calloc((size_t)rows, 1);
    t->rlen[0] = (uint16_t *)calloc((size_t)rows, sizeof(uint16_t));
    for (y = 0; y < rows; y++) {
        int x, idx = start + y;
        t->row[0][y] = (Cell *)malloc(sizeof(Cell) * cols);
        for (x = 0; x < cols; x++) t->row[0][y][x] = blank;
        if (idx < out.n) {
            memcpy(t->row[0][y], out.v[idx].c, sizeof(Cell) * out.v[idx].n);
            t->wrapf[0][y] = out.v[idx].wrapped != 0;
            t->rlen[0][y] = (uint16_t)out.v[idx].n;
            if (out.v[idx].wrapped) {    /* a wide char went on: the rest of the row is padding, and counts as used */
                for (x = out.v[idx].wrapped; x < cols; x++) t->row[0][y][x].wflag = W_PAD;
                t->rlen[0][y] = (uint16_t)cols;
            }
            free(out.v[idx].c);
        }
    }
    free(out.v);
    for (k = nm - 1; k >= 0; k--) {
        int pend = 0;
        ccx = m[k].x;
        ccy = m[k].prow - start;
        if (m[k].pend) {         /* it sat after the char at its place: still after it, wrapping when that ends a row */
            if (++ccx >= cols) { ccx = cols - 1; pend = 1; }
        }
        if (ccy < 0) ccy = 0;
        if (ccy >= rows) ccy = rows - 1;
        if (ccx >= cols) ccx = cols - 1;
        if (k == 0 && !t->alt) { t->cx = ccx; t->cy = ccy; t->wrapnext = pend; }
        else { t->saved[0].cx = ccx; t->saved[0].cy = ccy; t->saved[0].wrapnext = pend; }
    }
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
    free(t->rlen[1]);
    t->row[1] = nr;
    t->wrapf[1] = nw;
    t->rlen[1] = (uint16_t *)malloc(sizeof(uint16_t) * rows);
    for (y = 0; y < rows; y++) t->rlen[1][y] = (uint16_t)cols;
    if (t->alt) {
        if (t->cy >= rows) t->cy = rows - 1;
        if (t->cx >= cols) t->cx = cols - 1;
    }
}

/* While a window is being resized only the newest REFLOW_TAIL lines of
   scrollback are rewrapped at each step, so a step costs the same with 20,000
   lines as with 200 (measured: 2.2 ms a step per full tab before, 14-16 ms
   for eight tabs). The rest catches up once, in term_settle, after the
   resizing stops; a height-only resize never needs it. */
#define REFLOW_TAIL 500
static int g_reflow_all;         /* tests: rewrap everything at every step, the old way */

static void term_resize(Term *t, int cols, int rows) {
    int s, x, from = 0;
    if (cols < 2) cols = 2;      /* a wide char needs two columns: with one, reflow would never get past it */
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows) return;
    if (t->hcount > REFLOW_TAIL && !g_reflow_all) {
        from = t->hcount - REFLOW_TAIL;
        /* back to a logical line's start, but not without end: one line can be
           the whole scrollback (minified JSON, base64). Cut mid-line, the rows
           above keep the width they wrapped at, which settle rejoins. */
        while (from > 0 && t->hcount - from < 2 * REFLOW_TAIL && t->hist[(t->hstart + from - 1) % HIST_LINES].wrapped) from--;
    }
    reflow_main(t, cols, rows, from);
    if (!from) t->hist_cols = cols;
    else if (cols != t->hist_cols) t->hist_stale = 1;
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
    if (t->alt) t->wrapnext = 0;  /* (the main screen's was carried over by reflow_main) */
    t->view = 0;
    t->dirty = 1;
}

/* The rest of the scrollback, rewrapped to the current width. Returns 1 if
   there was anything to do. */
static int term_settle(Term *t) {
    if (!t->hist_stale) return 0;
    reflow_main(t, t->cols, t->rows, 0);
    t->hist_cols = t->cols;
    t->hist_stale = 0;
    t->dirty = 1;
    return 1;
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
    if (t->cx > 0 && CELL(t, t->cx, t->cy)->wflag == W_CONT) {   /* splitting a wide char: both halves go */
        *CELL(t, t->cx - 1, t->cy) = b;
        *CELL(t, t->cx, t->cy) = b;
    }
    for (x = t->cols - 1; x >= t->cx + n; x--) *CELL(t, x, t->cy) = *CELL(t, x - n, t->cy);
    for (x = t->cx; x < t->cx + n; x++) *CELL(t, x, t->cy) = b;
    if (CELL(t, t->cols - 1, t->cy)->wflag == W_WIDE) *CELL(t, t->cols - 1, t->cy) = b;   /* its second half fell off */
    row_used(t, t->cy, t->cols);
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
        CELL(t, t->cx, t->cy)->wflag = W_PAD;
        t->wrapf[t->alt][t->cy] = 1;
        t->cx = 0;
        term_lf(t);
    }
    if (t->m_insert) insert_blanks(t, w);
    fix_wide(t, t->cx, w);
    c = CELL(t, t->cx, t->cy);
    row_used(t, t->cy, t->cx + w);
    if (t->cx + w >= t->cols) t->wrapf[t->alt][t->cy] = 0;   /* a new end: it wraps again only if more follows */
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
        row_used(t, y, t->cols);
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
            row_used(t, y, x + 1);
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

#define SUBP(t, i) ((i) < (t)->np && ((t)->sub >> (i) & 1))

static void sgr(Term *t) {
    int i;
    for (i = 0; i < t->np; i++) {
        int p = t->params[i];
        Cell *pen = &t->pen;
        if (SUBP(t, i + 1)) {    /* colon form: this code's sub-parameters follow it */
            int k = i + 1, n;
            while (SUBP(t, k)) k++;
            n = k - i - 1;
            if (p == 4) {        /* 4:0 off, 4:1..5 a style of underline (all drawn as one) */
                if (t->params[i + 1]) pen->attr |= A_UNDER;
                else pen->attr &= ~A_UNDER;
            } else if ((p == 38 || p == 48) && n >= 2) {
                const int *q = &t->params[i + 1];
                uint32_t col = 0;
                int ok = 1;
                if (q[0] == 5) col = C_PAL(q[1] & 255);
                else if (q[0] == 2 && n >= 4) col = C_RGB(q[n - 3] & 255, q[n - 2] & 255, q[n - 1] & 255);   /* 2:[cs]:r:g:b */
                else ok = 0;
                if (ok && p == 38) pen->fg = col;
                else if (ok) pen->bg = col;
            }
            i = k - 1;           /* 58 (underline color) and anything else: skipped whole */
            continue;
        }
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
        if (on && !t->m_sync) t->sync_since = GetTickCount();   /* a repeated BSU can't hold painting off forever */
        t->m_sync = on;
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
        if (t->cx > 0 && CELL(t, t->cx, t->cy)->wflag == W_CONT) *CELL(t, t->cx - 1, t->cy) = b;   /* its first half stays behind */
        for (x = t->cx; x < t->cols - n; x++) *CELL(t, x, t->cy) = *CELL(t, x + n, t->cy);
        for (x = t->cols - n; x < t->cols; x++) *CELL(t, x, t->cy) = b;
        if (CELL(t, t->cx, t->cy)->wflag == W_CONT) *CELL(t, t->cx, t->cy) = b;   /* a second half moved in alone */
        if (b.bg) row_used(t, t->cy, t->cols);
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
            snprintf(buf, sizeof(buf), "\x1b[6;%d;%dt", cell_h(g_tfont), cell_w(g_tfont));
            term_reply(t, buf);
        } else if (P(t, 0, 0) == 14) {   /* text area in pixels */
            snprintf(buf, sizeof(buf), "\x1b[4;%d;%dt", t->rows * cell_h(g_tfont), t->cols * cell_w(g_tfont));
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
    (void)tmp; (void)i;
    if (!p || !*p) return;       /* kept as reported: Linux paths from WSL, Windows paths from PowerShell */
    if (strcmp(t->cwd, p) != 0) {
        snprintf(t->cwd, sizeof(t->cwd), "%s", p);
        t->dirty = 1;
        t->side = 1;
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
    utf8_to_wide(buf, out, cap);
    for (i = 0; out[i]; i++) if (out[i] < 32) out[i] = L' ';   /* one line */
}

/* OSC 7501 ; state=S:app=A[:id=I][:kind=K][:progress=N][:title=B64][:msg=B64]
   Claude Code sends this once the terminal has answered "OSC 7501 ; ?".
   No id = the session itself; an id = one of its background tasks. */
static void program_status_(Term *t, char *s);
static void program_status(Term *t, char *s) {
    program_status_(t, s);
    t->side = 1;
}

static void program_status_(Term *t, char *s) {
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
    if (t->nmarks) t->had_activity = 1;
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
    if (!s) return;              /* nothing was stored */
    t->osc[t->osc_n] = 0;
    code = (int)strtol(s, &rest, 10);
    if (rest == s) return;
    if (*rest == ';') rest++;
    switch (code) {
    case 0: case 2:
        utf8_to_wide(rest, t->title, 256);
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
    case 7700:                   /* from rc.bash: mnt=<wslpath -u C:\>, when drives aren't under /mnt */
        if (!strncmp(rest, "claude=", 7)) {      /* claude=ID as Claude Code starts, claude= as it exits */
            snprintf(t->claude, sizeof(t->claude), "%s", rest + 7);
            if (!t->claude[0]) {
                t->claude_args[0] = 0;
                /* Claude Code has exited, maybe killed: what it turned on mustn't
                   outlive it in the shell (clicks as escape codes, focus reports,
                   painting held for a sync that never ends, no cursor) */
                t->m_mouse = t->m_mouse_sgr = t->m_focus = t->m_sync = 0;
                t->m_cursor_vis = 1;
            }
        } else if (!strncmp(rest, "cmd=", 4)) {
            snprintf(t->cmd, sizeof(t->cmd), "%s", rest + 4);
            if (t->cmd[0]) t->had_activity = 1;
            t->side = 1;
        } else if (!strncmp(rest, "claude-args=", 12)) {
            snprintf(t->claude_args, sizeof(t->claude_args), "%s", rest + 12);
        } else if (!strncmp(rest, "mnt=", 4) && rest[4] == '/') {
            char *e;
            snprintf(g_mnt_root, sizeof(g_mnt_root), "%s", rest + 4);
            e = g_mnt_root + strlen(g_mnt_root);
            if (e > g_mnt_root && e[-1] == '/') *--e = 0;          /* "/c/" → "/c" → "/" */
            while (e > g_mnt_root && e[-1] != '/') *--e = 0;
        }
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
            if (!out) break;
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
                /* A terminal attachment does not own the host clipboard.
                   Preserve the GPU backend's behavior without calling Cocoa
                   from the terminal session owner (including remote shells). */
                if (!g_term_reply_backend) set_clipboard_utf8((char *)out, n);
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
        t->sub = 0;
        t->csi_bad = 0;
        break;
    case ']': t->st = S_OSC; t->osc_n = t->osc_over = 0; break;
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
                if (--t->utf8_need == 0) {
                    uint32_t cp = t->utf8_acc;   /* overlong, a surrogate or past U+10FFFF: not a character */
                    term_put(t, cp < t->utf8_min || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF ? 0xFFFD : cp);
                }
                return;
            }
            t->utf8_need = 0;
            term_put(t, 0xFFFD);
        }
        if (b < 0x20 || b == 0x7F) { control(t, b); return; }
        if (b < 0x80) { term_put(t, b); return; }
        if ((b & 0xE0) == 0xC0) { t->utf8_acc = b & 0x1F; t->utf8_need = 1; t->utf8_min = 0x80; }
        else if ((b & 0xF0) == 0xE0) { t->utf8_acc = b & 0x0F; t->utf8_need = 2; t->utf8_min = 0x800; }
        else if ((b & 0xF8) == 0xF0) { t->utf8_acc = b & 0x07; t->utf8_need = 3; t->utf8_min = 0x10000; }
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
            if (t->np < MAX_PARAMS - 1) {
                t->params[++t->np] = 0;
                if (b == ':') t->sub |= 1u << t->np;
            }
        } else if (b >= '<' && b <= '?') {
            t->priv = b;
        } else if (b >= 0x20 && b <= 0x2F) {
            t->inter = b;
        } else if (b >= 0x40 && b <= 0x7E) {
            t->np++;
            t->st = S_GROUND;
            if (!t->csi_bad) csi_dispatch(t, b);
        } else if (b < 0x20) {
            control(t, b);       /* C0 inside CSI executes in place */
        } else if (b >= 0x80) {
            t->csi_bad = 1;      /* (DEL, 0x7F, is ignored where it stands) */
        }
        return;
    case S_OSC:
        if (b == 0x07) { if (!t->osc_over) osc_dispatch(t); t->st = S_GROUND; }
        else if (b == 0x1B) t->st = S_OSC_ESC;
        else if (b == 0x18 || b == 0x1A) t->st = S_GROUND;   /* CAN, SUB: abandoned */
        else if (b < 0x20) {}                                 /* other C0: not part of the string */
        else if (t->osc_n + 1 < t->osc_cap) t->osc[t->osc_n++] = (char)b;
        else if (!t->osc_over) {
            int cap = t->osc_cap ? t->osc_cap * 4 : 1024;
            char *more = cap <= OSC_MAX ? (char *)realloc(t->osc, (size_t)cap) : NULL;
            if (more) { t->osc = more; t->osc_cap = cap; t->osc[t->osc_n++] = (char)b; }
            else t->osc_over = 1;   /* too long to keep: never act on a truncated one */
        }
        return;
    case S_OSC_ESC:
        if (!t->osc_over) osc_dispatch(t);
        t->st = S_GROUND;
        if (b != '\\') { t->st = S_ESC; vt_byte(t, b); }   /* ESC ends the string and starts a sequence */
        return;
    case S_STR:
        if (b == 0x1B) t->st = S_STR_ESC;
        else if (b == 0x07 || b == 0x18 || b == 0x1A) t->st = S_GROUND;
        return;
    case S_STR_ESC:
        t->st = S_GROUND;
        if (b != '\\') { t->st = S_ESC; vt_byte(t, b); }   /* as in an OSC: a stray DCS can't swallow what follows */
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
                if (t->cx > t->rlen[t->alt][t->cy]) t->rlen[t->alt][t->cy] = (uint16_t)t->cx;
                t->last_ch = b;
                continue;
            }
        }
        /* fast path: a whole, valid UTF-8 character (CJK, box drawing) in the middle of a row */
        if (b >= 0xC2 && b <= 0xF4 && t->st == S_GROUND && !t->utf8_need && !t->wrapnext && !t->m_insert) {
            int len = b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4, k, w;
            uint32_t cp = b & (len == 2 ? 0x1F : len == 3 ? 0x0F : 0x07);
            if (i + len <= n) {
                for (k = 1; k < len && (s[i + k] & 0xC0) == 0x80; k++) cp = cp << 6 | (s[i + k] & 0x3F);
                if (k == len && cp >= (len == 2 ? 0x80u : len == 3 ? 0x800u : 0x10000u) &&
                    !(cp >= 0xD800 && cp <= 0xDFFF) && cp <= 0x10FFFF && (w = cp_width(cp)) > 0 &&
                    t->cx + w < t->cols) {
                    Cell *c = &t->row[t->alt][t->cy][t->cx];
                    if (!c->wflag && (w == 1 || !c[1].wflag)) {
                        *c = t->pen;
                        c->ch = cp;
                        if (w == 2) { c[1] = t->pen; c[1].ch = 0; c[1].wflag = W_CONT; c->wflag = W_WIDE; }
                        t->cx += w;
                        if (t->cx > t->rlen[t->alt][t->cy]) t->rlen[t->alt][t->cy] = (uint16_t)t->cx;
                        t->last_ch = cp;
                        i += len - 1;
                        continue;
                    }
                }
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
        *wrapped = t->wrapf[t->alt][y] ? wrap_width(*cells, t->cols) : 0;
    }
}

static long long term_base(const Term *t) { return t->hpushed - t->hcount; }
static long long term_abs_line(const Term *t, int y) { return t->hpushed + y; }

/* The main screen and its scrollback, the newest max_rows rows of it, back
   out as text with its colors: what a restored tab replays. Wrapped rows run
   on (so they rewrap at whatever width the next window has); wide-char gaps
   (W_PAD) are left out, the replay makes them again. */
static void sgr_color(char *o, size_t cap, uint32_t spec, int bg) {
    if (!spec) o[0] = 0;
    else if (spec & 0x01000000u) snprintf(o, cap, ";%d;5;%u", bg ? 48 : 38, spec & 255);
    else snprintf(o, cap, ";%d;2;%u;%u;%u", bg ? 48 : 38, (spec >> 16) & 255, (spec >> 8) & 255, spec & 255);
}

static int utf8_encode(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 63)); o[2] = (char)(0x80 | (cp & 63)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 63)); o[2] = (char)(0x80 | ((cp >> 6) & 63)); o[3] = (char)(0x80 | (cp & 63));
    return 4;
}


#ifdef _WIN32
static void term_dump(Term *t, FILE *f, int max_rows) {
    static const int abit[8] = {1, 2, 3, 4, 5, 7, 8, 9};     /* A_BOLD.. A_STRIKE → SGR */
    int alt = t->alt, last, L, first, total;
    uint32_t pfg = 0, pbg = 0;
    uint8_t pat = 0;
    t->alt = 0;                  /* the main screen, even under a full-screen program */
    last = alt ? t->saved[0].cy : t->cy;
    for (L = t->rows - 1; L > last; L--) {      /* down to the cursor or the last row with text */
        int x;
        for (x = 0; x < t->rlen[0][L] && row_is_blank_default(&t->row[0][L][x]); x++) {}
        if (x < t->rlen[0][L]) { last = L; break; }
    }
    total = t->hcount + last + 1;
    first = total > max_rows ? total - max_rows : 0;
    for (L = first; L < total; L++) {
        const Cell *c;
        int n, wr, x;
        term_line(t, L, &c, &n, &wr);
        if (!wr) while (n > 0 && row_is_blank_default(&c[n - 1])) n--;
        for (x = 0; c && x < n; x++) {
            char ch[8], sg[96], fs[32], bs[32];
            int k, m;
            if (c[x].wflag == W_CONT || c[x].wflag == W_PAD) continue;
            if (c[x].fg != pfg || c[x].bg != pbg || c[x].attr != pat) {
                m = snprintf(sg, sizeof(sg), "\x1b[0");
                for (k = 0; k < 8; k++) if (c[x].attr & (1 << k)) m += snprintf(sg + m, sizeof(sg) - m, ";%d", abit[k]);
                sgr_color(fs, sizeof(fs), c[x].fg, 0);
                sgr_color(bs, sizeof(bs), c[x].bg, 1);
                snprintf(sg + m, sizeof(sg) - m, "%s%sm", fs, bs);
                fputs(sg, f);
                pfg = c[x].fg; pbg = c[x].bg; pat = c[x].attr;
            }
            fwrite(ch, 1, (size_t)utf8_encode(c[x].ch ? c[x].ch : ' ', ch), f);
        }
        if (!wr || L == total - 1) {
            fputs(pfg || pbg || pat ? "\x1b[0m\r\n" : "\r\n", f);
            pfg = pbg = 0; pat = 0;
        }
    }
    t->alt = alt;
}

#endif

/* ── a terminal's whole state as text ──────────────────────────────────
   For handing a tab to a newer hardcore without stopping what runs in it:
   the screens, scrollback, cursor, scroll region, colors, links, tab stops
   and modes, written as the escape sequences that build them. Replayed into
   a fresh Term of the same size it gives back the same cells, row for row,
   whatever the new build's structs look like. Programs (Claude Code, ConPTY
   itself) keep a model of the screen and send only changes against it, so
   "the same" has to mean exactly. term_snapshot_check() in --unit holds it
   to that. */
typedef struct { char *p; size_t n, cap; } SBuf;

static void sb_put(SBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        char *q = (char *)realloc(b->p, cap);
        if (!q) return;
        b->p = q;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_printf(SBuf *b, const char *fmt, ...) {
    char tmp[1200];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(b, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
}

typedef struct { uint32_t fg, bg; uint8_t attr; uint16_t link; } SnapPen;

static void snap_sgr(SBuf *b, const Term *t, SnapPen *cur, uint32_t fg, uint32_t bg, uint8_t attr, uint16_t link) {
    static const int abit[8] = {1, 2, 3, 4, 5, 7, 8, 9};
    if (fg != cur->fg || bg != cur->bg || attr != cur->attr) {
        char fs[32] = "", bs[32] = "";
        int k;
        sb_put(b, "\x1b[0", 3);
        for (k = 0; k < 8; k++) if (attr & (1 << k)) sb_printf(b, ";%d", abit[k]);
        sgr_color(fs, sizeof(fs), fg, 0);
        sgr_color(bs, sizeof(bs), bg, 1);
        sb_printf(b, "%s%sm", fs, bs);
        cur->fg = fg; cur->bg = bg; cur->attr = attr;
    }
    if (link != cur->link) {
        const char *u = link && link <= MAX_LINKS && t->links[link - 1] ? t->links[link - 1] : "";
        sb_printf(b, "\x1b]8;;%s\x1b\\", u);
        cur->link = link;
    }
}

/* one row's cells; trailing default blanks left out unless the row wraps */
static void snap_cells(SBuf *b, const Term *t, SnapPen *cur, const Cell *c, int n, int wrapped) {
    int x;
    char u[8];
    if (!wrapped) while (n > 0 && row_is_blank_default(&c[n - 1]) && !c[n - 1].link) n--;
    for (x = 0; c && x < n; x++) {
        if (c[x].wflag == W_CONT || c[x].wflag == W_PAD) continue;   /* the replay makes these */
        snap_sgr(b, t, cur, c[x].fg, c[x].bg, c[x].attr, c[x].link);
        sb_put(b, u, (size_t)utf8_encode(c[x].ch ? c[x].ch : ' ', u));
    }
}

/* CUP to (x, y), 0-based, as the current origin mode reads it */
static void snap_cup(SBuf *b, int x, int y) { sb_printf(b, "\x1b[%d;%dH", y + 1, x + 1); }

static void term_snapshot(Term *t, SBuf *b) {
    SnapPen cur = {0, 0, 0, 0};
    int alt = t->alt, L, y, x, total;
    term_settle(t);                          /* scrollback all wrapped at today's width */
    sb_put(b, "\x1b[0m", 4);
    /* scrollback, then every main-screen row, as one flow at this width */
    t->alt = 0;
    total = t->hcount + t->rows;
    for (L = 0; L < total; L++) {
        const Cell *c;
        int n, wr;
        term_line(t, L, &c, &n, &wr);
        if (L == total - 1) wr = 0;          /* the last row: nothing below to wrap into */
        snap_cells(b, t, &cur, c, n, wr);
        if (!wr && L < total - 1) {
            snap_sgr(b, t, &cur, 0, 0, 0, 0);
            sb_put(b, "\r\n", 2);
        }
    }
    snap_sgr(b, t, &cur, 0, 0, 0, 0);
    if (alt) {
        /* the main screen's cursor as the program left it, saved by ?1049h with its pen */
        const SavedCursor *s = &t->saved[0];
        snap_cup(b, s->cx, s->cy);
        snap_sgr(b, t, &cur, s->pen.fg, s->pen.bg, s->pen.attr, 0);
        sb_put(b, s->g0dec ? "\x1b(0" : "\x1b(B", 3);
        sb_put(b, "\x1b[?1049h\x1b(B", 11);
        snap_sgr(b, t, &cur, 0, 0, 0, 0);
        sb_put(b, "\x1b[H", 3);
        t->alt = 1;
        for (y = 0; y < t->rows; y++) {
            int wr = t->wrapf[1][y] && y < t->rows - 1;
            snap_cells(b, t, &cur, t->row[1][y], t->rlen[1][y] < t->cols ? t->rlen[1][y] : t->cols, wr);
            if (!wr && y < t->rows - 1) { snap_sgr(b, t, &cur, 0, 0, 0, 0); sb_put(b, "\r\n", 2); }
        }
        snap_sgr(b, t, &cur, 0, 0, 0, 0);
    }
    t->alt = alt;
    /* this screen's DECSC slot (for the alt screen ?1049h already filled it from the main one) */
    {
        const SavedCursor *s = &t->saved[alt];
        if (!alt || memcmp(&t->saved[1], &t->saved[0], sizeof(SavedCursor))) {
            /* origin mode homes the cursor when it's set: set it first, then a CUP
               relative to the scroll region (which isn't set yet: the whole screen) */
            if (s->origin) sb_put(b, "\x1b[?6h", 5);
            snap_cup(b, s->cx, s->cy);
            snap_sgr(b, t, &cur, s->pen.fg, s->pen.bg, s->pen.attr, 0);
            sb_put(b, s->g0dec ? "\x1b(0" : "\x1b(B", 3);
            sb_put(b, "\x1b" "7", 2);
            if (s->origin) sb_put(b, "\x1b[?6l", 5);
            sb_put(b, "\x1b(B", 3);
        }
    }
    /* tab stops, if they aren't every 8 */
    for (x = 0; x < t->cols && t->tabs[x] == (x % 8 == 0); x++) {}
    if (x < t->cols) {
        sb_put(b, "\x1b[3g", 4);
        for (x = 0; x < t->cols; x++) if (t->tabs[x]) { snap_cup(b, x, 0); sb_put(b, "\x1bH", 2); }
    }
    /* modes; autowrap last (drawing needed it), then the scroll region, origin, pen, cursor */
    sb_printf(b, "\x1b[?1%c\x1b[?25%c\x1b[?1004%c\x1b[?1006%c\x1b[?2004%c",
              t->m_appcursor ? 'h' : 'l', t->m_cursor_vis ? 'h' : 'l', t->m_focus ? 'h' : 'l',
              t->m_mouse_sgr ? 'h' : 'l', t->m_bracketed ? 'h' : 'l');
    if (t->m_mouse) sb_printf(b, "\x1b[?%dh", t->m_mouse);
    sb_printf(b, "\x1b[%d q", t->cursor_shape == 1 ? 4 : t->cursor_shape == 2 ? 6 : 2);
    if (t->top != 0 || t->bot != t->rows - 1) sb_printf(b, "\x1b[%d;%dr", t->top + 1, t->bot + 1);
    if (!t->m_autowrap) sb_put(b, "\x1b[?7l", 5);
    if (t->m_origin) sb_put(b, "\x1b[?6h", 5);
    sb_put(b, t->g0dec ? "\x1b(0" : "\x1b(B", 3);
    if (t->wrapnext && t->cx == t->cols - 1 && t->m_autowrap) {
        /* a pending wrap: print the last cell again, which leaves exactly that */
        const Cell *c = &t->row[alt][t->cy][t->cx];
        char u[8];
        snap_cup(b, t->cx, t->m_origin ? t->cy - t->top : t->cy);
        snap_sgr(b, t, &cur, c->fg, c->bg, c->attr, c->link);
        if (t->g0dec) sb_put(b, "\x1b(B", 3);
        sb_put(b, u, (size_t)utf8_encode(c->ch > ' ' ? c->ch : ' ', u));
        if (t->g0dec) sb_put(b, "\x1b(0", 3);
    } else snap_cup(b, t->cx, t->m_origin ? t->cy - t->top : t->cy);
    if (t->m_insert) sb_put(b, "\x1b[4h", 4);      /* after the reprint above: insert mode would shift it */
    if (t->m_newline) sb_put(b, "\x1b[20h", 5);
    snap_sgr(b, t, &cur, t->pen.fg, t->pen.bg, t->pen.attr, t->pen.link);
}

static uint32_t resolve(uint32_t spec, int is_fg, int bold) {
    if (!spec) return is_fg ? (bold ? g_look.bold : DEF_FG) : DEF_BG;
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

#ifndef HARDCORE_TTY_ONLY

/* --unit: term_snapshot replayed into a fresh Term gives back the same cells
   and state. Returns 0 if equal, else describes the first difference. */
static int cell_same(const Term *a, const Cell *x, const Term *b, const Cell *y) {
    const char *la = x->link && a->links[x->link - 1] ? a->links[x->link - 1] : "";
    const char *lb = y->link && b->links[y->link - 1] ? b->links[y->link - 1] : "";
    return x->ch == y->ch && x->fg == y->fg && x->bg == y->bg && x->attr == y->attr && x->wflag == y->wflag && !strcmp(la, lb);
}

static int snapshot_roundtrip(Term *a, char *why, size_t cap) {
    SBuf b = {0};
    Term t;
    int L, s, y, x, ok = 1;
    term_snapshot(a, &b);
    if (!term_init(&t, a->cols, a->rows)) { snprintf(why, cap, "init"); return 1; }
    vt_feed(&t, (const unsigned char *)b.p, (int)b.n);
    free(b.p);
#define DIFF(cond, ...) do { if (ok && (cond)) { snprintf(why, cap, __VA_ARGS__); ok = 0; } } while (0)
    DIFF(t.hcount != a->hcount, "history %d vs %d rows", t.hcount, a->hcount);
    for (L = 0; ok && L < a->hcount; L++) {
        const Cell *c1, *c2;
        int n1, n2, w1, w2, ia = a->alt, it = t.alt;
        a->alt = t.alt = 0;
        term_line(a, L, &c1, &n1, &w1);
        term_line(&t, L, &c2, &n2, &w2);
        a->alt = ia; t.alt = it;
        while (n1 > 0 && row_is_blank_default(&c1[n1 - 1]) && !w1) n1--;
        while (n2 > 0 && row_is_blank_default(&c2[n2 - 1]) && !w2) n2--;
        DIFF(n1 != n2 || w1 != w2, "history row %d: %d cells wrapped=%d vs %d cells wrapped=%d", L, n2, w2, n1, w1);
        for (x = 0; ok && x < n1; x++) DIFF(!cell_same(a, &c1[x], &t, &c2[x]), "history row %d col %d: U+%04X vs U+%04X", L, x, c2[x].ch, c1[x].ch);
    }
    for (s = 0; s < 2; s++)
        for (y = 0; ok && y < a->rows; y++) {
            for (x = 0; ok && x < a->cols; x++)
                DIFF(!cell_same(a, &a->row[s][y][x], &t, &t.row[s][y][x]), "%s row %d col %d: U+%04X fg %x attr %x vs U+%04X fg %x attr %x",
                     s ? "alt" : "main", y, x, t.row[s][y][x].ch, t.row[s][y][x].fg, t.row[s][y][x].attr,
                     a->row[s][y][x].ch, a->row[s][y][x].fg, a->row[s][y][x].attr);
            DIFF(!s && y < a->rows - 1 && t.wrapf[0][y] != a->wrapf[0][y], "main row %d wrap flag", y);
        }
    DIFF(t.alt != a->alt, "alt screen");
    DIFF(t.cx != a->cx || t.cy != a->cy || t.wrapnext != a->wrapnext, "cursor %d,%d wrap %d vs %d,%d wrap %d", t.cx, t.cy, t.wrapnext, a->cx, a->cy, a->wrapnext);
    DIFF(t.pen.fg != a->pen.fg || t.pen.bg != a->pen.bg || t.pen.attr != a->pen.attr, "pen");
    for (s = 0; s < 2; s++)
        DIFF(t.saved[s].cx != a->saved[s].cx || t.saved[s].cy != a->saved[s].cy || t.saved[s].g0dec != a->saved[s].g0dec ||
             t.saved[s].origin != a->saved[s].origin || t.saved[s].pen.fg != a->saved[s].pen.fg || t.saved[s].pen.attr != a->saved[s].pen.attr,
             "saved cursor %d: %d,%d vs %d,%d", s, t.saved[s].cx, t.saved[s].cy, a->saved[s].cx, a->saved[s].cy);
    DIFF(t.top != a->top || t.bot != a->bot, "scroll region %d-%d vs %d-%d", t.top, t.bot, a->top, a->bot);
    DIFF(t.g0dec != a->g0dec, "charset");
    DIFF(t.m_autowrap != a->m_autowrap || t.m_origin != a->m_origin || t.m_insert != a->m_insert || t.m_cursor_vis != a->m_cursor_vis ||
         t.m_appcursor != a->m_appcursor || t.m_bracketed != a->m_bracketed || t.m_mouse != a->m_mouse || t.m_mouse_sgr != a->m_mouse_sgr ||
         t.m_focus != a->m_focus || t.m_newline != a->m_newline, "modes");
    DIFF(t.cursor_shape != a->cursor_shape, "cursor shape");
    DIFF(memcmp(t.tabs, a->tabs, (size_t)a->cols), "tab stops");
#undef DIFF
    term_free(&t);
    return !ok;
}

static int term_snapshot_check(FILE *f) {
    static const char *names[3] = {"shell", "full-screen program", "odd modes"};
    int k, fails = 0;
    for (k = 0; k < 3; k++) {
        Term t;
        char why[256] = "", line[512];
        int i, n;
        if (!term_init(&t, 40, 12)) return 1;
        for (i = 0; i < 300; i++) {             /* scrollback: colors, long lines, CJK at the edge */
            n = snprintf(line, sizeof(line), "\x1b[3%dmline %d\x1b[0m %s%s\r\n", i % 8, i,
                         i % 7 == 0 ? "a long line that wraps around the forty column edge and then some more" : "",
                         i % 11 == 0 ? "x\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5" : "");
            vt_feed(&t, (const unsigned char *)line, n);
        }
        vt_feed(&t, (const unsigned char *)"\x1b]8;;https://example.com\x1b\\link\x1b]8;;\x1b\\ \x1b[44m\x1b[K\x1b[0m\r\n$ ", 54);
        if (k == 0) { const char *s = "\x1b[5;9H\x1b[1;32mmid\x1b[0m\x1b[6;40HZ"; vt_feed(&t, (const unsigned char *)s, (int)strlen(s)); }   /* ends on a pending wrap */
        if (k == 1) {
            const char *s = "\x1b[3;5H\x1b" "7\x1b[?1049h\x1b[H\x1b[2J\x1b[1;31mtitle bar\x1b[0m\r\n\x1b[7m inverse \x1b[0m"
                            "\x1b[?1002h\x1b[?1006h\x1b[?2004h\x1b[?1h\x1b[?25l\x1b[2;10r\x1b[4;3H\x1b" "7\x1b[6;1H"
                            "\x1b[12;30Hend of the last row:0123456789\x1b[1;31m";
            vt_feed(&t, (const unsigned char *)s, (int)strlen(s));
        }
        if (k == 2) {
            const char *s = "\x1b[3g\x1b[1;5H\x1bH\x1b[1;13H\x1bH\x1b[3;8r\x1b[?6h\x1b[2;4H\x1b(0lqk\x1b[4h\x1b[6 q";
            vt_feed(&t, (const unsigned char *)s, (int)strlen(s));
        }
        if (snapshot_roundtrip(&t, why, sizeof(why))) {
            fprintf(f, "FAIL snapshot %s: %s\n", names[k], why);
            fails++;
        } else fprintf(f, "ok   snapshot %s round trip (history %d, cursor %d,%d)\n", names[k], t.hcount, t.cx, t.cy);
        term_free(&t);
    }
    return fails;
}

/* ── tabs ──────────────────────────────────────────────────────────── */

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

static void term_message(Term *t, const char *msg) {
    vt_feed(t, (const unsigned char *)msg, (int)strlen(msg));
}

#ifdef _WIN32
/* ── ConPTY + reader thread ───────────────────────────────────────── */

#define WM_APP_OUTPUT (WM_APP + 1)
#define WM_APP_EXITED (WM_APP + 2)
#define WM_APP_SETTINGS (WM_APP + 3)     /* another window changed the font or scheme */
#define WM_APP_HANDED_OVER (WM_APP + 4)  /* a reload went through: end this process now */

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
/* HARDCORE_VTLOG=1: everything ConPTY hands us, raw, to %TEMP%\\hardcore-vt.log,
   and "<ms since start> <offset> <bytes>" per read to hardcore-vt.idx, so a
   recording can be cut back into the chunks it arrived in */
static FILE *g_vtlog, *g_vtidx;
static long long g_vtoff;

static int tab_drain(Tab *tb) {
    char *data;
    int n;
    if (tb->slurp_hwnd) return slurp_drain(tb);
    EnterCriticalSection(&tb->lock);
    data = tb->in;
    n = tb->in_n;
    tb->in = NULL;
    tb->in_n = tb->in_cap = 0;
    InterlockedExchange(&tb->posted, 0);
    LeaveCriticalSection(&tb->lock);
    if (n && g_vtlog) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        fwrite(data, 1, (size_t)n, g_vtlog);
        fflush(g_vtlog);
        if (g_vtidx) { fprintf(g_vtidx, "%.2f %lld %d\n", lat_ms(g_t0, now), g_vtoff, n); fflush(g_vtidx); }
        g_vtoff += n;
    }
    if (n && g_perf) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        vt_feed(&tb->t, (unsigned char *)data, n);
        QueryPerformanceCounter(&b);
        g_pf.parse += lat_ms(a, b);
        g_pf.bytes += n;
        g_pf.drains++;
        perf_tick();
    } else if (n) vt_feed(&tb->t, (unsigned char *)data, n);
    free(data);
    return n;
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
    "# drives mounted somewhere other than /mnt (wsl.conf automount root): tell hardcore where\n"
    "[ -d /mnt/c ] || printf '\\033]7700;mnt=%s\\033\\\\' \"$(wslpath -u 'C:\\' 2>/dev/null)\"\n"
    "# tell programs what this terminal can do (WSL doesn't carry WT_SESSION-style hints):\n"
    "# 24-bit color (chalk/supports-color, so Claude Code's theme isn't cut to 256 colors)\n"
    "# and OSC 8 hyperlinks (supports-hyperlinks)\n"
    "export COLORTERM=truecolor TERM_PROGRAM=hardcore FORCE_HYPERLINK=1\n"
    "# a pre-warmed spare tab: profile already loaded, wait (silently) to be told where to go\n"
    "if [ -n \"$HARDCORE_SPARE\" ]; then\n"
    "  unset HARDCORE_SPARE\n"
    "  IFS= read -rs __hardcore_dir\n"
    "  case $__hardcore_dir in [A-Za-z]:*) __hardcore_dir=$(wslpath -u \"$__hardcore_dir\") ;; esac\n"
    "  cd -- \"$__hardcore_dir\" 2>/dev/null\n"
    "  unset __hardcore_dir\n"
    "fi\n"
    "__hardcore_cwd() { printf '\\033]9;9;%s\\033\\\\\\033]133;A\\033\\\\' \"$PWD\"; }\n"
    "# report the folder on every cd too, so \"cd x && claude\" labels the tab x right away\n"
    "if ! declare -F cd >/dev/null; then\n"
    "  cd() { builtin cd \"$@\" && __hardcore_cwd; }\n"
    "  pushd() { builtin pushd \"$@\" && __hardcore_cwd; }\n"
    "  popd() { builtin popd \"$@\" && __hardcore_cwd; }\n"
    "fi\n"
    "# at each prompt: the folder, nothing running any more, and (once, in a restored\n"
    "# tab) the command to put in history, so up-arrow offers it\n"
    "__hardcore_prompt() {\n"
    "  __hardcore_cwd\n"
    "  printf '\\033]7700;cmd=\\033\\\\'\n"
    "  if [ -n \"$__hardcore_once\" ]; then history -s \"$__hardcore_once\"; unset __hardcore_once; fi\n"
    "}\n"
    "case \";${PROMPT_COMMAND[0]};\" in *\";__hardcore_prompt;\"*) ;; *)\n"
    "  PROMPT_COMMAND=\"__hardcore_prompt${PROMPT_COMMAND:+;$PROMPT_COMMAND}\" ;;\n"
    "esac\n"
    "# as each command starts (PS0): the line as typed, for a restored tab to offer again\n"
    "__hardcore_preexec() {\n"
    "  local c; c=$(fc -ln -0 2>/dev/null); c=${c//$'\\n'/ }; c=${c#\"${c%%[![:space:]]*}\"}\n"
    "  printf '\\033]7700;cmd=%.400s\\033\\\\' \"$c\"\n"
    "}\n"
    "case $PS0 in *__hardcore_preexec*) ;; *) PS0=\"${PS0}\\$(__hardcore_preexec)\" ;; esac\n"
    "[ -n \"$HARDCORE_HIST\" ] && { __hardcore_once=$HARDCORE_HIST; unset HARDCORE_HIST; }\n"
    "# hardcore itself: `hardcore` opens a new window here (or in DIR), `hardcore update`\n"
    "# installs the latest release, `hardcore --version` says which one this is\n"
    "# and as a command in every WSL shell (Claude Code's ! too): one symlink to the\n"
    "# installed exe in ~/.local/bin, unless something called hardcore is already there\n"
    "case $HARDCORE_EXE in */hardcore/hardcore.exe)\n"
    "  [ -e ~/.local/bin/hardcore ] || [ -L ~/.local/bin/hardcore ] || { mkdir -p ~/.local/bin && ln -s \"$HARDCORE_EXE\" ~/.local/bin/hardcore; } 2>/dev/null ;;\n"
    "esac\n"
    "if [ -n \"$HARDCORE_EXE\" ] && ! declare -F hardcore >/dev/null; then\n"
    "  hardcore() {\n"
    "    case $1 in\n"
    "      here) echo 'Native terminal mode: run a Linux build of hardcore here in WSL.' >&2; return 2 ;;\n"
    "      update|--update|version|--version|-v|help|--help|-h) \"$HARDCORE_EXE\" \"$@\" | cat ;;\n"
    "      *) local d; d=$(cd -- \"${1:-.}\" 2>/dev/null && pwd) || { echo \"hardcore: no folder $1\" >&2; return 1; }\n"
    "         ( \"$HARDCORE_EXE\" --cd \"$d\" </dev/null >/dev/null 2>&1 & ) ;;\n"
    "    esac\n"
    "  }\n"
    "fi\n"
    "# Claude Code: tell hardcore which session runs in this tab (OSC 7700;claude=ID), so\n"
    "# closing hardcore and opening it again resumes it. New sessions get their id up\n"
    "# front (--session-id); --resume ID is known; --continue is looked up; subcommands,\n"
    "# -p and the like pass straight through.\n"
    "__hardcore_claude_mode() {\n"
    "  local a prev= id= mode=new\n"
    "  case $1 in [a-z]*) case $1 in *[!a-z-]*) ;; *) mode=sub ;; esac ;; esac\n"
    "  for a in \"$@\"; do\n"
    "    case $prev in -r|--resume) case $a in -*) ;; *) id=$a; mode=known ;; esac ;; --session-id) id=$a; mode=known ;; esac\n"
    "    case $a in\n"
    "      -p|--print|-h|--help|-v|--version) mode=sub ;;\n"
    "      --resume=*|--session-id=*) id=${a#*=}; mode=known ;;\n"
    "      -r|--resume) [ \"$mode\" = known ] || mode=pick ;;\n"
    "      -c|--continue) [ \"$mode\" = known ] || mode=continue ;;\n"
    "      --fork-session) mode=pick ;;\n"
    "    esac\n"
    "    prev=$a\n"
    "  done\n"
    "  printf '%s %s\\n' \"$mode\" \"$id\"\n"
    "}\n"
    "# the flags to start it with again (not the session ones, not the first prompt),\n"
    "# shell-quoted; how many words each flag takes follows claude --help\n"
    "__hardcore_claude_keep() {\n"
    "  local a take=0 opt=0 var=0 drop=0 out=()\n"
    "  for a in \"$@\"; do\n"
    "    if [ $take = 1 ]; then out+=(\"$a\"); take=0; continue; fi\n"
    "    if [ $opt = 1 ]; then opt=0; case $a in -*) ;; *) out+=(\"$a\"); continue ;; esac; fi\n"
    "    if [ $var = 1 ]; then case $a in -*) var=0 ;; *) out+=(\"$a\"); continue ;; esac; fi\n"
    "    if [ $drop = 1 ]; then drop=0; case $a in -*) ;; *) continue ;; esac; fi\n"
    "    case $a in\n"
    "      --session-id|--resume|-r) drop=1 ;;\n"
    "      --session-id=*|--resume=*|-c|--continue|--fork-session) ;;\n"
    "      --add-dir|--betas|--file|--mcp-config|--tools|--allowedTools|--allowed-tools|--disallowedTools|--disallowed-tools) out+=(\"$a\"); var=1 ;;\n"
    "      --agent|--agents|--append-system-prompt|--autocompact|--debug-file|--effort|--environment|--fallback-model|--input-format|--json-schema|--max-budget-usd|--model|-n|--name|--output-format|--permission-mode|--permission-prompts|--plugin-dir|--plugin-url|--setting-sources|--settings|--system-prompt|--system-prompt-snapshot) out+=(\"$a\"); take=1 ;;\n"
    "      -d|--debug|--cloud|--from-pr|--prompt-suggestions|--remote-control|--teleport|-w|--worktree) out+=(\"$a\"); opt=1 ;;\n"
    "      -*) out+=(\"$a\") ;;\n"
    "    esac\n"
    "  done\n"
    "  [ ${#out[@]} -gt 0 ] && printf '%q ' \"${out[@]}\"\n"
    "}\n"
    "if ! declare -F claude >/dev/null && ! alias claude >/dev/null 2>&1; then\n"
    "  claude() {\n"
    "    local m id d rc keep\n"
    "    read -r m id < <(__hardcore_claude_mode \"$@\")\n"
    "    keep=$(__hardcore_claude_keep \"$@\")\n"
    "    case $m in\n"
    "      sub) command claude \"$@\"; return ;;\n"
    "      new) id=$(cat /proc/sys/kernel/random/uuid); set -- --session-id \"$id\" \"$@\" ;;\n"
    "      continue) d=~/.claude/projects/$(printf '%s' \"$PWD\" | sed 's/[^A-Za-z0-9]/-/g')\n"
    "                id=$(ls -t \"$d\"/*.jsonl 2>/dev/null | head -1); id=${id##*/}; id=${id%.jsonl} ;;\n"
    "      pick) id= ;;\n"
    "    esac\n"
    "    printf '\\033]7700;claude-args=%s\\033\\\\\\033]7700;claude=%s\\033\\\\' \"$keep\" \"${id:-?}\"\n"
    "    command claude \"$@\"; rc=$?\n"
    "    printf '\\033]7700;claude=\\033\\\\'\n"
    "    return $rc\n"
    "  }\n"
    "fi\n"
    "# a restored tab: hardcore hands over a command to run first (claude --resume ...)\n"
    "if [ -n \"$HARDCORE_RUN\" ]; then\n"
    "  __hardcore_run=$HARDCORE_RUN; unset HARDCORE_RUN\n"
    "  # a session closed before its first message has no transcript yet: start a fresh one\n"
    "  case $__hardcore_run in *\" --resume \"*)\n"
    "    ls ~/.claude/projects/*/\"${__hardcore_run##* --resume }.jsonl\" >/dev/null 2>&1 || __hardcore_run=${__hardcore_run% --resume *} ;;\n"
    "  esac\n"
    "  __hardcore_once=$__hardcore_run; eval \"$__hardcore_run\"; unset __hardcore_run\n"
    "fi\n";

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
    (void)wpath;
    /* WSLENV's /p flag hands rc.bash's path to Linux translated for whatever
       mount root the distro uses; no /mnt assumption on our side */
    {
        wchar_t old[2048] = L"", wslenv[2200];
        DWORD n = GetEnvironmentVariableW(L"WSLENV", old, 2048);
        wchar_t exe[MAX_PATH];
        SetEnvironmentVariableW(L"HARDCORE_RC", path);
        GetModuleFileNameW(NULL, exe, MAX_PATH);   /* for rc.bash's `hardcore` (new window, update) */
        SetEnvironmentVariableW(L"HARDCORE_EXE", exe);
        _snwprintf(wslenv, 2200, L"%ls%lsHARDCORE_RC/p:HARDCORE_EXE/p", old, n ? L":" : L"");
        wslenv[2199] = 0;
        SetEnvironmentVariableW(L"WSLENV", wslenv);
    }
    {   /* `hardcore` in this window's PowerShell and cmd tabs (and hardcore.exe in its WSL ones) */
        wchar_t exe[MAX_PATH], *sl, path[8192] = L"", np[8400];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        if ((sl = wcsrchr(exe, L'\\'))) *sl = 0;
        GetEnvironmentVariableW(L"PATH", path, 8192);
        _snwprintf(np, 8400, L"%ls;%ls", exe, path);
        np[8399] = 0;
        SetEnvironmentVariableW(L"PATH", np);
    }
    /* what PowerShell tabs (and Claude Code for Windows in them) should know */
    SetEnvironmentVariableW(L"COLORTERM", L"truecolor");
    SetEnvironmentVariableW(L"TERM_PROGRAM", L"hardcore");
    SetEnvironmentVariableW(L"FORCE_HYPERLINK", L"1");
}

/* Distros registered for this user, and the default one's name. No process,
   no WSL service start: just the registry, so it costs microseconds. */
static int wsl_distros(void) {
    HKEY k, d;
    wchar_t name[128], def[64] = L"";
    DWORD i, n = 0, sz;
    g_default_distro[0] = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss", 0, KEY_READ, &k)) return 0;
    sz = sizeof(def);
    RegGetValueW(k, NULL, L"DefaultDistribution", RRF_RT_REG_SZ, NULL, def, &sz);
    for (i = 0;; i++) {
        wchar_t sub[64], dn[128];
        DWORD subn = 64;
        if (RegEnumKeyExW(k, i, sub, &subn, NULL, NULL, NULL, NULL)) break;
        sz = sizeof(dn);
        if (RegOpenKeyExW(k, sub, 0, KEY_READ, &d)) continue;
        if (!RegGetValueW(d, NULL, L"DistributionName", RRF_RT_REG_SZ, NULL, dn, &sz)) {
            n++;
            if (!_wcsicmp(sub, def) || !g_default_distro[0]) _snwprintf(g_default_distro, 128, L"%ls", dn);
        }
        RegCloseKey(d);
    }
    RegCloseKey(k);
    (void)name;
    return (int)n;
}

/* A folder for a PowerShell tab: Windows paths as-is, <mount root>/c/x → C:\x,
   other Linux paths through \\wsl.localhost\<distro>. */
static void to_windows_dir(const char *p, wchar_t *out, int cap) {
    char tmp[1200];
    size_t m = strlen(g_mnt_root), i;
    if (!p[0]) { _snwprintf(out, cap, L"%ls", _wgetenv(L"USERPROFILE") ? _wgetenv(L"USERPROFILE") : L"C:\\"); return; }
    if (p[1] == ':') snprintf(tmp, sizeof(tmp), "%s", p);
    else if (!strncmp(p, g_mnt_root, m) && isalpha((unsigned char)p[m]) && (p[m + 1] == '/' || !p[m + 1]))
        snprintf(tmp, sizeof(tmp), "%c:%s", toupper((unsigned char)p[m]), p[m + 1] ? p + m + 1 : "\\");
    else if (p[0] == '/') {
        char dn[128];
        WideCharToMultiByte(CP_UTF8, 0, g_distro[0] ? L"" : g_default_distro, -1, dn, sizeof(dn), NULL, NULL);
        snprintf(tmp, sizeof(tmp), "\\\\wsl.localhost\\%s%s", g_distro[0] ? g_distro : dn, p);
    } else snprintf(tmp, sizeof(tmp), "%s", p);
    for (i = 0; tmp[i]; i++) if (tmp[i] == '/') tmp[i] = '\\';
    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, out, cap);
    out[cap - 1] = 0;
}

/* PowerShell (7 if installed, else Windows PowerShell), with a prompt hook
   that reports the folder (OSC 9;9) and marks the prompt (OSC 133;A), the
   same two things rc.bash does for bash. Your profile still loads. */
static void powershell_cmd(wchar_t *out, int cap) {
    static const char hook[] =
        "if ($env:HARDCORE_TEST_CONFIG) { try { Set-PSReadLineOption -HistorySaveStyle SaveNothing "
        "-HistorySavePath (Join-Path $env:HARDCORE_TEST_CONFIG 'history.txt') } catch {} }\n"
        "$global:__hc_prompt = $function:prompt\n"
        "function global:prompt { $e = [char]27; "
        "[Console]::Write(\"$e]9;9;$($executionContext.SessionState.Path.CurrentLocation.ProviderPath)$e\\$e]133;A$e\\\"); "
        "& $global:__hc_prompt }\n"
        "if ($env:HARDCORE_RUN) { $c = $env:HARDCORE_RUN; Remove-Item env:HARDCORE_RUN; "     /* restored/slurped: claude --resume */
        "try { [Microsoft.PowerShell.PSConsoleReadLine]::AddToHistory($c) } catch {}; Invoke-Expression $c }\n";
    wchar_t exe[MAX_PATH], w[1024];
    char b64[3000];
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int n = MultiByteToWideChar(CP_UTF8, 0, hook, -1, w, 1024) - 1, i, k = 0;
    const unsigned char *b = (const unsigned char *)w;
    int len = n * 2;             /* -EncodedCommand is base64 of UTF-16LE */
    for (i = 0; i < len; i += 3) {
        unsigned v = (unsigned)b[i] << 16 | (i + 1 < len ? (unsigned)b[i + 1] << 8 : 0) | (i + 2 < len ? b[i + 2] : 0);
        b64[k++] = tbl[(v >> 18) & 63];
        b64[k++] = tbl[(v >> 12) & 63];
        b64[k++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        b64[k++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    b64[k] = 0;
    if (!SearchPathW(NULL, L"pwsh.exe", NULL, MAX_PATH, exe, NULL)) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        _snwprintf(exe, MAX_PATH, L"%ls\\WindowsPowerShell\\v1.0\\powershell.exe", sys);
    }
    _snwprintf(out, cap, L"\"%ls\" -NoLogo -NoExit %ls-EncodedCommand %hs", exe,
               _wgetenv(L"HARDCORE_TEST_CONFIG") ? L"-NoProfile " : L"", b64);
    out[cap - 1] = 0;
}

static int tab_spawn(Tab *tb) {
    HANDLE in_r = NULL, in_w = NULL, out_r = NULL, out_w = NULL;
    COORD sz;
    HRESULT hr;
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T attr_sz = 0;
    char cmd[4096];
    wchar_t wcmd[4096], wdir[MAX_PATH * 2];
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
    hr = pCreatePC(sz, in_r, out_w, (g_conpty_kind[0] == 'O' ? 0x10 /* GLYPH_WIDTH_WCSWIDTH */ : 0) |
                   (tb->inherit ? 0x1 /* PSEUDOCONSOLE_INHERIT_CURSOR: restored text is above */ : 0), &tb->hpc);
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

    if (tb->slurp_command[0]) {
        wcscpy(wcmd, tb->slurp_command);
        wdir[0] = 0;
    } else if (tb->kind == TK_PS) {
        powershell_cmd(wcmd, 4096);
        to_windows_dir(tb->start_dir, wdir, MAX_PATH * 2);
    } else if (tb->kind == TK_CMD) {
        /* cmd.exe; its PROMPT reports the folder and marks the prompt, the way
           rc.bash does for bash (OSC 9;9, OSC 133;A); /k runs a restored command first */
        wchar_t run[620];
        utf8_to_wide(tb->run, run, 620);
        _snwprintf(wcmd, 4096, L"cmd.exe%ls%ls", tb->run[0] ? L" /k " : L"", tb->run[0] ? run : L"");
        to_windows_dir(tb->start_dir, wdir, MAX_PATH * 2);
    } else {
        /* bash with our rc (its path arrives through WSLENV, translated), or,
           in a distro without bash, whatever its default shell is */
        snprintf(cmd, sizeof(cmd), "wsl.exe%s%s%s --cd \"%s\"%s",
                 g_distro[0] ? " -d \"" : "", g_distro, g_distro[0] ? "\"" : "", tb->start_dir,
                 g_no_bash ? "" : " -e bash -c \"exec bash --rcfile \\\"$HARDCORE_RC\\\" -i\"");
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 4096);
    }
    memset(&pi, 0, sizeof(pi));
    /* tabs are only spawned from the UI thread, so a temporary env var is safe;
       WSLENV carries it across into Linux */
    if (tb->kind == TK_CMD) SetEnvironmentVariableW(L"PROMPT", L"$e]9;9;$P$e\\$e]133;A$e\\$P$G");
    if (tb->spare || ((tb->run[0] || tb->hist[0]) && (tb->kind == TK_WSL || tb->kind == TK_PS))) {
        wchar_t old[2048] = L"", wslenv[2100], run[620];
        const wchar_t *name = tb->spare ? L"HARDCORE_SPARE" : tb->run[0] ? L"HARDCORE_RUN" : L"HARDCORE_HIST";
        DWORD n = GetEnvironmentVariableW(L"WSLENV", old, 2048);
        _snwprintf(wslenv, 2100, L"%ls%ls%ls", old, n ? L":" : L"", name);
        wslenv[2099] = 0;
        utf8_to_wide(tb->run[0] ? tb->run : tb->hist, run, 620);
        SetEnvironmentVariableW(name, tb->spare ? L"1" : run);
        SetEnvironmentVariableW(L"WSLENV", wslenv);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            NULL, tb->kind != TK_WSL && GetFileAttributesW(wdir) != INVALID_FILE_ATTRIBUTES ? wdir : NULL,
                            &si.StartupInfo, &pi);
        SetEnvironmentVariableW(name, NULL);
        SetEnvironmentVariableW(L"WSLENV", n ? old : NULL);
    } else {
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                            NULL, tb->kind != TK_WSL && GetFileAttributesW(wdir) != INVALID_FILE_ATTRIBUTES ? wdir : NULL,
                            &si.StartupInfo, &pi);
    }
    if (tb->kind == TK_CMD) SetEnvironmentVariableW(L"PROMPT", NULL);
    if (!ok) {
        char msg[256];
        snprintf(msg, sizeof(msg), "\x1b[31mhardcore: could not start %s (error %lu)\x1b[0m\r\n",
                 tb->kind == TK_PS ? "PowerShell" : tb->kind == TK_CMD ? "cmd.exe" : "wsl.exe", GetLastError());
        term_message(&tb->t, msg);
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        return 0;
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    CloseHandle(pi.hThread);
    tb->proc = pi.hProcess;
    tb->born = GetTickCount();
    tb->reader = CreateThread(NULL, 0, reader_thread, tb, 0, NULL);
    RegisterWaitForSingleObject(&tb->wait, tb->proc, on_child_exit, tb, INFINITE, WT_EXECUTEONLYONCE);
    return 1;
}

/* In place of a shell: what's wrong and the one key that fixes it. Plain
   text in the terminal itself, one font, nothing to click through. */
static void show_help(Tab *tb, int what, DWORD code) {
    Term *t = &tb->t;
    char buf[1024];
    tb->help = what;
    InterlockedExchange(&tb->exited, 1);
    t->m_cursor_vis = 0;
    term_message(t, "\r\n");
    if (what == HELP_NO_DISTRO) {
        term_message(t, "\x1b[1mhardcore\x1b[0m runs your Linux shell through WSL, and this PC has no Linux\r\n"
                        "distribution installed yet.\r\n\r\n");
        term_message(t, "  \x1b[38;2;255;166;13mI\x1b[0m  install WSL and Ubuntu     \x1b[2mruns wsl --install; Windows asks for admin,\r\n"
                        "                                and may want a restart and a Linux user name\x1b[0m\r\n");
        term_message(t, "  \x1b[38;2;255;166;13mP\x1b[0m  use PowerShell instead     \x1b[2mremembered in config.ini (shell=powershell)\x1b[0m\r\n");
        term_message(t, "  \x1b[38;2;255;166;13mEnter\x1b[0m  check again\r\n");
    } else {
        snprintf(buf, sizeof(buf), "\r\n\x1b[2m%s exited right away (code %ld).\x1b[0m\r\n\r\n",
                 tb->kind == TK_PS ? "PowerShell" : "WSL", (long)code);
        term_message(t, buf);
        term_message(t, "  \x1b[38;2;255;166;13mEnter\x1b[0m  try again\r\n");
        if (tb->kind == TK_WSL)
            term_message(t, "  \x1b[38;2;255;166;13mP\x1b[0m  use PowerShell instead     \x1b[2mremembered in config.ini (shell=powershell)\x1b[0m\r\n");
    }
    t->dirty = 1;
}

/* A tab from a saved session: its text replayed first, then the shell (with
   the command to run first, if any) starts below it. kind < 0: the default. */
static Tab *tab_create_ex(const char *dir, int spare, int kind, const char *run, const char *hist,
                          const char *text, size_t text_n) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) {
        free(tb);
        return NULL;
    }
    tb->id = g_next_id++;
    InitializeCriticalSection(&tb->lock);
    tb->spare = spare;
    tb->kind = kind >= 0 ? kind : g_shell_ps ? TK_PS : TK_WSL;
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", dir && *dir ? dir : g_start_dir);
    if (tb->start_dir[1] == ':' || tb->start_dir[0] == '/') set_cwd(&tb->t, tb->start_dir);
    if (run) snprintf(tb->run, sizeof(tb->run), "%s", run);
    if (hist) snprintf(tb->hist, sizeof(tb->hist), "%s", hist);
    if ((text && text_n) || run || hist) {
        /* "── restored ──", and what comes next: Claude resuming, or the command
           that was running, waiting in history (never run for you) */
        char sep[600], shown[200];
        int k = 0, max = tb->t.cols - 30;
        const char *c;
        for (c = hist ? hist : ""; *c && k < (int)sizeof(shown) - 4 && k < (max > 10 ? max : 10); c++)
            shown[k++] = (unsigned char)*c < 0x20 ? ' ' : *c;   /* it's displayed: no control characters */
        if (*c) shown[k++] = '.', shown[k++] = '.', shown[k++] = '.';
        shown[k] = 0;
        snprintf(sep, sizeof(sep), "\x1b[0m\x1b[2m\xe2\x94\x80\xe2\x94\x80 restored%s%s \xe2\x94\x80\xe2\x94\x80\x1b[0m\r\n",
                 run ? " \xc2\xb7 resuming Claude" : hist ? " \xc2\xb7 \xe2\x86\x91 reruns: " : "", run ? "" : shown);
        if (text && text_n) vt_feed(&tb->t, (const unsigned char *)text, (int)text_n);
        vt_feed(&tb->t, (const unsigned char *)sep, (int)strlen(sep));
        tb->inherit = 1;
    }
    if (tb->kind == TK_WSL && !g_have_distro) show_help(tb, HELP_NO_DISTRO, 0);
    else tab_spawn(tb);
    return tb;
}

static Tab *tab_create(const char *dir, int spare) { return tab_create_ex(dir, spare, -1, NULL, NULL, NULL, 0); }

/* Closing ConPTY can block until conhost drains, so tear down off the UI thread. */
static DWORD WINAPI tab_teardown(LPVOID arg) {
    Tab *tb = (Tab *)arg;
    if (tb->slurp_hwnd) slurp_detach(tb);
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
    wchar_t path[MAX_PATH], *la = _wgetenv(L"LOCALAPPDATA"), *test = _wgetenv(L"HARDCORE_TEST_CONFIG");
    char line[1200];
    FILE *f;
    if (test) _snwprintf(g_cfg_dir, MAX_PATH, L"%ls", test);
    else _snwprintf(g_cfg_dir, MAX_PATH, L"%ls\\hardcore", la ? la : L".");
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
        else if (!strcmp(line, "last")) snprintf(g_last_dir, sizeof(g_last_dir), "%s", v);
        else if (!strcmp(line, "distro")) snprintf(g_distro, sizeof(g_distro), "%s", v);
        else if (!strcmp(line, "shell")) g_shell_ps = !_stricmp(v, "powershell");
        else if (!strcmp(line, "width")) g_win_w = atoi(v);
        else if (!strcmp(line, "height")) g_win_h = atoi(v);
        else config_look(line, v);
    }
    fclose(f);
}

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
    fprintf(f, "# shell=powershell runs PowerShell in tabs instead of WSL; empty = WSL\n");
    fprintf(f, "shell=%s\n", g_shell_ps ? "powershell" : "");
    fprintf(f, "width=%d\nheight=%d\n", g_win_w, g_win_h);
    fprintf(f, "# the folder the last window closed in; new windows open there (empty: use dir=)\n");
    fprintf(f, "last=%s\n", g_last_dir);
    config_look_save(f);
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

#endif /* _WIN32: ConPTY, config, clipboard */


/* ── window, input, rendering ─────────────────────────────────────── */

static int g_focused = 1;
static int g_mouse_x = -1, g_mouse_y = -1;
#ifdef _WIN32
static LARGE_INTEGER g_last_paint;   /* QPC: GetTickCount only moves every ~15.6 ms */
#else
static DWORD g_last_paint;
#endif

static Tab *active_tab(void) { return g_ntabs ? g_tabs[g_active] : NULL; }
#ifndef _WIN32
static int g_cutie_focus_id;
static int cutie_focus_valid(void);
static void cutie_input_follow(void);
#endif
static Tab *input_tab(void) {
#ifndef _WIN32
    if(g_cutie_focus_id && cutie_focus_valid())return tab_by_id(g_cutie_focus_id);
    if(g_cutie_focus_id)mac_cutie_blur();
#endif
    return active_tab();
}

#include "hardcore_tab_preview.h"
static TabPreview g_tab_preview;
static int chrome_preview_active(void) {
    Tab *selected=active_tab();
    tab_preview_validate(&g_tab_preview,selected?selected->id:0,
        tab_by_id(g_tab_preview.target_id)!=NULL,g_focused);
    return selected && tab_preview_active(&g_tab_preview,selected->id);
}
static Tab *display_tab(void) {
    return chrome_preview_active()?tab_by_id(g_tab_preview.target_id):active_tab();
}
static void chrome_preview_cancel(void) {
    if(tab_preview_cancel(&g_tab_preview))plat_invalidate();
}
/* the cell, in pixels: the terminal's, or the tab column's while it draws (see UI_FONT) */
#define CW cell_w(g_cf)
#define CH cell_h(g_cf)
#define SCROLLBAR_W (6 * g_scale)
/* Chrome uses the same cell grid as terminal text. Underlays occupy no PTY columns. */
#define SB_COLS 28
#define UI_FONT const Font *ui_was_ = g_cf; g_cf = g_font
#define UI_DONE g_cf = ui_was_
static int sidebar_w(void) { return SB_COLS * cell_w(g_font); }
#ifdef _WIN32
static int caption_h(void) { return cell_h(g_font); }   /* the glass buttons' height */
#else
static int caption_h(void) { return 0; }   /* macOS keeps its own traffic lights, over the tab column */
#endif
/* The chrome overlays the right edge; it never changes the pty grid. */
static int sidebar_x(void) {
    int w, h;
    plat_client_size(&w, &h);
    if (g_tab_underlay != 1) return 0;
    int col = w / cell_w(g_font) - SB_COLS;
    return (col > 0 ? col : 0) * cell_w(g_font);
}
#ifdef _WIN32
static int in_sidebar(int x) { return x >= sidebar_x() && x < sidebar_x() + sidebar_w(); }
#endif
/* (0,0) is the native drawable surface, inside any system-owned border.
   Rounded corners may obscure cells; they never shift the grid. */
static int term_ox(void) { return 0; }
static int term_oy(void) { return 0; }

static void compute_grid(int w, int h, int *cols, int *rows) {
    *cols = (w - term_ox()) / CW;
    *rows = h / CH;
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
#include "hardcore_reflow.h"
static ChromeReflow g_display;
static const Term *g_display_term;
static int g_display_view, g_display_hcount;
/* A displayed cell retains its original terminal address for selection/copy. */
static int display_source(const Term *t, int col, int row) {
    if (g_tab_underlay && g_chrome_reflow && g_display_term == t && g_display.cells &&
        g_display.cols == t->cols && g_display.rows == t->rows &&
        g_display_view == t->view && g_display_hcount == t->hcount &&
        col >= 0 && col < t->cols && row >= 0 && row < t->rows)
        return g_display.source[row * t->cols + col];
    return row * t->cols + col;
}

static void tab_set_size(Tab *tb, int cols, int rows);   /* tell the pty */

/* A tab (and its pty) to the window's grid, if it isn't there yet. */
static void tab_fit(Tab *tb) {
    if (!tb || (tb->t.cols == g_cols && tb->t.rows == g_rows)) return;
    tab_drain(tb);
    term_resize(&tb->t, g_cols, g_rows);
    tab_set_size(tb, g_cols, g_rows);
}

/* While the window is being resized only the tab you see follows each step.
   The others, and the spare, wait until it stops (settle_all): each pty
   resize makes ConPTY repaint that tab, and with 16 tabs doing it at every
   step a drag step took 177 ms. */
static void resize_all(void) {
    int w, h, cols, rows;
    plat_client_size(&w, &h);
    if (w < 1 || h < 1) return;
    compute_grid(w, h, &cols, &rows);
    if (cols == g_cols && rows == g_rows) return;
    chrome_preview_cancel();
#ifndef _WIN32
    mac_cutie_blur();
#endif
    g_cols = cols;
    g_rows = rows;
    g_sel_on = 0;
    if (g_ntabs) tab_fit(g_tabs[g_active]);
    plat_timer(TIMER_SETTLE, 250);   /* re-armed by every step: fires once the resizing stops */
}

/* Once a resize has settled, one step per tick, so even sixteen full tabs
   never hold a frame up for long: the hidden tabs to the new size, then the
   rest of each one's scrollback rewrapped, the tab you see first (all at
   once measured 69 ms; one tab ~4 ms). */
static void settle_all(void) {
    Tab *tb = active_tab();
    int i;
    if (tb && term_settle(&tb->t)) {
        g_sel_on = 0;            /* line numbers moved under the selection */
        plat_timer(TIMER_SETTLE, 16);
        return;
    }
    for (i = 0; i <= g_ntabs; i++) {
        tb = i < g_ntabs ? g_tabs[i] : g_spare;
        if (tb && (tb->t.cols != g_cols || tb->t.rows != g_rows)) {
            tab_fit(tb);
            plat_timer(TIMER_SETTLE, 16);
            return;
        }
        if (tb && term_settle(&tb->t)) {
            plat_timer(TIMER_SETTLE, 16);
            return;
        }
    }
}

static void sel_norm(long long *r0, int *c0, long long *r1, int *c1) {
    if (g_sel_a_row < g_sel_b_row || (g_sel_a_row == g_sel_b_row && g_sel_a_col <= g_sel_b_col)) {
        *r0 = g_sel_a_row; *c0 = g_sel_a_col; *r1 = g_sel_b_row; *c1 = g_sel_b_col;
    } else {
        *r0 = g_sel_b_row; *c0 = g_sel_b_col; *r1 = g_sel_a_row; *c1 = g_sel_a_col;
    }
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
            n += utf8_encode((cells[x].wflag & 8) ? (cells[x].attr ? ' ' : 0xFFFC) : cells[x].ch ? cells[x].ch : ' ', out + n);
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
    Tab *tb = input_tab();
    if (!tb) return;
#ifndef _WIN32
    if (mac_input_held(tb)) return;
    cutie_input_follow();
#endif
    if (tb->t.view
#ifndef _WIN32
        && !g_cutie_focus_id
#endif
    ) {
        tb->t.view = 0;
        plat_invalidate();
    }
    tab_write(tb, s, n);
}

/* Sends text the way a paste arrives: CRLF/LF become CR like a typed Enter,
   ESC is stripped so a paste can't end bracketed-paste mode early. */
static void paste_utf8(const char *in) {
    Tab *tb = input_tab();
    size_t n = strlen(in);
    char *s, *o;
    const char *p;
    if (!tb || !n) return;
    s = o = (char *)malloc(n + 16);
    for (p = in; *p; p++) {
        if (*p == '\r' && p[1] == '\n') continue;
        if (*p == 0x1B) continue;
        *o++ = (*p == '\n') ? '\r' : *p;
    }
    if (tb->t.m_bracketed) send_to_active("\x1b[200~", 6);
    send_to_active(s, (int)(o - s));
    if (tb->t.m_bracketed) send_to_active("\x1b[201~", 6);
    free(s);
}

#ifdef _WIN32
static void paste_wide(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;
    if (n <= 1) return;
    s = (char *)malloc(n);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    paste_utf8(s);
    free(s);
}

/* A Windows path as you'd type it in WSL, shell-quoted only when needed:
   C:\x y → '/mnt/c/x y', \\wsl.localhost\Debian\home → /home */
static void dropped_path(const wchar_t *path, wchar_t *q, size_t cap) {
    wchar_t lin[MAX_PATH * 4 + 16];
    const wchar_t *p, *safe = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-+:@%,=";
    size_t k, j = 0;
    int needq = 0;
    if (active_tab() && active_tab()->kind == TK_PS) {   /* PowerShell: the Windows path, '...' if needed */
        for (k = 0; path[k]; k++) if (!wcschr(safe, path[k]) && path[k] != L'\\') needq = 1;
        if (needq) q[j++] = L'\'';
        for (k = 0; path[k] && j + 4 < cap; k++) {
            if (path[k] == L'\'') q[j++] = L'\'';
            q[j++] = path[k];
        }
        if (needq) q[j++] = L'\'';
        q[j] = 0;
        return;
    }
    if (iswalpha(path[0]) && path[1] == L':') {
        _snwprintf(lin, MAX_PATH * 4 + 16, L"%hs%c%ls", g_mnt_root, towlower(path[0]), path + 2);
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

#endif /* _WIN32: drops, clipboard formats */

/* ── glyph atlas: on-demand 40×40 slots in one texture ─────────────── */

/* A slot holds one glyph in its font's pixels (the GPU scales those up to
   the cell), or, for box drawing redrawn with thin lines, in the display's.
   Courier's 20-pixel cell needs 40 pixels for a two-cell Unifont fallback. */
#define SLOT 40
#define ATLAS 4096
#define ATLAS_S "4096.0"
#define SLOTS_PER_ROW (ATLAS / SLOT)
#define MAX_SLOTS (SLOTS_PER_ROW * SLOTS_PER_ROW)
#define HASH_SIZE 65536
static uint32_t g_hkey[HASH_SIZE];   /* cp + 1 + font index << 21, 0 = empty */
static uint16_t g_hslot[HASH_SIZE];
static uint8_t g_hwidth[HASH_SIZE], g_hheight[HASH_SIZE], g_hwide[HASH_SIZE];   /* in the slot's pixels */
static int g_nslots;

static void glyph_cache_reset(void);

/* The drawing code is immediate mode: r_color() sets the color, and quad() /
   r_quad_uv() append one 20-byte instance (pixel rectangle, atlas rectangle,
   color) to a CPU-side array. The backend's vertex shader turns each instance
   into two triangles, and r_present() hands the array over and draws it all
   at once: a full screen of text is a few hundred KB, not the 120 bytes per
   quad that six float vertices took. The backends (Direct3D 11 below, Metal
   in hardcore_mac.m) supply r_flush / r_upload / r_begin / r_present. */

typedef struct {
    int16_t x0, y0, x1, y1;      /* pixels */
    uint16_t u0, v0, u1, v1;     /* atlas pixels; u0 == 0xFFFF: a solid quad */
    uint32_t rgba;
} Inst;

static float g_draw_alpha = 1; /* shared draw opacity, including menu transitions */
static uint32_t g_rgba = 0xFFFFFFFF;
#define VB_INST 65536            /* per draw; a frame that needs more draws in pieces */
static Inst g_inst[VB_INST];
static int g_ninst;
static int g_clip_w = 32767, g_clip_h = 32767;
static int g_chrome_x; /* translation for the right-side overlay only */
/* Menu quads are staged so a whole row can borrow whitespace before any of
   its glyphs reach the renderer. Capture cannot flush the terminal's batch. */
static Inst *g_chrome_quads;
static int g_chrome_quad_count, g_chrome_quad_capacity, g_chrome_capture, g_chrome_capture_failed;

static void r_flush(void);
static void r_upload(int x, int y, const unsigned char *bmp);

static void r_color(uint32_t rgb, float a) {
    a *= g_draw_alpha;
    g_rgba = ((rgb >> 16) & 255) | (rgb & 0xFF00) | ((rgb & 255) << 16) | ((uint32_t)(a * 255.0f + 0.5f) << 24);
}

/* pixel positions round to whole pixels: the font is a bitmap, it should land on the grid */
static int16_t px(float v) { return (int16_t)(v < 0 ? v - 0.5f : v + 0.5f); }

/* textured quad, atlas rectangle in atlas pixels */
static void r_quad_uv(float x0, float y0, float x1, float y1, int u0, int v0, int u1, int v1) {
    Inst *q;
    x0 += g_chrome_x; x1 += g_chrome_x;
    /* The fractional right/bottom cell remainder is background only. */
    if (x1 <= 0 || y1 <= 0 || x0 >= g_clip_w || y0 >= g_clip_h) return;
    if (u0 != 0xFFFF) {
        float du = (float)(u1 - u0) / (x1 - x0), dv = (float)(v1 - v0) / (y1 - y0);
        if (x0 < 0) u0 += (int)(-x0 * du);
        if (y0 < 0) v0 += (int)(-y0 * dv);
        if (x1 > g_clip_w) u1 -= (int)((x1 - g_clip_w) * du);
        if (y1 > g_clip_h) v1 -= (int)((y1 - g_clip_h) * dv);
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_clip_w) x1 = (float)g_clip_w;
    if (y1 > g_clip_h) y1 = (float)g_clip_h;
    if (g_chrome_capture) {
        if (g_chrome_capture_failed) return;
        if (g_chrome_quad_count == g_chrome_quad_capacity) {
            int capacity = g_chrome_quad_capacity ? g_chrome_quad_capacity * 2 : 1024;
            Inst *more = (Inst *)realloc(g_chrome_quads, (size_t)capacity * sizeof(*more));
            if (!more) { g_chrome_capture_failed = 1; return; }
            g_chrome_quads = more; g_chrome_quad_capacity = capacity;
        }
        q = &g_chrome_quads[g_chrome_quad_count++];
    } else {
        if (g_ninst == VB_INST) r_flush();
        q = &g_inst[g_ninst++];
    }
    q->x0 = px(x0); q->y0 = px(y0); q->x1 = px(x1); q->y1 = px(y1);
    q->u0 = (uint16_t)u0; q->v0 = (uint16_t)v0; q->u1 = (uint16_t)u1; q->v1 = (uint16_t)v1;
    q->rgba = g_rgba;
}

static void quad(float x0, float y0, float x1, float y1) { r_quad_uv(x0, y0, x1, y1, 0xFFFF, 0, 0, 0); }

#ifdef _WIN32
/* ── renderer: Direct3D 11 on a flip-model swap chain ──────────────── */

/* Why not OpenGL any more: a WGL window is a "redirected" surface that DWM
   copies into its composition. Measured here, our whole key→echo→render path
   took 1.4 ms, then the frame waited 6 ms (p50) to 19 ms (p99) on that copy.
   A flip-model swap chain hands DWM the buffer itself, and with tearing
   allowed Windows can promote the window to an independent flip or a
   hardware overlay plane, skipping composition altogether.

   The instance array (see quad() above) is drawn with one DrawInstanced per frame. */


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
static ID3D11Buffer *g_cb;       /* the frame size, for pixel → clip space */
static int g_cb_w, g_cb_h;

static const char HLSL[] =
    "cbuffer F : register(b0) { float2 inv; };\n"     /* 2 / frame size */
    "struct VI { int4 r : RECT; uint4 t : TEX; float4 c : COLOR; uint id : SV_VertexID; };\n"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "VO vs(VI i) {\n"
    "  uint k = i.id < 3 ? i.id : i.id - 2;\n"         /* 0 1 2, 1 3 2: corners of a quad */
    "  float2 f = float2(k & 1, k >> 1);\n"
    "  float2 p = lerp(float2(i.r.xy), float2(i.r.zw), f);\n"
    "  VO o; o.p = float4(p.x * inv.x - 1, 1 - p.y * inv.y, 0, 1);\n"
    "  o.uv = i.t.x == 65535 ? float2(-1, -1) : lerp(float2(i.t.xy), float2(i.t.zw), f) / " ATLAS_S ";\n"
    "  o.c = i.c; return o; }\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "float4 ps(VO i) : SV_Target {\n"
    "  float a = i.uv.x < 0 ? 1 : t.SampleLevel(s, i.uv, 0).a;\n"
    "  return float4(i.c.rgb, i.c.a * a); }\n";

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
                                         LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);


static void r_flush(void) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (!g_ninst || !g_ctx) return;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, g_inst, sizeof(Inst) * g_ninst);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
        ID3D11DeviceContext_DrawInstanced(g_ctx, 6, (UINT)g_ninst, 0, 0);
    }
    g_ninst = 0;
}


/* one SLOT×SLOT alpha glyph into the atlas */
static void r_upload(int x, int y, const unsigned char *bmp) {
    D3D11_BOX b = {(UINT)x, (UINT)y, 0, (UINT)x + SLOT, (UINT)y + SLOT, 1};
    ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_atlas, 0, &b, bmp, SLOT, 0);
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
    const float bg[4] = {((DEF_BG >> 16) & 255) / 255.0f, ((DEF_BG >> 8) & 255) / 255.0f, (DEF_BG & 255) / 255.0f, 1};
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    UINT stride = sizeof(Inst), off = 0;
    if (w != g_cb_w || h != g_cb_h) {
        float inv[4] = {2.0f / (float)w, 2.0f / (float)h, 0, 0};
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_cb, 0, NULL, inv, 0, 0);
        g_cb_w = w;
        g_cb_h = h;
    }
    g_ninst = 0;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, bg);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rs);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_il);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
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
            {"RECT", 0, DXGI_FORMAT_R16G16B16A16_SINT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEX", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 8, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1}};
        ID3D11Device_CreateInputLayout(g_dev, el, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_il);
    }
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    {
        D3D11_BUFFER_DESC bd = {sizeof(g_inst), D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
        D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vb);
        ID3D11Device_CreateBuffer(g_dev, &cd, NULL, &g_cb);
        g_cb_w = g_cb_h = 0;
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
    REL(g_vb); REL(g_cb); REL(g_il); REL(g_ps); REL(g_vs); REL(g_ctx); REL(g_dev);
    glyph_cache_reset();
    if (gfx_init_device()) gfx_init_swapchain(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

#endif /* _WIN32: Direct3D 11 */

static void glyph_cache_reset(void) {
    r_flush();                   /* quads already queued still point at the old slots */
    memset(g_hkey, 0, sizeof(g_hkey));
    g_nslots = 0;
}

/* A glyph in font f's pixels, one byte a pixel, rows SLOT apart: its own,
   its fallback font's, or Unifont's squeezed into f's cell (nearest pixel).
   Returns its width: f->w, or 2 f->w for a wide one. */
static int glyph_bits(const Font *f, uint32_t cp, unsigned char *bmp) {
    const Font *fb;
    const unsigned char *g = font_record(f, cp);
    int w, x, y, bpr;
    if (!g && f->fallback && (fb = font_by_key(f->fallback)) && fb->w == f->w && fb->h == f->h) g = font_record(fb, cp);
    if (!g) {
        const unsigned char *u = font_record(FONTS, cp);
        int uw, tw;
        if (!u) u = font_record(FONTS, 0xFFFD);
        if (!u) return f->w;
        uw = u[0];
        tw = uw == 8 ? f->w : 2 * f->w;
        bpr = (uw + 7) / 8;
        for (y = 0; y < f->h; y++) {
            const unsigned char *row = u + 1 + (y * 16 / f->h) * bpr;
            for (x = 0; x < tw; x++) {
                int sx = x * uw / tw;
                if (row[sx >> 3] & (0x80 >> (sx & 7))) bmp[y * SLOT + x] = 255;
            }
        }
        return tw;
    }
    w = g[0];
    bpr = (w + 7) / 8;
    for (y = 0; y < f->h; y++)
        for (x = 0; x < w; x++)
            if (g[1 + y * bpr + (x >> 3)] & (0x80 >> (x & 7))) bmp[y * SLOT + x] = 255;
    return w;
}

/* Box drawing at scale 2 with 1-pixel lines instead of doubled ones. Each
   lit pixel of the glyph becomes a point at twice its position, joined to
   its lit neighbours by 1-pixel lines (a diagonal only where no straight
   path joins them). A solid 2×2 block, a heavy line, stays solid, and a
   stroke that reaches the right or bottom edge runs on to meet the next
   cell's. The glyph's own shape stays: corners, arcs, double lines. */
static int thin_on(const unsigned char *s, int w, int h, int x, int y) {
    if (x == w && y >= 0 && y < h) return w > 1 && s[y * SLOT + w - 1] && s[y * SLOT + w - 2];
    if (y == h && x >= 0 && x < w) return h > 1 && s[(h - 1) * SLOT + x] && s[(h - 2) * SLOT + x];
    return x >= 0 && x < w && y >= 0 && y < h && s[y * SLOT + x];
}

static void thin_box(const unsigned char *s, int w, int h, unsigned char *d) {
    int x, y, k, dd;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            int ax = 2 * x, ay = 2 * y;
            if (!s[y * SLOT + x]) continue;
            d[ay * SLOT + ax] = 255;
            if (thin_on(s, w, h, x + 1, y))
                for (k = 1; k <= 2 && ax + k < 2 * w; k++) d[ay * SLOT + ax + k] = 255;
            if (thin_on(s, w, h, x, y + 1))
                for (k = 1; k <= 2 && ay + k < 2 * h; k++) d[(ay + k) * SLOT + ax] = 255;
            for (dd = -1; dd <= 1; dd += 2)
                if (thin_on(s, w, h, x + dd, y + 1) && !thin_on(s, w, h, x + dd, y) && !thin_on(s, w, h, x, y + 1))
                    for (k = 1; k <= 2; k++)
                        if (ax + dd * k >= 0 && ax + dd * k < 2 * w && ay + k < 2 * h) d[(ay + k) * SLOT + ax + dd * k] = 255;
            if (thin_on(s, w, h, x + 1, y) && thin_on(s, w, h, x, y + 1) && thin_on(s, w, h, x + 1, y + 1) &&
                ax + 1 < 2 * w && ay + 1 < 2 * h)
                d[(ay + 1) * SLOT + ax + 1] = 255;
        }
}

/* The slot holding cp in the font CW/CH measure, uploaded the first time;
   *aw × *ah: the glyph's size in the slot, *wide: it takes two cells. */
static int glyph_slot(uint32_t cp, int *aw, int *ah, int *wide) {
    const Font *f = g_cf;
    uint32_t key = cp + 1 + ((uint32_t)(f - FONTS) << 21), h = (key * 2654435761u) & (HASH_SIZE - 1);
    unsigned char bits[SLOT * SLOT], thin[SLOT * SLOT];
    int slot, w, gh = f->h;
    while (g_hkey[h]) {
        if (g_hkey[h] == key) { *aw = g_hwidth[h]; *ah = g_hheight[h]; *wide = g_hwide[h]; return g_hslot[h]; }
        h = (h + 1) & (HASH_SIZE - 1);
    }
    if (g_nslots >= MAX_SLOTS || g_nslots >= HASH_SIZE / 2) {
        glyph_cache_reset();
        return glyph_slot(cp, aw, ah, wide);
    }
    memset(bits, 0, sizeof(bits));
    w = glyph_bits(f, cp, bits);
    *wide = w > f->w;
    if (f->thin && !f->dev && g_scale == 2 && cp >= 0x2500 && cp <= 0x257F && 2 * w <= SLOT && 2 * f->h <= SLOT) {
        memset(thin, 0, sizeof(thin));
        thin_box(bits, w, f->h, thin);
        memcpy(bits, thin, sizeof(bits));
        w *= 2;
        gh *= 2;
    }
    slot = g_nslots++;
    r_upload((slot % SLOTS_PER_ROW) * SLOT, (slot / SLOTS_PER_ROW) * SLOT, bits);
    g_hkey[h] = key;
    g_hslot[h] = (uint16_t)slot;
    g_hwidth[h] = (uint8_t)w;
    g_hheight[h] = (uint8_t)gh;
    g_hwide[h] = (uint8_t)*wide;
    *aw = w;
    *ah = gh;
    return slot;
}

static void fill_rect(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
    r_color(rgb, a);
    quad(x0, y0, x1, y1);
}

/* Draws one glyph into a w_cells-wide box at (x, y), in the font CW/CH
   measure. A wide glyph in a 1-cell slot is shrunk to fit, square and
   centred, the way dumbterm did it; a narrow one in two cells is centred. */
static void draw_glyph(uint32_t cp, float x, float y, int w_cells, int scale) {
    if (g_chrome_capture && cp == ' ') return;
    int aw, ah, wide, slot = glyph_slot(cp, &aw, &ah, &wide);
    float cw = (float)CW, chh = (float)CH;
    int u0 = (slot % SLOTS_PER_ROW) * SLOT, v0 = (slot / SLOTS_PER_ROW) * SLOT, u1 = u0 + aw, v1 = v0 + ah;
    float x0 = x, y0 = y, x1, y1 = y + chh;
    (void)scale;
    if (wide && w_cells == 1) {
        float hh = cw * (float)ah / (float)aw;
        y0 = y + (chh - hh) * 0.5f;
        y1 = y0 + hh;
        x1 = x + cw;
    } else if (!wide && w_cells == 2) {
        x0 = x + cw * 0.5f;
        x1 = x0 + cw;
    } else {
        x1 = x + cw * w_cells;
    }
    r_quad_uv(x0, y0, x1, y1, u0, v0, u1, v1);
}

/* how far a bold glyph is smeared right: one of the font's pixels */
static int smear(void) { return g_cf->dev ? 1 : g_scale; }

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
    if (per < 1) return 1;
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

/* A short executable name, never the program's decorative window title.
   Shell reports may start with assignments or the transparent exec/env wrappers. */
static void command_program(const char *command, char *out, int cap) {
    char word[1024];
    const char *p = command;
    out[0] = 0;
    while (*p) {
        int n = 0, quote = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            int c = (unsigned char)*p++;
            if (!quote && (c == '\'' || c == '"')) { quote = c; continue; }
            if (quote && c == quote) { quote = 0; continue; }
            if (n < (int)sizeof(word)-1) word[n++] = (char)c;
        }
        word[n] = 0;
        if (strchr(word, '=') || !strcmp(word, "env") || !strcmp(word, "exec") || !strcmp(word, "command")) continue;
        if (word[0] == '-') continue;
        const char *base = word;
        for (const char *q = word; *q; q++) if (*q == '/' || *q == '\\') base = q+1;
        snprintf(out, cap, "%s", base);
        n = (int)strlen(out);
        if (n > 4 && !strcmp(out+n-4, ".exe")) out[n-4] = 0;
        return;
    }
}

static void tab_label_fit(Tab *tb, wchar_t *out, int cap, int cols) {
    if (cap<1) return;
    out[0]=0;
    if (tb->kind == TK_BROWSER) {
#ifndef _WIN32
        mac_browser_tab_label(tb,out,cap,cols);
#else
        wchar_t title[256];
        utf8_to_wide(tb->browser_url, title, 256);
        _snwprintf(out, cap, L"%ls%ls", tb->parent_id ? L"↳ " : L"", tb->t.title[0] ? tb->t.title : title);
        out[cap-1] = 0;
#endif
        return;
    }
    const char *dir = tb->t.cwd[0] ? tb->t.cwd : tb->start_dir, *b = NULL, *p;
    char app[64];
    for (p = dir; *p; p++) if ((*p == '/' || *p == '\\') && p[1]) b = p+1;
#ifdef _WIN32
    if (!dir[0]) dir = "wsl";
    command_program(tb->t.cmd, app, sizeof(app));
#else
    if (!dir[0]) dir = "~";
    snprintf(app, sizeof(app), "%s", tb->program);
#endif
    if (!app[0]) snprintf(app, sizeof(app), "%s", tb->t.had_activity || tb->t.nmarks > 1 || tb->t.hcount || tb->run[0] || tb->hist[0] ? "term" : "<new>");
    utf8_to_wide(b ? b : dir, out, cap);
    int n = (int)wcslen(out);
    if (app[0] && n + 2 < cap) {
        out[n++] = ':';
        utf8_to_wide(app, out+n, cap-n);
    }
    (void)cols;
#ifndef _WIN32
    if (!strncmp(tb->run, "terminal:", 9)) {
        n = (int)wcslen(out);
        if (n + 9 < cap) wcscpy(out+n, L" [bridge]");
    }
#endif
}
static void tab_label(Tab *tb, wchar_t *out, int cap) { tab_label_fit(tb,out,cap,cap-1); }

static int unit_tab_labels(FILE *f) {
    static const struct { const char *in, *want; } cases[] = {
        {"", ""}, {"codex --resume", "codex"}, {"/usr/local/bin/claude", "claude"},
        {"FOO=1 env exec codex", "codex"}, {"\"/path with spaces/grok\" --help", "grok"},
        {"C:\\tools\\codex.exe", "codex"}
    };
    int fails = 0;
    char got[64];
    for (int i=0;i<(int)(sizeof(cases)/sizeof(cases[0]));i++) {
        command_program(cases[i].in, got, sizeof(got));
        fails += strcmp(got,cases[i].want) != 0;
    }
    Tab *tb = calloc(1, sizeof(Tab));
    if (!tb) return fails+1;
    snprintf(tb->start_dir, sizeof(tb->start_dir), "/work/hardcore");
    snprintf(tb->program, sizeof(tb->program), "codex");
    snprintf(tb->t.cmd, sizeof(tb->t.cmd), "codex --resume");
    wchar_t label[256]; tab_label(tb,label,256);
    fails += wcscmp(label,L"hardcore:codex") != 0;
    tb->program[0] = tb->t.cmd[0] = 0; tab_label(tb,label,256);
    fails += wcscmp(label,L"hardcore:<new>") != 0;
    tb->t.had_activity = 1; tab_label(tb,label,256);
    fails += wcscmp(label,L"hardcore:term") != 0;
    tb->t.had_activity = 0; tb->t.nmarks = 2; tab_label(tb,label,256);
    fails += wcscmp(label,L"hardcore:term") != 0;
    tb->t.nmarks = 1;
#ifndef _WIN32
    snprintf(tb->run, sizeof(tb->run), "terminal:/dev/ttys123");
    snprintf(tb->t.cwd, sizeof(tb->t.cwd), "/work/ruffian");
    snprintf(tb->program, sizeof(tb->program), "claude");
    tab_label(tb,label,256);
    fails += wcscmp(label,L"ruffian:claude [bridge]") != 0;
#endif
    free(tb);
    fprintf(f,"%s folder:program tab labels\n",fails ? "FAIL" : "ok  ");
    return fails;
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

#define BELL_GLYPH 0xF8F0        /* hand-drawn, font/hardcore-extra.hex */

#ifdef _WIN32
static int caption_btn_w(void) { return 46 * g_scale; }
#endif

#ifndef _WIN32
static int mac_caf_active(void);
static int mac_record_active(void);
static int mac_record_exports_active(void);
static int mac_music_menu_visible(void);
static int mac_music_playing(void);
static int mac_music_loading(void);
static void mac_music_compact_label(wchar_t *,int);
static int mac_caf_toggle(void);
static double mac_caf_elapsed(void);
static const char *mac_caf_power(void);
#endif
#include "hardcore_chrome.h"
#include "hardcore_cutie.h"
static void chrome_preview_pointer(int mx,int my) {
    int index=-1,hit=hit_tabbar(mx,my,&index);
    Tab *selected=active_tab();
    int target=(hit==HIT_TAB || hit==HIT_CLOSE || hit==HIT_CUTIE) && index>=0 && index<g_ntabs?g_tabs[index]->id:0;
    int eligible=g_focused && !g_sel_drag && !g_chrome_drag && !g_sb_drag;
#ifndef _WIN32
    eligible=eligible && g_tv_drag_slot<0;
#endif
    if(tab_preview_point(&g_tab_preview,selected?selected->id:0,target,eligible))plat_invalidate();
}


#ifdef _WIN32
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
static int g_top_hover;          /* the mouse is on the top line: it drags the window */

/* the top line, left of the buttons: a title bar that only shows itself on hover */
static int in_top_line(int mx, int my) { return mx >= 0 && !in_sidebar(mx) && my >= 0 && my < caption_h(); }

/* ── low disk ─────────────────────────────────────────────────────────
   WSL's disk is a .vhdx on a Windows drive that grows as Linux writes. When
   that drive fills, the guest still thinks it has room; its writes fail
   underneath it, programs die with SIGBUS ("Bus error") and then the whole
   VM goes, taking every tab with it. So the tab column warns early. */
#define DISK_WARN_GB 20.0
#define DISK_BAD_GB 5.0
static double g_disk_free_gb = -1;
static wchar_t g_disk_name[4] = L"C:";

static void check_disk(void) {
    wchar_t p[MAX_PATH], *la = _wgetenv(L"LOCALAPPDATA");
    ULARGE_INTEGER avail;
    double gb, was = g_disk_free_gb;
    _snwprintf(p, MAX_PATH, L"%ls\\wsl\\", la ? la : L"C:");
    p[MAX_PATH - 1] = 0;
    if (!GetDiskFreeSpaceExW(p, &avail, NULL, NULL) && !GetDiskFreeSpaceExW(la ? la : L"C:\\", &avail, NULL, NULL)) return;
    if (la && la[0] && la[1] == ':') { g_disk_name[0] = towupper(la[0]); g_disk_name[1] = ':'; g_disk_name[2] = 0; }
    gb = (double)avail.QuadPart / (1024.0 * 1024 * 1024);
    g_disk_free_gb = gb;
    if ((was < DISK_WARN_GB) != (gb < DISK_WARN_GB) || (gb < DISK_WARN_GB && (int)(was * 10) != (int)(gb * 10)))
        InvalidateRect(g_hwnd, NULL, FALSE);
}

static void render_disk_warning(int win_h) {
    wchar_t l1[64], l2[64];
    int total = (win_h - term_oy()) / CH;
    float y = (float)sb_row_y(total - 3), w = (float)((SB_COLS - 2) * CW);
    uint32_t col;
    if (g_disk_free_gb < 0 || g_disk_free_gb >= DISK_WARN_GB) return;
    col = g_disk_free_gb < DISK_BAD_GB ? g_look.red : AMBER;
    _snwprintf(l1, 64, L"%ls %.1f GB free", g_disk_name, g_disk_free_gb);
    _snwprintf(l2, 64, L"WSL crashes when %ls fills", g_disk_name);
    fill_rect(0, y, (float)sidebar_w(), (float)win_h, BAR_BG, 1);
    layout_text(l1, (float)CW, y, w, 1, col, 1);
    layout_text(l2, (float)CW, y + CH, w, 2, g_look.dim, 1);
    fill_rect(0, y, (float)g_scale, y + 3 * CH, col, 1);
}

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
        uint32_t fg = g_win_active ? DEF_FG : g_shown == SCHEMES ? 0x7A7A7A : g_look.detail;
        fill_rect(x, 0, x + bw, (float)bh, BAR_BG, 0.62f);       /* smoked glass: text shows through */
        if (hov && code == HTCLOSE) {
            fill_rect(x, 0, x + bw, (float)bh, prs ? 0x94241A : 0xC42B1C, 1);
            fg = 0xFFFFFF;
        } else if (hov) {
            fill_rect(x, 0, x + bw, (float)bh, g_shown == SCHEMES ? (prs ? 0x262626 : 0x2D2D2D) : prs ? g_look.bar : TAB_HOVER, 1);
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

#endif /* _WIN32: caption buttons */

/* text on the grid, cell by cell from (col, row); spaces left as they are */
static void grid_text(const wchar_t *s, int col, int row, uint32_t rgb) {
    uint32_t cps[64];
    uint8_t ws[64];
    int n = text_cells(s, cps, ws, 64), i;
    r_color(rgb, 1);
    for (i = 0; i < n && col + ws[i] <= g_chrome_surface_cols; col += ws[i++])
        if (cps[i] != ' ') draw_glyph(cps[i], (float)(col * CW), row_y(row), ws[i], g_scale);
}

#ifndef _WIN32
static uint32_t plat_title_mix(uint32_t fg, uint32_t bg, float alpha);
static uint32_t plat_title_background_color(uint32_t bg);
static void plat_render_title_background(float x0, float y0, float x1, float y1, uint32_t bg, uint32_t base);
static void plat_render_title_underlay(void);
static void plat_render_title_overlay(void);
static void mac_camera_draw(void);
static void mac_camera_layout(Term *t);
static void mac_music_draw(void);
static void mac_music_layout(Term *t);
#endif
static int chrome_intrusion_span(int row, int *left, int *right);
static int chrome_intrusion_span_at(int row,int dock,int top,int bottom,int *left,int *right);
#include "hardcore_chrome_draw.h"

/* Foreground determines the continuous terminal span cut from chrome.
   Background color alone is not foreground: a highlighted blank tail still
   exposes the opaque menu, composited above that background. */
static int cell_covers_chrome(const Cell *c) {
    uint32_t fg, bg;
    int space = c->ch <= ' ' || c->ch == 0xA0 || (c->ch >= 0x2000 && c->ch <= 0x200A) ||
                c->ch == 0x202F || c->ch == 0x205F || c->ch == 0x3000;
    cell_colors(c, &fg, &bg);
    if (fg == bg) return 0;
    return !space || (c->wflag & 8) || c->wflag == W_CONT || (c->attr & (A_UNDER | A_STRIKE)) || c->link;
}

static int chrome_terminal_span(const Cell *cells, int n, int *left, int *right) {
    *left = n; *right = 0;
    for (int x = 0; x < n; x++) if (cell_covers_chrome(&cells[x])) {
        if (x < *left) *left = x;
        *right = x + 1;
    }
    return *right > *left;
}

static int unit_underlay(FILE *f) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab)), *saved = g_tabs[0];
    int ntabs = g_ntabs, active = g_active, fails = 0, cols, rows;
    int saved_underlay = g_tab_underlay;
    g_tab_underlay = 1;
    if (!tb || !term_init(&tb->t, 40, 4)) { free(tb); return 1; }
    g_tabs[0] = tb; g_ntabs = 1; g_active = 0;
    tb->t.m_cursor_vis = 0;
    Cell blank = {' ', 0, 0, 0, 0, 0};
    fails += cell_covers_chrome(&blank) != 0;
    blank.ch = 'X'; fails += !cell_covers_chrome(&blank);
    blank.ch = ' '; blank.wflag = W_CONT; fails += !cell_covers_chrome(&blank);
    blank.wflag = 0; blank.bg = C_PAL(2); fails += cell_covers_chrome(&blank);
    blank.ch = 'X'; fails += !cell_covers_chrome(&blank);
    blank.ch = ' ';
    blank.bg = 0; blank.attr = A_UNDER; fails += !cell_covers_chrome(&blank);
    blank.attr = 0; blank.link = 1; fails += !cell_covers_chrome(&blank);
    blank.link = 0;
    blank.attr = A_DIM | A_BOLD | A_ITALIC | A_BLINK; fails += cell_covers_chrome(&blank);
    blank.bg = C_RGB((DEF_BG >> 16) & 255, (DEF_BG >> 8) & 255, DEF_BG & 255);
    fails += cell_covers_chrome(&blank);
    blank.ch = 'X'; blank.attr = A_HIDDEN; fails += cell_covers_chrome(&blank);
    blank.ch = 0xA0; blank.attr = A_DIM; fails += cell_covers_chrome(&blank);
    {
        Cell row[4] = {{'X'}, {' ', 0, C_PAL(2)}, {'X'}, {' '}};
        int left, right;
        fails += !chrome_terminal_span(row, 4, &left, &right) || left != 0 || right != 3;
        row[0].attr = row[2].attr = A_HIDDEN;
        fails += chrome_terminal_span(row, 4, &left, &right) != 0;
    }
#ifndef _WIN32
    /* There must be no invisible midpoint, nor an excessive shift, on any
       gray background. Sample the RGB cube too, including saturated colors. */
    for (int gray = 0; gray < 256; gray++) {
        uint32_t color = plat_title_background_color((uint32_t)gray * 0x010101);
        int delta = abs((int)(color & 255) - gray);
        fails += delta < 16 || delta > 32;
        fails += ((color >> 16) & 255) != (color & 255) || ((color >> 8) & 255) != (color & 255);
    }
    for (int r = 0; r <= 255; r += 17) for (int g = 0; g <= 255; g += 17) for (int b = 0; b <= 255; b += 17) {
        uint32_t color = plat_title_background_color((uint32_t)(r << 16 | g << 8 | b));
        int before = (77*r + 150*g + 29*b) / 256;
        int after = (77*((color >> 16) & 255) + 150*((color >> 8) & 255) + 29*(color & 255)) / 256;
        fails += abs(after - before) < 15 || abs(after - before) > 33;
    }
#endif
    const Font *saved_font = g_cf;
    int fi;
    for (fi = 0; fi < NFONTS; fi++) {
        g_cf = &FONTS[fi];
        compute_grid(120 * CW, 24 * CH, &cols, &rows);
        fails += cols != 120 || rows != 24;
        fails += term_ox() != 0 || term_oy() != 0;
        compute_grid(120 * CW + CW - 1, 24 * CH + CH - 1, &cols, &rows);
        fails += cols != 120 || rows != 24;
        compute_grid(121 * CW, 25 * CH, &cols, &rows);
        fails += cols != 121 || rows != 25;
    }
    g_cf = saved_font;
    config_look("tab_underlay", "0");
    compute_grid(120 * CW, 24 * CH, &cols, &rows);
    fails += g_tab_underlay != 2 || cols != 120 || rows != 24 || term_ox() != 0;
    config_look("chrome_reflow", "0"); config_look("chrome_debug", "1");
    fails += !g_chrome_reflow || g_chrome_debug;
    config_look("tab_underlay", "2");
    compute_grid(120 * CW, 24 * CH, &cols, &rows);
    fails += g_tab_underlay != 2 || cols != 120 || rows != 24 || term_ox() != 0 || sidebar_x() != 0;
    config_look("tab_underlay", "1");
    fails += g_tab_underlay != 1;
    { /* The same config writer is used on Windows and macOS. */
#ifdef _WIN32
        /* The Windows CRT's tmpfile() tries the drive root, which is often
           unwritable. Use the user's temporary directory for this fixture. */
        wchar_t tmpdir[MAX_PATH], tmppath[MAX_PATH] = L"";
        DWORD len = GetTempPathW(MAX_PATH, tmpdir);
        FILE *cfg = NULL;
        if (len && len < MAX_PATH && GetTempFileNameW(tmpdir, L"hct", 0, tmppath))
            cfg = _wfopen(tmppath, L"w+b");
#else
        FILE *cfg = tmpfile();
#endif
        char line[256];
        int found = 0;
        if (cfg) {
            config_look_save(cfg); rewind(cfg);
            while (fgets(line, sizeof(line), cfg)) if (!strcmp(line, "tab_underlay=1\n")) found = 1;
            fclose(cfg);
        }
#ifdef _WIN32
        if (tmppath[0]) DeleteFileW(tmppath);
#endif
        fails += !found;
    }
    g_tab_underlay = saved_underlay;
    term_free(&tb->t); free(tb);
    g_tabs[0] = saved; g_ntabs = ntabs; g_active = active;
    fprintf(f, "%s underlay: opaque glyph/style cells, both docks, shared full-width grid, persisted config\n", fails ? "FAIL" : "ok  ");
    return fails;
}

static void r_begin(int w, int h);
#include "hardcore_display.h"

/* Include spaces between terminal words so no menu glyph can leak into them.
   Return the interval in terminal columns, clipped to the current dock. */
static int chrome_intrusion_span_at(int row,int dock,int top,int bottom,int *left,int *right) {
    Tab *tb = display_tab();
    const Cell *cells;
    int n, occupied, cursor_col, cursor_row;
    *left = *right = 0;
    if (!tb || (tb->browser_native && !chrome_preview_active()) || row < 0 || row >= tb->t.rows ||
        row < top || row >= bottom) return 0;
    display_line(&tb->t, row, &cells, &n);
    occupied = chrome_terminal_span(cells, n, left, right);
    display_cursor_position(&tb->t, &cursor_col, &cursor_row);
    if (tb->t.view == 0 && tb->t.m_cursor_vis && cursor_row == row && tb->t.cx < tb->t.cols) {
        int end = cursor_col + (CELL(&tb->t, tb->t.cx, tb->t.cy)->wflag == W_WIDE ? 2 : 1);
        if (!occupied || cursor_col < *left) *left = cursor_col;
        if (end > *right) *right = end;
    }
    if (*left < dock) *left = dock;
    if (*right > dock + g_chrome_surface_cols) *right = dock + g_chrome_surface_cols;
    if (*right <= *left) { *left = *right = 0; return 0; }
    return 1;
}

static int chrome_intrusion_span(int row,int *left,int *right) {
    return chrome_intrusion_span_at(row,sidebar_x()/CW,g_chrome.top,g_chrome.bottom,left,right);
}
/* Terminal fills are below the quiet chrome, while terminal foreground cells
   are above it. This lets the menu show through a highlighted row's blank
   tail without washing out the highlight or drawing over its text. */
static void render_terminal_backgrounds(const Term *t, int ox, int oy) {
    for (int y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n;
        float y0 = (float)(oy + y*CH);
        display_line(t, y, &cells, &n);
        int end = g_sel_on ? t->cols : n;
        for (int x = 0; x < end; x++) {
            uint32_t fg, bg = DEF_BG;
            if (x < n && !(cells[x].wflag & 8)) cell_colors(&cells[x], &fg, &bg);
            if (display_selected(t, x, y)) bg = SEL_BG;
            if (bg == DEF_BG) continue;
            float x0 = (float)(ox + x*CW);
            fill_rect(x0, y0, x0+CW, y0+CH, bg, 1);
#ifndef _WIN32
            plat_render_title_background(x0, y0, x0+CW, y0+CH, bg, bg);
#endif
        }
    }
}

static void render(void) {
    struct { int right, bottom; } r;
    Tab *tb = display_tab();
    Term *t;
    int y, x, ox = term_ox(), oy = term_oy(), dock_col = sidebar_x()/CW;
    int overlay = g_tab_underlay || (tb && tb->kind == TK_BROWSER);
    plat_client_size(&r.right, &r.bottom);
#ifndef _WIN32
    if (tb && tb->kind == TK_BROWSER) mac_browser_prepare(tb);
    int browser_hover = tb && tb->kind==TK_BROWSER ? mac_browser_hover_link() : 0;
#endif
    g_clip_w = r.right / CW * CW;
    g_clip_h = r.bottom / CH * CH;
    r_begin(r.right, r.bottom);
    /* Browser/native and empty-window paths do not prepare a terminal grid.
       They must still end an old tab's fade and discard its pending target. */
    if (!tb || (tb->kind == TK_BROWSER && tb->browser_native && !chrome_preview_active())) {
        cm_reset(&g_chrome_motion); g_chrome_motion_alpha = 1;
        plat_kill_timer(TIMER_CHROME);
    }
#ifndef _WIN32
    if (tb && tb->kind == TK_BROWSER && tb->browser_native && !chrome_preview_active()) {
        chrome_tv_clock();tab_layout();chrome_tv_prepare(&tb->t);mac_camera_layout(&tb->t);mac_music_layout(&tb->t);
        render_tabbar(r.right, r.bottom);
        g_draw_alpha = 1;
        mac_browser_draw_overlay();
        render_cuties(r.right);
        mac_camera_draw();
        mac_music_draw();
        plat_render_title_overlay();
        return;
    }
#endif
#ifndef _WIN32
    plat_render_title_underlay();
#endif
#ifdef _WIN32
    if (!tb) { render_tabbar(r.right, r.bottom); render_caption_buttons(r.right); return; }
#else
    if (!tb) { render_tabbar(r.right, r.bottom); plat_render_title_overlay(); return; }
#endif
    t = &tb->t;
    chrome_prepare_display(t);
    dock_col = sidebar_x()/CW;
    render_terminal_backgrounds(t, ox, oy);
    if (overlay) {
        render_tabbar(r.right, r.bottom);
        g_draw_alpha = 1;
    } else render_tabbar(r.right, r.bottom);

    /* Restore the complete terminal intrusion, including spaces between its
       words, before drawing any glyphs. All masks precede row glyphs so a bold
       smear cannot be erased by the following cell's background. */
    for (y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n;
        float y0 = (float)(oy + y * CH);
        display_line(t, y, &cells, &n);
        int mask_begin = dock_col, mask_end = dock_col;
        if (overlay) chrome_intrusion_span(y, &mask_begin, &mask_end);
        for (x = mask_begin; x < mask_end; x++) {
            uint32_t fg, bg;
            if (x >= n || (cells[x].wflag & 8)) {fg=DEF_FG;bg=DEF_BG;} else cell_colors(&cells[x], &fg, &bg);
            if (display_selected(t, x, y)) bg = SEL_BG;
            r_color(bg, 1);
            quad((float)(ox + x * CW), y0, (float)(ox + (x + 1) * CW), y0 + CH);
#ifndef _WIN32
            /* Quiet title circles tint the resolved background, including
               selection, but never the glyphs/decorations drawn afterward. */
            plat_render_title_background((float)(ox + x * CW), y0, (float)(ox + (x + 1) * CW), y0 + CH, bg, bg);
#endif
        }
        for (x = 0; x < n; x++) {
            const Cell *c = &cells[x];
            uint32_t fg, bg;
            int w;
#ifndef _WIN32
            if(tb->kind==TK_BROWSER && (c->wflag & 8)){mac_browser_draw_tile(tb,c,x,y);continue;}
#endif
            if (c->ch <= ' ' || c->wflag == W_CONT) continue;
            cell_colors(c, &fg, &bg);
            if (fg == bg) continue;
#ifndef _WIN32
            if(tb->kind==TK_BROWSER && c->link && c->link==browser_hover) fg=g_shown->pal[12];
#endif
            w = (c->wflag == W_WIDE) ? 2 : 1;
            r_color(fg, 1);
            draw_glyph(c->ch, (float)(ox + x * CW), y0, w, g_scale);
            /* the fonts have no bold; smear one of their pixels right, except box drawing */
            if ((c->attr & A_BOLD) && !(c->ch >= 0x2500 && c->ch <= 0x259F))
                draw_glyph(c->ch, (float)(ox + x * CW + smear()), y0, w, g_scale);
        }
        for (x = 0; x < n; x++) {
            const Cell *c = &cells[x];
            uint32_t fg, bg;
            float x0 = (float)(ox + x * CW);
#ifndef _WIN32
            if(tb->kind==TK_BROWSER && (c->wflag & 8))continue;
            if(tb->kind==TK_BROWSER && c->link) {
                if(!(c->wflag & 8) && c->link!=browser_hover) {
                    cell_colors(c,&fg,&bg);fill_rect(x0,y0+CH-1,x0+CW,y0+CH,fg,.18f);
                }
                continue;
            }
#endif
            if (!(c->attr & (A_UNDER | A_STRIKE)) && !c->link) continue;
            cell_colors(c, &fg, &bg);
            if (fg == bg) continue;
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
        int cursor_col, cursor_row;
        display_cursor_position(t, &cursor_col, &cursor_row);
        float cx = (float)(ox + cursor_col * CW), cy = (float)(oy + cursor_row * CH);
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

    chrome_draw_markers(t);

    /* scrollbar, dumbterm-style thin amber thumb */
    if (t->hcount > 0 && !t->alt) {
        float track_top = (float)caption_h(), track_h = (float)(oy + t->rows * CH - caption_h());   /* starts under the glass */
        float total = (float)(t->hcount + t->rows);
        float bar_h = track_h * t->rows / total, pos, bx = (float)(term_ox() + t->cols * CW - SCROLLBAR_W + g_scale);
        if (bar_h < 12 * g_scale) bar_h = (float)(12 * g_scale);
        pos = 1.0f - (float)t->view / t->hcount;
        fill_rect(bx, track_top, bx + SCROLLBAR_W - 2 * g_scale, track_top + track_h, TAB_HOVER, 0.5f);
        fill_rect(bx, track_top + pos * (track_h - bar_h), bx + SCROLLBAR_W - 2 * g_scale,
                  track_top + pos * (track_h - bar_h) + bar_h, AMBER, t->view ? 0.8f : 0.3f);
    }
#ifdef _WIN32
    if (g_top_hover)                   /* the top line shows it's a title bar only while you point at it */
        fill_rect(0, 0, (float)(r.right - 3 * caption_btn_w()), (float)caption_h(), BAR_BG, 0.55f);
    render_caption_buttons(r.right);
    if (overlay && (chrome_hovered() || chrome_preview_active())) render_tabbar(r.right, r.bottom);
#else
    if (overlay && (chrome_hovered() || chrome_any_tv_hovered())) render_tabbar(r.right, r.bottom);
    mac_browser_draw_overlay();
    render_cuties(r.right);
    mac_camera_draw();
    mac_music_draw();
    plat_render_title_overlay();
#endif
}

static void update_title(void) {
    Tab *tb = active_tab();
    wchar_t label[256], title[300];
    if (!tb) return;
    tab_label(tb, label, 256);
    _snwprintf(title, 300, L"%ls — hardcore", label);
    title[299] = 0;
    plat_set_title(title);
}

#ifdef _WIN32
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

#else
static void snap_backbuffer(void);
static char g_snap_path[MAX_PATH];
#endif


static void r_present(void);

static void paint_now(void) {
    Tab *tb = active_tab();
#ifdef _WIN32
    LARGE_INTEGER p0, p1, p2;
#endif
    if (tb && tb->t.m_sync && GetTickCount() - tb->t.sync_since < 200) {
        plat_timer(TIMER_SYNC, 20);
        return;
    }
    /* a spare that just became this tab answers with its prompt within a
       few milliseconds: hold the frame so the tab and its prompt arrive
       together, instead of an empty tab a refresh or two before it. The mark
       (OSC 133;A) comes before your prompt hooks run; the cursor leaving
       column 0 means the prompt itself has been printed. */
    if (tb && tb->adopted && (!tb->t.nmarks || tb->t.cx == 0) && GetTickCount() - tb->adopted < 50) {
        plat_timer(TIMER_SYNC, 2);
        return;
    }
    if (tb && tb->adopted) {     /* held long enough: no stray repaint after this frame */
        tb->adopted = 0;
        plat_kill_timer(TIMER_SYNC);
    }
#ifdef _WIN32
    if (g_lat && g_lat_out.QuadPart) QueryPerformanceCounter(&g_lat_render);
    if (g_perf) QueryPerformanceCounter(&p0);
#endif
    render();
    if (g_snap_path[0]) {
        r_flush();
        snap_backbuffer();
        g_snap_path[0] = 0;
    }
#ifdef _WIN32
    if (g_perf) QueryPerformanceCounter(&p1);
#endif
    r_present();
#ifdef _WIN32
    slurp_commit_all();
    if (g_perf) {
        QueryPerformanceCounter(&p2);
        g_pf.build += g_pf_last_build = lat_ms(p0, p1);
        g_pf.present += g_pf_last_present = lat_ms(p1, p2);
        g_pf.frames++;
        perf_tick();
    }
    QueryPerformanceCounter(&g_last_paint);
    if (g_lat && g_nt_t0.QuadPart && active_tab() && active_tab()->id == g_nt_id) {
        LARGE_INTEGER t;
        Term *nt = &active_tab()->t;
        int y, x, ink = 0;
        QueryPerformanceCounter(&t);
        if (g_nt_shown < 0) g_nt_shown = lat_ms(g_nt_t0, t);
        for (y = 0; y < nt->rows && !ink; y++)
            for (x = 0; x < nt->rlen[nt->alt][y] && !ink; x++) ink = CELL(nt, x, y)->ch > ' ';
        if (ink) {
            FILE *f;
            wchar_t tp[MAX_PATH];
            _snwprintf(tp, MAX_PATH, L"%ls\\hardcore-newtab.txt", _wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L".");
            if ((f = _wfopen(tp, L"ab"))) { fprintf(f, "%.2f %.2f\n", g_nt_shown, lat_ms(g_nt_t0, t)); fclose(f); }
            g_nt_t0.QuadPart = 0;
        }
    }
    if (g_lat && g_lat_out.QuadPart) {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        lat_finish();
        snprintf(g_lat_line, sizeof(g_lat_line), "%.2f %.2f %.2f", lat_ms(g_lat_key, g_lat_out), lat_ms(g_lat_key, g_lat_render), lat_ms(g_lat_key, t));
        g_lat_pkey = g_lat_key;
        IDXGISwapChain1_GetLastPresentCount(g_sc, &g_lat_pid);
        g_lat_key.QuadPart = g_lat_out.QuadPart = 0;
    }
#else
    g_last_paint = GetTickCount();
#endif
}

/* Re-checks which tabs are animating a spinner title. A tab that stops while
   you're looking elsewhere gets the bell mark: that's "Claude is done". */
static int g_throb_on;
static void update_busy(int tick) {
    int i, any = 0, changed = 0;
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        int b = tb->kind == TK_BROWSER ? tb->busy : term_busy(&tb->t);
        if (tb->t.ps_state != tb->ps_seen) {
            /* Claude stopped to ask you something: say so if you aren't looking */
            if ((tb->t.ps_state == PS_BLOCKED || tb->t.ps_state == PS_ERROR) && !g_focused) plat_flash();
            tb->ps_seen = tb->t.ps_state;
            changed = 1;
        }
        if (b != tb->busy) {
            changed = 1;
            if (!b && (i != g_active || !g_focused)) {
                tb->attention = 1;
                if (!g_focused) plat_flash();   /* finished while you were in another window */
            }
            tb->busy = b;
        }
        any |= b;
    }
#ifndef _WIN32
    any |= mac_record_exports_active();
    any |= mac_music_loading();
#endif
    if (any && !g_throb_on) plat_timer(TIMER_THROB, DRIP_MS);
    if (!any && g_throb_on) plat_kill_timer(TIMER_THROB);
    g_throb_on = any;
    /* the throbber moves on its timer's ticks; output only redraws on a change */
    if ((tick && any) || changed) plat_invalidate();
}

/* ── tab management ───────────────────────────────────────────────── */

static void activate(int i) {
#ifndef _WIN32
    mac_tv_drag_end();mac_cutie_blur();
#endif
    if (i < 0 || i >= g_ntabs) return;
    chrome_preview_cancel();
    term_settle(&g_tabs[i]->t);  /* a tab not caught up since a resize: before it shows */
    g_active = i;
    resize_all();
    tab_fit(g_tabs[i]);          /* still at an old size if the window changed while it was hidden */
    g_tabs[i]->activity = 0;
    g_tabs[i]->attention = 0;
    g_sel_on = 0;
#ifdef _WIN32
    slurp_selection(g_tabs[i]);
#else
    mac_browser_sync();
#endif
    update_title();
    plat_invalidate();
}

static void schedule_spare(void) { plat_timer(TIMER_SPARE, 1000); }

#ifdef _WIN32
static int spare_ok(void) { return !g_shell_ps && !g_no_bash && g_have_distro; }
static int tab_running(const Tab *tb) { return tb->proc != NULL; }
#else
static int spare_ok(void);       /* macOS: your shell is zsh or bash (they have the hook that waits) */
static int tab_running(const Tab *tb) { return tb->fd >= 0; }
#endif

static void make_spare(void) {
    if (g_spare || !g_ntabs || !spare_ok()) return;
    g_spare = tab_create(g_start_dir, 1);
}

static void new_tab_here(void) {
    Tab *cur = active_tab(), *tb = NULL;
    int i;
    const char *dir = (cur && cur->t.cwd[0]) ? cur->t.cwd : cur ? cur->start_dir : g_start_dir;
#ifdef _WIN32
    if (cur && cur->slurp_hwnd && !cur->t.cwd[0]) dir = g_start_dir;
#endif
    if (g_ntabs >= MAX_TABS) return;
    /* the spare already has its shell (and wsl.exe) and your profile up; it only needs a cd */
    if (g_spare && !g_spare->exited && tab_running(g_spare)) {
        char lin[1100];
        tab_fit(g_spare);        /* before its prompt: the window may have changed since it was made */
        tb = g_spare;
        g_spare = NULL;
        snprintf(lin, sizeof(lin) - 1, "%s", dir);
        snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", lin);
        set_cwd(&tb->t, lin);
        strcat(lin, "\n");
        tab_write(tb, lin, (int)strlen(lin));
        tab_drain(tb);
        tb->spare = 0;
        tb->adopted = GetTickCount();
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

#ifdef _WIN32
/* ── sessions: close hardcore, open it again, and the tabs come back ──
   On close (and when Windows shuts down) every tab's shell, folder, text and
   the Claude Code session running in it go to %LOCALAPPDATA%\hardcore\session.
   The next start rebuilds them: the text replayed, the shell started under
   it, and claude --resume ID where Claude was running. Exiting the last tab
   yourself clears it. Test copies (--no-activate) leave it alone. */
static int g_session_off;
static int dir_exists(const char *p);

static void session_path(wchar_t *out, const wchar_t *name) {
    _snwprintf(out, MAX_PATH, L"%ls\\session\\%ls", g_cfg_dir, name);
    out[MAX_PATH - 1] = 0;
}

static void session_clear(void) {
    wchar_t p[MAX_PATH];
    if (g_session_off) return;
    session_path(p, L"session.txt");
    DeleteFileW(p);
}

static void session_save(void) {
    wchar_t dir[MAX_PATH], p[MAX_PATH];
    FILE *f;
    int i, n = 0;
    if (g_session_off || !g_ntabs) return;
    _snwprintf(dir, MAX_PATH, L"%ls\\session", g_cfg_dir);
    CreateDirectoryW(dir, NULL);
    session_path(p, L"session.txt");
    if (!(f = _wfopen(p, L"wb"))) return;
    fprintf(f, "# hardcore: the tabs open when the window closed, restored on the next start\n");
    fprintf(f, "active=%d\n", g_active);
    {
        WINDOWPLACEMENT wp = {sizeof(wp)};
        if (GetWindowPlacement(g_hwnd, &wp))
            fprintf(f, "placement=%u %ld %ld %ld %ld\n", wp.showCmd == SW_SHOWMINIMIZED ? SW_SHOWNORMAL : wp.showCmd,
                    wp.rcNormalPosition.left, wp.rcNormalPosition.top, wp.rcNormalPosition.right, wp.rcNormalPosition.bottom);
    }
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        wchar_t name[32];
        FILE *v;
        if (tb->help || tb->slurp_hwnd) continue;
        _snwprintf(name, 32, L"tab%d.txt", n);
        session_path(p, name);
        if ((v = _wfopen(p, L"wb"))) {
            term_dump(&tb->t, v, 3000);
            fclose(v);
        }
        {
            char cmd[400];
            int k;
            snprintf(cmd, sizeof(cmd), "%s", tb->t.claude[0] ? "" : tb->t.cmd);   /* Claude comes back on its own */
            for (k = 0; cmd[k]; k++) if (cmd[k] == '\r' || cmd[k] == '\n') cmd[k] = ' ';
            fprintf(f, "\n[tab]\nview=%d\nkind=%s\ndir=%s\nclaude=%s\nclaude_args=%s\ncmd=%s\ntext=tab%d.txt\n", tb->t.view,
                    tb->kind == TK_PS ? "powershell" : tb->kind == TK_CMD ? "cmd" : "wsl", tb->t.cwd[0] ? tb->t.cwd : tb->start_dir, tb->t.claude,
                    tb->t.claude[0] ? tb->t.claude_args : "", cmd, n);
        }
        n++;
    }
    fclose(f);
}

static char *read_file(const wchar_t *path, size_t *n) {
    FILE *f = _wfopen(path, L"rb");
    char *d;
    long len;
    *n = 0;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = (char *)malloc((size_t)len + 1);
    if (d && fread(d, 1, (size_t)len, f) == (size_t)len) { d[len] = 0; *n = (size_t)len; }
    else { free(d); d = NULL; }
    fclose(f);
    return d;
}

/* the flags go into it too: what printf %q makes of plain flags, nothing that
   could start another command (no $ ` ; & | < > quotes or newlines) */
static int session_args_ok(const char *a) {
    const char *p;
    for (p = a; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr(" -_./:=,@%+\\", *p)) return 0;
    return 1;
}

/* the id goes into a command bash evals: letters, digits and dashes only */
static int session_id_ok(const char *id) {
    const char *p;
    if (!id[0] || strlen(id) > 64) return 0;
    for (p = id; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-') return 0;
    return 1;
}

static void session_add_tab(const char *kind, const char *dir, const char *claude, const char *args, const char *cmd,
                            const char *text, int view) {
    wchar_t p[MAX_PATH], wname[64];
    char run[600] = "", *data = NULL;
    size_t n = 0;
    Tab *tb;
    if (g_ntabs >= MAX_TABS || !dir[0]) return;
    if (!session_args_ok(args)) args = "";
    if (!strcmp(claude, "?")) snprintf(run, sizeof(run), "claude %s--continue", args);
    else if (session_id_ok(claude)) snprintf(run, sizeof(run), "claude %s--resume %s", args, claude);
    if (text[0]) {
        utf8_to_wide(text, wname, 64);
        session_path(p, wname);
        data = read_file(p, &n);
    }
    tb = tab_create_ex(dir_exists(dir) ? dir : NULL, 0, !strcmp(kind, "powershell") ? TK_PS : !strcmp(kind, "cmd") ? TK_CMD : TK_WSL,
                       run[0] ? run : NULL, !run[0] && cmd[0] && !strcmp(kind, "wsl") ? cmd : NULL, data, n);
    free(data);
    if (tb) {
        tb->t.view = view > 0 ? (view < tb->t.hcount ? view : tb->t.hcount) : 0;   /* scrolled back as far as it was */
        g_tabs[g_ntabs++] = tb;
    }
}

/* The window's place when it closed (position, size, maximized), to open
   there again; before it's shown, so it doesn't jump. */
static int session_placement(WINDOWPLACEMENT *wp) {
    wchar_t p[MAX_PATH];
    size_t n;
    char *d, *pl;
    unsigned show;
    long l, t, r, b;
    int ok = 0;
    if (g_session_off) return 0;
    session_path(p, L"session.txt");
    if (!(d = read_file(p, &n))) return 0;
    if ((pl = strstr(d, "\nplacement=")) && sscanf(pl + 11, "%u %ld %ld %ld %ld", &show, &l, &t, &r, &b) == 5 && r > l && b > t) {
        memset(wp, 0, sizeof(*wp));
        wp->length = sizeof(*wp);
        wp->showCmd = show;
        wp->rcNormalPosition.left = l; wp->rcNormalPosition.top = t;
        wp->rcNormalPosition.right = r; wp->rcNormalPosition.bottom = b;
        ok = 1;
    }
    free(d);
    return ok;
}

/* Returns how many tabs came back. */
static int session_restore(void) {
    wchar_t p[MAX_PATH];
    size_t n;
    char *d, *line, *next, kind[16] = "", dir[1024] = "", claude[72] = "", args[400] = "", cmd[400] = "", text[64] = "";
    int active = 0, intab = 0, view = 0;
    if (g_session_off) return 0;
    session_path(p, L"session.txt");
    if (!(d = read_file(p, &n))) return 0;
    for (line = d; line && *line; line = next) {
        char *v, *e;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if ((e = strchr(line, '\r'))) *e = 0;
        if (!strcmp(line, "[tab]")) {
            if (intab) session_add_tab(kind, dir, claude, args, cmd, text, view);
            intab = 1;
            kind[0] = dir[0] = claude[0] = args[0] = cmd[0] = text[0] = 0;
            view = 0;
            continue;
        }
        if (line[0] == '#' || !(v = strchr(line, '='))) continue;
        *v++ = 0;
        if (!strcmp(line, "active")) active = atoi(v);
        else if (!strcmp(line, "kind")) snprintf(kind, sizeof(kind), "%s", v);
        else if (!strcmp(line, "dir")) snprintf(dir, sizeof(dir), "%s", v);
        else if (!strcmp(line, "claude")) snprintf(claude, sizeof(claude), "%s", v);
        else if (!strcmp(line, "claude_args")) snprintf(args, sizeof(args), "%s", v);
        else if (!strcmp(line, "cmd")) snprintf(cmd, sizeof(cmd), "%s", v);
        else if (!strcmp(line, "view")) view = atoi(v);
        else if (!strcmp(line, "text")) snprintf(text, sizeof(text), "%s", v);
    }
    if (intab) session_add_tab(kind, dir, claude, args, cmd, text, view);
    free(d);
    if (g_ntabs) activate(active >= 0 && active < g_ntabs ? active : 0);
    return g_ntabs;
}

/* ── reloading in place: a newer hardcore takes over without stopping anything ──
   `hardcore update` asks each open window to hand itself to the new exe. The
   programs in its tabs never notice: each tab's ConPTY (OpenConsole and its
   pipes) is kept alive by handing its handles to the new process, which goes
   on reading the same pipes. What's on the screen goes over as term_snapshot
   text, so the new build's structs can differ from this one's. The new window
   opens where this one is, draws, says "ok", and only then does this process
   end (without closing the pseudoconsoles: the new process holds them now).
   If anything goes wrong first, this window carries on as if nothing happened. */
static wchar_t g_exe_path[MAX_PATH];     /* where this exe was started from: an update puts the new one there */
static int g_adopting;                   /* this process is taking over from an older one */

/* conpty.dll's HPCON points at {signal pipe, reference handle, OpenConsole
   process}. Checked before relying on it: three live handles, the last one a
   process running OpenConsole (or conhost, for the inbox ConPTY). */
static int hpcon_parts(HPCON_ pc, HANDLE out[3]) {
    HANDLE *h = (HANDLE *)pc;
    DWORD flags, pid, n = MAX_PATH;
    wchar_t img[MAX_PATH], *b;
    HANDLE ph;
    int i, ok = 0;
    if (!pc) return 0;
    for (i = 0; i < 3; i++) if (!h[i] || !GetHandleInformation(h[i], &flags)) return 0;
    if (!(pid = GetProcessId(h[2])) || !(ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) return 0;
    if (QueryFullProcessImageNameW(ph, 0, img, &n)) {
        b = wcsrchr(img, L'\\');
        b = b ? b + 1 : img;
        ok = !_wcsicmp(b, L"OpenConsole.exe") || !_wcsicmp(b, L"conhost.exe");
    }
    CloseHandle(ph);
    if (ok) for (i = 0; i < 3; i++) out[i] = h[i];
    return ok;
}

static void reader_stop(Tab *tb) {
    int k;
    if (tb->wait) { UnregisterWaitEx(tb->wait, INVALID_HANDLE_VALUE); tb->wait = NULL; }
    for (k = 0; tb->reader && k < 400; k++) {     /* only works while it's inside ReadFile: retry */
        CancelSynchronousIo(tb->reader);
        if (WaitForSingleObject(tb->reader, 5) == WAIT_OBJECT_0) break;
    }
    if (tb->reader) { CloseHandle(tb->reader); tb->reader = NULL; }
    tab_drain(tb);                               /* what it read before stopping */
}

static void reader_start(Tab *tb) {
    tb->reader = CreateThread(NULL, 0, reader_thread, tb, 0, NULL);
    RegisterWaitForSingleObject(&tb->wait, tb->proc, on_child_exit, tb, INFINITE, WT_EXECUTEONLYONCE);
}

static void blob_line(SBuf *b, const char *key, const char *v) {
    char clean[1200];
    size_t k;
    snprintf(clean, sizeof(clean), "%s", v ? v : "");
    for (k = 0; clean[k]; k++) if (clean[k] == '\n' || clean[k] == '\r') clean[k] = ' ';
    sb_printf(b, "%s %s\n", key, clean);
}

/* Hands every live tab to EXE. Returns 1 once the new process has them (the
   caller then ends this one), 0 with *why on failure (nothing changed). */
static int reload_into(const wchar_t *exe, char *why, size_t cap) {
    Tab *live[MAX_TABS];
    HANDLE parts[MAX_TABS][3], blob_r = NULL, blob_w = NULL, ack_r = NULL, ack_w = NULL, list[2];
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T asz = 0;
    WINDOWPLACEMENT wp = {sizeof(wp)};
    SBuf b = {0};
    char ack[8] = "";
    DWORD w, got = 0, avail;
    int i, n = 0, active = 0, k;
    wchar_t cl[MAX_PATH + 4];
    for (i = 0; i < g_ntabs; i++) if (g_tabs[i]->slurp_hwnd) {
        snprintf(why, cap, "detach native slurped consoles before reloading"); return 0;
    }
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tb->help || tb->exited || !tb->hpc) continue;           /* nothing running: not carried over */
        if (!hpcon_parts(tb->hpc, parts[n])) { snprintf(why, cap, "this ConPTY doesn't allow it"); return 0; }
        if (i == g_active) active = n;
        live[n++] = tb;
    }
    if (!n) { snprintf(why, cap, "no running tabs"); return 0; }
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) { snprintf(why, cap, "no exe to reload into"); return 0; }
    if (!CreatePipe(&blob_r, &blob_w, &sa, 1 << 20) || !CreatePipe(&ack_r, &ack_w, &sa, 0)) { snprintf(why, cap, "pipes"); return 0; }
    SetHandleInformation(blob_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ack_r, HANDLE_FLAG_INHERIT, 0);
    for (i = 0; i < n; i++) reader_stop(live[i]);     /* from here no output is read here; it waits in the pipes */
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = blob_r;
    si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = ack_w;
    list[0] = blob_r;
    list[1] = ack_w;
    InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, asz);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asz);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof(list), NULL, NULL);
    _snwprintf(cl, MAX_PATH + 4, L"\"%ls\"", exe);
    SetEnvironmentVariableW(L"HARDCORE_ADOPT", L"1");
    if (!CreateProcessW(exe, cl, NULL, NULL, TRUE, CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL, &si.StartupInfo, &pi)) {
        SetEnvironmentVariableW(L"HARDCORE_ADOPT", NULL);
        snprintf(why, cap, "couldn't start the new exe (error %lu)", GetLastError());
        goto undo;
    }
    SetEnvironmentVariableW(L"HARDCORE_ADOPT", NULL);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    si.lpAttributeList = NULL;
    CloseHandle(blob_r); blob_r = NULL;
    CloseHandle(ack_w); ack_w = NULL;
    GetWindowPlacement(g_hwnd, &wp);
    sb_printf(&b, "HARDCORE-ADOPT 1\nplacement %u %ld %ld %ld %ld\nactive %d\n", wp.showCmd,
              wp.rcNormalPosition.left, wp.rcNormalPosition.top, wp.rcNormalPosition.right, wp.rcNormalPosition.bottom, active);
    for (i = 0; i < n; i++) {
        Tab *tb = live[i];
        Term *t = &tb->t;
        HANDLE src[6] = {tb->pin_w, tb->pout_r, tb->proc, parts[i][0], parts[i][1], parts[i][2]}, dst[6];
        SBuf snap = {0};
        char tmp[800];
        for (k = 0; k < 6; k++)
            if (!DuplicateHandle(GetCurrentProcess(), src[k], pi.hProcess, &dst[k], 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                snprintf(why, cap, "couldn't hand over a tab (error %lu)", GetLastError());
                TerminateProcess(pi.hProcess, 1);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                free(b.p);
                goto undo;
            }
        term_snapshot(t, &snap);
        sb_printf(&b, "tab\nkind %d\nsize %d %d\nview %d\n", tb->kind, t->cols, t->rows, t->view);
        sb_printf(&b, "handles %llx %llx %llx %llx %llx %llx\n", (unsigned long long)(uintptr_t)dst[0], (unsigned long long)(uintptr_t)dst[1],
                  (unsigned long long)(uintptr_t)dst[2], (unsigned long long)(uintptr_t)dst[3], (unsigned long long)(uintptr_t)dst[4],
                  (unsigned long long)(uintptr_t)dst[5]);
        blob_line(&b, "dir", tb->start_dir);
        blob_line(&b, "cwd", t->cwd);
        blob_line(&b, "claude", t->claude);
        blob_line(&b, "claude_args", t->claude_args);
        blob_line(&b, "cmd", t->cmd);
        WideCharToMultiByte(CP_UTF8, 0, t->title, -1, tmp, sizeof(tmp), NULL, NULL);
        blob_line(&b, "title", tmp);
        sb_printf(&b, "ps %d %d %d %d\n", t->ps_state, t->ps_pct, t->ps_tasks, t->progress);
        blob_line(&b, "ps_kind", t->ps_kind);
        WideCharToMultiByte(CP_UTF8, 0, t->ps_msg, -1, tmp, sizeof(tmp), NULL, NULL);
        blob_line(&b, "ps_msg", tmp);
        sb_printf(&b, "marks %d", t->nmarks);
        for (k = 0; k < t->nmarks; k++) sb_printf(&b, " %lld", t->hpushed - t->marks[k]);
        sb_printf(&b, "\nsnap %zu\n", snap.n);
        sb_put(&b, snap.p ? snap.p : "", snap.n);
        sb_put(&b, "\n", 1);
        free(snap.p);
    }
    sb_put(&b, "end\n", 4);
    WriteFile(blob_w, b.p, (DWORD)b.n, &w, NULL);
    CloseHandle(blob_w); blob_w = NULL;
    free(b.p);
    AllowSetForegroundWindow(pi.dwProcessId);
    ResumeThread(pi.hThread);
    for (k = 0; k < 1500; k++) {                  /* up to 15 s for "ok": D3D, the window, a frame */
        if (PeekNamedPipe(ack_r, NULL, 0, NULL, &avail, NULL) && avail) { ReadFile(ack_r, ack, sizeof(ack) - 1, &got, NULL); break; }
        if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0) break;
    }
    if (got >= 2 && !strncmp(ack, "ok", 2)) {
        CloseHandle(ack_r);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return 1;
    }
    snprintf(why, cap, "the new hardcore didn't take over");
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
undo:
    if (si.lpAttributeList) { DeleteProcThreadAttributeList(si.lpAttributeList); HeapFree(GetProcessHeap(), 0, si.lpAttributeList); }
    if (blob_r) CloseHandle(blob_r);
    if (blob_w) CloseHandle(blob_w);
    if (ack_r) CloseHandle(ack_r);
    if (ack_w) CloseHandle(ack_w);
    for (i = 0; i < n; i++) reader_start(live[i]);   /* carry on as before */
    return 0;
}

/* The new side: reads what reload_into sent on stdin. Returns the tabs made. */
static char *g_adopt_blob;
static WINDOWPLACEMENT g_adopt_wp;
static int g_adopt_active;

static void adopt_read(void) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    size_t n = 0, cap = 0;
    DWORD got;
    char *d = NULL;
    for (;;) {
        if (n + 65537 > cap) { char *q = (char *)realloc(d, cap = (n + 65537) * 2); if (!q) break; d = q; }
        if (!ReadFile(in, d + n, 65536, &got, NULL) || !got) break;
        n += got;
    }
    if (d) d[n] = 0;
    if (!d || strncmp(d, "HARDCORE-ADOPT 1\n", 17)) { free(d); return; }
    g_adopt_blob = d;
    g_adopting = 1;
    {
        unsigned show = SW_SHOWNORMAL;
        long l, t, r, bo;
        if (sscanf(strstr(d, "placement ") + 10, "%u %ld %ld %ld %ld", &show, &l, &t, &r, &bo) == 5) {
            g_adopt_wp.length = sizeof(g_adopt_wp);
            g_adopt_wp.showCmd = show;
            g_adopt_wp.rcNormalPosition.left = l; g_adopt_wp.rcNormalPosition.top = t;
            g_adopt_wp.rcNormalPosition.right = r; g_adopt_wp.rcNormalPosition.bottom = bo;
        }
        if (strstr(d, "\nactive ")) g_adopt_active = atoi(strstr(d, "\nactive ") + 8);
    }
}

static int adopt_tabs(void) {
    char *p = g_adopt_blob, *line;
    Tab *tb = NULL;
    if (!p) return 0;
    while ((line = p) && *p) {
        char *nl = strchr(p, '\n'), *v;
        if (!nl) break;
        *nl = 0;
        p = nl + 1;
        v = strchr(line, ' ');
        if (v) *v++ = 0; else v = line + strlen(line);
        if (!strcmp(line, "tab")) {
            if (g_ntabs >= MAX_TABS) break;
            tb = (Tab *)calloc(1, sizeof(Tab));
            if (!tb) break;
            tb->id = g_next_id++;
            InitializeCriticalSection(&tb->lock);
            tb->born = GetTickCount() - 60000;    /* not a new tab: an early exit isn't a setup problem */
        } else if (!tb) continue;
        else if (!strcmp(line, "kind")) tb->kind = atoi(v);
        else if (!strcmp(line, "size")) {
            int c = 80, r = 24;
            sscanf(v, "%d %d", &c, &r);
            if (!term_init(&tb->t, c, r)) { free(tb); tb = NULL; }
        } else if (!strcmp(line, "view")) tb->t.view = atoi(v);
        else if (!strcmp(line, "handles")) {
            unsigned long long h[6] = {0};
            HANDLE *pc = (HANDLE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HANDLE) * 3);
            sscanf(v, "%llx %llx %llx %llx %llx %llx", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]);
            tb->pin_w = (HANDLE)(uintptr_t)h[0];
            tb->pout_r = (HANDLE)(uintptr_t)h[1];
            tb->proc = (HANDLE)(uintptr_t)h[2];
            if (pc) { pc[0] = (HANDLE)(uintptr_t)h[3]; pc[1] = (HANDLE)(uintptr_t)h[4]; pc[2] = (HANDLE)(uintptr_t)h[5]; }
            tb->hpc = pc;                         /* the same shape conpty.dll's own HPCON has (checked by the old side) */
        } else if (!strcmp(line, "dir")) snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", v);
        else if (!strcmp(line, "cwd")) snprintf(tb->t.cwd, sizeof(tb->t.cwd), "%s", v);
        else if (!strcmp(line, "claude")) snprintf(tb->t.claude, sizeof(tb->t.claude), "%s", v);
        else if (!strcmp(line, "claude_args")) snprintf(tb->t.claude_args, sizeof(tb->t.claude_args), "%s", v);
        else if (!strcmp(line, "cmd")) snprintf(tb->t.cmd, sizeof(tb->t.cmd), "%s", v);
        else if (!strcmp(line, "title")) utf8_to_wide(v, tb->t.title, 256);
        else if (!strcmp(line, "ps")) sscanf(v, "%d %d %d %d", &tb->t.ps_state, &tb->t.ps_pct, &tb->t.ps_tasks, &tb->t.progress);
        else if (!strcmp(line, "ps_kind")) snprintf(tb->t.ps_kind, sizeof(tb->t.ps_kind), "%s", v);
        else if (!strcmp(line, "ps_msg")) utf8_to_wide(v, tb->t.ps_msg, 200);
        else if (!strcmp(line, "marks")) {
            char *q = v;
            int m = (int)strtol(q, &q, 10), k;
            long long off[512];
            for (k = 0; k < m && k < 512; k++) off[k] = strtoll(q, &q, 10);
            tb->t.nmarks = 0;
            tb->adopt_marks = k;            /* applied after the snapshot sets hpushed */
            memcpy(tb->mark_off, off, sizeof(long long) * (size_t)k);
        } else if (!strcmp(line, "snap")) {
            size_t len = (size_t)strtoull(v, NULL, 10);
            int view = tb->t.view, k;
            if (strlen(p) < len) break;
            tb->t.view = 0;
            vt_feed(&tb->t, (const unsigned char *)p, (int)len);
            p += len + 1;
            for (k = 0; k < tb->adopt_marks; k++) tb->t.marks[tb->t.nmarks++] = tb->t.hpushed - tb->mark_off[k];
            tb->t.view = view < tb->t.hcount ? view : tb->t.hcount;
            tb->t.side = 1;
            reader_start(tb);
            g_tabs[g_ntabs++] = tb;
            tb = NULL;
        }
    }
    free(g_adopt_blob);
    g_adopt_blob = NULL;
    if (g_ntabs) {
        int i;
        activate(g_adopt_active >= 0 && g_adopt_active < g_ntabs ? g_adopt_active : 0);
        for (i = 0; i < g_ntabs; i++) if (i != g_active) g_tabs[i]->activity = 0;
    }
    return g_ntabs;
}

/* told by the new process: it has the tabs and has drawn them */
static void adopt_done(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD w;
    if (!g_adopting || !out || out == INVALID_HANDLE_VALUE) return;
    WriteFile(out, "ok\n", 3, &w, NULL);
    CloseHandle(out);
    SetStdHandle(STD_OUTPUT_HANDLE, NULL);
    SetStdHandle(STD_ERROR_HANDLE, NULL);
    g_adopting = 0;
}

#endif

/* The folder the window is closing in, for the next window to open in. */
static void remember_dir(Tab *tb) {
    const char *d;
    if (!tb || tb->help || tb->kind == TK_BROWSER) return;
#ifdef _WIN32
    if (tb->slurp_hwnd) return;
#endif
    d = tb->t.cwd[0] ? tb->t.cwd : tb->start_dir;
    if (d[0]) snprintf(g_last_dir, sizeof(g_last_dir), "%s", d);
}

static void close_tab(int i) {
    chrome_preview_cancel();
    Tab *tb;
    if (i < 0 || i >= g_ntabs) return;
    tb = g_tabs[i];
#ifndef _WIN32
    if(tb->id==g_cutie_focus_id)mac_cutie_blur();
#endif
    memmove(&g_tabs[i], &g_tabs[i + 1], sizeof(Tab *) * (g_ntabs - i - 1));
    g_ntabs--;
    if (!g_ntabs) {             /* the last tab: its folder is where the next window opens */
        remember_dir(tb);
        config_save();
#ifdef _WIN32
        session_clear();         /* you closed them all yourself: the next window starts fresh */
#endif
    }
    tab_destroy_async(tb);
    if (!g_ntabs) {
        plat_close_window();
        return;
    }
    if (g_active > i || g_active >= g_ntabs) g_active--;
    if (g_active < 0) g_active = 0;
    activate(g_active);
}

/* ── keyboard ──────────────────────────────────────────────────────── */

static void scroll_view(int lines) {
    Tab *tb = active_tab();
#ifndef _WIN32
    if (tb && tb->kind == TK_BROWSER) { mac_browser_scroll(-lines); return; }
#endif
    if (!tb || tb->t.alt) return;
    term_settle(&tb->t);         /* scrolling back: the old lines at today's width first */
    tb->t.view += lines;
    if (tb->t.view > tb->t.hcount) tb->t.view = tb->t.hcount;
    if (tb->t.view < 0) tb->t.view = 0;
    plat_invalidate();
}

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
    term_settle(t);
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
    plat_invalidate();
}

#ifdef _WIN32
static int mods_param(void) {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= 1;
    if (GetKeyState(VK_MENU) & 0x8000) m |= 2;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= 4;
    return m ? m + 1 : 0;
}

static int auto_scale(void) { return g_dpi >= 144 ? (g_dpi + 48) / 96 : 1; }

/* Returns 1 if the key was consumed (its WM_CHAR is then dropped). */
static int help_key(Tab *tb, uint32_t c);

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
    if (tb->help && vk == VK_RETURN && !ctrl && !alt) return help_key(tb, '\r');

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
    /* AltGr arrives as Ctrl+Alt with the right Alt down; on many layouts
       AltGr+7/8/9/0 is how you type { [ ] }, so it's never a tab jump */
    if (ctrl && alt && !(GetKeyState(VK_RMENU) & 0x8000) && vk >= '1' && vk <= '9') { activate(vk == '9' ? g_ntabs - 1 : (int)(vk - '1')); return 1; }
    if (shift && !ctrl && !alt) {
        switch (vk) {
        case VK_PRIOR: scroll_view(tb->t.rows - 1); return 1;
        case VK_NEXT: scroll_view(-(tb->t.rows - 1)); return 1;
        case VK_INSERT: paste(); return 1;
        }
    }
    if (ctrl && !shift && vk == VK_INSERT) { sel_copy(); return 1; }

    if (tb->slurp_hwnd && slurp_key(tb, vk)) return 1;

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
    Tab *tb = active_tab();
    if (tb && tb->help) { help_key(tb, cp); return; }
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

#endif /* _WIN32: keys */

/* ── mouse ─────────────────────────────────────────────────────────── */

enum { M_SHIFT = 1, M_ALT = 2, M_CTRL = 4 };   /* the modifiers a mouse report carries */

static void mouse_cell(int mx, int my, int *col, int *row) {
    Tab *tb = active_tab();
    int c = (mx - term_ox()) / CW, r = (my - term_oy()) / CH;
    if (mx < term_ox()) c = 0;
    if (my < term_oy()) r = 0;
    if (tb) {
        if (c >= tb->t.cols) c = tb->t.cols - 1;
        if (r >= tb->t.rows) r = tb->t.rows - 1;
    }
    if (tb) {
        int source = display_source(&tb->t, c, r);
        c = source % tb->t.cols; r = source / tb->t.cols;
    }
    *col = c;
    *row = r;
}

/* Run the real VT -> display adapter without AppKit/Win32 window creation.
   Include the styled Codex records from the real dock-overlap report: their
   expansion footers and hashes must move with them, never disappear. */
static int unit_chrome_display(FILE *f) {
    static const char paragraph[] =
        "\x1b[2;1H  Build and non-GUI tests pass. The disposable test app crashed during macOS registration, before rendering. Current sandbox restrictions prevent GUI"
        "\x1b[3;1H  verification, committing, and live reload, so your running window still uses the previous build."
        "\x1b[5;1H456\x1b[8;1H";
    static const char diff[] =
        "\x1b[2;1H\x1b[48;2;24;64;32m  5313 +    int result = snprintf(buffer, sizeof(buffer), \"/Users/example/Library/Support/hardcore/preferences/profile.json\", mouse_modes[mouse]);\x1b[K\x1b[0m"
        "\x1b[3;1H  + Show details\x1b[5;1H456\x1b[8;1H";
    static const char command[] =
        "\x1b[2;1H\x1b[32m•\x1b[0m Ran \x1b[36mhardcore\x1b[0m \x1b[31m--version\x1b[0m && codesign --verify --deep --strict "
        "\x1b]8;;file:///Users/wm/Applications/hardcore.app\x1b\\/Users/wm/Applications/hardcore.app\x1b]8;;\x1b\\ && git status --sho…"
        "\x1b[3;1H\x1b[2m  └ ## main...origin/main"
        "\x1b[4;1H    db1eefd120ebb6a3b3090ab988593954a9d37ae9"
        "\x1b[5;1H    db1eefd120ebb6a3b3090ab988593954a9d37ae9"
        "\x1b[6;1H    + 1 line (⌃t to expand)\x1b[0m"
        "\x1b[8;1H456\x1b[12;1H";
    static const char verification[] =
        "\x1b[2;1H\x1b[32m•\x1b[0m Ran \x1b[36mpython3\x1b[0m - <<'PY' …"
        "\x1b[3;1H\x1b[2m  └ Installed executable inode: 215350339"
        "\x1b[4;1H    Verification records: \x1b]8;;file:///var/folders/k9/z44_w2td7p19nrwm8gztmcc80000gn/T/hc-update-verification-8rcflsus\x1b\\"
        "/var/folders/k9/z44_w2td7p19nrwm8gztmcc80000gn/T/hc-update-verification-8rcflsus\x1b]8;;\x1b\\"
        "\x1b[5;1H    843:    g_mac_image = [mac_hash(@(g_mac_exe_path)) retain];"
        "\x1b[6;1H    + 3 lines (⌃t to expand)\x1b[0m"
        "\x1b[8;1H456\x1b[12;1H";
    static const char command_above[] =
        "\x1b[1;1H123\x1b[4;1H\x1b[32m•\x1b[0m Ran \x1b[36mhardcore\x1b[0m \x1b[31m--version\x1b[0m && codesign --verify --deep --strict "
        "\x1b]8;;file:///Users/wm/Applications/hardcore.app\x1b\\/Users/wm/Applications/hardcore.app\x1b]8;;\x1b\\ && git status --sho…"
        "\x1b[5;1H\x1b[2m  └ ## main...origin/main"
        "\x1b[6;1H    db1eefd120ebb6a3b3090ab988593954a9d37ae9"
        "\x1b[7;1H    db1eefd120ebb6a3b3090ab988593954a9d37ae9"
        "\x1b[8;1H    + 1 line (⌃t to expand)\x1b[0m"
        "\x1b[9;1H456\x1b[12;1H";
    static const struct {
        const char *name, *text;
        int cols, rows, sentinel, dock_y, dock_height, above_row;
    } fixtures[] = {
        {"paragraph", paragraph, 152, 8, 4, 1, 4, -1},
        {"diff", diff, 152, 8, 4, 1, 4, -1},
        {"Codex command", command, 120, 12, 7, 1, 6, -1},
        {"Codex verification", verification, 120, 12, 7, 1, 6, -1},
        {"Codex preceding blanks", command_above, 120, 12, 8, 1, 7, 3},
    };
    Tab *tb = (Tab *)calloc(1, sizeof(*tb)), *saved_tab = g_tabs[0];
    int saved_ntabs = g_ntabs, saved_active = g_active, saved_mode = g_tab_underlay, saved_reflow = g_chrome_reflow;
    ChromeReflow saved_display = g_display;
    const Term *saved_display_term = g_display_term;
    int saved_view = g_display_view, saved_hcount = g_display_hcount, fails = 0;
    if (!tb) return 1;
    memset(&g_display, 0, sizeof(g_display)); g_display_term = NULL;
    g_tabs[0] = tb; g_ntabs = 1; g_active = 0; g_chrome_reflow = 1;
    static const int mouse_modes[] = {0, 1000, 1002, 1003};
    for (int fixture = 0; fixture < (int)(sizeof(fixtures)/sizeof(fixtures[0])); fixture++) {
        int cols = fixtures[fixture].cols, rows = fixtures[fixture].rows;
        if (!term_init(&tb->t, cols, rows)) { fails++; break; }
        for (int side = 1; side <= 2; side++) for (int mouse = 0; mouse < 4; mouse++) {
            char modes[64];
            int n = snprintf(modes, sizeof(modes), "\x1b" "c\x1b[?25l\x1b[?%dh\x1b[?1006h", mouse ? mouse_modes[mouse] : 1000);
            vt_feed(&tb->t, (const unsigned char *)modes, n);
            if (!mouse) vt_feed(&tb->t, (const unsigned char *)"\x1b[?1000l", 8);
            vt_feed(&tb->t, (const unsigned char *)fixtures[fixture].text, (int)strlen(fixtures[fixture].text));
            g_tab_underlay = side;
            int dock = side == 1 ? cols-28 : 0, mode = tb->t.m_mouse;
            int seen[152*12] = {0}, above = 0, links = 0, dim = 0, initial_fails = fails;
            Cell before[152*12];
            for (int y = 0; y < rows; y++) memcpy(before+y*cols, tb->t.row[0][y], cols*sizeof(Cell));
            chrome_prepare_display_at(&tb->t, dock, fixtures[fixture].dock_y, fixtures[fixture].dock_height);
            fails += g_display_term != &tb->t || !g_display.changed;
            fails += mode != mouse_modes[mouse] || tb->t.m_mouse != mode || !tb->t.m_mouse_sgr;
            for (int y = 0; y < rows; y++) {
                fails += memcmp(before+y*cols, tb->t.row[0][y], cols*sizeof(Cell)) != 0;
                if (!g_display.cells) continue;
                for (int x = 0; x < cols; x++) {
                    const Cell *c = g_display.cells+y*cols+x;
                    if (y >= fixtures[fixture].dock_y && y < fixtures[fixture].sentinel &&
                        y < fixtures[fixture].dock_y+fixtures[fixture].dock_height) {
                        int begin = side == 1 ? dock-2 : dock, end = side == 1 ? dock+28 : dock+30;
                        if (x >= begin && x < end) fails += !cr_space(c->ch);
                    }
                    if (!cr_space(c->ch)) {
                        int col, row, source = g_display.source[y*cols+x];
                        if (source < 0 || source >= cols*rows) { fails++; continue; }
                        mouse_cell(x*CW+CW/2, y*CH+CH/2, &col, &row);
                        fails += row*cols+col != source;
                        fails += memcmp(c, before+source, sizeof(Cell)) != 0;
                        seen[source]++;
                        links += c->link != 0;
                        dim += !!(c->attr & A_DIM);
                        above |= source/cols == fixtures[fixture].above_row && y < source/cols;
                    }
                }
            }
            for (int i = 0; i < cols*rows; i++)
                if (!cr_space(before[i].ch)) fails += seen[i] != 1;
            if (g_display.cells) {
                fails += g_display.cells[fixtures[fixture].sentinel*cols].ch != '4';
                if (fixtures[fixture].above_row >= 0) {
                    fails += !above || g_display.cells[0].ch != '1';
                } else if (fixture >= 2) {
                    /* Following blanks win; an earlier blank must stay unused. */
                    for (int x = 0; x < cols; x++) fails += !cr_space(g_display.cells[x].ch);
                }
            }
            if (fixture >= 2) fails += !links || !dim;
            if (fails != initial_fails)
                fprintf(f, "FAIL chrome display fixture %s, side %d, mouse %d: %d checks\n",
                        fixtures[fixture].name, side, mouse_modes[mouse], fails-initial_fails);
        }
        term_free(&tb->t);
    }
    chrome_reflow_free(&g_display);
    g_display = saved_display; g_display_term = saved_display_term;
    g_display_view = saved_view; g_display_hcount = saved_hcount;
    g_tabs[0] = saved_tab; g_ntabs = saved_ntabs; g_active = saved_active;
    g_tab_underlay = saved_mode; g_chrome_reflow = saved_reflow;
    free(tb);
    fprintf(f, "%s chrome display: styled Codex records/footers/links, adjacent blanks, menu padding, both docks, mouse/source cells preserved\n", fails ? "FAIL" : "ok  ");
    return fails;
}

/* reports to an app that asked for the mouse (vim, htop, tmux...);
   mods: M_SHIFT | M_ALT | M_CTRL. Shift means "this one is for selecting". */
static int mouse_report(int button, int press, int motion, int mx, int my, int mods) {
    Tab *tb = active_tab();
    char buf[64];
    int col, row, b = button, n;
    if (!tb || !tb->t.m_mouse || (mods & M_SHIFT)) return 0;
#ifndef _WIN32
    if (mac_input_held(tb)) return 1;
#endif
    if (motion && tb->t.m_mouse != 1003 && !(tb->t.m_mouse == 1002 && button != 3)) return 1;
    if (motion && tb->t.m_mouse == 9) return 1;
    mouse_cell(mx, my, &col, &row);
    if (mods & M_CTRL) b |= 16;
    if (mods & M_ALT) b |= 8;
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

#ifdef _WIN32
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
#else
static void open_url(const char *url);
#endif

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
    plat_invalidate();
}

/* Parser-only replay of one file for conformance corpora: it goes through a
   fresh terminal the way a pty would deliver it (ONLCR: \n → \r\n), then the
   screen is written out, one right-trimmed line per row. */
static void replay_one(FILE *out, FILE *f, int cols, int rows) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    char line[8192];
    unsigned char buf[65536];
    size_t n, k;
    int y;
    if (!f || !tb || !term_init(&tb->t, cols, rows)) {
        fprintf(out, "(cannot open)\n");
        free(tb);
        return;
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

/* --bench-resize OUT: what a window drag costs the layout. A terminal with
   full scrollback (HIST_LINES) of transcript-like text, mixed lengths, color
   and CJK, many lines long enough to wrap, is reflowed through the sizes a
   drag passes through, one step at a time, and each step is timed. */
static double bench_now_ms(void);

static void bench_fill(Term *t, int lines) {
    static const char *words[] = {"the", "terminal", "Claude", "\x1b[1mbuild\x1b[0m", "\x1b[32mpassed\x1b[0m", "src/hardcore.c:2810",
                                  "\x1b[38;2;255;166;13mwarning\x1b[0m", "日本語", "→", "✓", "reflow", "scrollback", "a", "of", "and"};
    char line[2048];
    unsigned seed = 1;
    int i;
    for (i = 0; i < lines; i++) {
        int n = 0, len, w;
        seed = seed * 1103515245u + 12345u;
        len = (int)((seed >> 16) % 7 == 0 ? 200 + (seed >> 8) % 300 : (seed >> 16) % 110);   /* 1 in 7 lines wraps */
        while (n < len && n < (int)sizeof(line) - 64) {
            seed = seed * 1103515245u + 12345u;
            w = (int)((seed >> 16) % (sizeof(words) / sizeof(words[0])));
            n += snprintf(line + n, sizeof(line) - n, "%s ", words[w]);
        }
        n += snprintf(line + n, sizeof(line) - n, "\r\n");
        vt_feed(t, (const unsigned char *)line, n);
    }
}

static void bench_series(FILE *out, const char *name, Term **ts, int nt, const int (*sizes)[2], int n) {
    double *v = (double *)malloc(sizeof(double) * n), total = 0, tmp;
    int i, j, k;
    for (i = 0; i < n; i++) {
        double t0 = bench_now_ms();
        for (k = 0; k < nt; k++) term_resize(ts[k], sizes[i][0], sizes[i][1]);   /* resize_all: every tab */
        v[i] = bench_now_ms() - t0;
        total += v[i];
    }
    for (i = 1; i < n; i++)      /* insertion sort: n is small */
        for (j = i; j > 0 && v[j] < v[j - 1]; j--) { tmp = v[j]; v[j] = v[j - 1]; v[j - 1] = tmp; }
    fprintf(out, "%-26s %4d steps  p50 %7.2f  p90 %7.2f  max %7.2f ms  total %8.1f ms\n", name, n, v[n / 2], v[n * 9 / 10], v[n - 1], total);
    free(v);
}

/* --unit, both platforms: what each kind of row leaves in history (rlen
   bookkeeping), and recycling past HIST_LINES. Returns the failures. */
/* --unit: cases taken from other terminals' bug trackers (Alacritty, Ghostty,
   kitty, WezTerm), each the bytes from the report and what the screen must
   hold afterwards. */
static void unit_row(const Term *t, int y, char *o, size_t cap) {
    size_t n = 0;
    int x, len = 0;
    for (x = 0; x < t->cols; x++) if (t->row[t->alt][y][x].ch != ' ' || t->row[t->alt][y][x].wflag) len = x + 1;
    for (x = 0; x < len && n + 5 < cap; x++) {
        const Cell *c = &t->row[t->alt][y][x];
        if (c->wflag & (W_CONT | W_PAD)) continue;
        n += (size_t)utf8_encode(c->ch ? c->ch : ' ', o + n);
    }
    o[n] = 0;
}

static int unit_issues(FILE *f) {
    static const struct {
        const char *name;
        int cols, rows;
        const char *before;
        int rc, rr;              /* resize to, or 0 */
        const char *after;
        const char *want[4];     /* rows 0.. (NULL: not checked) */
        int cx, cy;              /* -1: not checked */
    } cases[] = {
        {"pending wrap survives a wider resize (alacritty#3584, ghostty#12933)", 20, 3,
         "abcdefghijklmnopqrst", 30, 3, "XYZ", {"abcdefghijklmnopqrstXYZ", "", NULL}, 23, 0},
        {"pending wrap survives a narrower resize (wezterm#6669)", 10, 4,
         "0123456789", 5, 4, "X", {"01234", "56789", "X", NULL}, 1, 2},
        {"pending wrap after a wide char survives a resize", 10, 3,
         "abcdefgh\xe6\x97\xa5", 12, 3, "XYZ", {"abcdefgh\xe6\x97\xa5" "XY", "Z", NULL}, 1, 1},
        {"EL 2 ends a row's wrap: no joining on resize (ghostty D#13616)", 10, 6,
         "-------------------------\x1b[1;1H\x1b[2KAAA\x1b[2;1H\x1b[2KBBB\x1b[3;1H\x1b[KCCC\x1b[5;1H", 20, 6, "",
         {"AAA", "BBB", "CCC", NULL}, -1, -1},
        {"a row rewritten to its end then CR LF isn't wrapped (alacritty#8930)", 10, 3,
         "012345678901234\x1b[HABCDEFGHIJ\r\nxy", 20, 3, "", {"ABCDEFGHIJ", "xy234", NULL}, -1, -1},
        {"ESC 7 on the main screen follows its text through reflow (wezterm#6669)", 20, 6,
         "0123456789abcdefghij\r\nsecond\r\n$ \x1b" "7", 10, 6, "\x1b" "8X", {"0123456789", "abcdefghij", "second", "$ X"}, -1, -1},
        {"a stray DCS doesn't swallow what follows (alacritty#6714, wezterm#1844)", 20, 3,
         "\x1b_Gabc\x1b[31mhello\r\nworld", 0, 0, "", {"hello", "world", NULL}, -1, -1},
        {"CAN aborts a DCS", 20, 3, "\x1bPq#0;2;0~~\x18" "ok", 0, 0, "", {"ok", NULL}, -1, -1},
        {"CAN aborts an OSC", 20, 3, "\x1b]0;title\x18hello", 0, 0, "", {"hello", NULL}, -1, -1},
        {"invalid UTF-8 is U+FFFD (overlong, surrogate, past U+10FFFF)", 20, 3,
         "a\xc0\xaf" "b\xed\xa0\x80" "c\xf4\x90\x80\x80" "d", 0, 0, "",
         {"a\xef\xbf\xbd" "b\xef\xbf\xbd" "c\xef\xbf\xbd" "d", NULL}, -1, -1},
        {"DEL inside a CSI is ignored (vt100.net)", 20, 3, "a\x1b[3\x7f" "Cb", 0, 0, "", {"a   b", NULL}, -1, -1},
        {"an 8-bit byte voids a CSI up to its final", 20, 3, "a\x1b[3\xc3\xa9" "Cb", 0, 0, "", {"ab", NULL}, -1, -1},
        {"EL from the middle of a wide char erases all of it (alacritty#8786)", 8, 2,
         "\xe6\x97\xa5\xe6\x9c\xacX\x1b[1;2H\x1b[K", 0, 0, "", {"", NULL}, -1, -1},
        {"DCH from the middle of a wide char", 8, 2, "\xe6\x97\xa5\xe6\x9c\xacX\x1b[1;2H\x1b[P", 0, 0, "",
         {" \xe6\x9c\xacX", NULL}, -1, -1},
        {"ICH pushing a wide char off the edge", 8, 2, "abcdef\xe6\x97\xa5\x1b[1;1H\x1b[@", 0, 0, "", {" abcdef", NULL}, -1, -1},
        {"HTS past the right edge (wezterm#8134)", 8, 4, "\x1b[;9H\x1bH\x1b[;100H\x1bH\x1b[999;999H\x1bHX", 0, 0, "",
         {NULL}, 7, 3},
    };
    int i, fails = 0;
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        Term t;
        char got[256], why[512] = "";
        int r, ok = term_init(&t, cases[i].cols, cases[i].rows);
        if (!ok) { fails++; continue; }
        vt_feed(&t, (const unsigned char *)cases[i].before, (int)strlen(cases[i].before));
        if (cases[i].rc) term_resize(&t, cases[i].rc, cases[i].rr);
        vt_feed(&t, (const unsigned char *)cases[i].after, (int)strlen(cases[i].after));
        for (r = 0; r < 4 && cases[i].want[r] && ok; r++) {
            unit_row(&t, r, got, sizeof(got));
            if (strcmp(got, cases[i].want[r])) { snprintf(why, sizeof(why), "row %d [%s] want [%s]", r, got, cases[i].want[r]); ok = 0; }
        }
        if (ok && cases[i].cx >= 0 && (t.cx != cases[i].cx || t.cy != cases[i].cy)) {
            snprintf(why, sizeof(why), "cursor (%d,%d) want (%d,%d)", t.cx, t.cy, cases[i].cx, cases[i].cy);
            ok = 0;
        }
        fprintf(f, "%s issue %s%s%s\n", ok ? "ok  " : "FAIL", cases[i].name, ok ? "" : ": ", why);
        fails += !ok;
        term_free(&t);
    }
    {   /* SGR colon sub-parameters (alacritty/vte#22) */
        Term t;
        int ok = term_init(&t, 20, 3);
        if (ok) {
            const char *in = "\x1b[1m\x1b[4:3mX\x1b[58:2::255:0:0mY\x1b[38:2::255:0:0mZ\x1b[4:0mW\x1b[48:5:4mV";
            const Cell *c = t.row[0][0];
            vt_feed(&t, (const unsigned char *)in, (int)strlen(in));
            ok = c[0].attr == (A_BOLD | A_UNDER) && c[1].attr == (A_BOLD | A_UNDER) &&
                 c[2].attr == (A_BOLD | A_UNDER) && c[2].fg == C_RGB(255, 0, 0) &&
                 c[3].attr == A_BOLD && c[3].fg == C_RGB(255, 0, 0) && c[4].bg == C_PAL(4);
            term_free(&t);
        }
        fprintf(f, "%s issue SGR colon forms: 4:3, 58:2::r:g:b, 38:2::r:g:b, 4:0, 48:5:n\n", ok ? "ok  " : "FAIL");
        fails += !ok;
    }
    {   /* scrolled back with the ring full: the view keeps showing the same text */
        Term t;
        int ok = term_init(&t, 40, 10), n;
        char line[64], got[64], want[64];
        if (ok) {
            for (n = 0; n < HIST_LINES + 100; n++) vt_feed(&t, (const unsigned char *)line, snprintf(line, sizeof(line), "line %d\r\n", n));
            t.view = 100;
            {
                const HLine *h = &t.hist[(t.hstart + t.hcount - t.view) % HIST_LINES];
                snprintf(want, sizeof(want), "%.*s", h->n, "");
                for (n = 0; n < h->n && n < 63; n++) want[n] = (char)h->c[n].ch;
                want[n] = 0;
            }
            for (n = 0; n < 10; n++) vt_feed(&t, (const unsigned char *)"more\r\n", 6);
            {
                const HLine *h = &t.hist[(t.hstart + t.hcount - t.view) % HIST_LINES];
                for (n = 0; n < h->n && n < 63; n++) got[n] = (char)h->c[n].ch;
                got[n] = 0;
            }
            ok = !strcmp(got, want) && t.view == 110;
            term_free(&t);
        }
        fprintf(f, "%s issue scrolled back with full scrollback, output doesn't move the view\n", ok ? "ok  " : "FAIL");
        fails += !ok;
    }
    {   /* Claude Code killed with modes on: its exit marker turns them off (kitty) */
        Term t;
        int ok = term_init(&t, 20, 3);
        if (ok) {
            const char *in = "\x1b]7700;claude=abc\x1b\\\x1b[?1002h\x1b[?1006h\x1b[?1004h\x1b[?2026h\x1b[?25l\x1b]7700;claude=\x1b\\";
            vt_feed(&t, (const unsigned char *)in, (int)strlen(in));
            ok = !t.m_mouse && !t.m_mouse_sgr && !t.m_focus && !t.m_sync && t.m_cursor_vis && !t.claude[0];
            term_free(&t);
        }
        fprintf(f, "%s issue modes Claude Code left on end when it exits\n", ok ? "ok  " : "FAIL");
        fails += !ok;
    }
    {   /* repeated BSU: the hold's clock starts at the first one */
        Term t;
        int ok = term_init(&t, 20, 3);
        if (ok) {
            vt_feed(&t, (const unsigned char *)"\x1b[?2026h", 8);
            t.sync_since -= 1000;
            vt_feed(&t, (const unsigned char *)"\x1b[?2026h", 8);
            ok = GetTickCount() - t.sync_since >= 1000;
            term_free(&t);
        }
        fprintf(f, "%s issue a repeated 2026h can't hold painting off forever (alacritty#8550)\n", ok ? "ok  " : "FAIL");
        fails += !ok;
    }
    {   /* an OSC longer than the old 8 KB cap still lands; one past OSC_MAX is dropped whole */
        Term t;
        int ok = term_init(&t, 20, 3);
        if (ok) {
            size_t n = 20000, k;
            unsigned char *b = (unsigned char *)malloc(n + 16);
            memcpy(b, "\x1b]2;", 4);
            for (k = 4; k < n; k++) b[k] = 'a' + (char)(k % 26);
            b[n] = 7;
            vt_feed(&t, b, (int)n + 1);
            ok = t.osc_n == (int)n - 2 && !t.osc_over;
            vt_feed(&t, (const unsigned char *)"\x1b]2;", 4);
            for (k = 0; k < (size_t)OSC_MAX / 16384 + 1; k++) {
                memset(b, 'x', 16384 < n ? 16384 : n);
                vt_feed(&t, b, 16384 < (int)n ? 16384 : (int)n);
            }
            ok = ok && t.osc_over && t.osc_cap <= OSC_MAX;
            vt_feed(&t, (const unsigned char *)"\x07ok", 3);
            unit_row(&t, 0, (char *)b, 64);
            ok = ok && !strcmp((char *)b, "ok");
            free(b);
            term_free(&t);
        }
        fprintf(f, "%s issue OSC past 8 KB kept; past OSC_MAX dropped whole, never truncated (vte#151)\n", ok ? "ok  " : "FAIL");
        fails += !ok;
    }
    return fails;
}

static int unit_scrollback(FILE *f) {
    int fails = 0;
    {   /* scrollback: what each kind of row leaves in history (rlen bookkeeping) */
        static const struct { const char *in, *want; int n; } sb[] = {
            {"abc\r\n", "abc", 3},
            {"\x1b[41m\x1b[K\x1b[0m\r\n", "", 20},          /* colored blanks are kept */
            {"abcdefghij\r\x1b[K\r\n", "", 0},
            {"abcdefghij\x1b[5G\x1b[K\r\n", "abcd", 4},
            {"abc\x1b[1G\x1b[2@\r\n", "  abc", 5},
            {"abcdef\x1b[1G\x1b[2P\r\n", "cdef", 4},
            {"\xe6\x97\xa5\xe6\x9c\xac\r\n", "\xe6\x97\xa5\xe6\x9c\xac", 4},
            {"abcdefghijklmnopqrstuvwxyz\r\n", "abcdefghijklmnopqrst", 20},
        };
        int k;
        for (k = 0; k < (int)(sizeof(sb) / sizeof(sb[0])); k++) {
            Term t;
            char got[256];
            int len = 0, x, ok;
            const HLine *h;
            if (!term_init(&t, 20, 4)) return fails + 1;
            vt_feed(&t, (const unsigned char *)sb[k].in, (int)strlen(sb[k].in));
            vt_feed(&t, (const unsigned char *)"\x1b[4H\n\n\n\n", 8);   /* push it up into history */
            h = t.hcount ? &t.hist[t.hstart] : NULL;
            for (x = 0; h && x < h->n; x++)
                if (h->c[x].wflag != W_CONT) len += utf8_encode(h->c[x].ch, got + len);
            while (len && got[len - 1] == ' ') len--;
            got[len] = 0;
            ok = h && h->n == sb[k].n && !strcmp(got, sb[k].want);
            fprintf(f, "%s hist  #%d n=%d \"%s\"\n", ok ? "ok  " : "FAIL", k, h ? h->n : -1, got);
            fails += !ok;
            term_free(&t);
        }
        {   /* past HIST_LINES: the oldest lines go, their buffers are reused */
            Term t;
            char line[32];
            const HLine *h;
            int ok;
            if (!term_init(&t, 20, 4)) return fails + 1;
            for (k = 0; k < HIST_LINES + 10; k++) {
                int n = snprintf(line, sizeof(line), "line %d\r\n", k);
                vt_feed(&t, (const unsigned char *)line, n);
            }
            h = &t.hist[(t.hstart + t.hcount - 1) % HIST_LINES];
            ok = t.hcount == HIST_LINES && h->n == 10 && h->c[5].ch == '2' && h->c[9].ch == '6';   /* "line 20006" */
            fprintf(f, "%s hist  recycled, count %d\n", ok ? "ok  " : "FAIL", t.hcount);
            fails += !ok;
            term_free(&t);
        }
    }
    {   /* reflow: everything (history + screen) read back as text, one string per
           logical line, must survive any sequence of widths unchanged; includes
           a wide char that wraps at 20 cols and leaves a W_PAD gap */
        static const char *feed =
            "short\r\n"
            "a line that is long enough to wrap several times at twenty columns wide\r\n"
            "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e wide x\r\n"
            "\r\n"
            "a\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5 odd\r\n"
            "tail line one\r\n"
            "the last line also wraps around the edge of the screen";
        static const int widths[] = {13, 7, 31, 20, 9, 40, 20};
        char want[2048], got[2048];
        Term t;
        int w, ok = 1;
        if (!term_init(&t, 20, 4)) return fails + 1;
        vt_feed(&t, (const unsigned char *)feed, (int)strlen(feed));
        for (w = -1; w < (int)(sizeof(widths) / sizeof(widths[0])); w++) {
            int L, len = 0;
            if (w >= 0) { term_resize(&t, widths[w], w % 2 ? 3 : 5); term_settle(&t); }
            for (L = 0; L < t.hcount + t.rows; L++) {   /* logical lines, joined on wrapped */
                const Cell *c;
                int n, wr, x;
                term_line(&t, L, &c, &n, &wr);
                for (x = 0; c && x < n; x++)
                    if (c[x].wflag != W_CONT && c[x].wflag != W_PAD) len += utf8_encode(c[x].ch, got + len);
                if (!wr) {
                    while (len > 0 && got[len - 1] == ' ') len--;
                    got[len++] = '|';
                }
            }
            while (len > 0 && (got[len - 1] == '|' || got[len - 1] == ' ')) len--;
            got[len] = 0;
            if (w < 0) memcpy(want, got, (size_t)len + 1);
            else if (strcmp(want, got)) {
                ok = 0;
                fprintf(f, "FAIL reflow at %d cols:\n  want %s\n  got  %s\n", widths[w], want, got);
            }
        }
        fprintf(f, "%s reflow through %d widths, history %d rows\n", ok ? "ok  " : "FAIL",
                (int)(sizeof(widths) / sizeof(widths[0])), t.hcount);
        fails += !ok;
        term_free(&t);
    }
    {   /* a taller window pulls lines back out of history; shorter pushes them again */
        Term t;
        char line[32];
        const Cell *c;
        int k, n, wr, ok;
        if (!term_init(&t, 20, 4)) return fails + 1;
        for (k = 0; k < 10; k++) {
            int m = snprintf(line, sizeof(line), k < 9 ? "L%d\r\n" : "L%d", k);
            vt_feed(&t, (const unsigned char *)line, m);
        }
        term_resize(&t, 20, 8); term_settle(&t);
        term_line(&t, t.hcount, &c, &n, &wr);            /* screen row 0 */
        ok = t.hcount == 2 && n >= 2 && c[1].ch == '2' && t.cy == 7;
        term_resize(&t, 20, 3); term_settle(&t);
        term_resize(&t, 20, 8); term_settle(&t);
        term_line(&t, t.hcount, &c, &n, &wr);
        ok = ok && t.hcount == 2 && n >= 2 && c[1].ch == '2' && t.cy == 7;
        fprintf(f, "%s reflow taller screen pulls history back (history %d, cursor row %d)\n", ok ? "ok  " : "FAIL", t.hcount, t.cy);
        fails += !ok;
        term_free(&t);
    }
    fails += term_snapshot_check(f);
    return fails;
}

/* --bench OUT COLS ROWS FILE: parser speed on a captured output stream
   (HARDCORE_VTLOG on Windows), fed in 64 KB chunks the way the reader does;
   HARDCORE_BENCH_CHUNK=N feeds N bytes at a time instead. */
static int parse_bench(const char *out_path, int cols, int rows, const char *file) {
    FILE *out = fopen(out_path, "wb"), *f = fopen(file, "rb");
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    unsigned char *data;
    long n, k;
    int rep, best = 0, chunk = getenv("HARDCORE_BENCH_CHUNK") ? atoi(getenv("HARDCORE_BENCH_CHUNK")) : 65536;
    double best_ms = 1e9;
    if (!out || !f || !tb || !term_init(&tb->t, cols, rows) || chunk < 1) return 3;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (unsigned char *)malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) return 3;
    fclose(f);
    for (rep = 0; rep < 5; rep++) {
        double t0 = bench_now_ms(), ms;
        for (k = 0; k < n; k += chunk) vt_feed(&tb->t, data + k, (int)(n - k < chunk ? n - k : chunk));
        ms = bench_now_ms() - t0;
        fprintf(out, "run %d: %.1f ms\n", rep, ms);
        if (ms < best_ms) { best_ms = ms; best = rep; }
    }
    fprintf(out, "%.1f MB in %.1f ms best of 5 (run %d): %.1f MB/s\n", n / 1e6, best_ms, best, n / 1e3 / best_ms);
    fclose(out);
    return 0;
}

#define BENCH_TABS 16             /* the stress case: this many tabs, each with full scrollback */

/* 1 if two terminals hold the same text: every screen row and every
   scrollback line they both still have (counted from the newest), the wraps
   and the cursor. *extra = how many more old lines a kept than b. */
static int term_same(const Term *a, const Term *b, int *extra) {
    int L, x, d = a->hcount - b->hcount;
    *extra = d;
    if (d < 0 || a->cols != b->cols || a->rows != b->rows || a->cx != b->cx || a->cy != b->cy) return 0;
    for (L = 0; L < b->hcount + b->rows; L++) {
        const Cell *ca, *cb;
        int na, nb, wa, wb;
        term_line(a, L + d, &ca, &na, &wa);
        term_line(b, L, &cb, &nb, &wb);
        if (!wa != !wb) return 0;
        for (x = 0; x < (na > nb ? na : nb); x++) {
            uint32_t ch_a = x < na ? ca[x].ch : ' ', ch_b = x < nb ? cb[x].ch : ' ';
            uint32_t fg_a = x < na ? ca[x].fg : 0, fg_b = x < nb ? cb[x].fg : 0;
            if (ch_a != ch_b || fg_a != fg_b) return 0;
        }
    }
    return 1;
}

/* The text of every logical line (wrapped rows joined), one per line: what
   a resize must never change */
static char *term_text(const Term *t) {
    size_t cap = 1 << 20, n = 0;
    char *out = (char *)malloc(cap);
    int L, x;
    for (L = 0; L < t->hcount + t->rows; L++) {
        const Cell *c;
        int len, wrapped;
        term_line(t, L, &c, &len, &wrapped);
        if (wrapped && len > wrapped) len = wrapped;
        if (!wrapped) while (len > 0 && c[len - 1].ch == ' ') len--;
        for (x = 0; x < len; x++) {
            if (c[x].wflag == W_CONT) continue;
            if (n + 8 >= cap) out = (char *)realloc(out, cap *= 2);
            n += utf8_encode(c[x].ch ? c[x].ch : ' ', out + n);
        }
        if (wrapped && len < wrapped) for (; x < wrapped; x++) out[n++] = ' ';   /* trimmed, but text */
        if (!wrapped) {
            if (n + 2 >= cap) out = (char *)realloc(out, cap *= 2);
            out[n++] = '\n';
        }
    }
    out[n] = 0;
    return out;
}

/* Resizing through many widths and back gives back the same text: no space
   gained or lost at a wrap (wide chars at the edge, trailing blanks). */
static int reflow_roundtrip(void) {
    Term *a = (Term *)calloc(1, sizeof(Tab)), *b = (Term *)calloc(1, sizeof(Tab));
    char *ta, *tb;
    int i, same;
    if (!a || !b || !term_init(a, 120, 40) || !term_init(b, 120, 40)) return 0;
    bench_fill(a, 3000);
    bench_fill(b, 3000);
    for (i = 0; i < 60; i++) term_resize(a, 120 - (i * 37) % 70, 40 - i % 9);
    term_resize(a, 120, 40);
    term_settle(a);
    ta = term_text(a);
    tb = term_text(b);
    same = !strcmp(ta, tb);
    free(ta);
    free(tb);
    term_free(a);
    term_free(b);
    free(a);
    free(b);
    return same;
}

/* Tail-only reflow plus term_settle gives exactly what rewrapping everything
   at every step gives: the same drag on two full terminals, compared. With
   full scrollback the old way also lost the oldest lines at every narrowing
   step (they overflowed HIST_LINES); the tail-only way keeps them, so it may
   hold more: *extra says how many. */
static int reflow_check(int lines, int *extra) {
    Term *a = (Term *)calloc(1, sizeof(Tab)), *b = (Term *)calloc(1, sizeof(Tab));
    int i, same;
    if (!a || !b || !term_init(a, 120, 40) || !term_init(b, 120, 40)) return 0;
    bench_fill(a, lines);
    bench_fill(b, lines);
    for (i = 0; i < 60; i++) {   /* narrower, shorter, then back out past where it started */
        int c = i < 30 ? 120 - i * 2 : 60 + (i - 30) * 3, r = i < 30 ? 40 - i / 3 : 30 + (i - 30) / 2;
        term_resize(a, c, r);
        g_reflow_all = 1;
        term_resize(b, c, r);
        g_reflow_all = 0;
    }
    term_settle(a);
    same = term_same(a, b, extra) && a->hist_cols == a->cols && !a->hist_stale;
    term_free(a);
    term_free(b);
    free(a);
    free(b);
    return same;
}

static int bench_resize(const char *out_path) {
    static int drag[240][2], tall[120][2], wide[120][2];
    Term *ts[BENCH_TABS];
    FILE *out = fopen(out_path, "wb");
    int i, k;
    double t0;
    if (!out) return 3;
    for (k = 0; k < BENCH_TABS; k++) {
        ts[k] = (Term *)calloc(1, sizeof(Tab));   /* a Tab's worth: the parser's replies look for one */
        if (!ts[k] || !term_init(ts[k], 120, 40)) return 3;
        t0 = bench_now_ms();
        bench_fill(ts[k], HIST_LINES + 40);
        if (!k) fprintf(out, "filled %d lines of scrollback + %dx%d screen in %.1f ms\n", ts[k]->hcount, ts[k]->cols, ts[k]->rows, bench_now_ms() - t0);
    }
    for (i = 0; i < 240; i++) {  /* a corner drag: narrower and shorter, then wider and taller */
        int d = i < 120 ? i : 239 - i;
        drag[i][0] = 120 - d / 2 - (i & 1);   /* every step a new size, as a drag delivers them */
        drag[i][1] = 40 - d / 6;
    }
    for (i = 0; i < 120; i++) {
        tall[i][0] = 120;        /* the bottom edge only: the width never changes */
        tall[i][1] = 40 - (i < 60 ? i / 3 : (119 - i) / 3) - (i & 1);
        wide[i][0] = 120 - (i < 60 ? i : 119 - i);   /* the side edge only */
        wide[i][1] = 40;
    }
    {
        int e1, e2, ok1 = reflow_check(5000, &e1), ok2 = reflow_check(HIST_LINES + 40, &e2);
        fprintf(out, "tail reflow + settle == full reflow: %s (5,000 lines), %s (full scrollback; it kept %d more old lines)\n",
                ok1 && !e1 ? "yes" : "NO", ok2 ? "yes" : "NO", e2);
    }
    fprintf(out, "60 resizes and back == never resized: %s\n", reflow_roundtrip() ? "yes" : "NO");
    for (k = 1; k <= BENCH_TABS; k *= BENCH_TABS) {
        char name[64];
        double t1;
        fprintf(out, "-- %d tab%s, each with full scrollback\n", k, k > 1 ? "s" : "");
        snprintf(name, sizeof(name), "corner drag (cols+rows)");
        bench_series(out, name, ts, k, drag, 240);
        bench_series(out, "bottom edge (rows only)", ts, k, tall, 120);
        bench_series(out, "side edge (cols only)", ts, k, wide, 120);
        bench_series(out, "corner drag (cols+rows)", ts, k, drag, 240);   /* leave it narrower than it started */
        t1 = bench_now_ms();
        {
            int j, did = 0;
            for (j = 0; j < k; j++) did += term_settle(ts[j]);
            fprintf(out, "%-26s %4d tabs   once, after the drag: %.2f ms\n", "settle (rest of history)", did, bench_now_ms() - t1);
        }
    }
    fclose(out);
    for (k = 0; k < BENCH_TABS; k++) {
        term_free(ts[k]);
        free(ts[k]);
    }
    return 0;
}

#ifdef _WIN32
/* ── window procedure ─────────────────────────────────────────────── */

static void plat_invalidate(void) { InvalidateRect(g_hwnd, NULL, FALSE); }
static void plat_timer(int id, int ms) { SetTimer(g_hwnd, (UINT_PTR)id, (UINT)ms, NULL); }
static void plat_kill_timer(int id) { KillTimer(g_hwnd, (UINT_PTR)id); }
static void plat_set_title(const wchar_t *title) { SetWindowTextW(g_hwnd, title); }
static void plat_close_window(void) { DestroyWindow(g_hwnd); }

static void plat_client_size(int *w, int *h) {
    RECT r = {0, 0, 0, 0};
    if (g_hwnd) GetClientRect(g_hwnd, &r);
    *w = r.right;
    *h = r.bottom;
}

static void plat_flash(void) {
    FLASHWINFO fi = {sizeof(fi), g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fi);
}

static int plat_lights_bottom(void) { return 0; }   /* the caption buttons float over the terminal instead */
static void plat_settings_shown(void) { InvalidateRect(g_hwnd, NULL, FALSE); }

/* every hardcore window is its own process: each rereads config.ini */
static BOOL CALLBACK tell_window(HWND h, LPARAM unused) {
    wchar_t cls[32];
    (void)unused;
    if (h != g_hwnd && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"hardcore")) PostMessageW(h, WM_APP_SETTINGS, 0, 0);
    return TRUE;
}
static void plat_settings_tell(void) {
    if (_wgetenv(L"HARDCORE_TEST_CONFIG")) return;
    EnumWindows(tell_window, 0);
}

/* Geometry inspection for an explicitly isolated GUI test. The controls and
   screenshot channel still exercise the normal native renderer and input. */
static int font_menu_test_state(const char *path) {
    wchar_t wide[MAX_PATH];
    FILE *f;
    int n, i, comma = 0;
    if (!_wgetenv(L"HARDCORE_TEST_CONFIG") ||
        !MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH)) return 0;
    tab_layout();
    f = _wfopen(wide, L"wb");
    if (!f) return 0;
    fprintf(f, "{\"fontkey\":\"%c\",\"settings_open\":%d,\"settings_tab\":%d,\"scale\":%d,"
               "\"cell\":{\"width\":%d,\"height\":%d},\"dock\":{\"x\":%d,\"width\":%d},"
               "\"pages\":{\"settings_row\":%d,\"section_row\":%d,"
               "\"settings\":{\"row\":%d,\"offset\":%d,\"capacity\":%d,\"total\":%d}},\"fonts\":[",
            g_font->key, g_settings_open, g_settings_tab, g_scale,
            cell_w(g_font), cell_h(g_font), sidebar_x() / cell_w(g_font), SB_COLS,
            g_chrome.settings_row, g_chrome.section_row, g_chrome.list_row,
            g_setting_offset[g_settings_tab], g_chrome.list_capacity, chrome_item_total());
    for (i = 0; i < NFONTS; i++) {
        const Font *font = &FONTS[i];
        fprintf(f, "%s{\"fontkey\":\"%c\",\"name\":\"%s\",\"width\":%d,\"height\":%d,\"dev\":%d}",
                i ? "," : "", font->key, font->name, cell_w(font), cell_h(font), font->dev);
    }
    fputs("],\"font_buttons\":[", f);
    if (g_settings_open && g_settings_tab == 0) {
        for (n = 0; n < g_chrome.list_count; n++) {
            const FontMenuRow *row = &g_font_menu_rows[g_setting_offset[0] + n];
            for (i = 0; i < row->count; i++) {
                const FontMenuSize *size = &row->sizes[i];
                const Font *font = &FONTS[size->font];
                fprintf(f, "%s{\"fontkey\":\"%c\",\"name\":\"%s\",\"width\":%d,\"height\":%d,"
                           "\"row\":%d,\"left\":%d,\"right\":%d}",
                        comma++ ? "," : "", font->key, font->name, cell_w(font), cell_h(font),
                        g_chrome.list_row + n, size->left, size->right);
            }
        }
    }
    fputs("]}\n", f);
    return fclose(f) == 0;
}

static void tab_set_size(Tab *tb, int cols, int rows) {
    COORD sz;
    sz.X = (SHORT)cols;
    sz.Y = (SHORT)rows;
    if (tb->hpc) pResizePC(tb->hpc, sz);
}

static int mouse_mods(WPARAM wp) {
    return ((wp & MK_SHIFT) ? M_SHIFT : 0) | ((wp & MK_CONTROL) ? M_CTRL : 0) |
           ((GetKeyState(VK_MENU) & 0x8000) ? M_ALT : 0);
}

static void save_window_size(void) {
    RECT r;
    if (!IsIconic(g_hwnd) && !IsZoomed(g_hwnd) && GetWindowRect(g_hwnd, &r)) {
        g_win_w = r.right - r.left;
        g_win_h = r.bottom - r.top;
    }
}

/* runs a command with no window and returns its exit code (-1 = didn't run / timed out) */
static long run_quiet(const wchar_t *cmdline, DWORD timeout_ms) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t cmd[1024];
    DWORD code = (DWORD)-1;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    _snwprintf(cmd, 1024, L"%ls", cmdline);
    cmd[1023] = 0;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (long)code;
}

/* a fresh shell in tab i, in the folder the old one was in */
static void replace_tab(int i) {
    Tab *old = g_tabs[i], *tb = tab_create(old->t.cwd[0] ? old->t.cwd : old->start_dir, 0);
    if (!tb) return;
    g_tabs[i] = tb;
    tab_destroy_async(old);
    activate(i);
}

static void diagnose(Tab *tb, DWORD code) {
    int i;
    if (tb->kind == TK_WSL) {
        g_have_distro = wsl_distros() > 0;
        if (!g_have_distro) { show_help(tb, HELP_NO_DISTRO, code); return; }
        if (!g_no_bash) {
            wchar_t cmd[300];
            _snwprintf(cmd, 300, L"wsl.exe%ls%hs%ls -e sh -c \"command -v bash >/dev/null\"",
                       g_distro[0] ? L" -d \"" : L"", g_distro, g_distro[0] ? L"\"" : L"");
            if (run_quiet(cmd, 8000) == 1) {   /* sh ran, bash isn't there (Alpine and friends) */
                g_no_bash = 1;
                if (g_spare) { tab_destroy_async(g_spare); g_spare = NULL; }
                for (i = 0; i < g_ntabs; i++)
                    if (g_tabs[i] == tb) {
                        replace_tab(i);
                        term_message(&g_tabs[i]->t, "\x1b[2mhardcore: this distro has no bash, so tabs run its default shell. "
                                                    "They won't follow cd for new tabs.\x1b[0m\r\n");
                    }
                return;
            }
        }
    }
    show_help(tb, HELP_FAILED, code);
}

/* keys on a help screen: I install, P PowerShell, Enter retry */
static int help_key(Tab *tb, uint32_t c) {
    int i, at = -1;
    if (tb->help == HELP_SLURP) return 1;
    for (i = 0; i < g_ntabs; i++) if (g_tabs[i] == tb) at = i;
    if (at < 0) return 0;
    if ((c == 'i' || c == 'I') && tb->help == HELP_NO_DISTRO) {
        ShellExecuteW(g_hwnd, L"runas", L"wsl.exe", L"--install", NULL, SW_SHOWNORMAL);
        term_message(&tb->t, "\r\n\x1b[2mInstalling in the window Windows opened. When it's done (a restart and a\r\n"
                             "Linux user name may come first), press Enter here.\x1b[0m\r\n");
        tb->t.dirty = 1;
        InvalidateRect(g_hwnd, NULL, FALSE);
        return 1;
    }
    if ((c == 'p' || c == 'P') && tb->kind == TK_WSL) {
        g_shell_ps = 1;
        config_save();
        if (g_spare) { tab_destroy_async(g_spare); g_spare = NULL; }
        replace_tab(at);
        return 1;
    }
    if (c == '\r') {
        g_have_distro = wsl_distros() > 0 && !_wgetenv(L"HARDCORE_PRETEND_NO_WSL");
        replace_tab(at);
        return 1;
    }
    return 1;                    /* nothing else means anything here */
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
/* --reflow-bench OUT: a full scrollback (HIST_LINES colored lines of mixed
   length, many wider than the window) at 178x55, then a drag: width steps
   down to 100 and back, then height steps, timing each term_resize. */
static int reflow_bench(const char *out_path) {
    FILE *out = fopen(out_path, "wb");
    Term t;
    char line[1024];
    int i, k, w;
    double sum_w = 0, max_w = 0, sum_h = 0, max_h = 0;
    int n_w = 0, n_h = 0, cycles = _wgetenv(L"HARDCORE_BENCH_CYCLES") ? _wtoi(_wgetenv(L"HARDCORE_BENCH_CYCLES")) : 1;
    unsigned seed = 1;
    if (!out || !term_init(&t, 178, 55)) return 3;
    QueryPerformanceFrequency(&g_qpf);
    for (i = 0; i < HIST_LINES + 55; i++) {
        int len, n = 0;
        seed = seed * 1103515245u + 12345u;
        len = (int)((seed >> 16) % 400);           /* 0..399 chars: up to 3 rows at 178 */
        n += snprintf(line + n, sizeof(line) - n, "\x1b[3%dm%06d ", i % 8, i);
        for (k = 0; k < len && n < (int)sizeof(line) - 16; k++) line[n++] = (char)('a' + (k * 7 + i) % 26);
        n += snprintf(line + n, sizeof(line) - n, "\x1b[0m\r\n");
        vt_feed(&t, (const unsigned char *)line, n);
    }
    fprintf(out, "history %d lines at %dx%d\n", t.hcount, t.cols, t.rows);
    for (k = 0; k < 2 * cycles; k++) {
        for (w = (k % 2 ? 100 : 176); k % 2 ? w <= 178 : w >= 100; w += k % 2 ? 2 : -2) {   /* width drag */
            LARGE_INTEGER a, b;
            double ms;
            QueryPerformanceCounter(&a);
            term_resize(&t, w, 55);
            QueryPerformanceCounter(&b);
            ms = lat_ms(a, b);
            sum_w += ms; n_w++;
            if (ms > max_w) max_w = ms;
        }
    }
    for (k = 0; k < 2; k++) {
        int h;
        for (h = (k ? 20 : 54); k ? h <= 55 : h >= 20; h += k ? 1 : -1) {      /* height drag */
            LARGE_INTEGER a, b;
            double ms;
            QueryPerformanceCounter(&a);
            term_resize(&t, 178, h);
            QueryPerformanceCounter(&b);
            ms = lat_ms(a, b);
            sum_h += ms; n_h++;
            if (ms > max_h) max_h = ms;
        }
    }
    fprintf(out, "width steps:  %d, avg %.2f ms, max %.2f ms\n", n_w, sum_w / n_w, max_w);
    fprintf(out, "height steps: %d, avg %.2f ms, max %.2f ms\n", n_h, sum_h / n_h, max_h);
    fprintf(out, "after: history %d lines at %dx%d\n", t.hcount, t.cols, t.rows);
    term_free(&t);
    fclose(out);
    return 0;
}


#include "hardcore_slurp.h"

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        trace("WM_CREATE");
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    case WM_CLOSE:
        session_save();
        remember_dir(active_tab());
        save_window_size();
        config_save();
        DestroyWindow(hwnd);
        return 0;
    case WM_QUERYENDSESSION:     /* Windows is shutting down or restarting: keep the tabs */
        session_save();
        remember_dir(active_tab());
        config_save();
        return TRUE;
    case WM_DESTROY:
        for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->slurp_hwnd) slurp_detach(g_tabs[i]);
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
        if (n && !buf[n - 1]) n--; /* WM_COPYDATA text senders include the terminator */
        if (cd->dwData == 0x48430001 && tb && tb->help) {   /* a help screen takes keys, not text */
            help_key(tb, buf[0] == '\n' ? '\r' : (unsigned char)buf[0]);
        } else if (cd->dwData == 0x48430001 && tb) {   /* type: \n becomes Enter */
            size_t i;
            if (g_lat && !g_lat_key.QuadPart) {        /* time it like a key, without taking focus */
                lat_finish();
                QueryPerformanceCounter(&g_lat_key);
            }
            for (i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = '\r';
            tab_write(tb, buf, (int)n);
        } else if (cd->dwData == 0x48430002) {          /* a hardcore command */
            if (!strncmp(buf, "slurp", 5)) return slurp_filter(buf) && slurp_start(0);
            if (!strcmp(buf, "newtab")) {
                if (g_lat) { QueryPerformanceCounter(&g_nt_t0); g_nt_shown = -1; }
                new_tab_here();
                if (g_lat) g_nt_id = active_tab() ? active_tab()->id : 0;
            }
            else if (!strcmp(buf, "nexttab")) activate((g_active + 1) % g_ntabs);
            else if (!strcmp(buf, "closetab")) close_tab(g_active);
            else if (!strcmp(buf, "close")) PostMessageW(hwnd, WM_CLOSE, 0, 0);   /* as the × does */
            else if (!strcmp(buf, "reload")) {          /* hand the tabs to the exe at our path (an update put a new one there) */
                char why[200] = "";
                wchar_t *alt = _wgetenv(L"HARDCORE_RELOAD_EXE");
                if (!reload_into(alt ? alt : g_exe_path, why, sizeof(why))) {
                    if (g_perf) { fprintf(g_perf, "reload failed: %s\n", why); fflush(g_perf); }
                    return FALSE;
                }
                PostMessageW(hwnd, WM_APP_HANDED_OVER, 0, 0);
                return TRUE;
            }
            else if (!strcmp(buf, "jump")) jump_to_attention();
            else if (!strncmp(buf, "tab ", 4)) activate(atoi(buf + 4));
            else if (!strncmp(buf, "hover ", 6)) {
                g_mouse_x = atoi(buf + 6);
                g_mouse_y = strchr(buf + 6, ',') ? atoi(strchr(buf + 6, ',') + 1) : -1;
                settings_point();
            } else if (!strncmp(buf, "fontmenu ", 9)) {
                return font_menu_test_state(buf + 9);
            } else if (!strncmp(buf, "click ", 6)) {
                settings_click(atoi(buf + 6), strchr(buf + 6, ',') ? atoi(strchr(buf + 6, ',') + 1) : -1);
            } else if (!strncmp(buf, "nchover ", 8)) g_nc_hover = atoi(buf + 8);
            else if (!strncmp(buf, "tophover ", 9)) g_top_hover = atoi(buf + 9);
            else if (!strncmp(buf, "sel ", 4) && tb) {   /* sel Y0 X0 Y1 X1: select, in screen rows */
                int y0, x0, y1, x1;
                if (sscanf(buf + 4, "%d %d %d %d", &y0, &x0, &y1, &x1) != 4) return FALSE;
                g_sel_a_row = term_abs_line(&tb->t, y0); g_sel_a_col = x0;
                g_sel_b_row = term_abs_line(&tb->t, y1); g_sel_b_col = x1;
                g_sel_on = 1;
            }
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
        int chrome_hit = hit_tabbar(pt.x, pt.y, &idx);
        if (g_chrome_drag || (chrome_hit != HIT_NONE && chrome_hit != HIT_BAR)) return HTCLIENT;
        if (!IsZoomed(hwnd) && pt.y < 5 * g_scale) {
            int corner = 16 * g_scale;
            return pt.x < corner ? HTTOPLEFT : pt.x >= r.right - corner ? HTTOPRIGHT : HTTOP;
        }
        if (chrome_hit == HIT_BAR) return HTCAPTION;
        if ((cap = caption_hit(pt.x, pt.y))) return cap;
        if (in_top_line(pt.x, pt.y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCMOUSEMOVE: {
        int h = (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) ? (int)wp : 0, top;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
        TrackMouseEvent(&tme);
        ScreenToClient(hwnd, &pt);
        top = (wp == HTCAPTION || h) && in_top_line(pt.x, pt.y);
        if (top != g_top_hover) { g_top_hover = top; InvalidateRect(hwnd, NULL, FALSE); }
        if (g_mouse_y >= 0) { g_mouse_x = g_mouse_y = -1; settings_point(); InvalidateRect(hwnd, NULL, FALSE); }
        if (h != g_nc_hover) { g_nc_hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        break;                   /* DefWindowProc still runs: Win11 snap layouts hang off HTMAXBUTTON */
    }
    case WM_NCMOUSELEAVE:
        if (g_nc_hover || g_nc_press || g_top_hover) { g_nc_hover = g_nc_press = g_top_hover = 0; InvalidateRect(hwnd, NULL, FALSE); }
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
        if (LOWORD(lp) == HTCAPTION) {   /* the top line and the empty tab column move the window */
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            return TRUE;
        }
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            int col, row;
            char url[2048];
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            mouse_cell(pt.x, pt.y, &col, &row);
            int idx = -1, hit = hit_tabbar(pt.x, pt.y, &idx);
            if (g_chrome_drag || hit == HIT_GRIP) SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
            else if (over_chrome(pt.x, pt.y)) SetCursor(LoadCursorW(NULL, (LPCWSTR)(tabbar_action(pt.x, pt.y) ? IDC_HAND : IDC_ARROW)));
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
            bar.left = bar.right; /* no opaque sidebar during startup */
            FillRect(ps.hdc, &r, bg);
            FillRect(ps.hdc, &bar, bb);
            DeleteObject(bg);
            DeleteObject(bb);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_THROB) { update_busy(1); return 0; }
        if (wp == TIMER_DISK) { check_disk(); return 0; }
        KillTimer(hwnd, wp);
        if (wp == TIMER_SPARE) make_spare();
        else if (wp == TIMER_SETTLE) settle_all();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            LARGE_INTEGER a, b, c, d;
            QueryPerformanceCounter(&a);
            r_resize();
            QueryPerformanceCounter(&b);
            resize_all();
            QueryPerformanceCounter(&c);
            /* draw now, not on the next WM_PAINT: during a live resize the
               compositor shows the swap chain's last frame until we do */
            if (g_gfx_ready) paint_now();
            else InvalidateRect(hwnd, NULL, FALSE);
            QueryPerformanceCounter(&d);
            if (g_perf) {
                fprintf(g_perf, "resize %dx%d: buffers %.2f ms  terminal %.2f ms  frame %.2f ms  (build %.2f present %.2f)\n",
                        LOWORD(lp), HIWORD(lp), lat_ms(a, b), lat_ms(b, c), lat_ms(c, d), g_pf_last_build, g_pf_last_present);
                fflush(g_perf);
            }
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
        glyph_cache_reset();     /* thin box lines are drawn for the scale */
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
    case WM_APP_SLURP:
        if (wp) InterlockedExchange(&g_slurp_running, 0);
        else if (lp) slurp_receive((SlurpTarget *)lp);
        return 0;
    case WM_APP_OUTPUT: {
        /* Hidden tabs are parsed (their state and scrollback stay current) but
           only cause a frame when something the tab column shows changes:
           otherwise 63 busy tabs meant ~1000 identical frames a second. */
        int i, changed = 0;
        for (i = 0; i < g_ntabs; i++) {
            Tab *tb = g_tabs[i];
            if (tab_drain(tb) <= 0) continue;
            if (i == g_active) {
                changed = 1;
                if (g_lat && g_lat_key.QuadPart && !g_lat_out.QuadPart) QueryPerformanceCounter(&g_lat_out);
            } else if (!tb->activity) {
                tb->activity = 1;
                changed = 1;
            }
            if (tb->t.bell) {
                tb->t.bell = 0;
                if (!g_focused || i != g_active) {
                    FLASHWINFO fi = {sizeof(fi), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
                    tb->attention = 1;
                    changed = 1;
                    if (!g_focused) FlashWindowEx(&fi);
                }
            }
            if (tb->t.side) {
                tb->t.side = 0;
                changed = 1;
            }
        }
        update_busy(0);
        if (changed) {
            static int first = 1;
            if (first) { first = 0; trace("first child output"); }
            update_title();
            InvalidateRect(hwnd, NULL, FALSE);
            /* under a flood of output WM_PAINT never comes (the queue is never
               empty), so draw one here at least every frame of a 60 Hz display */
            {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                if (lat_ms(g_last_paint, now) > 16.0) UpdateWindow(hwnd);
            }
        }
        return 0;
    }
    case WM_APP_SETTINGS:
        config_load();
        settings_apply();
        return 0;
    case WM_APP_HANDED_OVER:
        /* the new process has every pseudoconsole; ending here closes only our
           copies of their handles. No ClosePseudoConsole, no session save. */
        ExitProcess(0);
        return 0;
    case WM_APP_EXITED: {
        Tab *tb = tab_by_id((int)wp);
        DWORD code = 0;
        int i;
        if (!tb) return 0;
        tab_drain(tb);
        if (tb->proc) GetExitCodeProcess(tb->proc, &code);
        if ((tb->slurp_wsl || tb->slurp_hwnd) && (code || (tb->slurp_hwnd && !tb->slurp_ready))) {
            if (tb->slurp_hwnd) slurp_detach(tb);
            tb->help = HELP_SLURP;
            term_message(&tb->t, "\r\nAttachment failed or disconnected. The original terminal was not closed.\r\n");
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        /* died within 3 s with an error: a setup problem, not you typing exit.
           Keep the tab and say what's wrong instead of vanishing. */
        if (code != 0 && GetTickCount() - tb->born < 3000 && !tb->help) {
            diagnose(tb, code);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        for (i = 0; i < g_ntabs; i++)
            if (g_tabs[i] == tb) close_tab(i);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        g_chrome_input_at = GetTickCount();
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
        int was_bar = g_mouse_y >= 0 && in_sidebar(g_mouse_x), on_bar = over_chrome(mx, my);
        Tab *tb = active_tab();
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        g_mouse_x = mx;
        g_mouse_y = my;
        if (g_nc_hover) { g_nc_hover = 0; InvalidateRect(hwnd, NULL, FALSE); }
        if (was_bar || on_bar) InvalidateRect(hwnd, NULL, FALSE);
        if (was_bar || on_bar) settings_point();
        if (g_chrome_drag) { chrome_drag_move(mx, my); return 0; }
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
            mouse_report((wp & MK_LBUTTON) ? 0 : (wp & MK_MBUTTON) ? 1 : 2, 1, 1, mx, my, mouse_mods(wp));
        else if (!on_bar && tb && tb->t.m_mouse == 1003)
            mouse_report(3, 1, 1, mx, my, mouse_mods(wp));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mouse_x = g_mouse_y = -1;
        settings_point();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp), idx = -1, col, row, hit;
        RECT r;
        Tab *tb = active_tab();
        if (chrome_drag_begin(mx, my)) { SetCapture(hwnd); return 0; }
        hit = hit_tabbar(mx, my, &idx);
        if (hit == HIT_TAB) { activate(idx); return 0; }
        if (hit == HIT_CLOSE) { close_tab(idx); return 0; }
        if (hit == HIT_PLUS) { new_tab_here(); return 0; }
        if (chrome_setting_hit(hit)) { settings_click(mx, my); return 0; }
        if (hit == HIT_BAR) return 0;
        GetClientRect(hwnd, &r);
        if (tb && mx >= term_ox() + tb->t.cols * CW - SCROLLBAR_W && mx < term_ox() + tb->t.cols * CW && tb->t.hcount && !tb->t.alt) {
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
        if (mouse_report(0, 1, 0, mx, my, mouse_mods(wp))) return 0;
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
    case WM_CAPTURECHANGED:
        if (g_chrome_drag) chrome_drag_end();
        return 0;
    case WM_LBUTTONUP:
        if (g_chrome_drag) {
            chrome_drag_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); chrome_drag_end(); ReleaseCapture(); return 0;
        }
        if (g_sb_drag || g_sel_drag) {
            g_sb_drag = g_sel_drag = 0;
            ReleaseCapture();
            return 0;
        }
        mouse_report(0, 0, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mouse_mods(wp));
        return 0;
    case WM_MBUTTONDOWN: {
        int idx = -1;
        if (hit_tabbar(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx) == HIT_TAB ||
            hit_tabbar(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &idx) == HIT_CLOSE) {
            close_tab(idx);
            return 0;
        }
        mouse_report(1, 1, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mouse_mods(wp));
        return 0;
    }
    case WM_MBUTTONUP:
        mouse_report(1, 0, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mouse_mods(wp));
        return 0;
    case WM_RBUTTONDOWN:
        if (over_chrome(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) return 0;
        if (mouse_report(2, 1, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mouse_mods(wp))) return 0;
        /* conhost habit: right-click copies a selection, or pastes */
        if (g_sel_on) {
            sel_copy();
            g_sel_on = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        } else paste();
        return 0;
    case WM_RBUTTONUP:
        mouse_report(2, 0, 0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), mouse_mods(wp));
        return 0;
    case WM_MOUSEWHEEL: {
        short delta = (short)HIWORD(wp);
        int notches = delta / 120, i;
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        Tab *tb = active_tab();
        if (!notches) notches = delta > 0 ? 1 : -1;
        ScreenToClient(hwnd, &pt);
        if (!tb || over_chrome(pt.x, pt.y)) return 0;
        if (tb->t.m_mouse && !(GET_KEYSTATE_WPARAM(wp) & MK_SHIFT)) {
            for (i = 0; i < abs(notches); i++)
                mouse_report(notches > 0 ? 64 : 65, 1, 0, pt.x, pt.y, mouse_mods(GET_KEYSTATE_WPARAM(wp)));
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

/* Drive folders are checked; a folder inside the distro is taken on trust
   (looking would mean booting WSL before the window is up). */
static int dir_exists(const char *p) {
    wchar_t w[MAX_PATH * 2];
    DWORD a;
    if (p[0] == '/' && strncmp(p, g_mnt_root, strlen(g_mnt_root))) return 1;
    to_windows_dir(p, w, MAX_PATH * 2);
    if (!wcsncmp(w, L"\\\\wsl", 5)) return 1;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
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
        {L"C:\\Users\\you\\notes.txt", L"/mnt/c/Users/you/notes.txt"},
        {L"D:\\My Stuff\\a b.png", L"'/mnt/d/My Stuff/a b.png'"},
        {L"C:\\it's\\here", L"'/mnt/c/it'\\''s/here'"},
        {L"\\\\wsl.localhost\\Debian\\home\\you\\x.c", L"/home/you/x.c"},
        {L"\\\\wsl$\\Ubuntu\\etc\\hosts", L"/etc/hosts"},
        {L"\\\\server\\share\\f", L"//server/share/f"},
    };
    static const struct { const char *word; int at; const char *want; } urls[] = {
        {"https://example.com/a?b=c", 3, "https://example.com/a?b=c"},
        {"(see:https://x.io/y).", 10, "https://x.io/y"},
        {"(see:https://x.io/y).", 1, NULL},
        {"file:///mnt/c/Users/you", 0, "file:///mnt/c/Users/you"},
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
    fails += unit_scrollback(f);
    fails += unit_issues(f);
    {
        Term wrapped;
        int ok = term_init(&wrapped, 10, 3);
        if (ok) {
            vt_feed(&wrapped, (const unsigned char *)"ABCDEFGHIJKLMNO", 15);
            char *text = term_text(&wrapped);
            ok = text && strstr(text, "ABCDEFGHIJKLMNO");
            free(text); term_free(&wrapped);
        }
        fprintf(f, "%s wrapped screen text retains every column\n", ok ? "ok  " : "FAIL");
        fails += !ok;
    }
    fails += unit_underlay(f);
    fails += unit_chrome_display(f);
    fails += unit_chrome(f);
    fails += unit_tab_labels(f);
    for (i = 0; i < (int)(sizeof(urls) / sizeof(urls[0])); i++) {
        char got[256] = "";
        int hit = url_in_word(urls[i].word, urls[i].at, got, sizeof(got));
        int ok = urls[i].want ? (hit && !strcmp(got, urls[i].want)) : !hit;
        fprintf(f, "%s url   %s @%d -> %s\n", ok ? "ok  " : "FAIL", urls[i].word, urls[i].at, hit ? got : "(none)");
        fails += !ok;
    }
    {
        int e, ok = reflow_check(5000, &e) && !e, rt = reflow_roundtrip();
        fprintf(f, "%s reflow tail-only + settle == full reflow\n", ok ? "ok  " : "FAIL");
        fprintf(f, "%s reflow 60 resizes and back == never resized\n", rt ? "ok  " : "FAIL");
        fails += !ok + !rt;
    }
    fprintf(f, "%d failed\n", fails);
    fclose(f);
    return fails ? 1 : 0;
}

static int replay(const char *out_path, int cols, int rows, wchar_t **files, int nfiles) {
    FILE *out = fopen(out_path, "wb");
    int i;
    if (!out) return 3;
    for (i = 0; i < nfiles; i++) {
        FILE *f = _wfopen(files[i], L"rb");
        char name[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, files[i], -1, name, sizeof(name), NULL, NULL);
        fprintf(out, "=== %s\n", name);
        replay_one(out, f, cols, rows);
        if (f) fclose(f);
    }
    fclose(out);
    return 0;
}

static double bench_now_ms(void) {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ── hardcore update / --version ───────────────────────────────────
   Like `claude update`: asks GitHub for the latest release, downloads its
   hardcore.exe, checks it against the SHA-256 GitHub publishes for it, and
   swaps it in. A running hardcore.exe can't be overwritten but can be
   renamed, so windows that are open keep running the old copy and the next
   one starts the new. Run from a tab as `hardcore update` (rc.bash). */
#ifndef HARDCORE_BUILD
#define HARDCORE_BUILD "unknown"  /* git describe, from the Makefile: which commit this is */
#endif
#define UPDATE_REPO L"/repos/williamsharkey/hardcore/releases/latest"

static HANDLE g_cli_out;
static void cli(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    DWORD w;
    int n;
    if (!g_cli_out) {            /* a GUI exe: write to whatever started it (WSL's pipe, or the console) */
        g_cli_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (!g_cli_out || g_cli_out == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS))
                g_cli_out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        }
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0 && g_cli_out && g_cli_out != INVALID_HANDLE_VALUE) WriteFile(g_cli_out, buf, (DWORD)strlen(buf), &w, NULL);
}

/* GET over HTTPS (redirects followed): the body, malloc'd; NULL on failure */
static char *https_get(const wchar_t *url, size_t *len, int json) {
    URL_COMPONENTS u;
    wchar_t host[256], path[2048];
    HINTERNET s = NULL, c = NULL, r = NULL;
    char *body = NULL;
    size_t n = 0, cap = 0;
    DWORD status = 0, sz = sizeof(status), got;
    memset(&u, 0, sizeof(u));
    u.dwStructSize = sizeof(u);
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &u)) return NULL;
    s = WinHttpOpen(L"hardcore-update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (s) c = WinHttpConnect(s, host, u.nPort, 0);
    if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (r && json) WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (r && WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL) && status == 200) {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
            if (n + avail + 1 > cap) {
                char *b;
                cap = (n + avail + 1) * 2;
                if (!(b = (char *)realloc(body, cap))) { free(body); body = NULL; break; }
                body = b;
            }
            if (!WinHttpReadData(r, body + n, avail, &got) || !got) break;
            n += got;
        }
        if (body) body[n] = 0;
    } else if (status) cli("hardcore: %ls answered HTTP %lu\n", host, status);
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    *len = n;
    return body;
}

/* "key":"value" in a JSON blob, from `from` on (enough for GitHub's release JSON) */
static const char *json_str(const char *from, const char *key, char *out, size_t cap) {
    char pat[64];
    const char *p, *e;
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (!(p = strstr(from, pat))) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return NULL;
    e = strchr(++p, '"');
    if (!e || (size_t)(e - p) >= cap) return NULL;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return e;
}

/* v0.4 or v0.4.1 (and v0.4-3-gabc: three commits past v0.4) → comparable */
static long ver_num(const char *v, int *ahead) {
    int a = 0, b = 0, c = 0, k = 0;
    const char *d;
    if (*v == 'v') v++;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    d = strchr(v, '-');
    if (d) sscanf(d + 1, "%d", &k);
    if (ahead) *ahead = k;
    return (long)a * 1000000 + b * 1000 + c;
}

static int sha256_hex(const char *data, size_t n, char out[65]) {
    BCRYPT_ALG_HANDLE alg;
    unsigned char h[32];
    int i, ok;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) return 0;
    ok = !BCryptHash(alg, NULL, 0, (PUCHAR)data, (ULONG)n, h, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    for (i = 0; ok && i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
    return ok;
}

static int update_cmd(int force) {
    char *json, *bin, tag[64], url[1024], digest[96] = "", hex[65];
    const char *asset;
    size_t jn, bn;
    wchar_t wurl[1024], exe[MAX_PATH], tmp[MAX_PATH], old[MAX_PATH];
    long mine, latest;
    int ahead;
    FILE *f;
    SYSTEMTIME st;
    cli("hardcore %s: checking github.com/williamsharkey/hardcore for the latest release...\n", HARDCORE_VERSION);
    if (!(json = https_get(L"https://api.github.com" UPDATE_REPO, &jn, 1))) { cli("hardcore: couldn't reach GitHub\n"); return 1; }
    if (!json_str(json, "tag_name", tag, sizeof(tag))) { cli("hardcore: no release found\n"); free(json); return 1; }
    /* the asset named hardcore.exe: its download URL and digest come after its name */
    if (!(asset = strstr(json, "\"name\":\"hardcore.exe\"")) && !(asset = strstr(json, "\"name\": \"hardcore.exe\""))) {
        cli("hardcore: release %s has no hardcore.exe\n", tag); free(json); return 1;
    }
    if (!json_str(asset, "browser_download_url", url, sizeof(url))) { cli("hardcore: release %s: no download link\n", tag); free(json); return 1; }
    json_str(asset, "digest", digest, sizeof(digest));
    free(json);
    mine = ver_num(HARDCORE_VERSION, &ahead);
    latest = ver_num(tag, NULL);
    (void)ahead;
    if (!force && latest <= mine) {
        cli("hardcore: up to date (%s; latest release %s)\n", HARDCORE_VERSION, tag);
        return 0;
    }
    cli("hardcore: downloading %s...\n", tag);
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, 1024);
    if (!(bin = https_get(wurl, &bn, 0)) || bn < 100000 || bin[0] != 'M' || bin[1] != 'Z') {
        cli("hardcore: download failed\n"); free(bin); return 1;
    }
    if (!strncmp(digest, "sha256:", 7)) {
        if (!sha256_hex(bin, bn, hex) || _stricmp(hex, digest + 7)) {
            cli("hardcore: the download doesn't match GitHub's SHA-256, not installing it\n"); free(bin); return 1;
        }
        cli("hardcore: SHA-256 checks out\n");
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    {   /* copies left by earlier updates, once nothing runs them any more */
        WIN32_FIND_DATAW fd;
        wchar_t pat[MAX_PATH], dir[MAX_PATH], *sl;
        HANDLE h;
        _snwprintf(dir, MAX_PATH, L"%ls", exe);
        if ((sl = wcsrchr(dir, L'\\'))) *sl = 0;
        _snwprintf(pat, MAX_PATH, L"%ls\\hardcore.old-*.exe", dir);
        if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                _snwprintf(old, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                DeleteFileW(old);     /* fails, harmlessly, for one still running */
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    _snwprintf(tmp, MAX_PATH, L"%ls.new", exe);
    tmp[MAX_PATH - 1] = 0;
    if (!(f = _wfopen(tmp, L"wb")) || fwrite(bin, 1, bn, f) != bn) {
        if (f) fclose(f);
        cli("hardcore: can't write next to %ls\n", exe); free(bin); return 1;
    }
    fclose(f);
    free(bin);
    GetLocalTime(&st);              /* this exe may be running (it is now): move it aside, don't overwrite */
    _snwprintf(old, MAX_PATH, L"%.*ls.old-%04d%02d%02d%02d%02d%02d.exe", (int)(wcslen(exe) - 4), exe,
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    old[MAX_PATH - 1] = 0;
    if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING) || !MoveFileExW(tmp, exe, MOVEFILE_REPLACE_EXISTING)) {
        cli("hardcore: couldn't swap in the new exe (error %lu)\n", GetLastError()); return 1;
    }
    cli("hardcore: updated %s -> %s\n", HARDCORE_VERSION, tag);
    {   /* and every open window takes it up in place: tabs, programs and all keep going */
        HWND w = NULL, all[64];
        int ok = 0, bad = 0, nw = 0, i;
        while (nw < 64 && (w = FindWindowExW(NULL, w, L"hardcore", NULL)) != NULL) all[nw++] = w;   /* first, before any of them changes */
        for (i = 0; i < nw; i++) {
            COPYDATASTRUCT cd;
            DWORD_PTR res = 0;
            w = all[i];
            cd.dwData = 0x48430002;
            cd.cbData = 6;
            cd.lpData = (void *)"reload";
            if (SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 20000, &res) && res) ok++;
            else bad++;
        }
        if (ok) cli("hardcore: %d window%s reloaded in place\n", ok, ok == 1 ? "" : "s");
        if (bad) cli("hardcore: %d window%s couldn't reload in place: close and reopen %s to finish\n", bad, bad == 1 ? "" : "s", bad == 1 ? "it" : "them");
    }
    return 0;
}

/* Started from a command line (a shell, WSL, Claude Code's !) rather than
   from Explorer (Start menu, desktop, taskbar): then `hardcore` means "a
   window here", like any command. Explorer's launches keep the last session. */
static int from_command_line(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe;
    DWORD me = GetCurrentProcessId(), parent = 0;
    int shell = 0, pass;
    if (snap == INVALID_HANDLE_VALUE) return 0;
    for (pass = 0; pass < 2; pass++) {      /* find our parent, then its name */
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) do {
            if (!pass && pe.th32ProcessID == me) { parent = pe.th32ParentProcessID; break; }
            if (pass && pe.th32ProcessID == parent) { shell = _wcsicmp(pe.szExeFile, L"explorer.exe") != 0; break; }
        } while (Process32NextW(snap, &pe));
        if (!parent) break;
    }
    CloseHandle(snap);
    return shell;
}

/* \\wsl.localhost\Distro\home\you (how WSL hands a Linux folder to a Windows
   program) → /home/you; Windows folders stay as they are */
static void folder_for_tab(const wchar_t *w, char *out, size_t cap) {
    const wchar_t *p = NULL;
    size_t k;
    if (!_wcsnicmp(w, L"\\\\wsl.localhost\\", 16)) p = w + 16;
    else if (!_wcsnicmp(w, L"\\\\wsl$\\", 7)) p = w + 7;
    if (p && (p = wcschr(p, L'\\'))) {
        WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)cap, NULL, NULL);
        for (k = 0; out[k]; k++) if (out[k] == '\\') out[k] = '/';
    } else if (p) snprintf(out, cap, "/");
    else WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    int argc, i, cols, rows;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WNDCLASSW wc;
    RECT r;
    MSG msg;
    char selftest_out[MAX_PATH] = "";
    int dir_given = 0;           /* a folder on the command line: open there, don't restore the last session */
    int slurp_window = 0;
    HWND h;
    HANDLE gfx_thread = NULL;
    (void)prev; (void)cmdline;

    if (argc > 1 && (!wcscmp(argv[1], L"here") || !wcscmp(argv[1], L"exit") || !wcscmp(argv[1], L"detach"))) {
        fputs("hardcore here/exit/detach: use the native Linux build inside WSL. The Windows GUI executable does not contain a console attachment backend.\n", stderr);
        return 2;
    }

    if (argc == 2 && !wcscmp(argv[1], L"--slurp-fixture")) return slurp_fixture();
    if (argc == 2 && !wcscmp(argv[1], L"--slurp-owner-fixture")) { Sleep(INFINITE); return 0; }
    if (argc == 3 && !wcscmp(argv[1], L"--slurp-probe")) return slurp_probe(wcstoul(argv[2], NULL, 10));
    if (argc == 8 && !wcscmp(argv[1], L"--slurp-bridge"))
        return slurp_bridge(wcstoul(argv[2], NULL, 10), wcstoull(argv[3], NULL, 10),
            (HWND)(uintptr_t)wcstoull(argv[4], NULL, 10), (HANDLE)(uintptr_t)wcstoull(argv[5], NULL, 10),
            (HANDLE)(uintptr_t)wcstoull(argv[6], NULL, 10), (HANDLE)(uintptr_t)wcstoull(argv[7], NULL, 10));

    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_t0);
    GetModuleFileNameW(NULL, g_exe_path, MAX_PATH);   /* before an update renames this file */
    if (_wgetenv(L"HARDCORE_ADOPT")) {                /* an older hardcore is handing its tabs over */
        SetEnvironmentVariableW(L"HARDCORE_ADOPT", NULL);
        adopt_read();
    }
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
        _snwprintf(tp, MAX_PATH, L"%ls\\hardcore-vt.idx", _wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L".");
        g_vtidx = _wfopen(tp, L"wb");
    }
    if (_wgetenv(L"HARDCORE_PERF")) {
        wchar_t tp[MAX_PATH];
        _snwprintf(tp, MAX_PATH, L"%ls\\hardcore-perf.txt", _wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L".");
        g_perf = _wfopen(tp, L"wb");
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
    if (g_last_dir[0] && dir_exists(g_last_dir)) snprintf(g_start_dir, sizeof(g_start_dir), "%s", g_last_dir);
    for (i = 1; i < argc; i++) {
        char a[1024];
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, a, sizeof(a), NULL, NULL);
        if ((!strcmp(a, "--cd") || !strcmp(a, "-d")) && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_start_dir, sizeof(g_start_dir), NULL, NULL);
            dir_given = 1;
        } else if (!strcmp(a, "--distro") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, g_distro, sizeof(g_distro), NULL, NULL);
        } else if (!strcmp(a, "--unit") && i + 1 < argc) {
            char out[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, out, sizeof(out), NULL, NULL);
            return unit_tests(out);
        } else if (!strcmp(a, "--bench-resize") && i + 1 < argc) {
            char out[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, out, sizeof(out), NULL, NULL);
            return bench_resize(out);
        } else if (!strcmp(a, "slurp")) {
            return slurp_cmd(argc - i - 1, argv + i + 1);
        } else if (!strcmp(a, "--slurp-selftest") && i + 1 < argc) {
            return slurp_selftest(argv[i + 1]);
        } else if (!strcmp(a, "--slurp-wsl-selftest") && i + 1 < argc) {
            return slurp_wsl_selftest(argv[i + 1]);
        } else if (!strcmp(a, "--slurp-window")) {
            slurp_window = 1; dir_given = 1;
            if (i + 1 < argc && !wcsncmp(argv[i + 1], L"slurp", 5)) {
                char request[600];
                if (!WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, request, sizeof(request), NULL, NULL) || !slurp_filter(request)) return 2;
            }
        } else if (!strcmp(a, "here")) {
            cli("hardcore here uses the native Linux build in WSL. Run make install in a Linux checkout.\n");
            return 2;
        } else if (!strcmp(a, "browser")) {
            cli("hardcore: browser tabs are available on macOS in this first pass; Windows WebView2 is not implemented yet.\n");
            return 1;
        } else if (!strcmp(a, "update") || !strcmp(a, "--update")) {
            return update_cmd(i + 1 < argc && !wcscmp(argv[i + 1], L"--force"));
        } else if (!strcmp(a, "version") || !strcmp(a, "--version") || !strcmp(a, "-v")) {
            cli("hardcore %s (build %s)\n", HARDCORE_VERSION, HARDCORE_BUILD);
            return 0;
        } else if (!strcmp(a, "help") || !strcmp(a, "--help") || !strcmp(a, "-h")) {
            cli("hardcore %s: a terminal for WSL\n"
                "  hardcore [DIR]          a new window in DIR (from a tab: this folder)\n"
                "  hardcore here           use the native Linux build inside WSL\n"
                "  hardcore exit/detach    here session commands in the native Linux build\n"
                "  hardcore update         install the latest release (--force: even if current)\n"
                "  hardcore slurp          attach running WSL and classic Windows consoles\n"
                "  hardcore slurp --list   discover eligible sessions without attaching\n"
                "  hardcore --version      this build's version\n", HARDCORE_VERSION);
            return 0;
        } else if (!strcmp(a, "--no-activate")) {
            show = SW_SHOWNOACTIVATE;       /* tests: open behind, never take the keyboard */
            g_session_off = !_wgetenv(L"HARDCORE_SESSIONS");   /* ...and never touch your saved tabs */
        } else if (!strcmp(a, "--inbox-conpty")) {
            g_inbox_conpty = 1;
        } else if (!strcmp(a, "--reflow-bench") && i + 1 < argc) {
            char out[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, out, sizeof(out), NULL, NULL);
            return reflow_bench(out);
        } else if (!strcmp(a, "--bench") && i + 4 < argc) {
            char out[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, out, sizeof(out), NULL, NULL);
            char in[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 4], -1, in, sizeof(in), NULL, NULL);
            return parse_bench(out, _wtoi(argv[i + 2]), _wtoi(argv[i + 3]), in);
        } else if (!strcmp(a, "--replay") && i + 3 < argc) {
            /* --replay OUT COLS ROWS FILE... */
            char out[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, out, sizeof(out), NULL, NULL);
            return replay(out, _wtoi(argv[i + 2]), _wtoi(argv[i + 3]), argv + i + 4, argc - i - 4);
        } else if (!strcmp(a, "--selftest") && i + 1 < argc) {
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, selftest_out, sizeof(selftest_out), NULL, NULL);
        } else if (a[0] != '-') {
            snprintf(g_start_dir, sizeof(g_start_dir), "%s", a);
            dir_given = 1;
        }
    }
    /* `hardcore` or `hardcore DIR` typed in a shell: a window there (the shell's
       folder if none was named), started on its own so the shell gets its
       prompt back (WSL waits for the Windows programs it starts) */
    if (!slurp_window && !selftest_out[0] && show != SW_SHOWNOACTIVATE && !g_adopting && !_wgetenv(L"HARDCORE_DETACHED") && from_command_line()) {
        wchar_t self[MAX_PATH], here[MAX_PATH * 2], full[MAX_PATH * 2], cl[4096], wdir[1100];
        char dir[1100];
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        if (dir_given && g_start_dir[0] != '/') {          /* relative to the shell's folder */
            MultiByteToWideChar(CP_UTF8, 0, g_start_dir, -1, here, MAX_PATH * 2);
            if (!GetFullPathNameW(here, MAX_PATH * 2, full, NULL)) wcscpy(full, here);
            folder_for_tab(full, dir, sizeof(dir));
        } else if (dir_given) snprintf(dir, sizeof(dir), "%s", g_start_dir);
        else {
            GetCurrentDirectoryW(MAX_PATH * 2, here);
            folder_for_tab(here, dir, sizeof(dir));
        }
        MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, 1100);
        GetModuleFileNameW(NULL, self, MAX_PATH);
        _snwprintf(cl, 4096, L"\"%ls\" --cd \"%ls\"", self, wdir);
        cl[4095] = 0;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        SetEnvironmentVariableW(L"HARDCORE_DETACHED", L"1");
        if (CreateProcessW(self, cl, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return 0;
        }
        SetEnvironmentVariableW(L"HARDCORE_DETACHED", NULL);
        snprintf(g_start_dir, sizeof(g_start_dir), "%s", dir);  /* couldn't detach: just open here */
        dir_given = 1;
    }
    SetEnvironmentVariableW(L"HARDCORE_DETACHED", NULL);   /* not for the tabs' children */
    trace("config");
    load_conpty(g_inbox_conpty);
    trace("conpty loaded");
    default_start_dir();
    write_rc();
    if (!g_shell_ps) g_have_distro = wsl_distros() > 0 && !_wgetenv(L"HARDCORE_PRETEND_NO_WSL");   /* the env: tests */
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
        r.right = 120 * CW;
        r.bottom = 36 * CH;
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
    if (!g_adopting && !dir_given && session_placement(&g_adopt_wp)) {
        /* the last session's window: where it was, as big as it was */
        if (show == SW_SHOWNOACTIVATE && g_adopt_wp.showCmd == SW_SHOWNORMAL) g_adopt_wp.showCmd = SW_SHOWNOACTIVATE;
    }
    if (g_adopt_wp.length) {                     /* exactly where the old window is (a reload), or was (a session) */
        SetWindowPlacement(h, &g_adopt_wp);
        show = (int)g_adopt_wp.showCmd;
        GetClientRect(h, &r);
        compute_grid(r.right, r.bottom, &cols, &rows);
        g_cols = cols;
        g_rows = rows;
    }
    ShowWindow(h, show);
    UpdateWindow(h);
    trace("window shown");
    check_disk();
    SetTimer(h, TIMER_DISK, 60 * 1000, NULL);
    /* wsl.exe boots while the D3D device is created; the tabs from last time
       if there are any (unless a folder was asked for), else one new tab */
    if (g_adopting && adopt_tabs()) {
        tab_fit(g_tabs[g_active]);               /* a different size only if the screen's DPI changed */
        schedule_spare();
    } else if (slurp_window) { /* discovery starts after graphics initialization below */ }
    else if (dir_given || !session_restore()) new_tab_here();
    else schedule_spare();
    trace("first tab spawned");
    if (gfx_thread) { WaitForSingleObject(gfx_thread, 5000); CloseHandle(gfx_thread); }
    trace("D3D device ready");
    if (!gfx_init_swapchain(h)) {
        MessageBoxW(h, L"Couldn't start Direct3D 11.", L"hardcore", MB_ICONERROR);
        return 1;
    }
    trace("swap chain ready");
    if (slurp_window && !slurp_start(0)) new_tab_here();
    InvalidateRect(h, NULL, FALSE);
    if (g_adopting) {                            /* drawn on top of the old window, then tell it */
        paint_now();
        SetForegroundWindow(h);
        adopt_done();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LocalFree(argv);
    return 0;
}
#endif /* _WIN32 */

#endif /* !HARDCORE_TTY_ONLY: GPU frontend */

#ifndef _WIN32
#include "hardcore_here.h"
#endif

#if defined(HARDCORE_TTY_ONLY)
static void tab_write(Tab *tb, const void *s, int n) { (void)tb; (void)s; (void)n; }
static void set_clipboard_utf8(const char *s, int n) { (void)s; (void)n; }
int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "exit") || !strcmp(argv[1], "detach"))) return here_lifecycle_main(argc - 2, argv + 2, !strcmp(argv[1], "exit"));
    if (argc > 1 && !strcmp(argv[1], "--here-files")) return hc_here_files_with_media(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--here-web")) return hc_here_web_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--here-media")) return hc_here_media_main(argc - 1, argv + 1);
    if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version"))) {
        printf("hardcore %s (native terminal backend)\n", HARDCORE_VERSION); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "here")) return here_main(argc - 2, argv + 2);
    return here_main(argc - 1, argv + 1);
}
#elif defined(__APPLE__)
#include "hardcore_mac.m"
#endif
