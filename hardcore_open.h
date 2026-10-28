/* File paths in terminal text, and what opens them. No platform state.
 *
 * hc_path_at finds the path under a byte offset of one line of text: absolute,
 * ~/, file://, or relative to the tab's folder, with path:line[:col] and
 * Python's `File "x.py", line 42` forms, git's a/ b/ prefixes and --opt=PATH.
 * Only paths that exist count (stat), so ordinary words never light up.
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

static int hc_path_delim(unsigned char c) {
    return c <= ' ' || strchr("\"'`<>|()[]{},;", c) != NULL;
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
static int hc_path_at(const char *text, int at, const char *cwd, const char *home, HcPathHit *hit) {
    int len = (int)strlen(text), a = at, b = at;
    memset(hit, 0, sizeof(*hit));
    if (at < 0 || at >= len || hc_path_delim((unsigned char)text[at])) return 0;
    while (a > 0 && !hc_path_delim((unsigned char)text[a - 1])) a--;
    while (b < len && !hc_path_delim((unsigned char)text[b])) b++;
    /* sentence punctuation after a path is not part of it */
    while (b > a && strchr(".!?", text[b - 1])) b--;
    if (b <= a || at >= b) return 0;
    const char *tok = text + a; int n = b - a;
    /* URLs belong to the link path, not this one */
    { int i = 0; while (i < n && (isalnum((unsigned char)tok[i]) || tok[i] == '+' || tok[i] == '-' || tok[i] == '.')) i++;
      if (i > 1 && i + 2 < n && tok[i] == ':' && tok[i + 1] == '/' && tok[i + 2] == '/' && strncmp(tok, "file://", 7)) return 0; }
    int line = 0, col = 0, body = hc_path_suffix(tok, n, &line, &col);
    if (!line) {   /* File "x.py", line 42 */
        const char *after = text + b;
        if (b > 0 && text[b] == '"' && !strncmp(after + 1, ", line ", 7)) line = atoi(after + 8);
    }
    /* candidates, in order: the token, without a/ b/ (git diff), after the last = */
    const char *cand[3]; int cn[3], nc = 0;
    cand[nc] = tok; cn[nc++] = body;
    if (body > 2 && (tok[0] == 'a' || tok[0] == 'b') && tok[1] == '/') { cand[nc] = tok + 2; cn[nc++] = body - 2; }
    { const char *eq = NULL; for (int i = 0; i < body; i++) if (tok[i] == '=') eq = tok + i; if (eq && eq + 1 < tok + body) { cand[nc] = eq + 1; cn[nc++] = (int)(tok + body - eq - 1); } }
    for (int i = 0; i < nc; i++) {
        const char *t = cand[i]; int tn = cn[i], dir = 0, alnum = 0;
        for (int k = 0; k < tn; k++) if (isalnum((unsigned char)t[k])) alnum = 1;
        if (!alnum) continue;
        if (!hc_path_resolve(t, tn, cwd, home, hit->path, (int)sizeof(hit->path))) continue;
        if (!hc_path_exists(hit->path, &dir)) continue;
        int from = i == 1 && t == tok + 2 ? a : (int)(t - text);   /* git's b/ stays underlined */
        if (at < from) continue;                      /* pointing at the --opt= part */
        hit->start = from; hit->end = b;
        hit->line = line; hit->col = col; hit->dir = dir;
        return 1;
    }
    hit->path[0] = 0;
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
