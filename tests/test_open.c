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
    touch("my file.md", "# spaces\n"); touch("file.md", "short\n");
    touch("file[1].c", "int b;\n"); touch("file (1).txt", "copy\n");
    touch("src/name[v2].h", "int c;\n");

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

    /* spaces: quotes, backslash, or the longest existing run. Brackets stay in the name. */
    path_case("open \"my file.md\" now", "file.md", "", "my file.md", 0, 0, "my file.md");
    path_case("open 'my file.md' now", "file.md", "", "my file.md", 0, 0, "my file.md");
    path_case("open \"my file.md\"", "\"", "", "my file.md", 0, 0, "my file.md");
    path_case("see my\\ file.md:4 please", "file.md", "", "my file.md", 4, 0, NULL);
    path_case("see my file.md now", "my file", "", "my file.md", 0, 0, "my file.md");
    path_case("see my file.md now", "file.md", "", "my file.md", 0, 0, "my file.md");   /* longer than file.md */
    path_case("open ~/my file.md please", "file.md", "", "my file.md", 0, 0, NULL);
    path_case("build file[1].c:9:2: error", "file[1]", "", "file[1].c", 9, 2, "file[1].c:9:2:");
    path_case("note src/name[v2].h:8", "name[v2]", "", "src/name[v2].h", 8, 0, NULL);
    path_case("wrote file (1).txt today", "(1).txt", "", "file (1).txt", 0, 0, "file (1).txt");
    {   /* the space between the two words of the longest run */
        const char *line = "see my file.md now";
        HcPathHit h; char cwd[2048];
        const char *sp = strstr(line, "y f");
        snprintf(cwd, sizeof(cwd), "%s", root);
        int got = hc_path_at(line, sp ? (int)(sp + 1 - line) : -1, cwd, root, &h);
        char want[2048]; snprintf(want, sizeof(want), "%s/my file.md", root);
        check(got && !strcmp(h.path, want) && h.end - h.start == (int)strlen("my file.md"),
              "space inside 'my file.md' is part of the path");
    }
    {   /* a soft wrap is the two rows joined, with no character inserted */
        char full[2048];
        snprintf(full, sizeof(full), "note %s/my file.md end", root);
        const char *mp = strstr(full, "my file.md");
        int path_at = mp ? (int)(mp - full) : 0;
        int width = path_at + 4;   /* "my f" | "ile.md" */
        HcPathHit h;
        int ln[2], c0[2], c1[2], np, w2[2] = {width, 0}, one[2] = {80, 0};
        check(width < (int)strlen(full) && hc_path_at(full, path_at + 1, root, root, &h), "joined wrap text hits");
        w2[1] = h.end > width ? h.end - width : 0;
        np = hc_path_wrap_parts_n(w2, 2, h.start, h.end, ln, c0, c1, 2);
        check(np == 2 && ln[0] == 0 && ln[1] == 1 && c1[0] == width && c0[1] == 0 && c1[1] == h.end - width,
              "wrap splits into %d parts (%d:%d and %d:%d)", np, c0[0], c1[0], np > 1 ? c0[1] : -1, np > 1 ? c1[1] : -1);
        check(hc_path_at(full, width + 1, root, root, &h) && strstr(h.path, "my file.md"), "hit from the second wrapped row");
        check(hc_path_wrap_parts_n(one, 2, 2, 10, ln, c0, c1, 2) == 1 && ln[0] == 0 && c0[0] == 2 && c1[0] == 10, "a hit on one row stays one part");
    }
    {   /* default-app line jump: xed, vscode/cursor URL schemes, BBEdit */
        char u[1024];
        check(hc_open_line_kind("com.apple.dt.Xcode", "Xcode") == HC_LINE_XCODE, "kind Xcode");
        check(hc_open_line_kind("com.microsoft.VSCode", NULL) == HC_LINE_VSCODE, "kind VS Code");
        check(hc_open_line_kind("com.microsoft.VSCodeInsiders", NULL) == HC_LINE_VSCODE_INSIDERS, "kind VS Code Insiders");
        check(hc_open_line_kind(NULL, "Cursor") == HC_LINE_CURSOR, "kind Cursor by name");
        check(hc_open_line_kind("com.todesktop.230313mzl4w4u92", NULL) == HC_LINE_CURSOR, "kind Cursor by bundle id");
        check(hc_open_line_kind("com.barebones.bbedit", "BBEdit") == HC_LINE_BBEDIT, "kind BBEdit");
        check(hc_open_line_kind("com.apple.Preview", "Preview") == HC_LINE_NONE, "Preview has no line jump");
        check(hc_open_line_target(HC_LINE_XCODE, "/tmp/a.c", 42, 7, u, (int)sizeof(u)) && !strcmp(u, "xed -l 42 /tmp/a.c"), "xed target: %s", u);
        check(hc_open_line_target(HC_LINE_VSCODE, "/tmp/my file.c", 42, 7, u, (int)sizeof(u))
              && !strcmp(u, "vscode://file/tmp/my%20file.c:42:7"), "vscode target: %s", u);
        check(hc_open_line_target(HC_LINE_VSCODE_INSIDERS, "/tmp/a.c", 9, 0, u, (int)sizeof(u))
              && !strcmp(u, "vscode-insiders://file/tmp/a.c:9:1"), "vscode-insiders target: %s", u);
        check(hc_open_line_target(HC_LINE_CURSOR, "/tmp/a.c", 3, 0, u, (int)sizeof(u))
              && !strcmp(u, "cursor://file/tmp/a.c:3:1"), "cursor target: %s", u);
        check(hc_open_line_target(HC_LINE_BBEDIT, "/tmp/my file.c", 5, 2, u, (int)sizeof(u))
              && !strcmp(u, "x-bbedit://open?url=file:///tmp/my%20file.c&line=5&column=2"), "bbedit target: %s", u);
        check(!hc_open_line_target(HC_LINE_NONE, "/tmp/a.c", 4, 1, u, (int)sizeof(u)), "no target for Preview");
        check(!hc_open_line_target(HC_LINE_VSCODE, "/tmp/a.c", 0, 1, u, (int)sizeof(u)), "no target without a line");
    }

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
