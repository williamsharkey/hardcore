#ifndef HARDCORE_CUTIE_PROJECT_H
#define HARDCORE_CUTIE_PROJECT_H

/* A read-only compact view of a bounded terminal tail. Define Cell first.
 * Neither the terminal grid nor its PTY/scroll position is resized or changed.
 * Source addresses are indices into the caller's flattened input, not copies
 * of terminal coordinates. Keep that input's origin with the resulting view.
 * Initialize HcCutieProjection to zero before its first build/free. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
    HC_CUTIE_MAX_SOURCE_ROWS = 128,
    HC_CUTIE_MAX_SOURCE_CELLS = 48000,
    HC_CUTIE_MAX_COLS = 256,
    HC_CUTIE_MAX_ROWS = 128,
    HC_CUTIE_WIDE = 1,
    HC_CUTIE_CONT = 2
};

typedef struct {
    Cell *cells;
    int *source;
    int cols, rows;
    int cursor;                 /* output index, or -1 when outside this view */
    int total_rows, first_row;  /* compact row count and current viewport */
    int scroll_rows;            /* actual distance above the following view */
} HcCutieProjection;

typedef struct { int start, end; } HcCutieSlice;
typedef struct { HcCutieSlice *rows; int count, capacity, limit; } HcCutiePlan;

static inline void hc_cutie_project_free(HcCutieProjection *out) {
    if (!out) return;
    free(out->cells);
    free(out->source);
    memset(out, 0, sizeof(*out));
    out->cursor = -1;
}

static inline int hc_cutie_blank(const Cell *cell) {
    return (!cell->ch || cell->ch == ' ') && cell->wflag != HC_CUTIE_CONT;
}

static inline int hc_cutie_append(HcCutiePlan *plan, int start, int end) {
    if (plan->count == plan->capacity) {
        int capacity = plan->capacity ? plan->capacity * 2 : 64;
        if (capacity > plan->limit) capacity = plan->limit;
        if (capacity <= plan->count) return 0;
        HcCutieSlice *rows = (HcCutieSlice *)realloc(plan->rows, (size_t)capacity * sizeof(*rows));
        if (!rows) return 0;
        plan->rows = rows;
        plan->capacity = capacity;
    }
    plan->rows[plan->count++] = (HcCutieSlice){start, end};
    return 1;
}

/* Invalid dimensions or malformed wide pairs fail without replacing out.
 * Width must accommodate a complete wide glyph. Callers should provide only
 * full physical source rows; an incomplete pair cannot be guessed safely. */
static inline int hc_cutie_project_build(HcCutieProjection *out, const Cell *input,
        int source_cols, int source_rows, int cursor_index, int width, int height,
        int scroll_rows) {
    HcCutieProjection next = {0};
    HcCutiePlan plan = {0};
    int pending_blank = -1, cursor_row = -1;
    if (!out || !input || source_cols < 1 || source_rows < 1 ||
        source_rows > HC_CUTIE_MAX_SOURCE_ROWS ||
        source_cols > HC_CUTIE_MAX_SOURCE_CELLS / source_rows ||
        width < 2 || width > HC_CUTIE_MAX_COLS || height < 1 || height > HC_CUTIE_MAX_ROWS)
        return 0;
    int source_count = source_cols * source_rows;
    if (cursor_index < -1 || cursor_index >= source_count) return 0;
    plan.limit = source_count;
    next.cols = width;
    next.rows = height;
    next.cursor = -1;
    for (int row = 0; row < source_rows; row++) {
        int base = row * source_cols, end = 0;
        for (int col = 0; col < source_cols; col++) {
            const Cell *cell = input + base + col;
            if (cell->wflag == HC_CUTIE_WIDE &&
                (col + 1 >= source_cols || cell[1].wflag != HC_CUTIE_CONT)) goto failed;
            if (cell->wflag == HC_CUTIE_CONT &&
                (!col || cell[-1].wflag != HC_CUTIE_WIDE)) goto failed;
            if (!hc_cutie_blank(cell)) end = col + 1;
        }
        /* Blank cursor cells are real positions, even beyond the last letter
         * of a shell prompt or in the middle of an otherwise empty input. */
        if (cursor_index >= base && cursor_index < base + source_cols &&
            end <= cursor_index - base) end = cursor_index - base + 1;
        if (!end) {
            if (pending_blank < 0) pending_blank = base;
            continue;
        }
        if (pending_blank >= 0 && plan.count) {
            /* One physical blank separator survives. Copy its visible cells
             * so its style survives too; unused horizontal blanks need not
             * expand into multiple miniature rows. */
            int blank_width = source_cols < width ? source_cols : width;
            if (!hc_cutie_append(&plan, pending_blank, pending_blank + blank_width)) goto failed;
        }
        pending_blank = -1;
        for (int start = base; start < base + end;) {
            int cut = start + width, last_space = -1;
            if (cut >= base + end) cut = base + end;
            else {
                /* Never leave the leading half of a wide glyph at the right
                 * edge. Its continuation travels with it onto the next row. */
                if (input[cut].wflag == HC_CUTIE_CONT) cut--;
                for (int at = start + 1; at < cut; at++)
                    if (hc_cutie_blank(input + at)) last_space = at + 1;
                if (last_space > start) cut = last_space;
            }
            if (cut <= start || !hc_cutie_append(&plan, start, cut)) goto failed;
            if (cursor_index >= start && cursor_index < cut) cursor_row = plan.count - 1;
            start = cut;
        }
    }
    /* Decorative/status rows are deliberately retained: a terminal cell
     * alone cannot prove they are disposable. The live cursor, when supplied,
     * anchors the view so a footer below it cannot displace an editable draft. */
    next.total_rows = plan.count;
    int anchor = cursor_row >= 0 ? cursor_row : plan.count - 1;
    int follow = anchor >= height ? anchor - height + 1 : 0;
    if (scroll_rows < 0) scroll_rows = 0;
    next.scroll_rows = scroll_rows < follow ? scroll_rows : follow;
    next.first_row = follow - next.scroll_rows;
    size_t count = (size_t)width * (size_t)height;
    next.cells = (Cell *)calloc(count, sizeof(*next.cells));
    next.source = (int *)malloc(count * sizeof(*next.source));
    if (!next.cells || !next.source) goto failed;
    for (size_t i = 0; i < count; i++) {
        next.cells[i].ch = ' ';
        next.source[i] = -1;
    }
    for (int row = 0; row < height && next.first_row + row < plan.count; row++) {
        HcCutieSlice slice = plan.rows[next.first_row + row];
        for (int at = slice.start; at < slice.end; at++) {
            int destination = row * width + at - slice.start;
            next.cells[destination] = input[at];
            next.source[destination] = at;
            if (at == cursor_index) next.cursor = destination;
        }
    }
    free(plan.rows);
    hc_cutie_project_free(out);
    *out = next;
    return 1;
failed:
    free(plan.rows);
    hc_cutie_project_free(&next);
    return 0;
}

#endif
