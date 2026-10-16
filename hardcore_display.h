#ifndef _WIN32
static void chrome_tv_prepare(Term *t);
#endif
/* Display-only chrome reflow. The terminal and scrollback remain authoritative;
   moved cells carry source addresses so selection and copying use real text. */
static int chrome_background_safe(const Cell *c) {
    uint32_t fg, bg;
    cell_colors(c, &fg, &bg);
    return bg == DEF_BG;
}
#include "hardcore_chrome_motion.h"
#include "hardcore_panel_reflow.h"
static ChromeMotion g_chrome_motion;
static uint32_t chrome_panel_background(const Cell *cell) {
    uint32_t fg, bg;
    cell_colors(cell, &fg, &bg);
    return bg;
}

static uint64_t chrome_motion_hash(uint64_t hash, const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    while (length--) { hash ^= *bytes++; hash *= UINT64_C(1099511628211); }
    return hash;
}
static uint64_t chrome_motion_context(Term *t) {
    const uintptr_t identity[] = { (uintptr_t)t, (uintptr_t)g_font, (uintptr_t)g_scheme,
        (uintptr_t)(active_tab() ? active_tab()->id : 0), (uintptr_t)t->cols, (uintptr_t)t->rows,
        (uintptr_t)g_ntabs, (uintptr_t)g_active, (uintptr_t)g_settings_open, (uintptr_t)g_settings_tab,
        (uintptr_t)g_tab_offset, (uintptr_t)g_setting_offset[g_settings_tab],
        (uintptr_t)g_chrome.tab_capacity, (uintptr_t)g_chrome.list_capacity,
        (uintptr_t)g_chrome.tab_count, (uintptr_t)g_chrome.list_count };
    return chrome_motion_hash(UINT64_C(1469598103934665603), identity, sizeof(identity));
}
static ChromePlacementScore chrome_motion_score(const Cell *cells, const int *source,
                                                int cols, int rows, int side, int row,
                                                int height, int original_side, int original_row, const Term *term) {
    ChromePlacementScore score;
    int left = side == 1 ? cols - SB_COLS : 0, right = left + SB_COLS;
    memset(&score, 0, sizeof(score));
    score.side = side; score.row = row; score.switch_side = side != original_side;
    score.distance = abs(row - original_row);
    for (int y = source ? 0 : row; y < (source ? rows : row + height); y++) {
        int moved = 0, first = cols, last = -1;
        for (int x = 0; x < cols; x++) {
            int i = y * cols + x;
            if (!cell_covers_chrome(cells + i)) continue;
            if (source && source[i] != i) { score.moved_cells++; moved = 1; }
            if (x < first) first = x;
            last = x;
            if (y >= row && y < row + height && (x < left || x >= right) &&
                x >= left - 2 && x < right + 2) score.padding_cells++;
        }
        if (!term->view && term->m_cursor_vis && y == term->cy && term->cx >= 0 && term->cx < cols) {
            int cursor_end = term->cx + (cells[y * cols + term->cx].wflag == W_WIDE ? 2 : 1);
            if (term->cx < first) first = term->cx;
            if (cursor_end - 1 > last) last = cursor_end - 1;
        }
        score.moved_rows += moved;
        if (first < left) first = left;
        if (last >= right) last = right - 1;
        if (last >= first) {
            if (y >= row && y < row + height) {
                score.collision_rows++; score.collision_cells += last - first + 1;
            } else if (y >= row - 2 && y < row + height + 2) score.padding_cells += last - first + 1;
        }
    }
    return score;
}

/* All final and candidate layouts use the same panel pass. Recognized panel
 * continuations have explicit indentation, so their row boundary is a word
 * separator; no VT-only metadata is needed to reproduce the exact result. */
static int chrome_display_build(ChromeReflow *out, const Cell *input, Term *t,
                                 const unsigned char *protected_rows, int side,
                                 int dock_x, int dock_y, int dock_width, int dock_height) {
    int cursor = !t->view && t->m_cursor_vis && t->cx >= 0 && t->cx < t->cols &&
                 t->cy >= 0 && t->cy < t->rows ? t->cy*t->cols+t->cx : -1;
    if (!chrome_reflow_build_checked(out, input, t->cols, t->rows, protected_rows,
                                     side, dock_x, dock_y, dock_width, dock_height, 3,
                                     chrome_background_safe)) return 0;
    chrome_panel_reflow_apply(out, input, t->cols, t->rows, NULL, cursor,
                              side, dock_x, dock_y, dock_width, dock_height,
                              DEF_BG, chrome_panel_background, &out->cursor);
    return 1;
}
static ChromePlacementScore chrome_motion_display_score(const ChromeReflow *out, Term *t,
                                                        int side, int row, int height) {
    Term cursor = *t;
    if (out->cursor >= 0) { cursor.cx = out->cursor % t->cols; cursor.cy = out->cursor / t->cols; }
    return chrome_motion_score(out->cells, out->source, t->cols, t->rows,
                                side, row, height, g_tab_underlay, g_chrome.top, &cursor);
}

/* Rank cheap raw-grid candidates first, then run the exact reflow engine on
 * a small scratch shortlist. No probe may change display state or pagination.
 * The existing menu capacities must survive the move: shrinking a list is not
 * an improvement. The work budget also bounds unusually large terminals. */
static int chrome_motion_search(Term *t, const Cell *input, const unsigned char *protected_rows,
                                 ChromePlacementScore *winner) {
    enum { CANDIDATES = 24 };
    ChromePlacementScore choices[CANDIDATES], bins[3][6], baseline;
    int bin_valid[3][6] = {{0}};
    unsigned char *fixed = NULL;
    ChromeReflow scratch = {0};
    int counts[3] = {0}, height = g_chrome.bottom - g_chrome.top, side = g_tab_underlay;
    int *prefix = NULL;
    uint32_t began = GetTickCount();
    if (height <= 0 || (size_t)t->cols * t->rows > 48000) return 0;
    if (!chrome_display_build(&scratch, input, t, protected_rows,
            side, side == 1 ? t->cols - SB_COLS : 0, g_chrome.top, SB_COLS, height)) return 0;
    baseline = chrome_motion_display_score(&scratch, t, side, g_chrome.top, height);
    chrome_reflow_free(&scratch);
    *winner = baseline;
    if (!baseline.collision_rows) return 0;
    /* Prefix sums make all vertical placements O(rows * columns), rather
     * than rescanning the full menu rectangle for every possible position. */
    prefix = (int *)calloc((size_t)(t->rows + 1) * 10, sizeof(int));
    fixed = (unsigned char *)calloc((size_t)t->rows, 1);
    if (!prefix || !fixed) { free(prefix); free(fixed); return 0; }
    {   /* An optimistic lower bound separates immutable intrusions from
         * rows which might reflow. Otherwise many tiny protected rows can
         * crowd a much better prose region out of the bounded shortlist. */
        int fenced = 0, tree_indent = -1;
        ChromeReflowRow *info = (ChromeReflowRow *)calloc((size_t)t->rows, sizeof(*info));
        if (!info) { free(prefix); free(fixed); return 0; }
        for (int row = 0; row < t->rows; row++) {
            const Cell *line = input + row * t->cols;
            ChromeReflowRow *r = info+row;
            *r = cr_classify(line, t->cols, protected_rows && protected_rows[row],
                             fenced, chrome_background_safe, tree_indent);
            if (r->type == CR_RECORD && r->record == CR_RECORD_TREE) {
                if (tree_indent < 0) tree_indent = r->body;
            } else if (!(r->type == CR_RECORD && r->record != CR_RECORD_FOOTER &&
                         tree_indent >= 0 && r->indent >= tree_indent)) tree_indent = -1;
            if (cr_fence(line, r->indent, r->end)) fenced = !fenced;
        }
        cr_classify_records(info, input, t->cols, t->rows, protected_rows);
        for (int row = 0; row < t->rows; row++) fixed[row] = info[row].type == CR_PROTECTED;
        free(info);
        {   /* The panel pass deliberately protects its owned rows from the
             * generic engine, but can still clear them in a scratch plan. */
            ChromePanel panel; int scan = 0;
            while (cp_next(input, t->cols, t->rows, &scan, DEF_BG, chrome_panel_background, &panel))
                for (int row = panel.top; row < panel.bottom; row++) fixed[row] = 0;
        }
    }
    for (int lane = 1; lane <= 2; lane++) {
        int *occupied = prefix + (lane - 1) * 5 * (t->rows + 1);
        int *covered = occupied + t->rows + 1, *padding = covered + t->rows + 1;
        int *fixed_rows = padding + t->rows + 1, *fixed_cells = fixed_rows + t->rows + 1;
        for (int row = 0; row < t->rows; row++) {
            ChromePlacementScore line = chrome_motion_score(input, NULL, t->cols, t->rows,
                                                            lane, row, 1, side, g_chrome.top, t);
            occupied[row+1] = occupied[row] + line.collision_rows;
            covered[row+1] = covered[row] + line.collision_cells;
            padding[row+1] = padding[row] + line.padding_cells;
            fixed_rows[row+1] = fixed_rows[row] + (fixed[row] ? line.collision_rows : 0);
            fixed_cells[row+1] = fixed_cells[row] + (fixed[row] ? line.collision_cells : 0);
        }
    }
    free(fixed);
    for (int lane = 1; lane <= 2; lane++) for (int row = 0; row + height <= t->rows; row++) {
        ChromePlacementScore score;
        ChromeLayout measure;
        int position, base = (lane - 1) * (CANDIDATES / 2), count = counts[lane], bin;
        if (lane == side && row == g_chrome.top) continue;
        measure = chrome_primary_measure(t->rows, first_row(), g_ntabs, g_settings_open,
                                  lane, row, chrome_item_total());
        if (measure.top != row || measure.tab_capacity != g_chrome.tab_capacity ||
            measure.list_capacity != g_chrome.list_capacity) continue;
        {
            int *occupied = prefix + (lane - 1) * 5 * (t->rows + 1);
            int *covered = occupied + t->rows + 1, *padding = covered + t->rows + 1;
            int *fixed_rows = padding + t->rows + 1, *fixed_cells = fixed_rows + t->rows + 1;
            memset(&score, 0, sizeof(score));
            score.side = lane; score.row = row; score.switch_side = lane != side;
            score.distance = abs(row - g_chrome.top);
            score.collision_rows = fixed_rows[row+height] - fixed_rows[row];
            score.collision_cells = fixed_cells[row+height] - fixed_cells[row];
            score.moved_rows = occupied[row+height] - occupied[row] - score.collision_rows;
            score.moved_cells = covered[row+height] - covered[row] - score.collision_cells;
            score.padding_cells = padding[row+height] - padding[row] +
                covered[row] - covered[chrome_max(0, row-2)] +
                covered[chrome_min(t->rows, row+height+2)] - covered[row+height];
        }
        bin = row * 6 / (t->rows - height + 1);
        if (!bin_valid[lane][bin] || cm_score_better(&score, &bins[lane][bin])) {
            bins[lane][bin] = score; bin_valid[lane][bin] = 1;
        }
        for (position = 0; position < count; position++)
            if (cm_score_better(&score, choices + base + position)) break;
        if (position >= CANDIDATES / 4) continue;
        if (count < CANDIDATES / 4) count++;
        memmove(choices + base + position + 1, choices + base + position,
                (size_t)(count - position - 1) * sizeof(*choices));
        choices[base + position] = score;
        counts[lane] = count;
    }
    free(prefix);
    /* Six raw favorites and six vertical regions on each edge avoid a
     * shortlist of adjacent protected rows excluding a reflowable region.
     * Alternate edges and raw/region probes within the work budget. */
    for (int i = 0; i < CANDIDATES; i++) {
        int lane = i % 2 ? 3 - side : side, index = i / 4, regional = (i / 2) % 2;
        ChromePlacementScore score;
        if (regional) {
            int duplicate = 0;
            if (!bin_valid[lane][index]) continue;
            score = bins[lane][index];
            for (int j = 0; j < counts[lane]; j++)
                if (choices[(lane-1)*(CANDIDATES/2)+j].row == score.row) duplicate = 1;
            if (duplicate) continue;
        } else {
            if (index >= counts[lane]) continue;
            score = choices[(lane - 1) * (CANDIDATES / 2) + index];
        }
        if (!chrome_display_build(&scratch, input, t, protected_rows,
                score.side, score.side == 1 ? t->cols - SB_COLS : 0, score.row, SB_COLS, height)) continue;
        score = chrome_motion_display_score(&scratch, t, score.side, score.row, height);
        if (cm_score_better(&score, winner)) *winner = score;
        chrome_reflow_free(&scratch);
        if (i >= 1 && (uint32_t)(GetTickCount() - began) >= 12) break;
    }
    return cm_score_worth_move(winner, &baseline);
}
static void chrome_autoplace_prepare(Term *t, const Cell *input, const unsigned char *protected_rows) {
    uint32_t now = GetTickCount();
    uint64_t hash = chrome_motion_hash(UINT64_C(1469598103934665603), input,
                                       (size_t)t->cols * t->rows * sizeof(Cell));
    int cursor[] = {t->cx, t->cy, t->m_cursor_vis, t->view, t->hcount};
    int blocked, held, action;
    const char *test = getenv("HARDCORE_TEST_CHROME_MOTION");
    if (getenv("HARDCORE_CTL") && test && !strcmp(test, "0")) {
        cm_reset(&g_chrome_motion); g_chrome_motion_alpha = 1;
        plat_kill_timer(TIMER_CHROME); return;
    }
    hash = chrome_motion_hash(hash, cursor, sizeof(cursor));
    hash = chrome_motion_hash(hash, protected_rows, (size_t)t->rows);
    held = (!g_focused && !(getenv("HARDCORE_CTL") && test && !strcmp(test, "1"))) ||
           g_chrome_drag || chrome_hovered() || chrome_preview_active() || g_sel_on || g_sel_drag || g_sb_drag;
#ifndef _WIN32
    held=held || chrome_any_tv_hovered() || g_cutie_focus_id>0 || g_tv_drag_slot>=0;
#endif
    blocked = held || (uint32_t)(now - g_chrome_input_at) < 700;
    action = cm_observe(&g_chrome_motion, now, hash, chrome_motion_context(t),
                         g_tab_underlay, g_chrome.top, blocked);
    if (action == CM_SEARCH) {
        ChromePlacementScore winner;
        if (chrome_motion_search(t, input, protected_rows, &winner))
            cm_start(&g_chrome_motion, now, &winner);
    } else if (action == CM_COMMIT) {
        /* The only geometry mutation in a transition, on its zero-opacity
         * frame. Reflow below is rebuilt once against this exact geometry. */
        g_tab_underlay = g_chrome_motion.side;
        g_dock_row = g_chrome_motion.row;
        tab_layout();
        g_chrome_motion.context = chrome_motion_context(t);
    }
    g_chrome_motion_alpha = g_chrome_motion.alpha;
    if (held) plat_kill_timer(TIMER_CHROME);
    else if (blocked) plat_timer(TIMER_CHROME, (int)(700 - (uint32_t)(now - g_chrome_input_at)));
    else {
        unsigned wait = cm_next_tick(&g_chrome_motion, now);
        if (wait) plat_timer(TIMER_CHROME, (int)wait);
        else plat_kill_timer(TIMER_CHROME);
    }
}

static void chrome_prepare_display_internal(Term *t, int dock_x, int dock_y, int dock_height, int automatic) {
    Cell *input;
    unsigned char *protected_rows;
    int x, y;
    chrome_reflow_free(&g_display);
    g_display_term = NULL;
    if (!g_tab_underlay || !g_chrome_reflow || t->cols <= SB_COLS + 4 ||
        (display_tab() && &display_tab()->t == t && display_tab()->kind == TK_BROWSER)) {
        if (automatic) {
            cm_reset(&g_chrome_motion); g_chrome_motion_alpha = 1;
            plat_kill_timer(TIMER_CHROME);
        }
        return;
    }
    input = (Cell *)calloc((size_t)t->cols * t->rows, sizeof(Cell));
    protected_rows = (unsigned char *)calloc((size_t)t->rows, 1);
    if (!input || !protected_rows) { free(input); free(protected_rows); return; }
    for (y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n, wrapped;
        term_line(t, t->hcount - t->view + y, &cells, &n, &wrapped);
        if (n > t->cols) n = t->cols;
        for (x = 0; x < t->cols; x++) input[y*t->cols+x].ch = ' ';
        if (n > 0) memcpy(input + y*t->cols, cells, (size_t)n * sizeof(Cell));
    }
    /* VT autowrap can split a word across physical rows. The prose engine
       treats row breaks as word boundaries, so protect both halves instead
       of inventing a space. Also inspect the row just above the viewport. */
    for (y = 0; y < t->rows; y++) {
        const Cell *previous;
        int n, wrapped, line = t->hcount - t->view + y - 1;
        if (line < 0) continue;
        term_line(t, line, &previous, &n, &wrapped);
        if (wrapped > 0 && n >= wrapped && previous[wrapped-1].ch > ' ' && input[y*t->cols].ch > ' ') {
            int hyphen_join = wrapped >= 2 && previous[wrapped-1].ch == '-' &&
                (cr_letter(previous[wrapped-2].ch) || (previous[wrapped-2].ch >= '0' && previous[wrapped-2].ch <= '9')) &&
                cr_letter(input[y*t->cols].ch);
            if (hyphen_join) continue; /* explicit classifier joins this one known boundary */
            protected_rows[y] = 1;
            if (y) protected_rows[y-1] = 1;
        }
    }
    {   /* A continuation below the viewport cannot be classified safely. */
        const Cell *last; int n, wrapped;
        term_line(t, t->hcount - t->view + t->rows - 1, &last, &n, &wrapped);
        if (wrapped > 0 && n >= wrapped && last[wrapped-1].ch > ' ') protected_rows[t->rows-1] = 1;
    }
    /* Never move an input/cursor row, even when the application hides it. */
    if (t->cy + t->view >= 0 && t->cy + t->view < t->rows)
        protected_rows[t->cy + t->view] = 1;
    chrome_panel_protect(input, t->cols, t->rows, DEF_BG, chrome_panel_background, protected_rows);
    if (automatic) {
        chrome_autoplace_prepare(t, input, protected_rows);
        dock_x = sidebar_x()/CW; dock_y = g_chrome.top;
        dock_height = g_chrome.bottom - g_chrome.top;
    }
    if (chrome_display_build(&g_display, input, t, protected_rows,
                             g_tab_underlay, dock_x, dock_y, SB_COLS, dock_height)) {
        g_display_term = t;
        g_display_view = t->view;
        g_display_hcount = t->hcount;
    }
    free(input); free(protected_rows);
}

static void chrome_prepare_display_at(Term *t, int dock_x, int dock_y, int dock_height) {
    chrome_prepare_display_internal(t, dock_x, dock_y, dock_height, 0);
}
static void chrome_prepare_display(Term *t) {
#ifndef _WIN32
    chrome_tv_clock();
#endif
    tab_layout();
    chrome_prepare_display_internal(t, sidebar_x()/CW, g_chrome.top, g_chrome.bottom-g_chrome.top, 1);
#ifndef _WIN32
    chrome_tv_prepare(t);
    mac_camera_layout(t);
    mac_music_layout(t);
#endif
}

static void display_cursor_position(const Term *t, int *col, int *row) {
    *col = t->cx; *row = t->cy;
    if (g_display_term == t && g_display.cells && g_display.cursor >= 0 &&
        g_display.cursor < t->cols*t->rows && g_display.cols == t->cols && g_display.rows == t->rows &&
        g_display_view == t->view && g_display_hcount == t->hcount) {
        *col = g_display.cursor % t->cols;
        *row = g_display.cursor / t->cols;
    }
}

static void display_line(const Term *t, int row, const Cell **cells, int *n) {
    if (g_display_term == t && g_display.cells) {
        *cells = g_display.cells + row*t->cols;
        *n = t->cols;
    } else {
        int wrapped, line = t->hcount - t->view + row;
        term_line(t, line, cells, n, &wrapped);
        if (line >= t->hcount && line - t->hcount < t->rows && t->rlen[t->alt][line-t->hcount] < *n)
            *n = t->rlen[t->alt][line-t->hcount];
        if (*n > t->cols) *n = t->cols;
    }
}

static int display_selected(const Term *t, int col, int row) {
    long long r0, r1, sr;
    int c0, c1, source;
    if (!g_sel_on || !active_tab() || t != &active_tab()->t) return 0;
    sel_norm(&r0, &c0, &r1, &c1);
    source = display_source(t, col, row);
    sr = term_base(t) + t->hcount - t->view + source / t->cols;
    col = source % t->cols;
    return sr >= r0 && sr <= r1 && (sr != r0 || col >= c0) && (sr != r1 || col <= c1);
}

static void chrome_draw_markers(Term *t) {
    int y;
    if (!g_chrome_debug || g_display_term != t || !g_display.cells) return;
    for (y = 0; y < t->rows; y++) {
        int col = g_display.markers[y];
        float x, top, mid;
        if (col < 0) continue;
        x = (float)(col*CW); top = (float)(y*CH); mid = top + CH/2;
        /* A small arrow into a stem at the right edge of existing indentation. */
        r_color(g_look.text_lo, .65f);
        quad(x+CW-1, top+2, x+CW, top+CH-2);
        quad(x+1, mid, x+CW-2, mid+1);
        quad(x+CW-4, mid-2, x+CW-3, mid+3);
        quad(x+CW-3, mid-1, x+CW-2, mid+2);
    }
}

#include "hardcore_tv_layout.h"
