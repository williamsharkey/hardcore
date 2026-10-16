/* A narrow, explicit terminal toolbar is an obstacle, never paragraph text.
 * Return its ink interval [left,right); the caller reserves its own gutter. */
#ifndef HARDCORE_BADGE_REFLOW_H
#define HARDCORE_BADGE_REFLOW_H
static int chrome_badge_span(const Cell *input, int cols, int row, int *left, int *right) {
    const Cell *line = input + (size_t)row * cols;
    static const char body[] = "Back to bottom";
    int first = 0, last = cols, x, i;
    *left = *right = 0;
    while (first < last && (line[first].ch == 0 || line[first].ch == ' ')) first++;
    while (last > first && (line[last-1].ch == 0 || line[last-1].ch == ' ')) last--;
    if (last-first < 20 || first < 4 || last > cols-2) return 0;
    if (line[first].ch != 0x2193) return 0;
    x = first+1;
    if (line[x].ch != ' ') return 0;
    while (x < last && line[x].ch == ' ') x++;
    for (i = 0; body[i]; i++, x++) if (x >= last || line[x].ch != (unsigned char)body[i]) return 0;
    if (x >= last || line[x].ch != ' ') return 0;
    while (x < last && line[x].ch == ' ') x++;
    if (x >= last || line[x++].ch != 0xb7) return 0;
    if (x >= last || line[x].ch != ' ') return 0;
    while (x < last && line[x].ch == ' ') x++;
    if (x+3 != last || line[x].ch != 'e' || line[x+1].ch != 's' || line[x+2].ch != 'c') return 0;
    for (x = 0; x < cols; x++) if (line[x].wflag || (line[x].attr & (16u | 64u))) return 0;
    *left = first; *right = last;
    return 1;
}

/* A floating status is an intact, isolated row with an unusually large left
 * margin and an anchor at the right edge. This is deliberately separate from
 * paragraph wrapping: moving it must not change spacing, line count, styles,
 * links, or the coordinates to which input/selection is mapped. */
static int chrome_float_space(const Cell *cell) {
    return (cell->ch == 0 || cell->ch == ' ') && !cell->wflag && !cell->link &&
           !(cell->attr & (8u | 16u | 32u | 64u | 128u));
}
static int chrome_float_span(const Cell *input, int cols, int row, int *left, int *right) {
    const Cell *line = input + (size_t)row * cols;
    int first = 0, last = cols, letters = 0, known;
    *left = *right = 0;
    while (first < last && chrome_float_space(line+first)) first++;
    while (last > first && chrome_float_space(line+last-1)) last--;
    if (first == last || first < 6) return 0;
    known = chrome_badge_span(input, cols, row, left, right);
    if (!known && (first < cols/4 || last < cols-2 || last-first < 4)) return 0;
    if (!known && (line[first].ch == '$' || line[first].ch == '%' ||
        line[first].ch == '>' || line[first].ch == '#' || line[last-1].ch == ':')) return 0;
    for (int x = 0; x < cols; x++) {
        const Cell *c = line+x;
        if (c->attr & (16u | 64u)) return 0;
        if (c->wflag == 1) {
            if (x+1 >= cols || line[x+1].wflag != 2) return 0;
        } else if (c->wflag == 2) {
            if (!x || line[x-1].wflag != 1) return 0;
        } else if (c->wflag) return 0;
        if (x < first || x >= last) continue;
        if ((c->ch >= 0x2500 && c->ch <= 0x259f) || c->ch == '|' || c->ch == '\t' ||
            (c->ch < 128 && c->ch && strchr("{}[]=;`", (int)c->ch))) return 0;
        if ((c->ch >= 'a' && c->ch <= 'z') || (c->ch >= 'A' && c->ch <= 'Z') ||
            (c->ch >= 0xc0 && c->ch != 0x2022 && c->ch != 0xb7)) letters++;
        /* Several aligned columns are a table, not a movable status. The
         * known badge has an explicit shape and does not need this heuristic. */
        if (!known && x > first && x+1 < last && !c->wflag && !line[x+1].wflag &&
            (c->ch == 0 || c->ch == ' ') && (line[x+1].ch == 0 || line[x+1].ch == ' ')) return 0;
    }
    if (!letters) return 0;
    /* Move a badge's painted padding along with its ink. Leading/trailing
     * canvas whitespace remains in place, including a full-row status fill. */
    while (first && chrome_float_space(line+first-1) && line[first-1].bg != line[0].bg) first--;
    while (last < cols && chrome_float_space(line+last) && line[last].bg != line[0].bg) last++;
    if (first < 4) return 0;
    for (int x = 0; x < first; x++) if (line[x].bg != line[0].bg) return 0;
    for (int x = last; x < cols; x++) if (line[x].bg != line[0].bg) return 0;
    *left = first; *right = last;
    return 1;
}

/* Whole-span movement is bounded to existing blank cells on the same row.
 * Ordinary reflow can have borrowed a known badge's blank wing already; its
 * output must remain untouched. Prefer a two-cell gutter, then a flush fit. */
static int chrome_float_shift(ChromeReflow *out, const Cell *input, int row,
                               int left, int right, int side, int dock_x, int dock_width) {
    int cols = out->cols, length = right-left, target = -1;
    int obstacle_left = cr_bound(dock_x, cols);
    int obstacle_right = cr_bound((long long)dock_x+dock_width, cols);
    const Cell *line = input+(size_t)row*cols;
    const Cell *display = out->cells+(size_t)row*cols;
    if (obstacle_left >= obstacle_right) return 0;
    for (int x = left; x < right; x++)
        if (out->source[row*cols+x] != row*cols+x || memcmp(display+x,line+x,sizeof(Cell))) return 0;
    for (int gap = 2; gap >= 0 && target < 0; gap -= 2) {
        int begin = cr_bound((long long)obstacle_left-gap, cols);
        int end = cr_bound((long long)obstacle_right+gap, cols);
        int before = begin-length, after = end;
        if (right <= begin || left >= end) return 0;
        int choices[2] = {side == 2 ? after : before, side == 2 ? before : after};
        for (int choice = 0; choice < 2 && target < 0; choice++) {
            int candidate = choices[choice], fits = candidate >= 0 && candidate <= cols-length;
            for (int x = candidate; fits && x < candidate+length; x++)
                if ((x < left || x >= right) &&
                    (!chrome_float_space(display+x) || display[x].bg != line[0].bg)) fits = 0;
            if (fits) target = candidate;
        }
    }
    if (target < 0 || target == left) return 0;
    int base = row*cols, first = target < left ? target : left;
    int last = target+length > right ? target+length : right;
    for (int x = first; x < last; x++) {
        out->cells[base+x] = line[0]; out->source[base+x] = base;
    }
    for (int x = 0; x < length; x++) {
        out->cells[base+target+x] = line[left+x];
        out->source[base+target+x] = base+left+x;
    }
    out->markers[row] = target > 0 ? target-1 : -1;
    out->changed++;
    return 1;
}
#endif
