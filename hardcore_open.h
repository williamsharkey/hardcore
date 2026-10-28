/* File paths in terminal text, and what opens them. No platform state.
 *
 * hc_path_at finds the path under a byte offset of one line of text: absolute,
 * ~/, file://, or relative to the tab's folder, with path:line[:col] and
 * Python's `File "x.py", line 42` forms, git's a/ b/ prefixes and --opt=PATH.
 * A path may contain spaces: a quoted span ("…" or '…'), a backslash-escaped
 * space (my\ file), or the longest run of words that names an existing file.
 * Brackets are part of the name (file[1].c, file (1).txt). Only paths that
 * exist count (stat), so ordinary words never light up.
 *
 * A soft-wrapped path is physical rows joined with no extra character.
 * hc_path_wrap_parts_n splits a hit in that joined text back into row columns.
 * The caller joins the rows (hardcore_mac_open.m), at most three.
 *
 * The handler registry maps extensions to a command that runs in a hardcore
 * tab: built-in defaults, then ~/.config/hardcore/open.conf, whose lines are
 *     md markdown = hcmd %f
 *     png jpg = default
 * Keys are extensions (no dot), `text` for text files without a registered
 * extension, `/` for folders and `*` for anything else. Values are a /bin/sh
 * command with %f the file, %l its line (1 if none) and %d its folder, or
 * `default` (the system's app for it) or `files` (hardcore's file browser).
 * Later lines win, so the user's file overrides the defaults. */
#ifndef HARDCORE_OPEN_H
#define HARDCORE_OPEN_H
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    int start, end;          /* the byte span to underline, [start, end) */
    int line, col;           /* 0 = none */
    int dir;                 /* a folder */
    char path[4096];         /* absolute */
} HcPathHit;

/* A hard break ends a run. Space is a soft gap: the longest existing file
   may cross it. [] () {} stay in the name. Quotes, angles, comma and
   semicolon stay breaks, so (src/a.c:3) and File "x", line N keep working. */
static int hc_path_hard(unsigned char c) {
    return c < ' ' || c == 0x7f || strchr("\"'`<>|,;", c) != NULL;
}
static int hc_wrap_pair(char a, char b) {
    return (a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}');
}
static int hc_path_exists(const char *p, int *dir) {
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    *dir = S_ISDIR(st.st_mode);
    return 1;
}
/* "./a/../b" and "//" folded, so the hint shows what will open */
static void hc_path_clean(char *p) {
    char out[4096]; int n = 0;
    const char *s = p;
    while (*s) {
        while (*s == '/') s++;
        const char *e = s; while (*e && *e != '/') e++;
        int len = (int)(e - s);
        if (len == 1 && s[0] == '.') {}
        else if (len == 2 && s[0] == '.' && s[1] == '.') { while (n > 0 && out[n - 1] != '/') n--; if (n > 0) n--; }
        else if (len) { if (n + len + 2 >= (int)sizeof(out)) return; out[n++] = '/'; memcpy(out + n, s, (size_t)len); n += len; }
        s = e;
    }
    if (!n) out[n++] = '/';
    out[n] = 0;
    memcpy(p, out, (size_t)n + 1);
}
static int hc_path_resolve(const char *tok, int n, const char *cwd, const char *home, char *out, int cap) {
    char raw[4096];
    if (n <= 0 || n >= (int)sizeof(raw)) return 0;
    memcpy(raw, tok, (size_t)n); raw[n] = 0;
    if (!strncmp(raw, "file://", 7)) {   /* percent-decoded */
        const char *s = raw + 7; int k = 0;
        if (strncmp(s, "localhost/", 10) == 0) s += 9;
        if (*s != '/') return 0;
        for (; *s && k < cap - 1; s++) {
            if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) { char h[3] = {s[1], s[2], 0}; out[k++] = (char)strtol(h, NULL, 16); s += 2; }
            else out[k++] = *s;
        }
        out[k] = 0;
    } else if (raw[0] == '/') snprintf(out, (size_t)cap, "%s", raw);
    else if (raw[0] == '~' && (raw[1] == '/' || !raw[1])) { if (!home || !*home) return 0; snprintf(out, (size_t)cap, "%s%s", home, raw + 1); }
    else { if (!cwd || cwd[0] != '/') return 0; snprintf(out, (size_t)cap, "%s/%s", cwd, raw); }
    hc_path_clean(out);
    return 1;
}
/* Trailing :LINE or :LINE:COL (also :LINE: as grep and compilers print it). */
static int hc_path_suffix(const char *s, int n, int *line, int *col) {
    int e = n, nums[2] = {0, 0}, k = 0;
    if (e && s[e - 1] == ':') e--;
    while (k < 2) {
        int d = e; while (d > 0 && isdigit((unsigned char)s[d - 1])) d--;
        if (d == e || d == 0 || s[d - 1] != ':' || e - d > 9) break;
        nums[k++] = atoi(s + d);
        e = d - 1;
    }
    if (!k) return n && s[n - 1] == ':' ? n - 1 : n;   /* "see a.c:" */
    if (k == 2) { *line = nums[1]; *col = nums[0]; } else { *line = nums[0]; *col = 0; }
    return e;
}
static int hc_path_is_url(const char *s, int n) {
    int i = 0;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    if (i > 1 && i + 2 < n && s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/' && strncmp(s, "file://", 7)) return 1;
    return 0;
}
/* map[k] is the source offset of unescaped byte k; map[n] is one past the end. */
static int hc_unescape_map(const char *s, int n, char *out, int *map, int cap) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) c = s[++i];
        if (k + 1 >= cap) return -1;
        map[k] = i;
        out[k++] = c;
    }
    if (k >= cap) return -1;
    map[k] = n;
    out[k] = 0;
    return k;
}
/* One span [s, e) of the original line. Backslash escapes are removed for
   the stat. git's a/ b/ stays in the underline; --opt= keeps only the path. */
static int hc_path_commit(const char *text, int s, int e, int at, int extra_line,
                          const char *cwd, const char *home, HcPathHit *hit) {
    char raw[4096];
    int map[4096];
    int n = e - s;
    if (n <= 0 || n >= (int)sizeof(raw)) return 0;
    n = hc_unescape_map(text + s, n, raw, map, (int)sizeof(raw));
    if (n <= 0 || hc_path_is_url(raw, n)) return 0;
    int line = 0, col = 0, body = hc_path_suffix(raw, n, &line, &col);
    if (body <= 0) return 0;
    if (!line && extra_line) line = extra_line;
    int froms[3], offs[3], lens[3], nc = 0;
    froms[nc] = s; offs[nc] = 0; lens[nc] = body; nc++;
    if (body > 2 && (raw[0] == 'a' || raw[0] == 'b') && raw[1] == '/') {
        froms[nc] = s; offs[nc] = 2; lens[nc] = body - 2; nc++;   /* git's b/ stays underlined */
    }
    { int eq = -1; for (int i = 0; i < body; i++) if (raw[i] == '=') eq = i;
      if (eq >= 0 && eq + 1 < body) { froms[nc] = s + map[eq + 1]; offs[nc] = eq + 1; lens[nc] = body - (eq + 1); nc++; } }
    for (int i = 0; i < nc; i++) {
        int dir = 0, alnum = 0, tn = lens[i];
        const char *t = raw + offs[i];
        if (tn <= 0 || at < froms[i]) continue;          /* pointing at the --opt= part */
        for (int k = 0; k < tn; k++) if (isalnum((unsigned char)t[k])) alnum = 1;
        if (!alnum) continue;
        if (!hc_path_resolve(t, tn, cwd, home, hit->path, (int)sizeof(hit->path))) continue;
        if (!hc_path_exists(hit->path, &dir)) continue;
        hit->start = froms[i]; hit->end = e;
        hit->line = line; hit->col = col; hit->dir = dir;
        return 1;
    }
    hit->path[0] = 0;
    return 0;
}
/* The span, then the same span with one layer of wrapping () [] {} removed,
   so (src/a.c:3) hits src/a.c and file[1].c stays whole when that file exists. */
static int hc_path_try_span(const char *text, int s, int e, int at, int extra_line,
                            const char *cwd, const char *home, HcPathHit *hit) {
    for (int layer = 0; layer < 3; layer++) {
        while (e > s && strchr(".!?", text[e - 1]) && !(e - 1 > s && text[e - 2] == '\\')) e--;
        if (e <= s || at < s || at >= e) return 0;
        if (hc_path_commit(text, s, e, at, extra_line, cwd, home, hit)) return 1;
        if (e - s < 2 || !hc_wrap_pair(text[s], text[e - 1])) return 0;
        s++; e--;
    }
    return 0;
}
static int hc_tok_next(const char *s, int n, int i, int *ts, int *te) {
    while (i < n && s[i] == ' ') i++;
    if (i >= n) return 0;
    *ts = i;
    while (i < n) {
        if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
        if (s[i] == ' ') break;
        i++;
    }
    *te = i;
    return 1;
}
static int hc_span_longer(const void *A, const void *B) {
    const int *a = A, *b = B;
    return (b[1] - b[0]) - (a[1] - a[0]);
}
/* Longest existing run of tokens inside [seg_a, seg_b) that covers `at`.
   A backslash joins the next byte, including a space. At most 8 tokens
   either side of the cursor, so a hover does not stat the whole scrollback. */
static int hc_path_search(const char *text, int seg_a, int seg_b, int at, int extra_line,
                          const char *cwd, const char *home, HcPathHit *hit) {
    int ts[64], te[64], nt = 0, i = seg_a;
    while (i < seg_b) {
        int s, e, after = 0;
        if (!hc_tok_next(text, seg_b, i, &s, &e)) break;
        i = e;
        if (nt == 64) {
            memmove(ts, ts + 1, 63 * sizeof(int));
            memmove(te, te + 1, 63 * sizeof(int));
            nt--;
        }
        ts[nt] = s; te[nt] = e; nt++;
        for (int k = 0; k < nt; k++) if (te[k] > at) after++;
        if (after >= 8 && e > at) break;
    }
    if (!nt) return 0;
    int lo = -1, hi = -1;
    for (int k = 0; k < nt; k++) if (at >= ts[k] && at < te[k]) { lo = hi = k; break; }
    if (lo < 0 && at < seg_b && text[at] == ' ') {
        for (int k = 0; k < nt; k++) if (te[k] <= at) lo = k;
        for (int k = 0; k < nt; k++) if (ts[k] > at) { hi = k; break; }
    }
    if (lo < 0 || hi < 0) return 0;
    int sp[80][2], ns = 0;
    for (int a = lo; a >= 0 && lo - a < 8; a--)
        for (int b = hi; b < nt && b - hi < 8 && ns < 80; b++) {
            sp[ns][0] = ts[a]; sp[ns][1] = te[b]; ns++;
        }
    qsort(sp, (size_t)ns, sizeof(sp[0]), hc_span_longer);
    for (int k = 0; k < ns; k++)
        if (hc_path_try_span(text, sp[k][0], sp[k][1], at, extra_line, cwd, home, hit)) return 1;
    hit->path[0] = 0;
    return 0;
}
/* A quote pair that covers `at`, including the quote bytes. Double quotes
   take backslash escapes; single quotes are literal. Unclosed quotes do not. */
static int hc_quote_span(const char *text, int len, int at, int *qs, int *qe, int *body_s, int *body_e) {
    for (int i = 0; i < len; i++) {
        char q = text[i];
        if (q != '"' && q != '\'') continue;
        int j = i + 1;
        while (j < len) {
            if (q == '"' && text[j] == '\\' && j + 1 < len) { j += 2; continue; }
            if (text[j] == q) break;
            j++;
        }
        if (j >= len) return 0;
        if (at >= i && at <= j) { *qs = i; *qe = j + 1; *body_s = i + 1; *body_e = j; return 1; }
        i = j;
    }
    return 0;
}
static int hc_path_at(const char *text, int at, const char *cwd, const char *home, HcPathHit *hit) {
    int len = (int)strlen(text);
    memset(hit, 0, sizeof(*hit));
    if (at < 0 || at >= len) return 0;
    int seg_a, seg_b, extra = 0, at_use = at, qs, qe, bs, be;
    if (hc_quote_span(text, len, at, &qs, &qe, &bs, &be) && be > bs) {
        seg_a = bs; seg_b = be;
        if (qe + 7 <= len && !strncmp(text + qe, ", line ", 7)) extra = atoi(text + qe + 7);
        if (at == qs || at == qe - 1) at_use = bs;   /* the quote itself still opens the path */
    } else {
        if (hc_path_hard((unsigned char)text[at]) && text[at] != ' ') return 0;
        seg_a = at; seg_b = at + 1;
        while (seg_a > 0 && !hc_path_hard((unsigned char)text[seg_a - 1])) seg_a--;
        while (seg_b < len && !hc_path_hard((unsigned char)text[seg_b])) seg_b++;
    }
    return hc_path_search(text, seg_a, seg_b, at_use, extra, cwd, home, hit);
}

/* Soft wrap joins physical rows with no extra character. `start` and `end`
   are columns in that joined line. Writes up to `cap` parts: which row
   (0 = first row named in row_cols), and columns [c0, c1) on that row. */
static int hc_path_wrap_parts_n(const int *row_cols, int nrows, int start, int end,
                                int *line, int *c0, int *c1, int cap) {
    if (!row_cols || end <= start || nrows <= 0 || cap <= 0) return 0;
    int n = 0, base = 0;
    for (int r = 0; r < nrows; r++) {
        int width = row_cols[r] > 0 ? row_cols[r] : 0;
        int row_end = base + width;
        int a = start > base ? start : base;
        int b = end < row_end ? end : row_end;
        if (b > a && n < cap) { line[n] = r; c0[n] = a - base; c1[n] = b - base; n++; }
        base = row_end;
    }
    return n;
}

/* The default app can open at a line. Xcode's tool is xed -l. Visual Studio
   Code, its Insiders build and Cursor take vscode://file (or cursor://file)
   with :line:column. BBEdit takes x-bbedit://open. Other apps get no line. */
enum { HC_LINE_NONE, HC_LINE_XCODE, HC_LINE_VSCODE, HC_LINE_VSCODE_INSIDERS, HC_LINE_CURSOR, HC_LINE_BBEDIT };
static int hc_open_line_kind(const char *bundle_id, const char *app_name) {
    const char *b = bundle_id ? bundle_id : "", *n = app_name ? app_name : "";
    if (!strcmp(b, "com.apple.dt.Xcode") || !strcmp(n, "Xcode")) return HC_LINE_XCODE;
    if (!strcmp(b, "com.microsoft.VSCodeInsiders") || !strcmp(n, "Visual Studio Code - Insiders")) return HC_LINE_VSCODE_INSIDERS;
    if (!strcmp(b, "com.microsoft.VSCode") || !strcmp(n, "Visual Studio Code")) return HC_LINE_VSCODE;
    if (!strcmp(b, "com.barebones.bbedit") || !strcmp(n, "BBEdit")) return HC_LINE_BBEDIT;
    if (!strcmp(n, "Cursor") || !strcmp(b, "com.todesktop.230313mzl4w4u92") || !strcmp(b, "com.cursor.Cursor")) return HC_LINE_CURSOR;
    return HC_LINE_NONE;
}
static int hc_pct_byte(char *out, int cap, int k, unsigned char c) {
    static const char H[] = "0123456789ABCDEF";
    if (k + 3 >= cap) return -1;
    out[k++] = '%'; out[k++] = H[c >> 4]; out[k++] = H[c & 15];
    return k;
}
/* URL path: keep '/' and unreserved bytes. Spaces and ':' become %XX so the
   :line:column suffix stays unambiguous. */
static int hc_url_path(const char *path, char *out, int cap) {
    int k = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        unsigned char c = *p;
        int safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                   || c == '/' || c == '-' || c == '.' || c == '_' || c == '~';
        if (safe) { if (k + 1 >= cap) return 0; out[k++] = (char)c; }
        else { k = hc_pct_byte(out, cap, k, c); if (k < 0) return 0; }
    }
    if (k >= cap) return 0;
    out[k] = 0;
    return 1;
}
/* 1 and a target string. Xcode: "xed -l LINE PATH" (the caller splits it into
   argv; the path is not passed through a shell). The others: a URL.
   line < 1 or an unknown app returns 0. col < 1 opens at column 1. */
static int hc_open_line_target(int kind, const char *path, int line, int col, char *out, int cap) {
    if (line <= 0 || !path || path[0] != '/' || kind == HC_LINE_NONE || !out || cap < 16) return 0;
    if (col < 1) col = 1;
    if (kind == HC_LINE_XCODE) {
        int n = snprintf(out, (size_t)cap, "xed -l %d %s", line, path);
        return n > 0 && n < cap;
    }
    char enc[8192];
    if (!hc_url_path(path, enc, (int)sizeof(enc))) return 0;
    const char *scheme = kind == HC_LINE_VSCODE ? "vscode"
        : kind == HC_LINE_VSCODE_INSIDERS ? "vscode-insiders"
        : kind == HC_LINE_CURSOR ? "cursor" : NULL;
    if (scheme) {
        int n = snprintf(out, (size_t)cap, "%s://file%s:%d:%d", scheme, enc, line, col);
        return n > 0 && n < cap;
    }
    if (kind == HC_LINE_BBEDIT) {
        int n = snprintf(out, (size_t)cap, "x-bbedit://open?url=file://%s&line=%d&column=%d", enc, line, col);
        return n > 0 && n < cap;
    }
    return 0;
}

/* ── handlers ─────────────────────────────────────────────────────── */

enum { HC_OPEN_RULES = 256 };
typedef struct { char key[24]; char cmd[232]; } HcOpenRule;
typedef struct { HcOpenRule r[HC_OPEN_RULES]; int n; } HcOpenConf;

static void hc_open_parse(HcOpenConf *c, const char *text) {
    while (text && *text) {
        const char *e = strchr(text, '\n'); if (!e) e = text + strlen(text);
        const char *eq = memchr(text, '=', (size_t)(e - text));
        const char *s = text; while (s < e && isspace((unsigned char)*s)) s++;
        if (eq && *s != '#') {
            const char *v = eq + 1; while (v < e && isspace((unsigned char)*v)) v++;
            const char *ve = e; while (ve > v && isspace((unsigned char)ve[-1])) ve--;
            for (const char *k = s; k < eq;) {
                while (k < eq && (isspace((unsigned char)*k) || *k == ',' || *k == '.')) k++;
                const char *ke = k; while (ke < eq && !isspace((unsigned char)*ke) && *ke != ',') ke++;
                if (ke > k && c->n < HC_OPEN_RULES && ke - k < (int)sizeof(c->r[0].key) && ve - v < (int)sizeof(c->r[0].cmd)) {
                    HcOpenRule *r = &c->r[c->n++];
                    for (int i = 0; i < ke - k; i++) r->key[i] = (char)tolower((unsigned char)k[i]);
                    r->key[ke - k] = 0;
                    memcpy(r->cmd, v, (size_t)(ve - v)); r->cmd[ve - v] = 0;
                }
                k = ke;
            }
        }
        text = *e ? e + 1 : e;
    }
}
static const char hc_open_builtin[] =
    "md markdown mdown mkd mkdn mdx mdwn = hcmd %f\n"
    "txt text log c h m mm cc cpp cxx hpp hh hxx py pyi rb js mjs cjs ts tsx jsx json jsonl yaml yml toml ini cfg conf "
    "sh bash zsh fish rs go java kt kts swift lua pl pm sql csv tsv mk cmake diff patch xml css scss less "
    "metal cu cuh s asm tex rst org el clj ex exs erl hs ml cs fs vim gitignore gitattributes editorconfig "
    "env properties gradle dockerfile proto graphql ipynb lock sum mod = hcmd -p +%l %f\n"
    "text = hcmd -p +%l %f\n"
    "png jpg jpeg gif bmp tif tiff webp heic = hardcore --here-media --image %f\n"
    "/ = files\n"
    "* = default\n";
static void hc_open_defaults(HcOpenConf *c) { c->n = 0; hc_open_parse(c, hc_open_builtin); }

static const char *hc_open_find(const HcOpenConf *c, const char *key) {
    for (int i = c->n - 1; i >= 0; i--) if (!strcmp(c->r[i].key, key)) return c->r[i].cmd;
    return NULL;
}
/* What opens path: a command, "default" or "files". is_text: the file looks
   like text (no NUL in its first bytes), for names without an extension. */
static const char *hc_open_lookup(const HcOpenConf *c, const char *path, int is_dir, int is_text) {
    const char *r;
    if (is_dir) return (r = hc_open_find(c, "/")) ? r : "default";
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    char key[24];
    if (dot && dot[1] && strlen(dot + 1) < sizeof(key)) {
        int i = 0; for (const char *p = dot + 1; *p; p++) key[i++] = (char)tolower((unsigned char)*p);
        key[i] = 0;
        if ((r = hc_open_find(c, key))) return r;
    } else if (base[0] && strlen(base) < sizeof(key)) {   /* Makefile, Dockerfile, .gitignore */
        int i = 0; for (const char *p = base[0] == '.' ? base + 1 : base; *p; p++) key[i++] = (char)tolower((unsigned char)*p);
        key[i] = 0;
        if ((r = hc_open_find(c, key))) return r;
    }
    if (is_text && (!dot || !dot[1]) && (r = hc_open_find(c, "text"))) return r;
    return (r = hc_open_find(c, "*")) ? r : "default";
}
static int hc_open_in_hardcore(const char *handler) { return handler && strcmp(handler, "default") != 0; }

/* The handler as a /bin/sh script: %f %l %d become "$1" "$2" "$3", so the
   path itself is never parsed by the shell. */
static int hc_open_script(const char *handler, char *out, int cap) {
    int k = snprintf(out, (size_t)cap, "exec ");
    for (const char *p = handler; *p && k < cap - 8; p++) {
        if (p[0] == '%' && (p[1] == 'f' || p[1] == 'l' || p[1] == 'd')) {
            k += snprintf(out + k, (size_t)(cap - k), "\"$%c\"", p[1] == 'f' ? '1' : p[1] == 'l' ? '2' : '3');
            p++;
        } else out[k++] = *p;
    }
    if (k >= cap - 8) return 0;
    out[k] = 0;
    return 1;
}
#endif
