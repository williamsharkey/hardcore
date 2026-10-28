/* hcmd — a small terminal Markdown (and plain text) reader. libc only.
 *
 *   hcmd FILE.md          read a Markdown file (live-reloads when it changes)
 *   hcmd -p [+N] FILE     plain text with line numbers, opened at line N
 *   hcmd -                Markdown from stdin (keys come from /dev/tty)
 *   hcmd --dump=WxH FILE  print the first H rendered lines at width W, no
 *                         escapes (--dump-ansi=WxH keeps them); for tests
 *
 * Keys: j/k/arrows line, space/b/PgDn/PgUp page, d/u half page, g/G top/end,
 * / search, n/N next/previous match, r reload, q quit. Mouse wheel scrolls.
 * Links are OSC 8 hyperlinks (Cmd+click in hardcore, iTerm2, kitty, ...);
 * relative link targets resolve against the file's folder. Any terminal
 * works; when stdout is not a terminal the document is printed plain. */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/select.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* ── glyphs and lines ─────────────────────────────────────────────── */

enum { A_B = 1, A_I = 2, A_U = 4, A_S = 8, A_D = 16, A_R = 32 };
/* fg: 0 default, 1..16 = the terminal's 16 colors (index + 1).
   bg: 0 none, 1 code background. */
enum { C_RED = 2, C_GREEN = 3, C_YELLOW = 4, C_BLUE = 5, C_MAGENTA = 6, C_CYAN = 7,
       C_GRAY = 9, C_BRED = 10, C_BGREEN = 11, C_BYELLOW = 12, C_BBLUE = 13, C_BMAGENTA = 14, C_BCYAN = 15 };
typedef struct { unsigned char fg, bg, at; unsigned short link; } Sty;
typedef struct { char s[16]; unsigned char sl, w; Sty st; } G;   /* s="\n", w=0: a hard break */
typedef struct { G *g; int n, cap; int src; } GBuf;               /* src: source line (1-based) */

typedef struct {
    GBuf *line; int n, cap;
    char **link; int nlink, clink;
    char base[PATH_MAX];       /* folder that relative links resolve against, "" = none */
} Doc;

static void *xrealloc(void *p, size_t n) { void *q = realloc(p, n ? n : 1); if (!q) { perror("hcmd"); exit(1); } return q; }
static char *xstrndup(const char *s, size_t n) { char *p = xrealloc(NULL, n + 1); memcpy(p, s, n); p[n] = 0; return p; }

static void gb_push(GBuf *b, G g) {
    if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 32; b->g = xrealloc(b->g, sizeof(G) * (size_t)b->cap); }
    b->g[b->n++] = g;
}
static int gb_width(const GBuf *b, int a, int e) { int w = 0; for (int i = a; i < e; i++) w += b->g[i].w; return w; }

static int u8dec(const unsigned char *s, int n, uint32_t *cp) {
    if (n <= 0) { *cp = 0; return 0; }
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int len = s[0] >= 0xF0 && s[0] < 0xF8 ? 4 : s[0] >= 0xE0 ? 3 : s[0] >= 0xC0 ? 2 : 0;
    if (!len || len > n) { *cp = 0xFFFD; return 1; }
    uint32_t c = s[0] & (0x7F >> len);
    for (int i = 1; i < len; i++) { if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; } c = c << 6 | (s[i] & 0x3F); }
    *cp = c;
    return len;
}
static int u8enc(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}
/* Display width: enough of wcwidth for prose, CJK and emoji. */
static int cpw(uint32_t c) {
    if (c == 0) return 0;
    if (c < 32 || (c >= 0x7F && c < 0xA0)) return 1;            /* shown as a replacement */
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F) ||
        (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x20D0 && c <= 0x20FF) || c == 0x200D) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}
/* Appends text as glyphs. Tabs become spaces to the next multiple of 4
   (counted from the buffer start); control characters become U+FFFD. */
static void gb_text(GBuf *b, const char *s, int n, Sty st) {
    const unsigned char *u = (const unsigned char *)s;
    for (int i = 0; i < n;) {
        uint32_t c; int k = u8dec(u + i, n - i, &c);
        i += k;
        if (c == '\t') { int col = gb_width(b, 0, b->n); do { G g = {" ", 1, 1, st}; gb_push(b, g); } while (++col % 4); continue; }
        if (c == '\n') { G g = {"\n", 1, 0, st}; gb_push(b, g); continue; }
        if (c < 32 || c == 0x7F) c = 0xFFFD;
        int w = cpw(c);
        if (w == 0 && b->n && b->g[b->n - 1].sl + 4 < (int)sizeof(b->g[0].s) && b->g[b->n - 1].s[0] != '\n') {
            G *p = &b->g[b->n - 1]; p->sl += (unsigned char)u8enc(c, p->s + p->sl); p->s[p->sl] = 0; continue;
        }
        if (w == 0) continue;
        G g; memset(&g, 0, sizeof(g)); g.sl = (unsigned char)u8enc(c, g.s); g.w = (unsigned char)w; g.st = st;
        gb_push(b, g);
    }
}
static void gb_str(GBuf *b, const char *s, Sty st) { gb_text(b, s, (int)strlen(s), st); }
static void gb_cat(GBuf *b, const G *g, int n) { for (int i = 0; i < n; i++) gb_push(b, g[i]); }

static GBuf *doc_line(Doc *d, int src) {
    if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 128; d->line = xrealloc(d->line, sizeof(GBuf) * (size_t)d->cap); }
    GBuf *l = &d->line[d->n++]; memset(l, 0, sizeof(*l)); l->src = src;
    return l;
}
static void doc_free(Doc *d) {
    for (int i = 0; i < d->n; i++) free(d->line[i].g);
    for (int i = 0; i < d->nlink; i++) free(d->link[i]);
    free(d->line); free(d->link);
    char base[PATH_MAX]; memcpy(base, d->base, sizeof(base));
    memset(d, 0, sizeof(*d)); memcpy(d->base, base, sizeof(base));
}
static int is_scheme(const char *u) {
    int i = 0;
    if (!isalpha((unsigned char)u[0])) return 0;
    while (isalnum((unsigned char)u[i]) || u[i] == '+' || u[i] == '-' || u[i] == '.') i++;
    return u[i] == ':' && i > 1;
}
/* A link target as the terminal should get it: absolute URLs as they are,
   relative paths as file:// URLs under the document's folder. */
static unsigned short doc_link(Doc *d, const char *url, int n) {
    char raw[4096], out[8192];
    if (n <= 0 || n >= (int)sizeof(raw) || url[0] == '#') return 0;
    memcpy(raw, url, (size_t)n); raw[n] = 0;
    if (is_scheme(raw) || !d->base[0]) snprintf(out, sizeof(out), "%s", raw);
    else {
        char path[PATH_MAX * 2]; int k = 0;
        char *hash = strchr(raw, '#'); if (hash) *hash = 0;
        if (raw[0] == '/') snprintf(path, sizeof(path), "%s", raw);
        else if (raw[0] == '~' && raw[1] == '/' && getenv("HOME")) snprintf(path, sizeof(path), "%s%s", getenv("HOME"), raw + 1);
        else snprintf(path, sizeof(path), "%s/%s", d->base, raw);
        k = snprintf(out, sizeof(out), "file://");
        for (const unsigned char *p = (const unsigned char *)path; *p && k < (int)sizeof(out) - 4; p++) {
            if (*p <= ' ' || *p == '%' || *p == '"' || *p == '<' || *p == '>' || *p >= 0x7F) k += snprintf(out + k, sizeof(out) - (size_t)k, "%%%02X", *p);
            else out[k++] = (char)*p;
        }
        out[k] = 0;
    }
    for (int i = 0; i < d->nlink; i++) if (!strcmp(d->link[i], out)) return (unsigned short)(i + 1);
    if (d->nlink >= 65000) return 0;
    if (d->nlink == d->clink) { d->clink = d->clink ? d->clink * 2 : 16; d->link = xrealloc(d->link, sizeof(char *) * (size_t)d->clink); }
    d->link[d->nlink++] = strdup(out);
    return (unsigned short)d->nlink;
}

/* ── inline Markdown ──────────────────────────────────────────────── */

typedef struct { char *label, *url; } Ref;
static Ref *g_refs; static int g_nrefs;
static const char *ref_find(const char *label, int n) {
    for (int i = 0; i < g_nrefs; i++) {
        if ((int)strlen(g_refs[i].label) != n) continue;
        int k = 0; while (k < n && tolower((unsigned char)g_refs[i].label[k]) == tolower((unsigned char)label[k])) k++;
        if (k == n) return g_refs[i].url;
    }
    return NULL;
}

static const Sty S_LINK = {C_BBLUE, 0, A_U, 0}, S_CODE = {C_BYELLOW, 1, 0, 0};
static void inl(Doc *d, GBuf *o, const char *s, int n, Sty st);

/* [text](url) or [text][ref] or [ref], starting at s[0] == '['. Returns the
   bytes consumed (0 = not a link) and the text and target spans. */
static int parse_link(const char *s, int n, int *tn, const char **url, int *un) {
    int depth = 0, i = 0;
    for (; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) { i++; continue; }
        if (s[i] == '`') { int j = i + 1; while (j < n && s[j] != '`') j++; if (j < n) i = j; continue; }
        if (s[i] == '[') depth++;
        else if (s[i] == ']' && --depth == 0) break;
    }
    if (i >= n) return 0;
    *tn = i - 1;
    int e = i + 1;
    if (e < n && s[e] == '(') {
        int j = e + 1, par = 1;
        while (j < n && s[j] == ' ') j++;
        int us = j;
        if (j < n && s[j] == '<') { us = ++j; while (j < n && s[j] != '>') j++; *url = s + us; *un = j - us; while (j < n && s[j] != ')') j++; return j < n ? j + 1 : 0; }
        for (; j < n; j++) {
            if (s[j] == '\\' && j + 1 < n) { j++; continue; }
            if (s[j] == '(') par++;
            else if (s[j] == ')' && --par == 0) break;
        }
        if (j >= n) return 0;
        int ue = us; while (ue < j && s[ue] != ' ') ue++;
        *url = s + us; *un = ue - us;
        return j + 1;
    }
    const char *label = s + 1; int ln = *tn, used = e;
    if (e + 1 < n && s[e] == '[') {
        int j = e + 1; while (j < n && s[j] != ']') j++;
        if (j < n) { if (j > e + 1) { label = s + e + 1; ln = j - e - 1; } used = j + 1; }
    }
    const char *u = ref_find(label, ln);
    if (!u) return 0;
    *url = u; *un = (int)strlen(u);
    return used;
}

static int is_punct(int c) { return c > 32 && c < 127 && !isalnum(c); }
static int find_closer(const char *s, int n, int from, char c, int k) {
    for (int i = from; i + k <= n; i++) {
        if (s[i] == '\\') { i++; continue; }
        if (s[i] == '`') { int j = i + 1; while (j < n && s[j] != '`') j++; i = j; continue; }
        int r = 0; while (i + r < n && s[i + r] == c) r++;
        if (r >= k && i > from && s[i - 1] != ' ') return 1;
        if (r) i += r - 1;
    }
    return 0;
}

static void inl(Doc *d, GBuf *o, const char *s, int n, Sty st) {
    Sty cur = st;
    int bold = 0, ital = 0, strike = 0, run = 0;   /* run: start of pending literal text */
#define FLUSH(i) do { if ((i) > run) gb_text(o, s + run, (i) - run, cur); } while (0)
    for (int i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        int prev = i ? (unsigned char)s[i - 1] : ' ';
        if (c == '\\' && i + 1 < n && (is_punct((unsigned char)s[i + 1]) || s[i + 1] == '\n')) {
            FLUSH(i);
            if (s[i + 1] == '\n') gb_text(o, "\n", 1, cur); else gb_text(o, s + i + 1, 1, cur);
            i += 2; run = i; continue;
        }
        if (c == '`') {
            int k = 0; while (i + k < n && s[i + k] == '`') k++;
            int j = i + k, found = -1;
            while (j < n) {
                if (s[j] == '`') { int r = 0; while (j + r < n && s[j + r] == '`') r++; if (r == k) { found = j; break; } j += r; }
                else j++;
            }
            if (found < 0) { i += k; continue; }
            FLUSH(i);
            int a = i + k, b = found;
            if (b - a >= 2 && s[a] == ' ' && s[b - 1] == ' ') { a++; b--; }
            Sty cs = S_CODE; cs.link = cur.link; if (cur.link) cs.at |= A_U;
            for (int q = a; q < b; q++) { char ch = s[q] == '\n' ? ' ' : s[q]; gb_text(o, &ch, 1, cs); }
            i = found + k; run = i; continue;
        }
        if ((c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) && !cur.link) {
            int img = c == '!', tn = 0, un = 0; const char *url = NULL;
            int used = parse_link(s + i + img, n - i - img, &tn, &url, &un);
            if (used) {
                FLUSH(i);
                const char *text = s + i + img + 1;
                Sty ls = S_LINK; ls.at |= cur.at & (A_B | A_I); ls.link = doc_link(d, url, un);
                if (img) {
                    Sty is = ls; is.at &= ~A_U;
                    gb_str(o, "\xe2\x96\xa3 ", is);                 /* ▣ */
                    if (tn > 0) gb_text(o, text, tn, ls);
                    else { const char *b = url + un; while (b > url && b[-1] != '/') b--; gb_text(o, b, (int)(url + un - b), ls); }
                } else inl(d, o, text, tn, ls);
                i += img + used; run = i; continue;
            }
        }
        if (c == '<') {
            int j = i + 1; while (j < n && s[j] != '>' && s[j] != '<' && s[j] != '\n') j++;
            if (j < n && s[j] == '>') {
                const char *t = s + i + 1; int tl = j - i - 1;
                int sch = 0; while (sch < tl && (isalnum((unsigned char)t[sch]) || t[sch] == '+' || t[sch] == '.' || t[sch] == '-')) sch++;
                int auto_url = sch > 1 && sch < tl && t[sch] == ':' && !memchr(t, ' ', (size_t)tl);
                int mail = !auto_url && tl > 2 && memchr(t, '@', (size_t)tl) && !memchr(t, ' ', (size_t)tl);
                if (auto_url || mail) {
                    FLUSH(i);
                    char u[2048]; snprintf(u, sizeof(u), "%s%.*s", mail ? "mailto:" : "", tl, t);
                    Sty ls = S_LINK; ls.link = cur.link ? cur.link : doc_link(d, u, (int)strlen(u));
                    gb_text(o, t, tl, ls);
                    i = j + 1; run = i; continue;
                }
                if (tl && (isalpha((unsigned char)t[0]) || t[0] == '/' || t[0] == '!')) {   /* inline HTML: dropped, but <br> breaks */
                    FLUSH(i);
                    if (!strncasecmp(t, "br", 2) && (tl == 2 || t[2] == ' ' || t[2] == '/')) gb_text(o, "\n", 1, cur);
                    if (!strncasecmp(t, "img", 3)) {
                        const char *src = strstr(t, "src=\""); const char *alt = strstr(t, "alt=\"");
                        if (src && src < t + tl) {
                            src += 5; const char *se = memchr(src, '"', (size_t)(t + tl - src));
                            Sty ls = S_LINK; ls.at &= ~A_U; if (se) ls.link = doc_link(d, src, (int)(se - src));
                            gb_str(o, "\xe2\x96\xa3 ", ls); ls.at |= A_U;
                            if (alt && alt < t + tl) { alt += 5; const char *ae = memchr(alt, '"', (size_t)(t + tl - alt)); if (ae) gb_text(o, alt, (int)(ae - alt), ls); }
                            else if (se) gb_text(o, src, (int)(se - src), ls);
                        }
                    }
                    i = j + 1; run = i; continue;
                }
            }
        }
        if (c == '&') {
            static const struct { const char *e; const char *u; } ent[] = {
                {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
                {"&nbsp;", " "}, {"&mdash;", "\xe2\x80\x94"}, {"&ndash;", "\xe2\x80\x93"}, {"&hellip;", "\xe2\x80\xa6"}, {"&copy;", "\xc2\xa9"}};
            int done = 0;
            for (size_t e = 0; e < sizeof(ent) / sizeof(*ent); e++) {
                int el = (int)strlen(ent[e].e);
                if (i + el <= n && !strncmp(s + i, ent[e].e, (size_t)el)) { FLUSH(i); gb_str(o, ent[e].u, cur); i += el; run = i; done = 1; break; }
            }
            if (!done && i + 3 < n && s[i + 1] == '#') {
                int j = i + 2, hex = s[j] == 'x' || s[j] == 'X'; if (hex) j++;
                uint32_t v = 0; int dg = 0;
                while (j < n && dg < 7 && (hex ? isxdigit((unsigned char)s[j]) : isdigit((unsigned char)s[j]))) {
                    v = v * (hex ? 16 : 10) + (uint32_t)(isdigit((unsigned char)s[j]) ? s[j] - '0' : (tolower((unsigned char)s[j]) - 'a' + 10)); j++; dg++;
                }
                if (dg && j < n && s[j] == ';' && v && v < 0x110000) {
                    char u[8]; int k = u8enc(v, u); FLUSH(i); gb_text(o, u, k, cur); i = j + 1; run = i; done = 1;
                }
            }
            if (done) continue;
        }
        if ((c == '*' || c == '_' || c == '~')) {
            int k = 0; while (i + k < n && s[i + k] == (char)c) k++;
            int next = i + k < n ? (unsigned char)s[i + k] : ' ';
            int left = !isspace(next) && !(c == '_' && isalnum(prev));
            int right = !isspace(prev) && !(c == '_' && isalnum(next));
            int k0 = k;
            if (c == '~') {
                if (k == 2 && strike && right) { strike = 0; k = 0; }
                else if (k == 2 && left && find_closer(s, n, i + 2, '~', 2)) { strike = 1; k = 0; }
            } else {
                while (k > 0) {
                    if (k >= 2 && bold && right) { bold = 0; k -= 2; continue; }
                    if (ital && right) { ital = 0; k -= 1; continue; }
                    if (k >= 2 && !bold && left && find_closer(s, n, i + k, (char)c, 2)) { bold = 1; k -= 2; continue; }
                    if (!ital && left && find_closer(s, n, i + k, (char)c, 1)) { ital = 1; k -= 1; continue; }
                    break;
                }
            }
            if (k != k0) {
                FLUSH(i);
                if (k) gb_text(o, s + i, k, cur);
                cur.at = (unsigned char)((st.at & ~(A_B | A_I | A_S)) | (bold ? A_B : 0) | (ital ? A_I : 0) | (strike ? A_S : 0) | (st.at & (A_B | A_I | A_S)));
                i += k0; run = i; continue;
            }
            i += k; continue;
        }
        if ((c == 'h' || c == 'w') && !cur.link && (i == 0 || isspace(prev) || prev == '(') &&
            (!strncmp(s + i, "https://", 8) || !strncmp(s + i, "http://", 7) || !strncmp(s + i, "www.", 4))) {
            int j = i; while (j < n && !isspace((unsigned char)s[j]) && s[j] != '<') j++;
            while (j > i && strchr(".,;:!?)*_", s[j - 1])) {
                if (s[j - 1] == ')') { int op = 0, cl = 0; for (int q = i; q < j; q++) { op += s[q] == '('; cl += s[q] == ')'; } if (op >= cl) break; }
                j--;
            }
            FLUSH(i);
            char u[2048]; snprintf(u, sizeof(u), "%s%.*s", s[i] == 'w' ? "https://" : "", j - i, s + i);
            Sty ls = S_LINK; ls.at |= cur.at & (A_B | A_I); ls.link = doc_link(d, u, (int)strlen(u));
            gb_text(o, s + i, j - i, ls);
            i = j; run = i; continue;
        }
        i++;
    }
    FLUSH(n);
#undef FLUSH
}

/* ── wrapping ─────────────────────────────────────────────────────── */

/* Splits b into lines of at most width columns at spaces (a word longer than
   width is cut); '\n' glyphs force a break. ranges gets [start,end) pairs. */
static int wrap(const GBuf *b, int width, int *ranges, int cap) {
    int n = 0, i = 0;
    if (width < 1) width = 1;
    while (i < b->n && n < cap) {
        int start = i, col = 0, brk = -1, j, next;
        for (j = i;; j++) {
            if (j >= b->n) { next = j; break; }
            const G *g = &b->g[j];
            if (g->s[0] == '\n' && g->w == 0) { next = j + 1; break; }
            if (g->s[0] == ' ' && g->sl == 1) brk = j;
            if (col + g->w > width && j > start) {
                if (brk > start) { j = brk; next = brk; while (next < b->n && b->g[next].s[0] == ' ' && b->g[next].sl == 1) next++; }
                else next = j;
                break;
            }
            col += g->w;
        }
        int e = j; while (e > start && b->g[e - 1].s[0] == ' ' && b->g[e - 1].sl == 1) e--;
        ranges[2 * n] = start; ranges[2 * n + 1] = e; n++;
        i = next;
    }
    if (!n && cap) { ranges[0] = ranges[1] = 0; n = 1; }
    return n;
}

/* ── blocks ───────────────────────────────────────────────────────── */

typedef struct {
    GBuf first, cont;          /* prefixes: quote bars, list bullets, indentation */
    int first_pending;         /* the next line takes `first` */
    int width;                 /* columns for content after the prefix */
    int blocks;                /* blocks emitted so far: the next one gets a gap */
    int tight;                 /* a tight list item: no gaps between its blocks */
} Ctx;

static GBuf *ctx_line(Doc *d, Ctx *c, int src) {
    GBuf *l = doc_line(d, src);
    GBuf *p = c->first_pending ? &c->first : &c->cont;
    gb_cat(l, p->g, p->n);
    c->first_pending = 0;
    return l;
}
static void ctx_gap(Doc *d, Ctx *c, int src) {
    if (!c->blocks++ || c->tight) return;
    GBuf *l = ctx_line(d, c, src);
    while (l->n && l->g[l->n - 1].s[0] == ' ') l->n--;
}
static void emit_wrapped(Doc *d, Ctx *c, GBuf *b, int src, int width) {
    int cap = b->n + 2, *r = xrealloc(NULL, sizeof(int) * 2 * (size_t)cap);
    int k = wrap(b, width, r, cap);
    for (int i = 0; i < k; i++) { GBuf *l = ctx_line(d, c, src); gb_cat(l, b->g + r[2 * i], r[2 * i + 1] - r[2 * i]); }
    free(r);
}

static int indent_of(const char *s) { int i = 0; while (s[i] == ' ') i++; return i; }
static int blank(const char *s) { while (*s == ' ' || *s == '\t' || *s == '\r') s++; return !*s; }
static int fence_of(const char *s, char *ch) {
    int in = indent_of(s); if (in > 3) return 0;
    s += in; int k = 0; while (s[k] == '`') k++;
    if (k < 3) { k = 0; while (s[k] == '~') k++; if (k < 3) return 0; *ch = '~'; }
    else { *ch = '`'; if (strchr(s + k, '`')) return 0; }
    return k;
}
static int hr_of(const char *s) {
    if (indent_of(s) > 3) return 0;
    char c = 0; int k = 0;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\r') continue;
        if (*s != '-' && *s != '*' && *s != '_') return 0;
        if (c && *s != c) return 0;
        c = *s; k++;
    }
    return k >= 3;
}
static int heading_of(const char *s) {
    if (indent_of(s) > 3) return 0;
    s += indent_of(s); int k = 0; while (s[k] == '#') k++;
    return k >= 1 && k <= 6 && (s[k] == ' ' || s[k] == 0 || s[k] == '\r') ? k : 0;
}
/* A list marker: returns the content offset (0 = none); *ordered, *num. */
static int list_of(const char *s, int *ordered, long *num, char *bullet) {
    int in = indent_of(s), i = in;
    if (in > 3) return 0;
    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && (s[i + 1] == ' ' || !s[i + 1])) {
        *ordered = 0; *bullet = s[i]; i++;
    } else {
        int k = 0; long v = 0; while (isdigit((unsigned char)s[i + k]) && k < 9) v = v * 10 + (s[i + k] - '0'), k++;
        if (!k || (s[i + k] != '.' && s[i + k] != ')') || (s[i + k + 1] != ' ' && s[i + k + 1])) return 0;
        *ordered = 1; *num = v; *bullet = s[i + k]; i += k + 1;
    }
    int sp = 0; while (s[i + sp] == ' ') sp++;
    if (!s[i + sp]) return i + 1;
    if (sp > 4) sp = 1;
    return i + sp;
}
static int table_delim(const char *s) {
    int bars = 0, dashes = 0;
    for (; *s; s++) {
        if (*s == '|') bars++;
        else if (*s == '-') dashes++;
        else if (*s != ':' && *s != ' ' && *s != '\t' && *s != '\r') return 0;
    }
    return dashes >= 1 && (bars >= 1);
}

typedef struct { char **s; int *src; int n; } Lines;

static void render_blocks(Doc *d, Ctx *c, Lines L);

static void sub_ctx(Ctx *parent, Ctx *child, const char *bullet, Sty bst, int contw) {
    memset(child, 0, sizeof(*child));
    GBuf *p = parent->first_pending ? &parent->first : &parent->cont;
    gb_cat(&child->first, p->g, p->n);
    gb_str(&child->first, bullet, bst);
    gb_cat(&child->cont, parent->cont.g, parent->cont.n);
    for (int i = 0; i < contw; i++) gb_str(&child->cont, " ", (Sty){0});
    child->first_pending = 1;
    child->width = parent->width - contw;
    if (child->width < 8) child->width = 8;
}
static void ctx_done(Ctx *parent, Ctx *child) {
    if (!child->first_pending) parent->first_pending = 0;
    free(child->first.g); free(child->cont.g);
}

static void code_block(Doc *d, Ctx *c, Lines L, int a, int e, const char *lang) {
    Sty bg = {0, 1, 0, 0}, label = {C_GRAY, 1, 0, 0};
    int w = c->width;
    GBuf *l = ctx_line(d, c, L.src[a > 0 ? a - 1 : a]);
    int ll = lang && *lang ? (int)strlen(lang) : 0;
    for (int i = 0; i < w - ll - 1; i++) gb_str(l, " ", bg);
    if (ll) { gb_str(l, lang, label); gb_str(l, " ", bg); } else gb_str(l, " ", bg);
    while (l->n && gb_width(l, 0, l->n) > c->cont.n + w) l->n--;
    for (int i = a; i < e; i++) {
        GBuf t = {0};
        gb_str(&t, " ", bg);
        gb_str(&t, L.s[i], bg);
        if (!t.n || gb_width(&t, 0, t.n) == 0) gb_str(&t, " ", bg);
        /* code is not reflowed: long lines continue on the next row */
        for (int at = 0; at < t.n;) {
            GBuf *row = ctx_line(d, c, L.src[i]);
            int col = 0, j = at;
            if (at) { gb_str(row, " ", bg); col = 1; }
            while (j < t.n && col + t.g[j].w <= w) { gb_push(row, t.g[j]); col += t.g[j].w; j++; }
            if (j == at) { gb_push(row, t.g[j]); col += t.g[j].w; j++; }
            while (col < w) { gb_str(row, " ", bg); col++; }
            at = j;
        }
        free(t.g);
    }
    l = ctx_line(d, c, L.src[e > a ? e - 1 : a]);
    for (int i = 0; i < w; i++) gb_str(l, " ", bg);
}

static int split_cells(const char *s, char **out, int cap) {
    while (*s == ' ') s++;
    if (*s == '|') s++;
    int n = 0;
    const char *a = s;
    for (const char *p = s;; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '`') { const char *q = strchr(p + 1, '`'); if (q) { p = q; continue; } }
        if (*p == '|' || !*p || *p == '\r') {
            const char *e = p; while (e > a && (e[-1] == ' ' || e[-1] == '\t')) e--;
            const char *b = a; while (b < e && (*b == ' ' || *b == '\t')) b++;
            if (*p || b < e || n == 0) { if (n < cap) out[n++] = xstrndup(b, (size_t)(e - b)); }
            if (!*p || *p == '\r') break;
            a = p + 1;
            const char *r = a; while (*r == ' ' || *r == '\t' || *r == '\r') r++;
            if (!*r) break;
        }
    }
    return n;
}

static void table_block(Doc *d, Ctx *c, Lines L, int a, int e) {
    enum { MAXC = 32 };
    int ncol = 0, align[MAXC] = {0}, nrow = 0;
    char *cells[MAXC];
    int dn = split_cells(L.s[a + 1], cells, MAXC);
    for (int i = 0; i < dn; i++) {
        int len = (int)strlen(cells[i]);
        align[i] = (len && cells[i][len - 1] == ':') ? (cells[i][0] == ':' ? 2 : 1) : 0;   /* 0 left 1 right 2 center */
        free(cells[i]);
    }
    ncol = dn;
    int rows = e - a - 1;
    GBuf *gc = xrealloc(NULL, sizeof(GBuf) * (size_t)(rows * MAXC));
    memset(gc, 0, sizeof(GBuf) * (size_t)(rows * MAXC));
    int nat[MAXC] = {0};
    for (int r = 0, li = a; li < e; li++) {
        if (li == a + 1) continue;
        int k = split_cells(L.s[li], cells, MAXC);
        if (k > ncol && r == 0) ncol = k < MAXC ? k : MAXC;
        for (int i = 0; i < k; i++) {
            if (i < ncol) {
                Sty st = {0, 0, (unsigned char)(r == 0 ? A_B : 0), 0};
                inl(d, &gc[r * MAXC + i], cells[i], (int)strlen(cells[i]), st);
                int w = gb_width(&gc[r * MAXC + i], 0, gc[r * MAXC + i].n);
                if (w > nat[i]) nat[i] = w;
            }
            free(cells[i]);
        }
        r++; nrow = r;
    }
    int width[MAXC], avail = c->width - (3 * ncol + 1), total = 0;
    for (int i = 0; i < ncol; i++) { width[i] = nat[i] ? nat[i] : 1; total += width[i]; }
    while (total > avail) {   /* narrow the widest column until it fits (or each is down to 4) */
        int wi = 0; for (int i = 1; i < ncol; i++) if (width[i] > width[wi]) wi = i;
        if (width[wi] <= 4) break;
        width[wi]--; total--;
    }
    Sty bs = {C_GRAY, 0, 0, 0};
#define RULE(L0, M0, R0) do { GBuf *rl = ctx_line(d, c, L.src[a]); gb_str(rl, L0, bs); \
        for (int i = 0; i < ncol; i++) { for (int q = 0; q < width[i] + 2; q++) gb_str(rl, "\xe2\x94\x80", bs); gb_str(rl, i + 1 < ncol ? M0 : R0, bs); } } while (0)
    RULE("\xe2\x94\x8c", "\xe2\x94\xac", "\xe2\x94\x90");
    for (int r = 0; r < nrow; r++) {
        int *rg[MAXC], nr[MAXC], h = 1;
        for (int i = 0; i < ncol; i++) {
            GBuf *b = &gc[r * MAXC + i];
            rg[i] = xrealloc(NULL, sizeof(int) * 2 * (size_t)(b->n + 2));
            nr[i] = wrap(b, width[i], rg[i], b->n + 2);
            if (nr[i] > h) h = nr[i];
        }
        for (int y = 0; y < h; y++) {
            GBuf *row = ctx_line(d, c, L.src[a + (r ? r + 1 : 0)]);
            gb_str(row, "\xe2\x94\x82", bs);
            for (int i = 0; i < ncol; i++) {
                GBuf *b = &gc[r * MAXC + i];
                int s0 = y < nr[i] ? rg[i][2 * y] : 0, s1 = y < nr[i] ? rg[i][2 * y + 1] : 0;
                int w = gb_width(b, s0, s1), pad = width[i] - w, lp = align[i] == 1 ? pad : align[i] == 2 ? pad / 2 : 0;
                gb_str(row, " ", (Sty){0});
                for (int q = 0; q < lp; q++) gb_str(row, " ", (Sty){0});
                gb_cat(row, b->g + s0, s1 - s0);
                for (int q = 0; q < pad - lp; q++) gb_str(row, " ", (Sty){0});
                gb_str(row, " ", (Sty){0});
                gb_str(row, "\xe2\x94\x82", bs);
            }
        }
        for (int i = 0; i < ncol; i++) free(rg[i]);
        if (r == 0 && nrow > 1) RULE("\xe2\x94\x9c", "\xe2\x94\xbc", "\xe2\x94\xa4");
    }
    RULE("\xe2\x94\x94", "\xe2\x94\xb4", "\xe2\x94\x98");
#undef RULE
    for (int i = 0; i < rows * MAXC; i++) free(gc[i].g);
    free(gc);
}

static void para(Doc *d, Ctx *c, Lines L, int a, int e, Sty st) {
    size_t cap = 1; for (int i = a; i < e; i++) cap += strlen(L.s[i]) + 1;
    char *t = xrealloc(NULL, cap); size_t k = 0;
    for (int i = a; i < e; i++) {
        const char *s = L.s[i] + indent_of(L.s[i]);
        size_t n = strlen(s);
        while (n && (s[n - 1] == '\r')) n--;
        int hard = n >= 2 && s[n - 1] == ' ' && s[n - 2] == ' ';
        while (n && s[n - 1] == ' ') n--;
        memcpy(t + k, s, n); k += n;
        if (i + 1 < e) t[k++] = hard ? '\n' : ' ';
    }
    GBuf b = {0};
    inl(d, &b, t, (int)k, st);
    emit_wrapped(d, c, &b, L.src[a], c->width);
    free(b.g); free(t);
}

static int starts_block(const char *s) {
    char ch; int o; long num; char bul;
    if (heading_of(s) || fence_of(s, &ch) || hr_of(s)) return 1;
    if (s[indent_of(s)] == '>') return 1;
    if (indent_of(s) <= 3 && list_of(s, &o, &num, &bul)) return !o || num == 1;   /* only "1." interrupts a paragraph */
    return 0;
}

static void render_blocks(Doc *d, Ctx *c, Lines L) {
    static const char *bullets[] = {"\xe2\x80\xa2 ", "\xe2\x97\xa6 ", "\xe2\x96\xaa "};   /* • ◦ ▪ */
    static int depth;
    int i = 0;
    while (i < L.n) {
        const char *s = L.s[i];
        char fch; int fk, h, ordered; long num; char bul;
        if (blank(s)) { i++; continue; }
        if (!strncmp(s + indent_of(s), "<!--", 4)) {   /* HTML comments are not shown */
            while (i < L.n && !strstr(L.s[i], "-->")) i++;
            i++; continue;
        }
        if ((fk = fence_of(s, &fch))) {
            char lang[64]; const char *p = s + indent_of(s) + fk; while (*p == ' ') p++;
            int ln = 0; while (p[ln] && p[ln] != ' ' && p[ln] != '\r' && ln < 63) { lang[ln] = p[ln]; ln++; } lang[ln] = 0;
            int in = indent_of(s), a = i + 1, e = a;
            while (e < L.n) { char c2; int k2 = fence_of(L.s[e], &c2); if (k2 >= fk && c2 == fch && blank(L.s[e] + indent_of(L.s[e]) + k2)) break; e++; }
            for (int q = a; q < e; q++) { int ci = indent_of(L.s[q]); memmove(L.s[q], L.s[q] + (ci < in ? ci : in), strlen(L.s[q] + (ci < in ? ci : in)) + 1); size_t z = strlen(L.s[q]); if (z && L.s[q][z - 1] == '\r') L.s[q][z - 1] = 0; }
            ctx_gap(d, c, L.src[i]);
            code_block(d, c, L, a, e, lang);
            i = e < L.n ? e + 1 : e; continue;
        }
        if (indent_of(s) >= 4) {   /* indented code */
            int a = i, e = i;
            while (e < L.n && (indent_of(L.s[e]) >= 4 || blank(L.s[e]))) e++;
            while (e > a && blank(L.s[e - 1])) e--;
            for (int q = a; q < e; q++) { int ci = indent_of(L.s[q]); if (ci > 4) ci = 4; memmove(L.s[q], L.s[q] + ci, strlen(L.s[q] + ci) + 1); }
            ctx_gap(d, c, L.src[i]);
            code_block(d, c, L, a, e, NULL);
            i = e; continue;
        }
        if ((h = heading_of(s))) {
            const char *p = s + indent_of(s) + h; while (*p == ' ') p++;
            int n = (int)strlen(p); while (n && (p[n - 1] == ' ' || p[n - 1] == '\r')) n--;
            int m = n; while (m && p[m - 1] == '#') m--; if (m < n && (m == 0 || p[m - 1] == ' ')) { n = m; while (n && p[n - 1] == ' ') n--; }
            static const unsigned char col[] = {0, C_BMAGENTA, C_BCYAN, C_BYELLOW, C_BGREEN, C_BBLUE, C_GRAY};
            Sty st = {col[h], 0, (unsigned char)(A_B | (h == 1 ? A_U : 0)), 0};
            ctx_gap(d, c, L.src[i]);
            GBuf b = {0};
            if (h >= 3) { char pre[8]; snprintf(pre, sizeof(pre), "%.*s ", h, "######"); Sty ps = {C_GRAY, 0, 0, 0}; gb_str(&b, pre, ps); }
            inl(d, &b, p, n, st);
            emit_wrapped(d, c, &b, L.src[i], c->width);
            free(b.g);
            if (h <= 2) { GBuf *l = ctx_line(d, c, L.src[i]); Sty rs = {(unsigned char)(h == 1 ? col[1] : C_GRAY), 0, 0, 0}; for (int q = 0; q < c->width; q++) gb_str(l, h == 1 ? "\xe2\x94\x81" : "\xe2\x94\x80", rs); }
            i++; continue;
        }
        if (hr_of(s)) {
            ctx_gap(d, c, L.src[i]);
            GBuf *l = ctx_line(d, c, L.src[i]); Sty rs = {C_GRAY, 0, 0, 0};
            for (int q = 0; q < c->width; q++) gb_str(l, "\xe2\x94\x80", rs);
            i++; continue;
        }
        if (s[indent_of(s)] == '>') {   /* block quote, with lazy continuation lines */
            Lines q = {xrealloc(NULL, sizeof(char *) * (size_t)L.n), xrealloc(NULL, sizeof(int) * (size_t)L.n), 0};
            int lazy = 0;
            while (i < L.n) {
                const char *t = L.s[i]; int in = indent_of(t);
                if (t[in] == '>') { t += in + 1; if (*t == ' ') t++; lazy = !blank(t); }
                else if (lazy && !blank(t) && !starts_block(t)) {}
                else break;
                q.s[q.n] = strdup(t); q.src[q.n] = L.src[i]; q.n++; i++;
            }
            ctx_gap(d, c, q.src[0]);
            Ctx child; sub_ctx(c, &child, "\xe2\x94\x82 ", (Sty){C_GRAY, 0, 0, 0}, 2);
            child.cont.n -= 2; gb_str(&child.cont, "\xe2\x94\x82 ", (Sty){C_GRAY, 0, 0, 0});
            render_blocks(d, &child, q);
            ctx_done(c, &child);
            for (int k = 0; k < q.n; k++) free(q.s[k]);
            free(q.s); free(q.src);
            continue;
        }
        int off = list_of(s, &ordered, &num, &bul);
        if (off) {   /* a list: items of the same kind at this indentation */
            int base = indent_of(s), loose = 0, count = 0;
            long n0 = num;
            ctx_gap(d, c, L.src[i]);
            int save_blocks = c->blocks;
            while (i < L.n) {
                int o2; long nm; char b2;
                int off2 = list_of(L.s[i], &o2, &nm, &b2);
                if (!off2 || o2 != ordered || b2 != bul || indent_of(L.s[i]) > base + 1) break;
                Lines it = {xrealloc(NULL, sizeof(char *) * (size_t)(L.n - i)), xrealloc(NULL, sizeof(int) * (size_t)(L.n - i)), 0};
                const char *first = L.s[i] + off2;
                if (off2 > (int)strlen(L.s[i])) first = "";
                it.s[it.n] = strdup(first); it.src[it.n++] = L.src[i]; i++;
                int content = off2, gap = 0;
                while (i < L.n) {
                    const char *t = L.s[i];
                    if (blank(t)) { gap = 1; it.s[it.n] = strdup(""); it.src[it.n++] = L.src[i]; i++; continue; }
                    int ti = indent_of(t), o4; long n4; char b4;
                    if (ti < content && list_of(t, &o4, &n4, &b4)) break;   /* the next item */
                    if (ti >= content) { it.s[it.n] = strdup(t + content); it.src[it.n++] = L.src[i]; i++; gap = 0; continue; }
                    if (!gap && !starts_block(t)) { it.s[it.n] = strdup(t + ti); it.src[it.n++] = L.src[i]; i++; continue; }   /* lazy */
                    break;
                }
                while (it.n > 1 && blank(it.s[it.n - 1])) { free(it.s[--it.n]); }
                int inner = 0;   /* a blank line between two blocks of this item: loose */
                for (int k = 1; k + 1 < it.n; k++) if (blank(it.s[k]) && !blank(it.s[k + 1])) inner = 1;
                if (inner) loose = 1;
                if (count && loose) { GBuf *l = ctx_line(d, c, L.src[i - 1]); while (l->n && l->g[l->n - 1].s[0] == ' ') l->n--; }
                char mark[32]; Sty ms = {C_GRAY, 0, 0, 0};
                const char *body = it.s[0];
                char task = 0;
                if ((body[0] == '[') && (body[1] == ' ' || body[1] == 'x' || body[1] == 'X') && body[2] == ']' && (body[3] == ' ' || !body[3])) {
                    task = body[1] == ' ' ? ' ' : 'x';
                    memmove(it.s[0], it.s[0] + (body[3] ? 4 : 3), strlen(it.s[0] + (body[3] ? 4 : 3)) + 1);
                }
                if (ordered) snprintf(mark, sizeof(mark), "%ld%c ", n0 + count, bul);
                else snprintf(mark, sizeof(mark), "%s", bullets[depth % 3]);
                if (task) { snprintf(mark + strlen(mark), sizeof(mark) - strlen(mark), "%s ", task == 'x' ? "\xe2\x98\x91" : "\xe2\x98\x90"); }
                int mw = 0; { GBuf t = {0}; gb_str(&t, mark, ms); mw = gb_width(&t, 0, t.n); free(t.g); }
                Ctx child; sub_ctx(c, &child, mark, ordered ? (Sty){C_GRAY, 0, A_B, 0} : (Sty){C_BCYAN, 0, 0, 0}, mw);
                child.tight = !loose;
                depth++;
                render_blocks(d, &child, it);
                depth--;
                if (child.first_pending) { GBuf *l = ctx_line(d, &child, it.src[0]); (void)l; }
                ctx_done(c, &child);
                for (int k = 0; k < it.n; k++) free(it.s[k]);
                free(it.s); free(it.src);
                count++;
                if (i < L.n && blank(L.s[i])) {
                    int j = i; while (j < L.n && blank(L.s[j])) j++;
                    int o3; long n3; char b3;
                    if (j < L.n && list_of(L.s[j], &o3, &n3, &b3) && o3 == ordered && b3 == bul && indent_of(L.s[j]) <= base + 1) { loose = 1; i = j; }
                }
            }
            c->blocks = save_blocks;
            continue;
        }
        if (strchr(s, '|') && i + 1 < L.n && table_delim(L.s[i + 1]) && strchr(L.s[i + 1], '-')) {
            int e = i + 2; while (e < L.n && !blank(L.s[e]) && strchr(L.s[e], '|')) e++;
            ctx_gap(d, c, L.src[i]);
            table_block(d, c, L, i, e);
            i = e; continue;
        }
        /* a paragraph, or a setext heading */
        int a = i, e = i + 1;
        while (e < L.n && !blank(L.s[e]) && !starts_block(L.s[e]) && !(strchr(L.s[e], '|') && e + 1 < L.n && table_delim(L.s[e + 1]) && strchr(L.s[e], '|'))) {
            const char *t = L.s[e] + indent_of(L.s[e]);
            if (indent_of(L.s[e]) <= 3 && (t[0] == '=' || t[0] == '-')) {
                int k = 0; while (t[k] == t[0]) k++; while (t[k] == ' ' || t[k] == '\r') k++;
                if (!t[k]) break;
            }
            e++;
        }
        if (e < L.n && indent_of(L.s[e]) <= 3 && (L.s[e][indent_of(L.s[e])] == '=' || L.s[e][indent_of(L.s[e])] == '-')) {
            const char *t = L.s[e] + indent_of(L.s[e]); int k = 0; while (t[k] == t[0]) k++; while (t[k] == ' ' || t[k] == '\r') k++;
            if (!t[k]) {
                int h1 = t[0] == '=';
                Sty st = {(unsigned char)(h1 ? C_BMAGENTA : C_BCYAN), 0, (unsigned char)(A_B | (h1 ? A_U : 0)), 0};
                ctx_gap(d, c, L.src[a]);
                para(d, c, L, a, e, st);
                GBuf *l = ctx_line(d, c, L.src[e]); Sty rs = {(unsigned char)(h1 ? C_BMAGENTA : C_GRAY), 0, 0, 0};
                for (int q = 0; q < c->width; q++) gb_str(l, h1 ? "\xe2\x94\x81" : "\xe2\x94\x80", rs);
                i = e + 1; continue;
            }
        }
        ctx_gap(d, c, L.src[a]);
        para(d, c, L, a, e, (Sty){0});
        i = e;
    }
}

/* ── documents ────────────────────────────────────────────────────── */

static char *g_text; static size_t g_text_n;   /* the source as read */

static Lines split_lines(const char *t, size_t n) {
    Lines L = {0};
    int cap = 64;
    L.s = xrealloc(NULL, sizeof(char *) * (size_t)cap); L.src = xrealloc(NULL, sizeof(int) * (size_t)cap);
    size_t a = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && t[i] != '\n') continue;
        if (i == n && a == n) break;
        if (L.n == cap) { cap *= 2; L.s = xrealloc(L.s, sizeof(char *) * (size_t)cap); L.src = xrealloc(L.src, sizeof(int) * (size_t)cap); }
        /* leading tabs become 4 spaces each, so indentation counts as columns */
        size_t len = i - a, tabs = 0; for (size_t k = a; k < i && t[k] == '\t'; k++) tabs++;
        char *s = xrealloc(NULL, len + tabs * 3 + 1), *o = s;
        for (size_t k = a; k < a + tabs; k++) { memcpy(o, "    ", 4); o += 4; }
        memcpy(o, t + a + tabs, len - tabs); o[len - tabs] = 0;
        if (len && o[len - tabs - 1] == '\r') o[len - tabs - 1] = 0;
        L.s[L.n] = s; L.src[L.n] = L.n + 1; L.n++;
        a = i + 1;
    }
    return L;
}
static void lines_free(Lines *L) { for (int i = 0; i < L->n; i++) free(L->s[i]); free(L->s); free(L->src); memset(L, 0, sizeof(*L)); }

static void render_md(Doc *d, int width) {
    Lines L = split_lines(g_text, g_text_n);
    for (int i = 0; i < g_nrefs; i++) { free(g_refs[i].label); free(g_refs[i].url); }
    free(g_refs); g_refs = NULL; g_nrefs = 0;
    int fence = 0;
    for (int i = 0; i < L.n; i++) {   /* reference definitions: [label]: url */
        char fc; if (fence_of(L.s[i], &fc)) fence = !fence;
        const char *s = L.s[i] + indent_of(L.s[i]);
        if (fence || indent_of(L.s[i]) > 3 || s[0] != '[' || s[1] == '^') continue;
        const char *e = strstr(s, "]:"); if (!e || e == s + 1) continue;
        const char *u = e + 2; while (*u == ' ') u++;
        if (!*u) continue;
        int un = 0; if (*u == '<') { u++; while (u[un] && u[un] != '>') un++; } else while (u[un] && u[un] != ' ') un++;
        g_refs = xrealloc(g_refs, sizeof(Ref) * (size_t)(g_nrefs + 1));
        g_refs[g_nrefs].label = xstrndup(s + 1, (size_t)(e - s - 1)); g_refs[g_nrefs].url = xstrndup(u, (size_t)un); g_nrefs++;
        L.s[i][0] = 0;
    }
    Ctx c; memset(&c, 0, sizeof(c)); c.width = width; c.first_pending = 1;
    render_blocks(d, &c, L);
    free(c.first.g); free(c.cont.g);
    lines_free(&L);
}

static int g_gutter;   /* plain mode: digits in the line-number column */
static void render_plain(Doc *d, int width) {
    Lines L = {0};
    {   /* plain mode keeps tabs as they are; gb_text expands them per line */
        size_t a = 0; int cap = 64;
        L.s = xrealloc(NULL, sizeof(char *) * (size_t)cap); L.src = xrealloc(NULL, sizeof(int) * (size_t)cap);
        for (size_t i = 0; i <= g_text_n; i++) {
            if (i < g_text_n && g_text[i] != '\n') continue;
            if (i == g_text_n && a == g_text_n) break;
            if (L.n == cap) { cap *= 2; L.s = xrealloc(L.s, sizeof(char *) * (size_t)cap); L.src = xrealloc(L.src, sizeof(int) * (size_t)cap); }
            size_t len = i - a; if (len && g_text[a + len - 1] == '\r') len--;
            L.s[L.n] = xstrndup(g_text + a, len); L.src[L.n] = L.n + 1; L.n++;
            a = i + 1;
        }
    }
    g_gutter = 1; for (int v = L.n; v >= 10; v /= 10) g_gutter++;
    int w = width - g_gutter - 3; if (w < 8) w = 8;
    Sty gs = {C_GRAY, 0, 0, 0};
    for (int i = 0; i < L.n; i++) {
        GBuf t = {0};
        gb_text(&t, L.s[i], (int)strlen(L.s[i]), (Sty){0});
        int at = 0;
        do {
            GBuf *l = doc_line(d, L.src[i]);
            char num[32];
            if (!at) snprintf(num, sizeof(num), "%*d \xe2\x94\x82 ", g_gutter, L.src[i]);
            else snprintf(num, sizeof(num), "%*s \xe2\x94\x8a ", g_gutter, "");
            gb_str(l, num, gs);
            int col = 0;
            while (at < t.n && col + t.g[at].w <= w) { col += t.g[at].w; gb_push(l, t.g[at++]); }
            if (at < t.n && col == 0) gb_push(l, t.g[at++]);
        } while (at < t.n);
        free(t.g);
    }
    lines_free(&L);
}

/* ── terminal output ──────────────────────────────────────────────── */

typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->p = xrealloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n); b->n += n; b->p[b->n] = 0;
}
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bfmt(Buf *b, const char *f, ...) { char t[512]; va_list ap; va_start(ap, f); int n = vsnprintf(t, sizeof(t), f, ap); va_end(ap); if (n > 0) bput(b, t, (size_t)(n < (int)sizeof(t) ? n : (int)sizeof(t) - 1)); }

static int g_codebg = 236;    /* 256-color index for code; 254 on light backgrounds */

static void sgr(Buf *o, Sty s) {
    bstr(o, "\x1b[0");
    if (s.at & A_B) bstr(o, ";1");
    if (s.at & A_D) bstr(o, ";2");
    if (s.at & A_I) bstr(o, ";3");
    if (s.at & A_U) bstr(o, ";4");
    if (s.at & A_R) bstr(o, ";7");
    if (s.at & A_S) bstr(o, ";9");
    if (s.fg) bfmt(o, ";%d", s.fg <= 8 ? 29 + s.fg : 81 + s.fg);
    if (s.bg == 1) bfmt(o, ";48;5;%d", g_codebg);
    bstr(o, "m");
}
/* One rendered line: at most cols columns, styles and hyperlinks as escapes
   when ansi, else bare text. hl: glyph ranges [a,b) to show reversed. */
static void line_out(Buf *o, const Doc *d, const GBuf *l, int cols, int ansi, const int *hl, int nhl) {
    Sty cur = {0}; int col = 0, link = 0, styled = 0;
    for (int i = 0; i < l->n; i++) {
        const G *g = &l->g[i];
        if (g->w == 0) continue;
        if (col + g->w > cols) break;
        if (ansi) {
            Sty s = g->st;
            for (int h = 0; h < nhl; h++) if (i >= hl[2 * h] && i < hl[2 * h + 1]) s.at |= A_R;
            if (s.link != link) {
                if (link) bstr(o, "\x1b]8;;\x1b\\");
                link = s.link;
                if (link && link <= d->nlink) { bstr(o, "\x1b]8;;"); bstr(o, d->link[link - 1]); bstr(o, "\x1b\\"); }
            }
            if (memcmp(&s, &cur, 3) || !styled) { if (styled || s.fg || s.bg || s.at) sgr(o, s); cur = s; styled = 1; }
        }
        bput(o, g->s, g->sl);
        col += g->w;
    }
    if (ansi) { if (link) bstr(o, "\x1b]8;;\x1b\\"); if (cur.fg || cur.bg || cur.at) bstr(o, "\x1b[0m"); }
}

/* ── the viewer ───────────────────────────────────────────────────── */

static const char *g_path;          /* NULL: stdin */
static int g_plain, g_jump, g_mouse = 1;
static Doc g_doc;
static int g_cols = 80, g_rows = 24, g_top, g_margin;
static char g_query[256], g_msg[256];
static int *g_match, g_nmatch, g_cur = -1;    /* (line, start glyph, end glyph) triples */
static volatile sig_atomic_t g_winch;
static int g_wake[2] = {-1, -1}, g_tty = -1;
static struct termios g_saved;
static struct stat g_stat;

static int md_width(void) {
    int w = g_cols - 2 * g_margin;
    if (!g_plain && w > 100) w = 100;
    return w < 10 ? 10 : w;
}
static int load(void) {
    free(g_text); g_text = NULL; g_text_n = 0;
    int fd = g_path ? open(g_path, O_RDONLY) : 0;
    if (fd < 0) { snprintf(g_msg, sizeof(g_msg), "%s: %s", g_path, strerror(errno)); return 0; }
    size_t cap = 65536; g_text = xrealloc(NULL, cap);
    for (;;) {
        if (g_text_n + 4096 > cap) { cap *= 2; g_text = xrealloc(g_text, cap); }
        ssize_t r = read(fd, g_text + g_text_n, cap - g_text_n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        g_text_n += (size_t)r;
    }
    if (g_path) { fstat(fd, &g_stat); close(fd); }
    if (memchr(g_text, 0, g_text_n < 8192 ? g_text_n : 8192)) {
        char msg[256]; int n = snprintf(msg, sizeof(msg), "(binary file, %zu bytes)\n", g_text_n);
        memcpy(g_text, msg, (size_t)n); g_text_n = (size_t)n;
    }
    return 1;
}
static void build(void) {
    doc_free(&g_doc);
    if (g_plain) render_plain(&g_doc, g_cols - g_margin); else render_md(&g_doc, md_width());
    if (!g_doc.n) doc_line(&g_doc, 1);
}
static int body_rows(void) { return g_rows > 1 ? g_rows - 1 : 1; }
static int max_top(void) { int m = g_doc.n - body_rows(); return m > 0 ? m : 0; }
static void clamp_top(void) { if (g_top > max_top()) g_top = max_top(); if (g_top < 0) g_top = 0; }
static int line_for_src(int src) { for (int i = 0; i < g_doc.n; i++) if (g_doc.line[i].src >= src) return i; return g_doc.n - 1; }

static void plain_of(const GBuf *l, char *out, int cap, int *off) {
    int k = 0;
    for (int i = 0; i < l->n; i++) {
        off[i] = k;
        for (int j = 0; j < l->g[i].sl && k < cap - 1; j++) out[k++] = l->g[i].s[j];
    }
    off[l->n] = k; out[k] = 0;
}
static void search(void) {
    free(g_match); g_match = NULL; g_nmatch = 0; g_cur = -1;
    if (!g_query[0]) return;
    int fold = 1; for (const char *p = g_query; *p; p++) if (isupper((unsigned char)*p)) fold = 0;
    int cap = 0, ql = (int)strlen(g_query);
    for (int li = 0; li < g_doc.n; li++) {
        const GBuf *l = &g_doc.line[li];
        int start = g_plain ? 0 : 0;
        char *txt = xrealloc(NULL, (size_t)l->n * 16 + 1); int *off = xrealloc(NULL, sizeof(int) * (size_t)(l->n + 1));
        plain_of(l, txt, l->n * 16 + 1, off);
        if (g_plain) { for (int i = 0; i < l->n; i++) if (l->g[i].st.fg != C_GRAY) { start = off[i]; break; } }
        for (int at = start; txt[at]; at++) {
            int k = 0; while (k < ql && txt[at + k] && (fold ? tolower((unsigned char)txt[at + k]) == tolower((unsigned char)g_query[k]) : txt[at + k] == g_query[k])) k++;
            if (k < ql) continue;
            int a = 0, b = 0; while (a < l->n && off[a] < at) a++; b = a; while (b < l->n && off[b] < at + ql) b++;
            if (g_nmatch == cap) { cap = cap ? cap * 2 : 64; g_match = xrealloc(g_match, sizeof(int) * 3 * (size_t)cap); }
            g_match[3 * g_nmatch] = li; g_match[3 * g_nmatch + 1] = a; g_match[3 * g_nmatch + 2] = b; g_nmatch++;
            at += ql - 1;
        }
        free(txt); free(off);
    }
}
static void goto_match(int dir) {
    if (!g_nmatch) { snprintf(g_msg, sizeof(g_msg), "not found: %s", g_query); return; }
    if (g_cur < 0) {
        g_cur = 0; while (g_cur < g_nmatch && g_match[3 * g_cur] < g_top) g_cur++;
        if (g_cur >= g_nmatch) g_cur = 0;
        if (dir < 0) g_cur = (g_cur + g_nmatch - 1) % g_nmatch;
    } else g_cur = (g_cur + (dir < 0 ? g_nmatch - 1 : 1)) % g_nmatch;
    int line = g_match[3 * g_cur];
    if (line < g_top || line >= g_top + body_rows()) g_top = line - body_rows() / 3;
    clamp_top();
    snprintf(g_msg, sizeof(g_msg), "%d/%d", g_cur + 1, g_nmatch);
}

static void wout(const char *s, size_t n) { while (n) { ssize_t w = write(1, s, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return; s += w; n -= (size_t)w; } }

static void draw(int prompt) {
    Buf o = {0};
    bstr(&o, "\x1b[?2026h\x1b[H");
    for (int y = 0; y < body_rows(); y++) {
        int li = g_top + y;
        bfmt(&o, "\x1b[%d;1H\x1b[0m\x1b[2K", y + 1);
        if (li >= g_doc.n) { bstr(&o, "\x1b[90m~\x1b[0m"); continue; }
        for (int m = 0; m < g_margin; m++) bstr(&o, " ");
        int hl[64], nhl = 0;
        for (int k = 0; k < g_nmatch && nhl < 32; k++) if (g_match[3 * k] == li) { hl[2 * nhl] = g_match[3 * k + 1]; hl[2 * nhl + 1] = g_match[3 * k + 2]; nhl++; }
        GBuf *l = &g_doc.line[li];
        int jump = g_plain && g_jump && l->src == g_jump && (li == 0 || g_doc.line[li - 1].src != g_jump);
        if (jump) { for (int i = 0; i < l->n && l->g[i].st.fg == C_GRAY; i++) l->g[i].st.at |= A_R | A_B; }
        line_out(&o, &g_doc, l, g_cols - g_margin, 1, hl, nhl);
        if (jump) { for (int i = 0; i < l->n && l->g[i].st.fg == C_GRAY; i++) l->g[i].st.at &= (unsigned char)~(A_R | A_B); }
    }
    /* status line */
    bfmt(&o, "\x1b[%d;1H\x1b[0m\x1b[2K\x1b[7m", g_rows);
    char left[600], right[128];
    const char *name = g_path ? strrchr(g_path, '/') ? strrchr(g_path, '/') + 1 : g_path : "(stdin)";
    if (prompt) snprintf(left, sizeof(left), "/%s", g_query);
    else snprintf(left, sizeof(left), " %s%s%s", name, g_msg[0] ? "  \xc2\xb7  " : "", g_msg);
    int pct = g_doc.n <= body_rows() ? 100 : (int)((long)(g_top + body_rows()) * 100 / g_doc.n);
    snprintf(right, sizeof(right), "%s %d%%  q quit \xc2\xb7 / search ", g_top + body_rows() >= g_doc.n ? "end" : "", pct);
    GBuf lb = {0}, rb = {0}; gb_str(&lb, left, (Sty){0}); gb_str(&rb, right, (Sty){0});
    int lw = gb_width(&lb, 0, lb.n), rw = gb_width(&rb, 0, rb.n), col = 0;
    for (int i = 0; i < lb.n && col + lb.g[i].w <= g_cols; i++) { bput(&o, lb.g[i].s, lb.g[i].sl); col += lb.g[i].w; }
    if (lw + rw + 1 <= g_cols) { for (; col < g_cols - rw; col++) bstr(&o, " "); bstr(&o, right); }
    else for (; col < g_cols; col++) bstr(&o, " ");
    bstr(&o, "\x1b[0m");
    if (prompt) bfmt(&o, "\x1b[%d;%dH\x1b[?25h", g_rows, lw + 1 > g_cols ? g_cols : lw + 1);
    else bstr(&o, "\x1b[?25l");
    bstr(&o, "\x1b[?2026l");
    wout(o.p, o.n);
    free(o.p); free(lb.g); free(rb.g);
}

static void winsize(void) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) { g_cols = ws.ws_col; g_rows = ws.ws_row; }
}
static void on_winch(int s) { (void)s; g_winch = 1; if (g_wake[1] >= 0) { char c = 1; ssize_t r = write(g_wake[1], &c, 1); (void)r; } }
static void restore(void) {
    wout("\x1b[?1000l\x1b[?1006l\x1b[?25h\x1b[?1049l", strlen("\x1b[?1000l\x1b[?1006l\x1b[?25h\x1b[?1049l"));
    if (g_tty >= 0) tcsetattr(g_tty, TCSAFLUSH, &g_saved);
}
static void on_term(int s) { restore(); signal(s, SIG_DFL); raise(s); }

/* Reads one key: a byte, or an escape sequence decoded to a code below.
   Returns -1 when nothing arrived within ms. */
enum { K_UP = 1000, K_DOWN, K_PGUP, K_PGDN, K_HOME, K_END, K_WHEEL_UP, K_WHEEL_DOWN, K_RESIZE, K_ESC };
/* select(), not poll(): macOS poll() does not work on terminal devices */
static int ready(int fd, int fd2, int ms, int *which) {
    fd_set r; FD_ZERO(&r); FD_SET(fd, &r); if (fd2 >= 0) FD_SET(fd2, &r);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    int n = select((fd > fd2 ? fd : fd2) + 1, &r, NULL, NULL, ms < 0 ? NULL : &tv);
    if (n > 0 && which) *which = fd2 >= 0 && FD_ISSET(fd2, &r) ? 2 : 1;
    return n;
}
static int rd(unsigned char *c, int ms) {
    if (ready(g_tty, -1, ms, NULL) <= 0) return 0;
    return read(g_tty, c, 1) == 1;
}
static int key(int ms) {
    int which = 0, r = ready(g_tty, g_wake[0], ms, &which);
    if (r < 0 && errno == EINTR) return g_winch ? K_RESIZE : -1;
    if (r <= 0) return -1;
    if (which == 2) { char t[16]; ssize_t n = read(g_wake[0], t, sizeof(t)); (void)n; return K_RESIZE; }
    unsigned char c;
    if (read(g_tty, &c, 1) != 1) return 'q';
    if (c != 0x1b) return c;
    unsigned char a, b;
    if (!rd(&a, 30)) return K_ESC;
    if (a != '[' && a != 'O') return K_ESC;
    if (!rd(&b, 30)) return K_ESC;
    if (b == '<') {   /* SGR mouse: ESC [ < btn ; x ; y M/m */
        char t[32]; int n = 0; unsigned char ch;
        while (n < 31 && rd(&ch, 30)) { t[n++] = (char)ch; if (ch == 'M' || ch == 'm') break; }
        t[n] = 0;
        int btn = atoi(t);
        return btn == 64 ? K_WHEEL_UP : btn == 65 ? K_WHEEL_DOWN : -1;
    }
    if (b == 'A') return K_UP;
    if (b == 'B') return K_DOWN;
    if (b == 'H') return K_HOME;
    if (b == 'F') return K_END;
    if (isdigit(b)) {
        int v = b - '0'; unsigned char ch = 0;
        while (rd(&ch, 30) && isdigit(ch)) v = v * 10 + ch - '0';
        if (ch != '~') return -1;
        return v == 5 ? K_PGUP : v == 6 ? K_PGDN : v == 1 || v == 7 ? K_HOME : v == 4 || v == 8 ? K_END : -1;
    }
    return -1;
}

/* Background brightness, asked with OSC 11: light themes get a light code
   background. Terminals that don't answer keep the dark default. */
static void probe_background(void) {
    wout("\x1b]11;?\x1b\\", 8);
    char t[64]; int n = 0; unsigned char c;
    while (n < 63 && rd(&c, n ? 50 : 150)) { t[n++] = (char)c; if (c == '\\' || c == 7) break; }
    t[n] = 0;
    char *p = strstr(t, "rgb:");
    if (!p) return;
    unsigned r = 0, g = 0, b = 0;
    if (sscanf(p + 4, "%x/%x/%x", &r, &g, &b) != 3) return;
    int span = strchr(p + 4, '/') - (p + 4) > 2 ? 65535 : 255;
    if ((299 * r + 587 * g + 114 * b) / 1000 > (unsigned)span / 2) g_codebg = 254;
}

static void rebuild_keep_place(void) {
    int src = g_top < g_doc.n ? g_doc.line[g_top].src : 1;
    build();
    g_top = line_for_src(src);
    search(); g_cur = -1;
    clamp_top();
}

static int run_viewer(void) {
    g_tty = open("/dev/tty", O_RDWR);
    if (g_tty < 0) { perror("hcmd: /dev/tty"); return 1; }
    tcgetattr(g_tty, &g_saved);
    struct termios raw = g_saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    tcsetattr(g_tty, TCSAFLUSH, &raw);
    if (pipe(g_wake) == 0) { fcntl(g_wake[0], F_SETFL, O_NONBLOCK); fcntl(g_wake[1], F_SETFL, O_NONBLOCK); }
    signal(SIGWINCH, on_winch); signal(SIGTERM, on_term); signal(SIGHUP, on_term); signal(SIGINT, on_term);
    wout("\x1b[?1049h\x1b[?25l", 14);
    if (g_mouse) wout("\x1b[?1000h\x1b[?1006h", 16);
    probe_background();
    if (g_path) printf("\x1b]0;%s\x07", strrchr(g_path, '/') ? strrchr(g_path, '/') + 1 : g_path), fflush(stdout);
    winsize(); build();
    if (g_jump) { g_top = line_for_src(g_jump) - body_rows() / 3; clamp_top(); }
    int prompt = 0;
    char saved_query[256] = "";
    draw(0);
    for (;;) {
        int k = key(400);
        if (k == -1 || k == K_RESIZE) {
            int changed = 0;
            if (g_winch || k == K_RESIZE) { g_winch = 0; int oc = g_cols, orr = g_rows; winsize(); if (oc != g_cols || orr != g_rows) { rebuild_keep_place(); changed = 1; } }
            if (g_path && k == -1) {   /* live reload */
                struct stat st;
                if (stat(g_path, &st) == 0 && (st.st_mtime != g_stat.st_mtime || st.st_size != g_stat.st_size ||
#ifdef __APPLE__
                    st.st_mtimespec.tv_nsec != g_stat.st_mtimespec.tv_nsec || st.st_ino != g_stat.st_ino)) {
#else
                    st.st_mtim.tv_nsec != g_stat.st_mtim.tv_nsec || st.st_ino != g_stat.st_ino)) {
#endif
                    if (load()) { rebuild_keep_place(); snprintf(g_msg, sizeof(g_msg), "reloaded"); changed = 1; }
                }
            }
            if (changed) draw(prompt);
            continue;
        }
        if (prompt) {
            size_t ql = strlen(g_query);
            if (k == '\r' || k == '\n') { prompt = 0; search(); goto_match(1); }
            else if (k == K_ESC || k == 3 || k == 7) { prompt = 0; snprintf(g_query, sizeof(g_query), "%s", saved_query); g_msg[0] = 0; }
            else if (k == 127 || k == 8) { if (ql) { do ql--; while (ql && ((unsigned char)g_query[ql] & 0xC0) == 0x80); g_query[ql] = 0; } else prompt = 0; }
            else if (k >= 32 && k < 256 && ql < sizeof(g_query) - 1) { g_query[ql] = (char)k; g_query[ql + 1] = 0; }
            draw(prompt);
            continue;
        }
        int page = body_rows() - 1 > 0 ? body_rows() - 1 : 1;
        g_msg[0] = 0;
        switch (k) {
        case 'q': case 'Q': case K_ESC: case 3: restore(); return 0;
        case 'j': case K_DOWN: case '\r': case 'e': case 5: g_top++; break;
        case 'k': case K_UP: case 'y': case 25: g_top--; break;
        case K_WHEEL_DOWN: g_top += 3; break;
        case K_WHEEL_UP: g_top -= 3; break;
        case ' ': case 'f': case K_PGDN: case 6: g_top += page; break;
        case 'b': case K_PGUP: case 2: g_top -= page; break;
        case 'd': case 4: g_top += body_rows() / 2; break;
        case 'u': case 21: g_top -= body_rows() / 2; break;
        case 'g': case '<': case K_HOME: g_top = 0; break;
        case 'G': case '>': case K_END: g_top = max_top(); break;
        case '/': snprintf(saved_query, sizeof(saved_query), "%s", g_query); g_query[0] = 0; prompt = 1; break;
        case 'n': goto_match(1); break;
        case 'N': goto_match(-1); break;
        case 'r': case 12: if (g_path && load()) rebuild_keep_place(); break;
        default: break;
        }
        clamp_top();
        draw(prompt);
    }
}

static void usage(void) {
    fputs("usage: hcmd [-p] [+LINE] FILE   (FILE - reads stdin)\n"
          "  -p           plain text with line numbers (default for non-Markdown files)\n"
          "  -m           Markdown even for other extensions\n"
          "  +LINE        open at LINE (plain mode marks it)\n"
          "  -M           don't capture the mouse (no wheel scrolling)\n"
          "  --dump=WxH   print the first H rendered lines at width W, plain\n"
          "  --dump-ansi=WxH  the same with styles and OSC 8 links\n"
          "keys: j/k arrows, space/b pages, d/u half pages, g/G ends, / search, n/N, r reload, q quit\n", stderr);
}

static int is_md(const char *p) {
    const char *dot = strrchr(p, '.');
    if (!dot || strchr(dot, '/')) return 0;
    static const char *ext[] = {".md", ".markdown", ".mdown", ".mkd", ".mkdn", ".mdx", ".mdwn", ".ronn"};
    for (size_t i = 0; i < sizeof(ext) / sizeof(*ext); i++) if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

int main(int argc, char **argv) {
    int dump_w = 0, dump_h = 0, dump_ansi = 0, force_md = 0, force_plain = 0;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-p") || !strcmp(a, "--plain")) force_plain = 1;
        else if (!strcmp(a, "-m") || !strcmp(a, "--markdown")) force_md = 1;
        else if (!strcmp(a, "-M") || !strcmp(a, "--no-mouse")) g_mouse = 0;
        else if (a[0] == '+' && isdigit((unsigned char)a[1])) g_jump = atoi(a + 1);
        else if (!strncmp(a, "--line=", 7)) g_jump = atoi(a + 7);
        else if (!strncmp(a, "--dump=", 7) || !strncmp(a, "--dump-ansi=", 12)) {
            dump_ansi = a[6] == '-';
            if (sscanf(strchr(a, '=') + 1, "%dx%d", &dump_w, &dump_h) != 2 || dump_w < 10 || dump_h < 1) { usage(); return 2; }
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--")) { if (i + 1 < argc) file = argv[++i]; }
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "hcmd: unknown option %s\n", a); usage(); return 2; }
        else file = a;
    }
    if (!file) { usage(); return 2; }
    g_path = strcmp(file, "-") ? file : NULL;
    g_plain = force_plain || (!force_md && g_path && !is_md(g_path));
    if (g_path) {
        char real[PATH_MAX];
        if (realpath(g_path, real)) { char *slash = strrchr(real, '/'); if (slash) { *slash = 0; snprintf(g_doc.base, sizeof(g_doc.base), "%s", real[0] ? real : "/"); } }
        struct stat st;
        if (stat(g_path, &st) == 0 && S_ISDIR(st.st_mode)) { fprintf(stderr, "hcmd: %s is a folder\n", g_path); return 1; }
    }
    if (!load()) { fprintf(stderr, "hcmd: %s\n", g_msg); return 1; }
    if (dump_w) {
        g_cols = dump_w; g_margin = 0;
        build();
        Buf o = {0};
        for (int i = 0; i < g_doc.n && i < dump_h; i++) {
            size_t at = o.n;
            line_out(&o, &g_doc, &g_doc.line[i], dump_w, dump_ansi, NULL, 0);
            if (!dump_ansi) { while (o.n > at && o.p[o.n - 1] == ' ') o.n--; }
            bstr(&o, "\n");
        }
        wout(o.p ? o.p : "", o.n);
        return 0;
    }
    if (!isatty(1)) {   /* piped: the document as plain text */
        const char *c = getenv("COLUMNS");
        g_cols = c && atoi(c) >= 20 ? atoi(c) : 80; g_margin = 0;
        build();
        Buf o = {0};
        for (int i = 0; i < g_doc.n; i++) { size_t at = o.n; line_out(&o, &g_doc, &g_doc.line[i], g_cols, 0, NULL, 0); while (o.n > at && o.p[o.n - 1] == ' ') o.n--; bstr(&o, "\n"); }
        wout(o.p ? o.p : "", o.n);
        return 0;
    }
    g_margin = g_plain ? 0 : 1;
    const char *term = getenv("TERM");
    if (term && !strcmp(term, "dumb")) {   /* no screen control: print and leave */
        winsize(); build();
        Buf o = {0};
        for (int i = 0; i < g_doc.n; i++) { line_out(&o, &g_doc, &g_doc.line[i], g_cols, 0, NULL, 0); bstr(&o, "\n"); }
        wout(o.p ? o.p : "", o.n);
        return 0;
    }
    return run_viewer();
}
