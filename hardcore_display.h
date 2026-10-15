/* Display-only chrome reflow. The terminal and scrollback remain authoritative;
   moved cells carry source addresses so selection and copying use real text. */
static int chrome_background_safe(const Cell *c) {
    uint32_t fg, bg;
    cell_colors(c, &fg, &bg);
    return bg == DEF_BG;
}
static void chrome_prepare_display(Term *t) {
    Cell *input;
    unsigned char *protected_rows;
    int x, y;
    chrome_reflow_free(&g_display);
    g_display_term = NULL;
    if (!g_tab_underlay || !g_chrome_reflow || t->m_mouse || t->cols <= SB_COLS + 4) return;
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
    tab_layout();
    if (chrome_reflow_build_checked(&g_display, input, t->cols, t->rows, protected_rows,
                            g_tab_underlay, sidebar_x()/CW, chrome_top_row(), SB_COLS,
                            chrome_bottom_row()-chrome_top_row(), 3, chrome_background_safe)) {
        g_display_term = t;
        g_display_view = t->view;
        g_display_hcount = t->hcount;
    }
    free(input); free(protected_rows);
}

static int display_selected(const Term *t, int col, int row) {
    long long r0, r1, sr;
    int c0, c1, source;
    if (!g_sel_on) return 0;
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
