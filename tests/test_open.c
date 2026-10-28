/* Cmd+hover / Cmd+click file opening without a window: path detection over
   sample lines with a cwd, the handler registry, the shell script a handler
   becomes, and the split-view grid geometry. make open-test runs it. */
#include "../hardcore_open.h"
#include "../hardcore_grid.h"
#include <stdarg.h>
#include <unistd.h>

static int fails, passes;
static void check(int ok, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    printf(ok ? "PASS " : "FAIL "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) passes++; else fails++;
}
static char root[1024];
static void touch(const char *rel, const char *body) {
    char p[2048]; snprintf(p, sizeof(p), "%s/%s", root, rel);
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); exit(2); }
    fputs(body, f); fclose(f);
}
/* hit at the first occurrence of `point` in line; expects rel ("" = no hit) */
static void path_case(const char *line, const char *point, const char *cwd_rel, const char *rel, int eline, int ecol, const char *span) {
    char cwd[2048], want[2048]; HcPathHit h;
    snprintf(cwd, sizeof(cwd), "%s%s%s", root, *cwd_rel ? "/" : "", cwd_rel);
    const char *p = strstr(line, point);
    int got = hc_path_at(line, p ? (int)(p - line) : -1, cwd, root, &h);
    if (!*rel) { check(!got, "no path in [%s] at '%s' (got %s)", line, point, got ? h.path : "-"); return; }
    snprintf(want, sizeof(want), "%s/%s", root, rel);
    if (!strcmp(rel, ".")) snprintf(want, sizeof(want), "%s", root);
    int ok = got && !strcmp(h.path, want) && h.line == eline && h.col == ecol;
    if (ok && span) ok = (int)strlen(span) == h.end - h.start && !strncmp(line + h.start, span, (size_t)(h.end - h.start));
    check(ok, "[%s] at '%s' -> %s:%d:%d span '%.*s' (want %s:%d:%d%s%s)", line, point, got ? h.path : "-", h.line, h.col,
          got ? h.end - h.start : 0, got ? line + h.start : "", want, eline, ecol, span ? " span " : "", span ? span : "");
}

int main(void) {
    char tmpl[] = "/tmp/hcopen-test-XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    if (!realpath(tmpl, root)) return 2;
    char d[2048];
    snprintf(d, sizeof(d), "%s/src", root); mkdir(d, 0755);
    snprintf(d, sizeof(d), "%s/sub", root); mkdir(d, 0755);
    touch("src/a.c", "int a;\n"); touch("README.md", "# hi\n"); touch("Makefile", "all:\n");
    touch("img.PNG", "\x89PNG"); touch("notes.pdf", "%PDF");

    /* compiler, grep and linter output */
    path_case("src/a.c:42: error: expected ';'", "a.c", "", "src/a.c", 42, 0, "src/a.c:42:");
    path_case("  src/a.c:42:7: warning: unused", "src/", "", "src/a.c", 42, 7, NULL);
    path_case("see README.md.", "README", "", "README.md", 0, 0, "README.md");
    path_case("edit README.md: done", "md:", "", "README.md", 0, 0, NULL);
    path_case("(src/a.c:3)", "src", "", "src/a.c", 3, 0, "src/a.c:3");
    /* absolute, home, file URL, relative with .. */
    { char line[2048]; snprintf(line, sizeof(line), "open %s/README.md now", root); path_case(line, "README", "", "README.md", 0, 0, NULL); }
    path_case("cat ~/src/a.c", "~/", "", "src/a.c", 0, 0, "~/src/a.c");
    { char line[2048]; snprintf(line, sizeof(line), "file://%s/README.md", root); path_case(line, "README", "", "README.md", 0, 0, NULL); }
    path_case("../README.md:2", "../", "src", "README.md", 2, 0, NULL);
    path_case("./a.c", "a.c", "src", "src/a.c", 0, 0, NULL);
    /* Python tracebacks, git diffs, --opt=PATH */
    path_case("  File \"src/a.c\", line 9, in <module>", "a.c", "", "src/a.c", 9, 0, "src/a.c");
    path_case("diff --git a/src/a.c b/src/a.c", "b/src", "", "src/a.c", 0, 0, "b/src/a.c");
    path_case("--input=src/a.c", "a.c", "", "src/a.c", 0, 0, "src/a.c");
    /* folders, and what must not light up */
    path_case("ls sub", "sub", "", "sub", 0, 0, NULL);
    path_case("missing.c:3 nope", "missing", "", "", 0, 0, NULL);
    path_case("visit https://example.com/src/a.c", "example", "", "", 0, 0, NULL);
    path_case("a plain sentence", "plain", "", "", 0, 0, NULL);
    path_case("... --", "...", "", "", 0, 0, NULL);
    path_case("src/a.c", " ", "", "", 0, 0, NULL);
    { HcPathHit h; char cwd[2048]; snprintf(cwd, sizeof(cwd), "%s", root);
      check(hc_path_at("ls sub", 6, cwd, root, &h) == 0, "offset past the end is no hit"); }

    /* registry: built-in defaults */
    HcOpenConf c; hc_open_defaults(&c);
    struct { const char *path; int dir, text; const char *want; } R[] = {
        {"/x/README.md", 0, 1, "hcmd %f"}, {"/x/notes.MARKDOWN", 0, 1, "hcmd %f"},
        {"/x/a.c", 0, 1, "hcmd -p +%l %f"}, {"/x/Makefile", 0, 1, "hcmd -p +%l %f"},
        {"/x/.gitignore", 0, 1, "hcmd -p +%l %f"}, {"/x/LICENSE", 0, 1, "hcmd -p +%l %f"},
        {"/x/blob", 0, 0, "default"}, {"/x/notes.pdf", 0, 0, "default"}, {"/x/app.dmg", 0, 0, "default"},
        {"/x/img.PNG", 0, 0, "hardcore --here-media --image %f"}, {"/x/sub", 1, 0, "files"},
    };
    for (size_t i = 0; i < sizeof(R) / sizeof(*R); i++) {
        const char *got = hc_open_lookup(&c, R[i].path, R[i].dir, R[i].text);
        check(!strcmp(got, R[i].want), "lookup %s -> %s (want %s)", R[i].path, got, R[i].want);
    }
    /* the user's open.conf overrides */
    hc_open_parse(&c, "# mine\nmd = default\n.pdf, epub = hcmd -p %f\n/ = default\n  \n");
    check(!strcmp(hc_open_lookup(&c, "/x/a.md", 0, 1), "default"), "open.conf: md -> default");
    check(!strcmp(hc_open_lookup(&c, "/x/b.pdf", 0, 0), "hcmd -p %f"), "open.conf: .pdf, epub -> hcmd (dot and comma ok)");
    check(!strcmp(hc_open_lookup(&c, "/x/b.epub", 0, 0), "hcmd -p %f"), "open.conf: epub");
    check(!strcmp(hc_open_lookup(&c, "/x/sub", 1, 0), "default"), "open.conf: folders -> default");
    check(!hc_open_in_hardcore("default") && hc_open_in_hardcore("hcmd %f") && hc_open_in_hardcore("files"), "in-hardcore test");
    char script[512];
    check(hc_open_script("hcmd -p +%l %f", script, sizeof(script)) && !strcmp(script, "exec hcmd -p +\"$2\" \"$1\""), "script: %s", script);

    /* split view: two panes side by side, or stacked in a tall window */
    HcGrid g; memset(&g, 0, sizeof(g));
    hc_grid_build(&g, 2, 200, 50);
    check(g.cols == 2 && g.rows == 1 && g.rect[0].w + g.rect[1].w == 200 && g.rect[1].x == g.rect[0].w, "split side by side: %dx%d", g.cols, g.rows);
    g.stack = 1; hc_grid_build(&g, 2, 60, 80);
    check(g.cols == 1 && g.rows == 2 && g.rect[0].w == 60 && g.rect[1].y == g.rect[0].h, "split stacked: %dx%d", g.cols, g.rows);
    check(!hc_grid_portrait(1656, 1920) && !hc_grid_portrait(1920, 1080) && hc_grid_portrait(1000, 1920), "split stacks only in a clearly portrait area");
    g.stack = 0; hc_grid_build(&g, 2, 200, 50);
    check(g.cols == 2 && g.rows == 1, "split back to side by side");

    char rm[2100]; snprintf(rm, sizeof(rm), "rm -rf '%s'", root); if (system(rm)) {}
    printf("%s %d passed, %d failed\n", fails ? "FAIL" : "ok", passes, fails);
    return fails != 0;
}
