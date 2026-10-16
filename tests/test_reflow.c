/* Standalone, platform-independent tests for display-only chrome reflow.
 * cc -std=c99 -Wall -Wextra -Werror tests/test_reflow.c -o /tmp/test-reflow
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t ch, fg, bg;
    uint8_t attr, wflag;
    uint16_t link;
} Cell;
#include "../hardcore_reflow.h"

static int checks;

static uint32_t next_character(const char **text) {
    const unsigned char *s = (const unsigned char *)*text;
    uint32_t c = *s++;
    int n = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    if (n) c &= (1u << (6 - n)) - 1;
    while (n--) { assert((*s & 0xc0) == 0x80); c = (c << 6) | (*s++ & 0x3f); }
    *text = (const char *)s;
    return c;
}

static Cell *make_grid(const char *const *lines, int rows, int cols) {
    Cell *grid = (Cell *)calloc((size_t)rows * (size_t)cols, sizeof(*grid));
    int y, x;
    assert(grid);
    for (y = 0; y < rows; y++) {
        const char *text = lines[y];
        for (x = 0; x < cols; x++) grid[y * cols + x].ch = *text ? next_character(&text) : ' ';
        assert(!*text);
    }
    return grid;
}

static void expect_row(const ChromeReflow *out, int row, const char *expected) {
    int x;
    for (x = 0; x < out->cols; x++) {
        uint32_t actual = out->cells[row * out->cols + x].ch;
        uint32_t want = *expected ? next_character(&expected) : ' ';
        if (actual != want) {
            fprintf(stderr, "row %d, col %d: wanted U+%04x, got U+%04x\n", row, x, want, actual);
            abort();
        }
    }
    assert(!*expected);
}

static uint32_t *normalized_words(const Cell *cells, int cols, int rows, const int *source) {
    uint32_t *text = (uint32_t *)malloc(((size_t)cols * (size_t)rows + (size_t)rows + 1) * sizeof(*text));
    int y, x, used = 0, separate = 0, previous_source = -1;
    assert(text);
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            uint32_t c = cells[y * cols + x].ch;
            if (cr_space(c)) { separate = !!used; continue; }
            /* A visual continuation inside a path is not a source word
               separator. All original separators must still be preserved. */
            if (source && previous_source >= 0 && source[y*cols+x] == previous_source+1 &&
                source[y*cols+x]/cols == previous_source/cols) separate = 0;
            if (separate) text[used++] = ' ';
            text[used++] = c;
            if (source) previous_source = source[y*cols+x];
            separate = 0;
        }
        separate = !!used;
    }
    text[used] = 0;
    return text;
}

/* Every visible non-space cell must retain both its original content/style
 * and source position. Reading it in display order must preserve word order. */
static void invariants(const ChromeReflow *out, const Cell *input) {
    int i, previous = -1, a = 0, b = 0, count = out->rows * out->cols;
    uint32_t *before_words = normalized_words(input, out->cols, out->rows, NULL);
    uint32_t *after_words = normalized_words(out->cells, out->cols, out->rows, out->source);
    for (i = 0; before_words[i] || after_words[i]; i++) assert(before_words[i] == after_words[i]);
    free(before_words);
    free(after_words);
    for (i = 0; i < count; i++) {
        int source = out->source[i];
        assert(source >= 0 && source < count);
        if (!cr_space(out->cells[i].ch)) {
            assert(!memcmp(out->cells + i, input + source, sizeof(Cell)));
            assert(source > previous);
            previous = source;
        }
    }
    while (a < count && b < count) {
        while (a < count && cr_space(input[a].ch)) a++;
        while (b < count && cr_space(out->cells[b].ch)) b++;
        if (a == count || b == count) break;
        assert(input[a++].ch == out->cells[b++].ch);
    }
    while (a < count && cr_space(input[a].ch)) a++;
    while (b < count && cr_space(out->cells[b].ch)) b++;
    assert(a == count && b == count);
    for (i = 0; i < out->rows; i++) {
        int col = out->markers[i];
        assert(col >= -1 && col < out->cols);
        if (col >= 0) assert(cr_space(out->cells[i * out->cols + col].ch));
    }
}

static ChromeReflow run_checked(const Cell *grid, int cols, int rows, const unsigned char *protected_rows,
                        int side, int x, int y, int width, int height, int blank_rows,
                        ChromeReflowBackgroundSafe background_safe) {
    ChromeReflow result;
    size_t bytes = (size_t)cols * (size_t)rows * sizeof(Cell);
    Cell *snapshot = (Cell *)malloc(bytes);
    assert(snapshot);
    memcpy(snapshot, grid, bytes);
    assert(chrome_reflow_build_checked(&result, grid, cols, rows, protected_rows, side,
                                       x, y, width, height, blank_rows, background_safe));
    assert(!memcmp(snapshot, grid, bytes));
    assert(result.rows == rows && result.cols == cols);
    invariants(&result, grid);
    free(snapshot);
    return result;
}

static ChromeReflow run(const Cell *grid, int cols, int rows, const unsigned char *protected_rows,
                        int side, int x, int y, int width, int height, int blank_rows) {
    return run_checked(grid, cols, rows, protected_rows, side, x, y, width, height, blank_rows, NULL);
}

static void unchanged(const char *const *lines, int rows, int cols) {
    Cell *grid = make_grid(lines, rows, cols);
    ChromeReflow result = run(grid, cols, rows, NULL, 1, 17, 0, cols - 17, rows, 3);
    int i;
    assert(!result.changed);
    assert(!memcmp(result.cells, grid, (size_t)rows * (size_t)cols * sizeof(Cell)));
    for (i = 0; i < rows * cols; i++) assert(result.source[i] == i);
    chrome_reflow_free(&result);
    free(grid);
}

static void test_fox_examples(void) {
    const char *lines[] = {"123", "  the quick brown fox jumps over", "  the sleeping", "  dog", "", "456", "  hello world"};
    const char *expected[] = {"123", "  the quick brown", "  fox jumps over", "  the sleeping", "  dog", "456", "  hello world"};
    Cell *grid = make_grid(lines, 7, 48);
    ChromeReflow out = run(grid, 48, 7, NULL, 1, 17, 0, 31, 20, 3);
    ChromeReflow repeated;
    int y;
    assert(out.changed == 1);
    for (y = 0; y < 7; y++) expect_row(&out, y, expected[y]);
    for (y = 1; y <= 4; y++) assert(out.markers[y] == 1);
    repeated = run(out.cells, 48, 7, NULL, 1, 17, 0, 31, 20, 3);
    assert(!repeated.changed);
    assert(!memcmp(out.cells, repeated.cells, 48 * 7 * sizeof(Cell)));
    chrome_reflow_free(&repeated);
    chrome_reflow_free(&out);
    free(grid);
    {
        const char *joined[] = {"123", "  the quick brown fox jumps over", "  the quick brown fox jumps over", "  the sleeping dog", "", "456"};
        const char *wanted[] = {"123", "  the quick brown fox", "  jumps over the quick", "  brown fox jumps over", "  the sleeping dog", "456"};
        grid = make_grid(joined, 6, 48);
        out = run(grid, 48, 6, NULL, 1, 22, 0, 26, 20, 3);
        assert(out.changed == 1);
        for (y = 0; y < 6; y++) expect_row(&out, y, wanted[y]);
        chrome_reflow_free(&out);
        free(grid);
    }
    checks++;
}

static void test_slack_no_blank(void) {
    const char *lines[] = {"123", "  the quick brown fox jumps", "  over the sleeping", "  dog", "456"};
    const char *expected[] = {"123", "  the quick brown fox", "  jumps over the", "  sleeping dog", "456"};
    Cell *grid = make_grid(lines, 5, 48);
    ChromeReflow out = run(grid, 48, 5, NULL, 1, 22, 0, 26, 20, 0);
    int y;
    assert(out.changed == 1);
    for (y = 0; y < 5; y++) expect_row(&out, y, expected[y]);
    chrome_reflow_free(&out);
    free(grid);
    {
        const char *with_blanks[] = {"  the quick brown fox jumps", "  over the sleeping", "  dog", "", "", "456"};
        grid = make_grid(with_blanks, 6, 48);
        out = run(grid, 48, 6, NULL, 1, 22, 0, 26, 20, 3);
        assert(out.changed == 1);
        expect_row(&out, 3, "");
        expect_row(&out, 4, "");
        expect_row(&out, 5, "456");
        chrome_reflow_free(&out);
        free(grid);
    }
    checks++;
}

static void test_left_and_band(void) {
    const char *lines[] = {"123", "  the quick brown fox jumps over", "  the sleeping", "  dog", "", "456", "  hello world"};
    const char *wanted[] = {"the quick brown", "fox jumps over", "the sleeping", "dog"};
    Cell *grid = make_grid(lines, 7, 45);
    ChromeReflow out = run(grid, 45, 7, NULL, 2, 0, 1, 28, 4, 3);
    ChromeReflow repeated;
    int y;
    assert(out.changed == 1);
    expect_row(&out, 0, "123");
    expect_row(&out, 5, "456");
    expect_row(&out, 6, "  hello world");
    for (y = 0; y < 4; y++) {
        char expected[64];
        memset(expected, ' ', 30);
        strcpy(expected + 30, wanted[y]);
        expect_row(&out, y + 1, expected);
        assert(out.markers[y + 1] == 29);
    }
    repeated = run(out.cells, 45, 7, NULL, 2, 0, 1, 28, 4, 3);
    assert(!repeated.changed);
    assert(!memcmp(out.cells, repeated.cells, 45 * 7 * sizeof(Cell)));
    chrome_reflow_free(&repeated);
    chrome_reflow_free(&out);
    free(grid);
    {
        const char *crossing[] = {
            "  one two three four five six seven eight nine ten eleven",
            "  twelve thirteen fourteen fifteen sixteen seventeen",
            "  eighteen nineteen twenty", "456"
        };
        grid = make_grid(crossing, 4, 60);
        out = run(grid, 60, 4, NULL, 2, 0, 1, 28, 1, 3);
        assert(out.changed == 1);
        assert(out.cells[2].ch == 'o');
        assert(out.cells[60 + 30].ch > ' ');
        assert(out.cells[120 + 2].ch > ' ');
        assert(out.cells[33].ch > ' ' && out.cells[120 + 33].ch > ' ');
        expect_row(&out, 3, "456");
        assert(out.markers[1] == 29);
        chrome_reflow_free(&out);
        out = run(grid, 60, 4, NULL, 1, 32, 1, 28, 1, 3);
        assert(out.changed == 1);
        for (y = 32; y < 60; y++) assert(cr_space(out.cells[60 + y].ch));
        assert(out.cells[33].ch > ' ' && out.cells[120 + 33].ch > ' ');
        chrome_reflow_free(&out);
        free(grid);
    }
    checks++;
}

static void test_protected_syntax(void) {
    const char *examples[][5] = {
        {"  const target = something();", "", "", "456", ""},
        {"  name      long value here", "", "", "456", ""},
        {"  name | very long column value", "", "", "456", ""},
        {"$ the quick brown fox jumps over", "", "", "456", ""},
        {"- one list item", "  continuation that should remain literal", "", "", "456"},
        {"```text", "the quick brown fox jumps over the dog", "", "```", "456"},
        {"  paragraph_variable another_variable", "", "", "456", ""},
        {"  extraordinarilylongword ordinary words", "", "", "456", ""},
        {"  the quick brown fox jumps over", "456", "", "", "789"},
        {"  First do the important work.", "  Then stop.", "456", "", ""},
        {"Setup Instructions", "Font and Theme", "", "456", ""},
        {"Important notes:", "the quick brown fox jumps over", "456", "", ""},
        {"Turn on the lamp", "Open the door", "456", "", ""},
        {"  call(one two three four five)", "", "", "456", ""},
        {"  user@host path $ long command arguments", "", "", "456", ""},
        {"  one two \033[31mthree four five", "", "", "456", ""},
        {"  one two\tthree four five", "", "", "456", ""}
    };
    size_t i;
    for (i = 0; i < sizeof(examples) / sizeof(examples[0]); i++) unchanged(examples[i], 5, 48);
    checks++;
}

static void test_cells_and_cursor_protection(void) {
    const char *lines[] = {"  the quick brown fox jumps over", "", "", "", "456"};
    Cell *grid = make_grid(lines, 5, 48);
    ChromeReflow out;
    int kind, x;
    for (kind = 0; kind < 9; kind++) {
        Cell saved = grid[10];
        unsigned char protect[5] = {0, 0, 0, 0, 0};
        switch (kind) {
        case 0: grid[10].bg = 0x02010203; break;
        case 1: grid[10].attr = 16; break;
        case 2: grid[10].attr = 8; break;
        case 3: grid[10].link = 1; break;
        case 4: grid[10].wflag = 1; break;
        case 5: grid[10].wflag = 2; break;
        case 6: grid[10].ch = 0x2500; break;
        case 7: grid[10].ch = 0x0301; break;
        case 8: protect[0] = 1; break;
        }
        out = run(grid, 48, 5, protect, 1, 17, 0, 31, 5, 3);
        assert(!out.changed);
        assert(!memcmp(out.cells, grid, 48 * 5 * sizeof(Cell)));
        chrome_reflow_free(&out);
        grid[10] = saved;
    }
    /* A colored trailing cell protects the whole row too. */
    grid[47].bg = 0x01000001;
    out = run(grid, 48, 5, NULL, 1, 17, 0, 31, 5, 3);
    assert(!out.changed);
    chrome_reflow_free(&out);
    grid[47].bg = 0;
    /* A styled empty row is a boundary, not available paragraph space. */
    grid[48 + 47].bg = 0x01000001;
    out = run(grid, 48, 5, NULL, 1, 17, 0, 31, 5, 3);
    assert(!out.changed);
    chrome_reflow_free(&out);
    grid[48 + 47].bg = 0;
    /* Ordinary foreground colors follow every individual glyph. */
    for (x = 2; x < 31; x++) grid[x].fg = 0x01000000u | (uint32_t)(x % 16);
    out = run(grid, 48, 5, NULL, 1, 17, 0, 31, 5, 3);
    assert(out.changed == 1);
    assert(out.cells[48 + 2].fg == grid[out.source[48 + 2]].fg);
    chrome_reflow_free(&out);
    free(grid);
    {
        const char *paragraph[] = {"  the quick brown fox jumps over", "  the sleeping dog", "", "456"};
        int styled_row;
        grid = make_grid(paragraph, 4, 48);
        for (styled_row = 0; styled_row < 2; styled_row++) {
            grid[styled_row * 48 + 8].bg = 0x02010203;
            out = run(grid, 48, 4, NULL, 2, 0, 0, 20, 4, 3);
            assert(!out.changed);
            assert(!memcmp(out.cells, grid, 48 * 4 * sizeof(Cell)));
            chrome_reflow_free(&out);
            grid[styled_row * 48 + 8].bg = 0;
        }
        free(grid);
    }
    checks++;
}

static int test_default_background(const Cell *cell) {
    return cell->bg == 0x022e3440;
}

static void test_styled_codex_prose(void) {
    const char *lines[] = {
        "• All checks are passing: the macOS build, 12 native UI checks, 71 terminal fixtures, reflow sanitizer tests, and live-update tests. Windows also passes a syntax",
        "  check; I haven’t run its GUI here. I’m committing the feature, pushing main, and installing the macOS build now.",
        "", "456"
    };
    Cell *grid = make_grid(lines, 4, 170);
    ChromeReflow out;
    int i, side, y, x;
    for (i = 0; i < 340; i++) {
        grid[i].attr = (uint8_t)(1 + i % 7); /* bold, dim, italic combinations */
        grid[i].fg = 0x01000000u | (uint32_t)(i % 16);
    }
    for (side = 1; side <= 2; side++) {
        out = run(grid, 170, 4, NULL, side, side == 1 ? 142 : 0, 0, 28, 2, 0);
        assert(out.changed == 1);
        assert(out.cells[side == 1 ? 0 : 28].ch == 0x2022);
        assert(out.source[side == 1 ? 0 : 28] == 0);
        for (y = 0; y < 2; y++)
            for (x = 0; x < 28; x++) assert(cr_space(out.cells[y * 170 + (side == 1 ? 142 : 0) + x].ch));
        expect_row(&out, 2, "");
        expect_row(&out, 3, "456");
        assert(out.markers[1] == (side == 1 ? 1 : 29));
        chrome_reflow_free(&out);
    }
    /* An explicit RGB encoding of the canvas is safe only when the native
     * color resolver has confirmed it. Preserve that encoding in moved cells. */
    for (i = 0; i < 340; i++) grid[i].bg = 0x022e3440;
    out = run(grid, 170, 4, NULL, 1, 142, 0, 28, 2, 0);
    assert(!out.changed);
    chrome_reflow_free(&out);
    out = run_checked(grid, 170, 4, NULL, 1, 142, 0, 28, 2, 0, test_default_background);
    assert(out.changed == 1);
    chrome_reflow_free(&out);
    grid[60].bg = 0x02010203;
    out = run_checked(grid, 170, 4, NULL, 1, 142, 0, 28, 2, 0, test_default_background);
    assert(!out.changed);
    chrome_reflow_free(&out);
    grid[60].bg = 0x022e3440;
    /* A protected cursor/link/effect on either half protects the entire bullet
     * paragraph; it must not leave a detached reflowed continuation. */
    for (y = 0; y < 2; y++) {
        unsigned char protect[] = {0, 0, 0, 0};
        protect[y] = 1;
        out = run_checked(grid, 170, 4, protect, 2, 0, 0, 28, 2, 0, test_default_background);
        assert(!out.changed);
        chrome_reflow_free(&out);
    }
    for (i = 8; i <= 128; i *= 2) {
        Cell saved = grid[170 + 10];
        grid[170 + 10].attr |= (uint8_t)i;
        out = run_checked(grid, 170, 4, NULL, 2, 0, 0, 28, 2, 0, test_default_background);
        assert(!out.changed);
        chrome_reflow_free(&out);
        grid[170 + 10] = saved;
    }
    free(grid);
    checks++;
}

static void test_bullet_commands_and_boundaries(void) {
    const char *lines[] = {
        "• Ran git add Makefile README.md hardcore.c hardcore_mac.m hardcore_mac_update.h",
        "", "└ created a file", "456"
    };
    Cell *grid = make_grid(lines, 4, 100);
    ChromeReflow out = run(grid, 100, 4, NULL, 1, 60, 0, 40, 4, 1);
    assert(out.changed == 1);
    assert(out.cells[0].ch == 0x2022);
    assert(out.cells[100 + 2].ch == 'h');
    expect_row(&out, 2, lines[2]);
    expect_row(&out, 3, "456");
    chrome_reflow_free(&out);
    free(grid);
    {
        const char *occupied[] = { lines[0], "└ created a file", "", "456" };
        grid = make_grid(occupied, 4, 100);
        out = run(grid, 100, 4, NULL, 1, 60, 0, 40, 4, 3);
        assert(out.changed == 1); /* Keep the tree output separate while using
                                    the blank after that local record block. */
        expect_row(&out, 2, "└ created a file");
        expect_row(&out, 3, "456");
        chrome_reflow_free(&out);
        free(grid);
    }
    {
        const char *bullets[] = {
            "• the quick brown fox jumps over the sleeping dog",
            "  in the quiet house", "",
            "• another sentence stands alone and keeps its position", "", "456"
        };
        grid = make_grid(bullets, 6, 70);
        out = run(grid, 70, 6, NULL, 1, 35, 0, 35, 6, 2);
        assert(out.changed == 2);
        assert(out.cells[0].ch == 0x2022 && out.cells[3 * 70].ch == 0x2022);
        assert(out.cells[70].ch == ' ' && out.cells[70 + 1].ch == ' ');
        expect_row(&out, 5, "456");
        chrome_reflow_free(&out);
        free(grid);
    }
    {
        const char *tight[] = { "• elephant", "", "", "456" };
        grid = make_grid(tight, 4, 20);
        out = run(grid, 20, 4, NULL, 1, 2, 0, 18, 1, 3);
        assert(!out.changed); /* Never strand the bullet on its own row. */
        chrome_reflow_free(&out);
        free(grid);
    }
    checks++;
}

static void test_row_budget_and_geometry(void) {
    const char *lines[] = {"  the quick brown fox jumps over the sleeping dog", "     ", "", "", "456"};
    Cell *grid = make_grid(lines, 5, 60);
    ChromeReflow out = run(grid, 60, 5, NULL, 1, 17, 0, 43, 5, 0);
    assert(!out.changed);
    chrome_reflow_free(&out);
    out = run(grid, 60, 5, NULL, 1, 17, 0, 43, 5, 2);
    assert(!out.changed);
    chrome_reflow_free(&out);
    out = run(grid, 60, 5, NULL, 1, 17, 0, 43, 5, 3);
    assert(out.changed == 1);
    expect_row(&out, 3, "  dog");
    expect_row(&out, 4, "456");
    chrome_reflow_free(&out);
    out = run(grid, 60, 5, NULL, 1, 17, 0, 43, 0, 3);
    assert(!out.changed);
    chrome_reflow_free(&out);
    out = run(grid, 60, 5, NULL, 1, 1, 0, 59, 5, 3);
    assert(!out.changed);
    chrome_reflow_free(&out);
    out = run(grid, 60, 5, NULL, 2, 0, 0, 60, 5, 3);
    assert(!out.changed);
    chrome_reflow_free(&out);
    out = run(grid, 60, 5, NULL, 2, 0, 0, INT_MAX, INT_MAX, 3);
    assert(!out.changed);
    chrome_reflow_free(&out);
    assert(!chrome_reflow_build(&out, grid, 0, 5, NULL, 1, 0, 0, 28, 5, 3));
    assert(!out.cells && !out.source && !out.markers);
    assert(!chrome_reflow_build(&out, grid, INT_MAX, 5, NULL, 1, 0, 0, 28, 5, 3));
    chrome_reflow_free(&out);
    free(grid);
    checks++;
}

static void test_structured_diff_records(void) {
    const char *lines[] = {
        "  5310 +    static const int mouse_modes[] = {0, 1000, 1002, 1003};",
        "  5312 -    int n = snprintf(modes, sizeof(modes), \"old format\", mouse);",
        "  5313 +    int n = snprintf(modes, sizeof(modes), \"new format\", mouse_modes[mouse]);",
        "  + Show details", "", "", "", "NEXT SECTION"
    };
    Cell *grid = make_grid(lines, 8, 100);
    for (int row = 0; row < 3; row++) for (int x = 0; x < 100; x++) {
        Cell *c = grid+row*100+x;
        c->bg = row == 1 ? 0x023f1717 : 0x02173f24;
        c->fg = 0x02000000u | (uint32_t)(x*14279+row);
        c->attr = (uint8_t)(1+row);
    }
    grid[25].link = 7; grid[25].attr |= 8;
    for (int side = 1; side <= 2; side++) {
        int dock = side == 1 ? 60 : 0, offset = side == 1 ? 0 : 40;
        ChromeReflow out = run(grid, 100, 8, NULL, side, dock, 0, 40, 7, 3);
        assert(out.changed == 3+(side == 2));
        int first_rows[3] = {-1, -1, -1};
        for (int y = 0; y < 7; y++) {
            for (int x = dock; x < dock+40; x++) assert(cr_space(out.cells[y*100+x].ch));
            for (int r = 0; r < 3; r++) if (out.source[y*100+offset+2] == r*100+2 &&
                out.cells[y*100+offset+2].ch == '5') first_rows[r] = y;
        }
        assert(first_rows[0] == 0 && first_rows[1] > first_rows[0] && first_rows[2] > first_rows[1]);
        for (int r = 0; r < 3; r++) {
            int base = first_rows[r]*100+offset;
            assert(out.cells[base+7].ch == (r == 1 ? '-' : '+'));
            assert(out.cells[base+12].ch == (r ? 'i' : 's'));
            assert(out.cells[(first_rows[r]+1)*100+offset+12].bg == grid[r*100+12].bg);
        }
        expect_row(&out, 7, "NEXT SECTION");
        chrome_reflow_free(&out);
    }
    free(grid);
    /* One spare row can improve one record without merging into or erasing
       the next numbered record, even though the whole block cannot fit. */
    const char *tight[] = {lines[1], lines[2], lines[3], "", "NEXT SECTION"};
    grid = make_grid(tight, 5, 100);
    ChromeReflow out = run(grid, 100, 5, NULL, 1, 60, 0, 40, 5, 1);
    assert(out.changed == 1);
    assert(out.cells[2].ch == '5' && out.cells[2*100+2].ch == '5');
    expect_row(&out, 2, lines[2]);
    expect_row(&out, 3, lines[3]);
    expect_row(&out, 4, "NEXT SECTION");
    chrome_reflow_free(&out); free(grid);
    checks++;
}

static void test_literal_paths_and_tool_output(void) {
    const char *paths[] = {
        "  /Users/example/Library/ApplicationSupport/hardcore/pasted/20261015/log/long-report.txt",
        "", "", "", "NEXT SECTION"
    };
    Cell *grid = make_grid(paths, 5, 100);
    for (int side = 1; side <= 2; side++) {
        ChromeReflow out = run(grid, 100, 5, NULL, side, side == 1 ? 44 : 0, 0, 56, 4, 3);
        assert(out.changed == 1);
        for (int y = 0; y < 3; y++) {
            int end = 100;
            while (end && cr_space(out.cells[y*100+end-1].ch)) end--;
            if (end && !cr_space(out.cells[(y+1)*100+(side == 1 ? 4 : 60)].ch))
                assert(out.cells[y*100+end-1].ch == '/');
        }
        expect_row(&out, 4, "NEXT SECTION");
        chrome_reflow_free(&out);
    }
    free(grid);
    const char *tool[] = {
        "  └ Original preserved: /Users/example/dev/hardcore/hardcore.app.previous",
        "    Updated and verified: /Users/example/Applications/hardcore.app",
        "  + 5 lines (^t to expand)", "", "", "", "NEXT SECTION"
    };
    grid = make_grid(tool, 7, 100);
    ChromeReflow out = run(grid, 100, 7, NULL, 1, 50, 0, 50, 6, 3);
    assert(out.changed == 2);
    assert(out.cells[2].ch == 0x2514);
    assert(out.cells[2*100+4].ch == 'U');
    expect_row(&out, 4, tool[2]);
    expect_row(&out, 6, "NEXT SECTION");
    chrome_reflow_free(&out); free(grid);
    const char *bad[] = {tool[0], "    | name | value |", "", "NEXT SECTION"};
    grid = make_grid(bad, 4, 100);
    out = run(grid, 100, 4, NULL, 1, 50, 0, 50, 4, 3);
    assert(!out.changed);
    expect_row(&out, 1, bad[1]);
    chrome_reflow_free(&out); free(grid);
    checks++;
}

static void test_unindented_and_mapping(void) {
    const char *lines[] = {"the quick brown fox jumps", "over the sleeping", "dog", "456"};
    Cell *grid = make_grid(lines, 4, 48);
    ChromeReflow out = run(grid, 48, 4, NULL, 1, 20, 0, 28, 4, 3);
    int y;
    assert(out.changed == 1);
    for (y = 0; y < 4; y++) assert(out.markers[y] == -1);
    expect_row(&out, 0, "the quick brown fox");
    expect_row(&out, 1, "jumps over the");
    expect_row(&out, 2, "sleeping dog");
    assert(out.source[48] == 20); /* moved j in jumps */
    assert(out.source[48 + 6] == 48); /* original o in over */
    assert(out.source[120] >= out.source[96 + 11]); /* trailing space follows last word */
    chrome_reflow_free(&out);
    free(grid);
    {
        const char *indented[] = {"123", "  the quick brown fox jumps over", "  the sleeping", "  dog", "", "456"};
        unsigned char protect[6] = {0, 0, 0, 0, 0, 0};
        grid = make_grid(indented, 6, 48);
        out = run(grid, 48, 6, NULL, 2, 0, 1, 31, 4, 3);
        assert(out.changed == 1);
        /* Original indentation maps to original indentation. A moved word
         * fragment's indentation maps to its preceding word boundary. */
        assert(out.source[48 + 31] == 48);
        assert(out.source[48 + 32] == 49);
        assert(out.source[48 + 33] == 50);
        assert(out.source[96 + 32] == out.source[96 + 33] - 1);
        chrome_reflow_free(&out);
        /* Native soft-wrap protection marks both halves of a split word.
         * Their entire surrounding same-indent paragraph must stay literal. */
        protect[2] = protect[3] = 1;
        out = run(grid, 48, 6, protect, 2, 0, 1, 20, 4, 3);
        assert(!out.changed);
        assert(!memcmp(out.cells, grid, 48 * 6 * sizeof(Cell)));
        chrome_reflow_free(&out);
        protect[2] = protect[3] = 0;
        protect[1] = protect[2] = 1;
        out = run(grid, 48, 6, protect, 2, 0, 1, 20, 4, 3);
        assert(!out.changed);
        chrome_reflow_free(&out);
        free(grid);
    }
    checks++;
}

static void test_fixed_following_rows(void) {
    const char *lines[] = {"123", "  the quick brown fox jumps over", "  the sleeping", "  dog", "", "456", "  we keep the next section in place", "", "", "789"};
    Cell *grid = make_grid(lines, 10, 48);
    ChromeReflow out = run(grid, 48, 10, NULL, 1, 17, 0, 31, 10, 3);
    assert(out.changed == 2);
    expect_row(&out, 0, "123");
    expect_row(&out, 5, "456");
    expect_row(&out, 9, "789");
    assert(!memcmp(out.cells + 5 * 48, grid + 5 * 48, 48 * sizeof(Cell)));
    chrome_reflow_free(&out);
    free(grid);
    checks++;
}

static uint32_t rng_state = 0x72345678;
static unsigned rnd(unsigned n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state % n;
}

static void test_randomized_invariants(void) {
    static const char *const vocabulary[] = {"the", "quick", "brown", "fox", "jumps", "over", "sleeping", "dog", "stays", "safely", "inside", "these", "words"};
    int iteration;
    for (iteration = 0; iteration < 500; iteration++) {
        int cols = 35 + (int)rnd(46), rows = 5 + (int)rnd(16), y, x;
        Cell *grid = (Cell *)calloc((size_t)rows * (size_t)cols, sizeof(Cell));
        ChromeReflow out;
        int side = 1 + (int)rnd(2), width = 4 + (int)rnd((unsigned)(cols - 4));
        int dock_y = (int)rnd((unsigned)(rows + 4)) - 2;
        int dock_height = 1 + (int)rnd((unsigned)rows);
        assert(grid);
        for (y = 0; y < rows; y++) {
            int indent = 2, used = 0, limit = 8 + (int)rnd((unsigned)(cols - 8));
            for (x = 0; x < cols; x++) grid[y * cols + x].ch = ' ';
            if (!rnd(4)) continue;
            while (indent + used < limit) {
                const char *word = vocabulary[rnd((unsigned)(sizeof(vocabulary) / sizeof(vocabulary[0])))];
                int length = (int)strlen(word);
                if (indent + used + !!used + length > limit) break;
                if (used) used++;
                for (x = 0; x < length; x++) {
                    Cell *c = grid + y * cols + indent + used++;
                    c->ch = (unsigned char)word[x];
                    c->fg = 0x01000000u | rnd(16);
                }
            }
        }
        out = run(grid, cols, rows, NULL, side, side == 1 ? cols - width : 0,
                  dock_y, width, dock_height, (int)rnd(4));
        chrome_reflow_free(&out);
        free(grid);
    }
    checks++;
}

static void test_randomized_records(void) {
    static const char *const tokens[] = {"alpha", "beta", "buffer,", "sizeof(buffer),", "/Users/example/Library/file.txt", "arguments[index]);"};
    for (int iteration = 0; iteration < 300; iteration++) {
        int cols = 40+(int)rnd(61), rows = 8, records = 1+(int)rnd(4);
        int side = 1+(int)rnd(2), width = 5+(int)rnd((unsigned)(cols-10));
        Cell *grid = (Cell *)calloc((size_t)cols*rows, sizeof(Cell));
        assert(grid);
        for (int i = 0; i < cols*rows; i++) grid[i].ch = ' ';
        for (int row = 0; row < records; row++) {
            char line[200];
            int used = snprintf(line, sizeof(line), "  %d %c    ", 100+row, row%2 ? '-' : '+');
            for (int word = 0; word < 10; word++) {
                const char *token = tokens[rnd(6)];
                int length = (int)strlen(token);
                if (used+length+1 > cols) break;
                memcpy(line+used, token, (size_t)length); used += length;
                line[used++] = ' ';
            }
            for (int x = 0; x < cols; x++) {
                Cell *c = grid+row*cols+x;
                c->ch = x < used ? (unsigned char)line[x] : ' ';
                c->fg = 0x01000000u | rnd(16);
                c->bg = 0x02010203u+row;
                c->attr = (uint8_t)(1+rnd(7));
            }
        }
        grid[7*cols].ch = '9';
        ChromeReflow out = run(grid, cols, rows, NULL, side, side == 1 ? cols-width : 0,
                              (int)rnd(3), width, 1+(int)rnd(6), (int)rnd(4));
        assert(out.cells[7*cols].ch == '9');
        chrome_reflow_free(&out); free(grid);
    }
    checks++;
}

int main(void) {
    test_fox_examples();
    test_slack_no_blank();
    test_left_and_band();
    test_protected_syntax();
    test_cells_and_cursor_protection();
    test_styled_codex_prose();
    test_bullet_commands_and_boundaries();
    test_structured_diff_records();
    test_literal_paths_and_tool_output();
    test_row_budget_and_geometry();
    test_unindented_and_mapping();
    test_fixed_following_rows();
    test_randomized_invariants();
    test_randomized_records();
    printf("PASS native reflow: %d groups, including 800 randomized grids\n", checks);
    return 0;
}
