/* Explicit shaded prompt panels own their rows and padding. Their text can
 * move inside that rectangle without changing terminal bytes or consuming a
 * neighboring status/footer row. Source addresses remain exact per cell. */
#ifndef HARDCORE_PANEL_REFLOW_H
#define HARDCORE_PANEL_REFLOW_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t (*ChromePanelBackground)(const Cell *cell);
typedef struct { int top, bottom, first, last, body; uint32_t bg; } ChromePanel;
static int cp_space(const Cell *c) {
    return c->wflag != 2 && !c->link && !(c->attr & (8u | 128u)) &&
           (c->ch == 0 || c->ch == ' ' || c->wflag == 4);
}
static int cp_prompt(uint32_t c) { return c == 0x203a || c == 0x276f || c == 0xbb; }
static int cp_row_background(const Cell *input, int cols, int row, uint32_t *bg,
                              ChromePanelBackground background) {
    const Cell *line = input + (size_t)row * cols;
    *bg = background(line);
    for (int x = 1; x < cols; x++) if (background(line+x) != *bg) return 0;
    return 1;
}
static int cp_end(const Cell *input, int cols, int row) {
    const Cell *line = input + (size_t)row * cols;
    int end = cols;
    while (end && cp_space(line+end-1)) end--;
    return end;
}
static int cp_shape(const Cell *input, int cols, int top, int bottom, uint32_t bg,
                     ChromePanel *panel) {
    int first = top, last = bottom-1, begin = 0, body;
    while (first < bottom && !cp_end(input, cols, first)) first++;
    if (first == bottom) return 0;
    while (begin < cols && cp_space(input + first*cols+begin)) begin++;
    if (begin > 2 || begin+2 >= cols || !cp_prompt(input[first*cols+begin].ch) ||
        !cp_space(input + first*cols+begin+1)) return 0;
    body = begin+2;
    while (last > first && !cp_end(input, cols, last)) last--;
    for (int y = first; y <= last; y++) {
        const Cell *line = input + y*cols;
        int end = cp_end(input, cols, y);
        /* Internal blank rows remain explicit paragraph breaks; padding
         * before the prompt or after the last body line can be borrowed. */
        if (y > first) {
            int indent = 0;
            if (end) {
                while (indent < end && cp_space(line+indent)) indent++;
                if (indent < body || cp_prompt(line[indent].ch)) return 0;
            }
        }
        for (int x = 0; x < cols; x++) {
            if (line[x].wflag == 1) {
                if (x+1 >= cols || line[x+1].wflag != 2) return 0;
            } else if (line[x].wflag == 2) {
                if (!x || line[x-1].wflag != 1) return 0;
            } else if (line[x].wflag && line[x].wflag != 4) return 0;
            if (line[x].attr & (16u | 64u)) return 0;
        }
    }
    for (int y = top; y < bottom; y++) if (!cp_end(input, cols, y))
        for (int x = 0; x < cols; x++)
            if (input[y*cols+x].link || (input[y*cols+x].attr & (8u | 16u | 64u | 128u))) return 0;
    *panel = (ChromePanel){top, bottom, first, last, body, bg};
    return 1;
}
static int cp_next(const Cell *input, int cols, int rows, int *scan, uint32_t default_bg,
                    ChromePanelBackground background, ChromePanel *panel) {
    while (*scan < rows) {
        int top = (*scan)++, bottom;
        uint32_t bg, next;
        if (!cp_row_background(input, cols, top, &bg, background) || bg == default_bg) continue;
        while (*scan < rows && cp_row_background(input, cols, *scan, &next, background) && next == bg) (*scan)++;
        bottom = *scan;
        /* A mixed-color row with the same leading fill is an uncertain
         * continuation, not a safe boundary at which to split a panel. */
        if (bottom < rows && background(input+bottom*cols) == bg) continue;
        if (top && background(input+(top-1)*cols) == bg) continue;
        if (cp_shape(input, cols, top, bottom, bg, panel)) return 1;
    }
    return 0;
}
static void chrome_panel_protect(const Cell *input, int cols, int rows, uint32_t default_bg,
                                  ChromePanelBackground background, unsigned char *protected_rows) {
    int scan = 0;
    ChromePanel panel;
    while (cp_next(input, cols, rows, &scan, default_bg, background, &panel))
        for (int y = panel.top; y < panel.bottom; y++) protected_rows[y] = 1;
}
static int cp_new_row(int *row, int *x, int *limit, int bottom, int cols, int body,
                       int side, int dock_x, int dock_y, int dock_width, int dock_height, int gap) {
    int offset;
    if (++*row >= bottom) return 0;
    cr_lane(*row, cols, side, dock_x, dock_y, dock_width, dock_height, gap, &offset, limit);
    *x = offset+body;
    return *x < *limit;
}
static int cp_pack(const Cell *input, const int *stream, int length, int *positions,
                    const ChromePanel *panel, int cols, int start, int gap,
                    int side, int dock_x, int dock_y, int dock_width, int dock_height,
                    int *prefix) {
    int row = start, offset, limit, x, at = 0;
    cr_lane(row, cols, side, dock_x, dock_y, dock_width, dock_height, gap, &offset, &limit);
    if (offset+panel->body >= limit) return 0;
    *prefix = row*cols+offset;
    x = offset+panel->body;
    while (at < length) {
        int end = at;
        if (stream[at] < 0) {
            positions[at++] = -1;
            if (!cp_new_row(&row, &x, &limit, panel->bottom, cols, panel->body,
                            side, dock_x, dock_y, dock_width, dock_height, gap)) return 0;
            offset = x-panel->body;
            continue;
        }
        /* Keep a separating run with its following word. When a long token
         * must split, retain every cell and never separate a wide glyph. */
        while (end < length && stream[end] >= 0 && cp_space(input+stream[end])) end++;
        while (end < length && stream[end] >= 0 && !cp_space(input+stream[end])) end++;
        if (end == at) end++;
        if (end-at > limit-x && x > offset+panel->body) {
            if (!cp_new_row(&row, &x, &limit, panel->bottom, cols, panel->body,
                            side, dock_x, dock_y, dock_width, dock_height, gap)) return 0;
            offset = x-panel->body;
        }
        while (at < end) {
            int take = end-at;
            if (take > limit-x) take = limit-x;
            if (take && input[stream[at+take-1]].wflag == 1) take--;
            for (int n = 0; n < take; n++, at++, x++) positions[at] = row*cols+x;
            if (at < end) {
                if (!cp_new_row(&row, &x, &limit, panel->bottom, cols, panel->body,
                                side, dock_x, dock_y, dock_width, dock_height, gap)) return 0;
                offset = x-panel->body;
            }
        }
    }
    return 1;
}
/* cursor_source is the original viewport cell, or -1. A successful active
 * panel always returns its exact destination via cursor_display, including
 * the blank cell after the final word. Synthetic padding cannot steal it. */
static int chrome_panel_reflow_apply(ChromeReflow *out, const Cell *input, int cols, int rows,
                                      const int *wrapped_rows, int cursor_source,
                                      int side, int dock_x, int dock_y, int dock_width, int dock_height,
                                      uint32_t default_bg, ChromePanelBackground background,
                                      int *cursor_display) {
    ChromePanel panel;
    int scan = 0, changed = 0;
    (void)wrapped_rows; /* Recognized continuations have explicit indentation,
                        * hence their physical boundary separates words. */
    while (cp_next(input, cols, rows, &scan, default_bg, background, &panel)) {
        int capacity = (panel.bottom-panel.top)*cols, length = 0, last = panel.last;
        int *stream, *positions, cursor_in, prefix = 0, fit = 0, previous_end = -1;
        if (capacity > 48000 || panel.top >= dock_y+dock_height || panel.bottom <= dock_y) continue;
        cursor_in = cursor_source >= panel.top*cols && cursor_source < panel.bottom*cols;
        {
            int intrudes = 0;
            for (int y = panel.top; y < panel.bottom && !intrudes; y++) {
                if (y < dock_y || y >= dock_y+dock_height) continue;
                for (int x = dock_x; x < dock_x+dock_width && x < cols; x++)
                    if (x >= 0 && !cp_space(input+y*cols+x)) intrudes = 1;
                if (cursor_in && cursor_source/cols == y && cursor_source%cols >= dock_x &&
                    cursor_source%cols < dock_x+dock_width) intrudes = 1;
            }
            if (!intrudes) continue;
        }
        if (cursor_in && cursor_source/cols < panel.first) continue;
        if (cursor_in && cursor_source/cols > last) last = cursor_source/cols;
        if (cursor_in && cursor_source%cols < panel.body && cursor_source/cols != panel.first) continue;
        /* A prior reflow must not be overwritten, even if a caller omitted
         * the panel-protection prepass. */
        if (memcmp(out->cells+panel.top*cols, input+panel.top*cols, (size_t)capacity*sizeof(Cell))) continue;
        stream = (int *)malloc((size_t)(capacity+rows) * sizeof(int));
        positions = (int *)malloc((size_t)(capacity+rows) * sizeof(int));
        if (!stream || !positions) { free(stream); free(positions); continue; }
        for (int y = panel.first; y <= last; y++) {
            int original_end = cp_end(input, cols, y), end = original_end;
            if (cursor_in && cursor_source/cols == y && cursor_source%cols >= end) end = cursor_source%cols+1;
            if (y > panel.first && (previous_end <= panel.body || original_end <= panel.body))
                stream[length++] = -1;
            else if (y > panel.first && previous_end >= panel.body && end > panel.body) {
                int previous = (y-1)*cols+previous_end-1;
                if (!cp_space(input+previous) && !cp_space(input+y*cols+panel.body))
                    stream[length++] = previous_end < cols ? previous+1 : y*cols+panel.body-1;
            }
            for (int x = panel.body; x < end; x++) if (input[y*cols+x].wflag != 4) stream[length++] = y*cols+x;
            previous_end = original_end;
        }
        for (int start = panel.first; start >= panel.top && !fit; start--)
            for (int gap = 2; gap >= 0 && !fit; gap -= 2)
                fit = cp_pack(input, stream, length, positions, &panel, cols, start, gap,
                              side, dock_x, dock_y, dock_width, dock_height, &prefix);
        if (fit) {
            int mapped = -1;
            if (cursor_in) {
                if (cursor_source/cols == panel.first && cursor_source%cols < panel.body)
                    mapped = prefix + cursor_source%cols;
                for (int i = 0; i < length; i++) if (stream[i] == cursor_source) mapped = positions[i];
                if (mapped < 0) fit = 0;
            }
            if (fit) {
                Cell blank = input[panel.top*cols];
                blank.ch = ' '; blank.wflag = 0; blank.link = 0; blank.attr &= ~(8u | 16u | 128u);
                for (int i = panel.top*cols; i < panel.bottom*cols; i++) {
                    out->cells[i] = blank; out->source[i] = panel.first*cols+panel.body;
                }
                for (int x = 0; x < panel.body; x++) {
                    out->cells[prefix+x] = input[panel.first*cols+x];
                    out->source[prefix+x] = panel.first*cols+x;
                }
                for (int i = 0; i < length; i++) {
                    if (stream[i] < 0) continue;
                    out->cells[positions[i]] = input[stream[i]];
                    out->source[positions[i]] = stream[i];
                }
                if (cursor_in && cursor_display) *cursor_display = mapped;
                out->changed++; changed++;
            }
        }
        free(stream); free(positions);
    }
    return changed;
}
#endif
