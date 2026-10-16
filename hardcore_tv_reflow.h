/* Transactional, bounded composition of a secondary TV's display-only reflow.
 * Include after Cell/ChromeReflow and hardcore_reflow.h. The builder stages an
 * independent output; failed preservation checks leave the old display intact. */
#ifndef HARDCORE_TV_REFLOW_H
#define HARDCORE_TV_REFLOW_H
#include "hardcore_tv.h"
#include <stdlib.h>
#include <string.h>
enum { HC_TV_REFLOW_MAX_CELLS = 48000 };
typedef int (*HcTvReflowInk)(const Cell *cell, void *ctx);
typedef int (*HcTvReflowBuild)(ChromeReflow *out, const Cell *input, int cols, int rows,
                              const unsigned char *protected_rows, int cursor_source,
                              HcTvRect obstacle, void *ctx);
static inline void hc_tv_reflow_discard(ChromeReflow *candidate, const ChromeReflow *original) {
    /* A builder is required to return fresh allocations. Guard the caller's
     * authoritative display even when a faulty callback violates that rule. */
    if (candidate->cells == original->cells) candidate->cells = NULL;
    if (candidate->source == original->source) candidate->source = NULL;
    if (candidate->markers == original->markers) candidate->markers = NULL;
    chrome_reflow_free(candidate);
}
static inline int hc_tv_reflow_compose(ChromeReflow *display, HcTvRect next,
                                       const HcTvRect *prior, int prior_count, int cursor_source,
                                       HcTvReflowBuild build, HcTvReflowInk ink, void *ctx) {
    ChromeReflow candidate = {0};
    unsigned char *protected_rows = NULL, *seen = NULL;
    int count, cols, rows, cursor = -1, accepted = 0;
    if (!display || !display->cells || !display->source || !build || !ink ||
        display->cols < 1 || display->rows < 1 || prior_count < 0 || prior_count > HC_TV_MAX_SLOTS ||
        (prior_count && !prior) || (int64_t)display->cols*display->rows > HC_TV_REFLOW_MAX_CELLS) return 0;
    cols = display->cols; rows = display->rows; count = cols*rows;
    if (!hc_tv_rect_fits(next, cols, rows) || cursor_source < -1 || cursor_source >= count) return 0;
    for (int p = 0; p < prior_count; p++)
        if (!hc_tv_rect_fits(prior[p], cols, rows) || hc_tv_rect_overlap(next, prior[p])) return 0;
    protected_rows = (unsigned char *)calloc((size_t)rows, 1);
    seen = (unsigned char *)calloc((size_t)count, 1);
    if (!protected_rows || !seen) goto done;
    for (int p = 0; p < prior_count; p++)
        for (int y = prior[p].y; y < prior[p].y+prior[p].h; y++) protected_rows[y] = 1;
    if (cursor_source >= 0) protected_rows[cursor_source/cols] = 1;
    candidate.cursor = -1;
    if (!build(&candidate, display->cells, cols, rows, protected_rows, cursor_source, next, ctx) ||
        !candidate.cells || !candidate.source || candidate.cols != cols || candidate.rows != rows ||
        candidate.cells == display->cells || candidate.source == display->source ||
        (candidate.markers && candidate.markers == display->markers)) goto done;
    /* Every foreground cell must survive exactly once with identical style,
     * hyperlink, and width metadata. New synthetic spacing may repeat an
     * address; it cannot duplicate or erase foreground. */
    for (int i = 0; i < count; i++) {
        int source = candidate.source[i];
        if (source < 0 || source >= count || display->source[source] < 0 || display->source[source] >= count) goto done;
        if (ink(candidate.cells+i, ctx)) {
            if (!ink(display->cells+source, ctx) || seen[source] ||
                memcmp(candidate.cells+i, display->cells+source, sizeof(Cell))) goto done;
            seen[source] = 1;
        }
    }
    for (int i = 0; i < count; i++) if (ink(display->cells+i, ctx) && !seen[i]) goto done;
    /* Existing intrusions can remain, but no prior TV may gain, lose, or
     * change foreground because a later TV tried to reflow around itself. */
    for (int p = 0; p < prior_count; p++)
        for (int y = prior[p].y; y < prior[p].y+prior[p].h; y++)
            for (int x = prior[p].x; x < prior[p].x+prior[p].w; x++) {
                int i = y*cols+x;
                if ((ink(display->cells+i, ctx) || ink(candidate.cells+i, ctx)) &&
                    (candidate.source[i] != i || memcmp(candidate.cells+i, display->cells+i, sizeof(Cell)))) goto done;
            }
    if (cursor_source >= 0) {
        if (candidate.cursor >= 0) cursor = candidate.cursor;
        else if (candidate.source[cursor_source] == cursor_source &&
                 !memcmp(candidate.cells+cursor_source, display->cells+cursor_source, sizeof(Cell))) cursor = cursor_source;
        else {
            for (int i = 0; i < count; i++)
                if (candidate.source[i] == cursor_source &&
                    !memcmp(candidate.cells+i, display->cells+cursor_source, sizeof(Cell))) {
                    if (cursor >= 0) goto done; /* ambiguous synthetic aliases */
                    cursor = i;
                }
        }
        if (cursor < 0 || cursor >= count || candidate.source[cursor] != cursor_source ||
            memcmp(candidate.cells+cursor, display->cells+cursor_source, sizeof(Cell))) goto done;
        /* A stationary blank cursor is an obstacle too, even though the
         * foreground callback correctly treats ordinary blanks as empty. */
        for (int p = 0; p < prior_count; p++) {
            HcTvRect dot = {cursor%cols, cursor/cols, display->cells[cursor_source].wflag == 1 ? 2 : 1, 1};
            if (hc_tv_rect_overlap(dot, prior[p]) && cursor != cursor_source) goto done;
        }
    }
    if (candidate.markers && display->markers) for (int y = 0; y < rows; y++) {
        int identity = 1;
        for (int x = 0; x < cols; x++) if (candidate.source[y*cols+x] != y*cols+x) identity = 0;
        if (candidate.markers[y] < 0 && identity &&
            !memcmp(candidate.cells+y*cols, display->cells+y*cols, (size_t)cols*sizeof(Cell)))
            candidate.markers[y] = display->markers[y];
    }
    for (int i = 0; i < count; i++) candidate.source[i] = display->source[candidate.source[i]];
    candidate.cursor = cursor;
    candidate.changed += display->changed;
    chrome_reflow_free(display);
    *display = candidate;
    memset(&candidate, 0, sizeof(candidate));
    accepted = 1;
done:
    hc_tv_reflow_discard(&candidate, display);
    free(protected_rows); free(seen);
    return accepted;
}
#endif
