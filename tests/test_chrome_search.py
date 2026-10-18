#!/usr/bin/env python3
"""Headless regression tests for the actual adaptive-menu placement search.

Compile the production search, scorer, layout measurement, and reflow engine
against owned terminal-cell fixtures. No application, shell, or PTY is opened.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def production_functions():
    display = (ROOT / "hardcore_display.h").read_text()
    start = display.index("static ChromePlacementScore chrome_motion_score(")
    end = display.index("static void chrome_autoplace_prepare(", start)
    chrome = (ROOT / "hardcore_chrome.h").read_text()
    layout = re.search(r"^static ChromeLayout chrome_measure\(.*?^\}", chrome,
                       re.MULTILINE | re.DOTALL)
    if layout is None:
        raise AssertionError("production chrome_measure function not found")
    primary = re.search(r"^static ChromeLayout chrome_primary_measure\(.*?^\}", chrome, re.MULTILINE | re.DOTALL)
    return layout.group(0) + "\n" + primary.group(0) + "\n" + display[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t ch, fg, bg; uint8_t attr, wflag; uint16_t link; } Cell;
#include "hardcore_reflow.h"
#include "hardcore_chrome_motion.h"
#include "hardcore_panel_reflow.h"
#define DEF_BG 0
#define SB_COLS 28
#define W_WIDE 1
typedef struct { int cols, rows, cx, cy, view, m_cursor_vis; } Term;
typedef struct {
    int top, grip_rows, tab_row, tab_capacity, tab_count;
    int settings_row, section_row, list_row, list_capacity, list_count, bottom;
} ChromeLayout;
static ChromeLayout g_chrome;
static int g_grid_on,g_grid_top=1;
static int g_tab_underlay = 1, g_ntabs = 7, g_settings_open, fixture_items;
static uint32_t tick, tick_step;
static int chrome_min(int a, int b) { return a < b ? a : b; }
static int chrome_max(int a, int b) { return a > b ? a : b; }
static int first_row(void) { return 0; }
static int chrome_item_total(void) { return fixture_items; }
/* Playback is off in these placement fixtures; dynamic transport geometry
 * is exercised independently by test_chrome_header.py. */
static int chrome_header_rows(void) { return 2; }
static uint32_t GetTickCount(void) { tick += tick_step; return tick; }
static int chrome_background_safe(const Cell *c) { return !c->bg; }
static uint32_t chrome_panel_background(const Cell *c) { return c->bg; }
/* These fixtures use ordinary visible ASCII; rendering-specific occupancy
 * (hidden attributes, combining cells, palette colors) has native coverage. */
static int cell_covers_chrome(const Cell *c) { return c->ch > ' '; }

/* PRODUCTION_FUNCTIONS */

static Cell *blank_grid(int cols, int rows) {
    Cell *grid = calloc((size_t)cols * rows, sizeof(Cell));
    assert(grid);
    for (int i = 0; i < cols * rows; i++) grid[i].ch = ' ';
    return grid;
}
static ChromePlacementScore exact_score(Term *t, Cell *grid, unsigned char *protected_rows,
                                         int side, int row) {
    ChromeReflow out = {0};
    int height = g_chrome.bottom - g_chrome.top;
    assert(chrome_reflow_build_checked(&out, grid, t->cols, t->rows, protected_rows,
            side, side == 1 ? t->cols - SB_COLS : 0, row, SB_COLS, height, 3,
            chrome_background_safe));
    int previous = -1;
    for (int i = 0; i < t->cols * t->rows; i++) if (!cr_space(out.cells[i].ch)) {
        int source = out.source[i];
        assert(source >= 0 && source < t->cols * t->rows && source > previous);
        assert(!memcmp(out.cells + i, grid + source, sizeof(Cell)));
        previous = source;
    }
    ChromePlacementScore score = chrome_motion_score(out.cells, out.source, t->cols, t->rows,
        side, row, height, g_tab_underlay, g_chrome.top, t);
    chrome_reflow_free(&out);
    return score;
}
static int checked_search(Term *t, Cell *grid, unsigned char *protected_rows,
                           ChromePlacementScore *winner) {
    size_t bytes = (size_t)t->cols * t->rows * sizeof(Cell);
    Cell *before = malloc(bytes);
    ChromeLayout layout = g_chrome;
    Term term = *t;
    int side = g_tab_underlay;
    assert(before);
    memcpy(before, grid, bytes);
    int moved = chrome_motion_search(t, grid, protected_rows, winner);
    assert(!memcmp(before, grid, bytes) && !memcmp(&term, t, sizeof(term)));
    assert(!memcmp(&layout, &g_chrome, sizeof(layout)) && side == g_tab_underlay);
    if (moved) {
        ChromeLayout destination = chrome_primary_measure(t->rows, 0, g_ntabs, g_settings_open,
            winner->side, winner->row, fixture_items);
        assert(destination.top == winner->row);
        assert(destination.tab_capacity == layout.tab_capacity);
        assert(destination.list_capacity == layout.list_capacity);
        assert(winner->row >= 0 && winner->row + layout.bottom - layout.top <= t->rows);
    }
    free(before);
    return moved;
}
static void regional(int side, int limited) {
    Term t = {100, 100, 0, 0, 0, 0};
    Cell *grid = blank_grid(t.cols, t.rows);
    unsigned char protected_rows[100] = {0};
    int column = side == 1 ? 80 : 10;
    for (int y = 0; y <= 54; y += 9) { grid[y*100+column].ch = 'X'; protected_rows[y] = 1; }
    for (int y = 64; y < 100; y += 9) { grid[y*100+column].ch = 'X'; protected_rows[y] = 1; }
    const char *text = "  this ordinary prose line has enough words to wrap locally into several remaining blank rows";
    for (int y = 55; y <= 60; y++) for (int x = 0; text[x]; x++) {
        grid[y*100+x].ch = (unsigned char)text[x];
        grid[y*100+x].fg = 0x01000001u + (unsigned)x % 15;
        grid[y*100+x].attr = (uint8_t)(x % 8);
    }
    g_tab_underlay = side;
    g_chrome = chrome_primary_measure(t.rows, 0, g_ntabs, 0, side, 0, 0);
    ChromePlacementScore known = exact_score(&t, grid, protected_rows, side, 55);
    assert(!known.collision_rows); /* The candidate exists, at full capacity. */
    /* Simulate an expensive search: both edges must still get an early
       useful probe before the 12ms work budget ends the shortlist. */
    tick_step = limited ? 12 : 0;
    ChromePlacementScore winner = {0};
    assert(checked_search(&t, grid, protected_rows, &winner));
    assert(!winner.collision_rows && winner.side == side);
    ChromePlacementScore actual = exact_score(&t, grid, protected_rows, winner.side, winner.row);
    assert(!actual.collision_rows);
    if (!limited) {
        ChromePlacementScore repeated = {0};
        assert(checked_search(&t, grid, protected_rows, &repeated));
        assert(!memcmp(&winner, &repeated, sizeof(winner)));
    }
    free(grid);
}
static void cursor(void) {
    Term t = {100, 20, 85, 3, 0, 1};
    Cell *grid = blank_grid(t.cols, t.rows);
    unsigned char protected_rows[20] = {0};
    protected_rows[3] = 1;
    g_chrome = chrome_primary_measure(t.rows, 0, g_ntabs, 0, 1, 0, 0);
    ChromePlacementScore current = exact_score(&t, grid, protected_rows, 1, 0);
    assert(current.collision_rows == 1 && current.collision_cells == 1);
    grid[3*100+85].wflag = W_WIDE;
    current = exact_score(&t, grid, protected_rows, 1, 0);
    assert(current.collision_rows == 1 && current.collision_cells == 2);
    ChromePlacementScore winner = {0};
    assert(checked_search(&t, grid, protected_rows, &winner));
    assert(!winner.collision_rows);
    t.view = 1;
    current = exact_score(&t, grid, protected_rows, 1, 0);
    assert(!current.collision_rows);
    t.view = 0; t.m_cursor_vis = 0;
    assert(!checked_search(&t, grid, protected_rows, &winner));
    free(grid);
}
static void capacity(void) {
    Term t = {100, 36, 0, 0, 0, 0};
    Cell *grid = blank_grid(t.cols, t.rows);
    unsigned char protected_rows[36] = {0};
    g_ntabs = 30; fixture_items = 20; g_settings_open = 1;
    g_chrome = chrome_primary_measure(t.rows, 0, g_ntabs, 1, 1, 8, fixture_items);
    for (int y = 8; y < 36; y++) { grid[y*100+80].ch = 'X'; protected_rows[y] = 1; }
    ChromePlacementScore winner = {0};
    assert(checked_search(&t, grid, protected_rows, &winner));
    assert(winner.side == 2 && !winner.collision_rows);
    /* There is clear space below the ink, but reducing page capacity must
       never masquerade as successfully fitting the complete current menu. */
    assert(winner.row == g_chrome.top);
    free(grid);
}
static void unchanged(void) {
    Term t = {100, 40, 0, 0, 0, 0};
    Cell *grid = blank_grid(t.cols, t.rows);
    unsigned char protected_rows[40] = {0};
    g_chrome = chrome_primary_measure(t.rows, 0, g_ntabs, 0, 1, 8, 0);
    ChromePlacementScore winner = {0};
    assert(!checked_search(&t, grid, protected_rows, &winner));
    for (int i = 0; i < t.cols * t.rows; i++) grid[i].ch = 'X';
    memset(protected_rows, 1, sizeof(protected_rows));
    assert(!checked_search(&t, grid, protected_rows, &winner));
    free(grid);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "right")) regional(1, 0);
    else if (!strcmp(argv[1], "left")) regional(2, 0);
    else if (!strcmp(argv[1], "budget")) { regional(1, 1); regional(2, 1); }
    else if (!strcmp(argv[1], "cursor")) cursor();
    else if (!strcmp(argv[1], "capacity")) capacity();
    else if (!strcmp(argv[1], "unchanged")) unchanged();
    else return 2;
    puts("PASS production chrome search");
    return 0;
}
'''


class ChromeSearchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="hc-chrome-search-")
        cls.addClassCleanup(cls.temporary.cleanup)
        folder = Path(cls.temporary.name)
        source = folder / "search.c"
        cls.executable = folder / "search"
        source.write_text(HARNESS.replace("/* PRODUCTION_FUNCTIONS */", production_functions()))
        command = shlex.split(os.environ.get("CC", "cc"))
        command += ["-std=c99", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-I", str(ROOT), str(source), "-o", str(cls.executable)]
        if os.environ.get("HC_TEST_SANITIZE"):
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        compiled = subprocess.run(command, capture_output=True, text=True, timeout=30)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def check_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True,
                                timeout=10, stdin=subprocess.DEVNULL)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS production chrome search", result.stdout)

    def test_reflowable_right_region_beats_sparse_protected_candidates(self):
        self.check_case("right")

    def test_reflowable_left_region_beats_sparse_protected_candidates(self):
        self.check_case("left")

    def test_work_budget_still_checks_useful_candidates(self):
        self.check_case("budget")

    def test_visible_cursor_is_an_intrusion(self):
        self.check_case("cursor")

    def test_move_preserves_complete_menu_capacity(self):
        self.check_case("capacity")

    def test_clear_or_unimprovable_content_does_not_move(self):
        self.check_case("unchanged")


if __name__ == "__main__":
    unittest.main()
