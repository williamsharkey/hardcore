/* cc -std=c99 -Wall -Wextra -Werror tests/test_reflow_records.c -o /tmp/hc-records */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t ch, fg, bg; uint8_t attr, wflag; uint16_t link; } Cell;
#include "../hardcore_reflow.h"

static Cell *grid_new(int cols, int rows) {
    Cell *grid = calloc((size_t)cols*rows, sizeof(Cell));
    assert(grid);
    for (int i = 0; i < cols*rows; i++) grid[i].ch = ' ';
    return grid;
}
static void put(Cell *grid, int cols, int row, int col, const char *text) {
    for (; *text; text++, col++) {
        assert(col < cols);
        grid[row*cols+col].ch = (unsigned char)*text;
    }
}
static void invariant(const ChromeReflow *out, const Cell *input) {
    int previous = -1, source_count = 0, visible = 0;
    for (int i = 0; i < out->rows*out->cols; i++) {
        source_count += !cr_space(input[i].ch);
        int source = out->source[i];
        assert(source >= 0 && source < out->rows*out->cols);
        if (!cr_space(out->cells[i].ch)) {
            assert(source > previous);
            assert(!memcmp(out->cells+i, input+source, sizeof(Cell)));
            previous = source; visible++;
        }
    }
    assert(source_count == visible);
}
static ChromeReflow run(Cell *input, int cols, int rows, const unsigned char *protected_rows,
                         int side, int dock, int width, int blanks) {
    ChromeReflow out;
    Cell *before = malloc((size_t)cols*rows*sizeof(Cell));
    assert(before); memcpy(before, input, (size_t)cols*rows*sizeof(Cell));
    assert(chrome_reflow_build(&out, input, cols, rows, protected_rows,
                               side, dock, 0, width, rows, blanks));
    assert(!memcmp(before, input, (size_t)cols*rows*sizeof(Cell)));
    invariant(&out, input);
    free(before);
    return out;
}
static int location(const ChromeReflow *out, int source) {
    for (int i = 0; i < out->cols*out->rows; i++)
        if (out->source[i] == source && !cr_space(out->cells[i].ch)) return i;
    assert(!"source glyph missing");
    return -1;
}
static void gap(const ChromeReflow *out, int begin, int end, int last_row) {
    for (int y = 0; y < last_row; y++) for (int x = begin; x < end; x++)
        assert(cr_space(out->cells[y*out->cols+x].ch));
}

static void wrapped_diff(void) {
    enum { COLS = 100, ROWS = 8, BODY = 6 };
    const char *literal = "build/hardcore: hardcore.c hardcore_chrome.h hardcore_chrome_draw.h hardcore_font_menu.h hardcore_display.h hardcore_reflow.h hardcore_mac.m hardcore_mac_dock.m hardcore_mac_update.h hardcore_mac_transfer.m hardcore_mac_terminal.m build/font.bin build/fonts.bin build/fonts.inc";
    Cell *grid = grid_new(COLS, ROWS);
    put(grid, COLS, 0, 0, " 149 -");
    int source = 0, physical = 0;
    while (literal[source]) {
        assert(physical < 3);
        for (int x = BODY; x < COLS && literal[source]; x++) grid[physical*COLS+x].ch = (unsigned char)literal[source++];
        physical++;
    }
    assert(physical == 3 && !cr_space(grid[COLS-1].ch) && !cr_space(grid[COLS+BODY].ch));
    for (int y = 0; y < physical; y++) for (int x = 0; x < COLS; x++) {
        Cell *c = grid+y*COLS+x;
        c->bg = 0x02243b24;
        c->fg = 0x02112233+(uint32_t)x;
        if (!cr_space(c->ch)) { c->attr = (uint8_t)(x%8); c->link = (uint16_t)(x%7); }
    }
    put(grid, COLS, 3, 0, "  + Show details");
    put(grid, COLS, 7, 0, "NEXT SECTION");
    for (int side = 1; side <= 2; side++) {
        int dock = side == 1 ? 72 : 0;
        ChromeReflow out = run(grid, COLS, ROWS, NULL, side, dock, 28, 3);
        assert(out.changed);
        gap(&out, side == 1 ? 70 : 0, side == 1 ? COLS : 30, 7);
        assert(!memcmp(out.cells+7*COLS, grid+7*COLS, COLS*sizeof(Cell)));
        /* Mid-word physical boundaries must not insert spaces. If a pair
           falls on a fresh display row, it still has no copied separator. */
        for (int row = 0; row < 2; row++) {
            int left = location(&out, row*COLS+COLS-1);
            int right = location(&out, (row+1)*COLS+BODY);
            if (left/COLS == right/COLS) assert(right == left+1);
        }
        chrome_reflow_free(&out);
    }
    ChromeReflow failed = run(grid, COLS, ROWS, NULL, 1, 16, 84, 3);
    assert(!memcmp(failed.cells, grid, 3*COLS*sizeof(Cell))); /* full-chain fallback */
    chrome_reflow_free(&failed);
    unsigned char protect[ROWS] = {0}; protect[1] = 1;
    failed = run(grid, COLS, ROWS, protect, 1, 72, 28, 3);
    assert(!memcmp(failed.cells+COLS, grid+COLS, COLS*sizeof(Cell)));
    chrome_reflow_free(&failed);
    free(grid);
}

static void command_column_case(int tree, int tpgid) {
    enum { COLS = 160, ROWS = 8, COMMAND = 60 };
    Cell *grid = grid_new(COLS, ROWS);
    put(grid, COLS, 0, 0, tpgid ? "    PID  PPID  PGID TPGID TTY      STARTED" : "    PID  PPID  PGID TTY      STARTED");
    if (tree) grid[2].ch = 0x2514;
    put(grid, COLS, 0, COMMAND, "COMMAND");
    put(grid, COLS, 1, 0, tpgid ? "    6191 20283 6191 -1 ??    Mon Oct 12 15:35:27 2026" : "    6191 20283 6191 ??       Mon Oct 12 15:35:27 2026");
    put(grid, COLS, 1, COMMAND, "/bin/zsh -c source /Users/test/.claude/shell-snapshots/zsh-snapshot-test.sh 2>/dev/null");
    put(grid, COLS, 2, 0, tpgid ? "    6192 6191 6192 6192 ttys004 Mon Oct 12 15:35:28 2026" : "    6192 6191 6192 ttys004  Mon Oct 12 15:35:28 2026");
    put(grid, COLS, 2, COMMAND, "/opt/homebrew/bin/node /Users/test/project/server.js --listen 127.0.0.1");
    put(grid, COLS, 3, 0, "  + 2 lines (^t to expand)");
    put(grid, COLS, 7, 0, "NEXT SECTION");
    for (int x = 0; x < 3*COLS; x++) if (!cr_space(grid[x].ch)) {
        grid[x].fg = 0x01000001+(uint32_t)(x%15); grid[x].attr = (uint8_t)(x%8);
    }
    ChromeReflow out = run(grid, COLS, ROWS, NULL, 1, 120, 40, 3);
    assert(out.changed);
    gap(&out, 118, COLS, 7);
    for (int row = 1; row <= 2; row++) {
        int first = location(&out, row*COLS+4);
        int found = 0;
        for (int y = first/COLS+1; y < 7; y++) {
            int i = y*COLS+COMMAND;
            if (!cr_space(out.cells[i].ch) && out.source[i]/COLS == row) {
                assert(out.source[i]%COLS >= COMMAND);
                for (int x = 0; x < COMMAND; x++) assert(cr_space(out.cells[y*COLS+x].ch));
                found = 1;
            }
        }
        assert(found);
    }
    assert(!memcmp(out.cells+7*COLS, grid+7*COLS, COLS*sizeof(Cell)));
    chrome_reflow_free(&out); free(grid);
}

static void one_status_bridge(void) {
    enum { COLS = 100, ROWS = 6 };
    Cell *grid = grid_new(COLS, ROWS);
    put(grid, COLS, 0, 0, "# Ran cp /Users/test/project/generated/artwork/finished-page.png /Users/test/project/output/page.png");
    grid[0].ch = 0x2022;
    put(grid, COLS, 1, 0, "  + Show details");
    put(grid, COLS, 2, 0, "# Completed /root/layout"); grid[2*COLS].ch = 0x2022;
    put(grid, COLS, 5, 0, "NEXT SECTION");
    ChromeReflow out = run(grid, COLS, ROWS, NULL, 1, 60, 40, 3);
    assert(out.changed);
    int status = location(&out, 2*COLS);
    assert(status/COLS > 2 && status%COLS == 0);
    for (int x = 0; x < 24; x++) assert(out.cells[status+x].ch == grid[2*COLS+x].ch);
    assert(!memcmp(out.cells+5*COLS, grid+5*COLS, COLS*sizeof(Cell)));
    gap(&out, 58, COLS, 5);
    chrome_reflow_free(&out);
    /* A second status is a boundary, not permission to cascade a page. */
    put(grid, COLS, 3, 0, "# Completed /root/another"); grid[3*COLS].ch = 0x2022;
    out = run(grid, COLS, ROWS, NULL, 1, 60, 40, 3);
    assert(!memcmp(out.cells+2*COLS, grid+2*COLS, 2*COLS*sizeof(Cell)));
    chrome_reflow_free(&out); free(grid);
}

static void badge(Cell *grid, int cols, int row, int col) {
    put(grid, cols, row, col, "# Back to bottom # esc");
    grid[row*cols+col].ch = 0x2193;
    grid[row*cols+col+17].ch = 0xb7;
    for (int x = col; x < col+22; x++) {
        grid[row*cols+x].fg = 0x020088cc;
        grid[row*cols+x].bg = 0x02333333;
    }
}

static void partial_badge_row(void) {
    enum { COLS = 100, ROWS = 4 };
    Cell *grid = grid_new(COLS, ROWS);
    put(grid, COLS, 0, 0, "# Ran cp /Users/test/project/generated/artwork/finished-page.png /Users/test/output/page.png");
    grid[0].ch = 0x2022;
    put(grid, COLS, 1, 0, "  + Show details");
    badge(grid, COLS, 2, 65);
    put(grid, COLS, 3, 0, "NEXT SECTION");
    ChromeReflow out = run(grid, COLS, ROWS, NULL, 1, 60, 40, 3);
    assert(out.changed);
    assert(location(&out, COLS+2)/COLS == 2);
    /* The footer uses the blank wing first. The intact badge can then slide
       into the remaining space without covering it or adding another row. */
    int badge_at = location(&out, 2*COLS+65);
    assert(badge_at/COLS == 2 && badge_at%COLS+22 == 58);
    assert(!memcmp(out.cells+badge_at, grid+2*COLS+65, 22*sizeof(Cell)));
    for (int x = 0; x < 22; x++) assert(out.source[badge_at+x] == 2*COLS+65+x);
    gap(&out, 58, COLS, 3);
    chrome_reflow_free(&out);
    /* If a failed full-width literal would land in the badge wing, discard
       the candidate. Restoring the badge must never erase terminal text. */
    for (int x = 0; x < COLS; x++) grid[COLS+x].ch = ' ';
    put(grid, COLS, 1, 0, "  125 + ");
    for (int x = 8; x < 95; x++) grid[COLS+x].ch = 'A'+x%26;
    out = run(grid, COLS, ROWS, NULL, 1, 60, 40, 3);
    assert(!memcmp(out.cells, grid, 2*COLS*sizeof(Cell)));
    badge_at = location(&out, 2*COLS+65);
    assert(badge_at/COLS == 2 && badge_at%COLS+22 == 58);
    assert(!memcmp(out.cells+badge_at, grid+2*COLS+65, 22*sizeof(Cell)));
    assert(!memcmp(out.cells+3*COLS, grid+3*COLS, COLS*sizeof(Cell)));
    chrome_reflow_free(&out); free(grid);
}

static uint32_t random_state = 0x19349af;
static unsigned random_number(unsigned limit) {
    random_state = random_state*1664525u+1013904223u;
    return random_state%limit;
}
static void randomized_chains(void) {
    const char *tokens[] = {"alpha", "beta", "source.c", "long_identifier", "input[idx]", "/Users/test/file.txt", "||"};
    for (int iteration = 0; iteration < 300; iteration++) {
        int cols = 60+(int)random_number(61), rows = 10, body = 6;
        Cell *grid = grid_new(cols, rows);
        char logical[350];
        int length = 0, target = cols+(int)random_number((unsigned)(cols*2));
        while (length < target) {
            const char *token = tokens[random_number(7)];
            int n = (int)strlen(token);
            if (length+n+1 >= (int)sizeof(logical)) break;
            if (length) logical[length++] = ' ';
            memcpy(logical+length, token, (size_t)n); length += n;
        }
        put(grid, cols, 0, 0, " 149 +");
        int from = 0, physical = 0;
        while (from < length) {
            for (int x = body; x < cols && from < length; x++) grid[physical*cols+x].ch = (unsigned char)logical[from++];
            for (int x = 0; x < cols; x++) {
                Cell *c = grid+physical*cols+x;
                c->bg = 0x02243b24; c->fg = 0x01000001+random_number(15);
                if (!cr_space(c->ch)) { c->attr = (uint8_t)random_number(8); c->link = (uint16_t)random_number(4); }
            }
            physical++;
        }
        assert(physical < rows-2);
        put(grid, cols, physical, 0, "  + Show details");
        put(grid, cols, rows-1, 0, "NEXT SECTION");
        unsigned char protect[10] = {0};
        if (!random_number(3)) protect[random_number((unsigned)physical)] = 1;
        int side = 1+(int)random_number(2), width = 8+(int)random_number((unsigned)(cols-16));
        ChromeReflow out = run(grid, cols, rows, protect, side, side == 1 ? cols-width : 0,
                              width, (int)random_number(4));
        for (int y = 0; y < rows; y++) if (protect[y]) assert(!memcmp(out.cells+y*cols, grid+y*cols, (size_t)cols*sizeof(Cell)));
        assert(!memcmp(out.cells+(rows-1)*cols, grid+(rows-1)*cols, (size_t)cols*sizeof(Cell)));
        chrome_reflow_free(&out); free(grid);
    }
}

int main(void) {
    wrapped_diff();
    for (int tree = 0; tree <= 1; tree++) for (int tpgid = 0; tpgid <= 1; tpgid++) command_column_case(tree, tpgid);
    one_status_bridge(); partial_badge_row(); randomized_chains();
    puts("PASS structured records: wrapped diffs, COMMAND columns, intact statuses, badge fallback; 300 randomized chains");
    return 0;
}
