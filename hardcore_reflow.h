/* Display-only local prose reflow. Include after the terminal's Cell type.
 *
 * This deliberately recognizes a small, safe subset of terminal output. A
 * paragraph keeps its own rows and may borrow adjacent empty rows, preferring
 * those below it; unrelated text never moves. Text outside the dock's vertical band
 * uses the full terminal width. The source terminal is never modified.
 *
 * source[display_index] maps each displayed character to its original cell.
 * Synthetic spacing maps to its adjacent word boundary, so mouse/selection
 * coordinates can still refer to the original terminal. markers[row] is an
 * existing indentation cell for a debug arrow, or -1. changed counts accepted
 * paragraphs. Always rebuild from original terminal cells, not prior output.
 *
 * Build requires a zeroed/freed output. Returns 1 on success (including no
 * changes), 0 on invalid dimensions/allocation failure; failure leaves it
 * empty. The caller owns the result until chrome_reflow_free().
 */
#ifndef HARDCORE_REFLOW_H
#define HARDCORE_REFLOW_H

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct ChromeReflow {
    Cell *cells;
    int *source;
    int *markers;
    int cols, rows, changed;
} ChromeReflow;

enum { CR_BLANK, CR_PROSE, CR_PROTECTED, CR_RECORD };
enum { CR_RECORD_NONE, CR_RECORD_CODE, CR_RECORD_PATH, CR_RECORD_TREE, CR_RECORD_COMMAND, CR_RECORD_FOOTER };
enum { CR_REASON_OTHER, CR_REASON_LIST, CR_REASON_UNSAFE };
typedef struct {
    int type, reason, indent, body, end, words, bullet, command, record;
} ChromeReflowRow;
typedef struct { int source, length; } ChromeReflowWord;
typedef struct { int first, count, offset, indent; } ChromeReflowPlacement;
typedef int (*ChromeReflowBackgroundSafe)(const Cell *cell);

static void chrome_reflow_free(ChromeReflow *out) {
    if (!out) return;
    free(out->cells);
    free(out->source);
    free(out->markers);
    memset(out, 0, sizeof(*out));
}

static int cr_letter(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= 0xc0 && c <= 0xff && c != 0xd7 && c != 0xf7);
}

static int cr_space(uint32_t c) { return c == 0 || c == ' '; }

/* Curly quotes, apostrophes, dashes and an ellipsis occur in ordinary CLI
 * prose. Their Cell width flags must still report one cell. Box drawings,
 * combining marks and arbitrary Unicode symbols remain protected. */
static int cr_prose_character(uint32_t c) {
    return (c >= 0x21 && c <= 0x7e) || cr_letter(c) ||
           (c >= 0x2010 && c <= 0x2015) || (c >= 0x2018 && c <= 0x201f) ||
           c == 0x2026 || c == 0x2022;
}

static int cr_word_is(const Cell *line, int begin, int end, const char *word) {
    int x;
    size_t n = strlen(word);
    if (n != (size_t)(end - begin)) return 0;
    for (x = begin; x < end; x++)
        if (line[x].ch != (unsigned char)word[x - begin]) return 0;
    return 1;
}

static int cr_starts(const Cell *line, int begin, int end, const char *text) {
    size_t x, n = strlen(text);
    if (n > (size_t)(end - begin)) return 0;
    for (x = 0; x < n; x++)
        if (line[begin + (int)x].ch != (unsigned char)text[x]) return 0;
    return 1;
}

static int cr_fence(const Cell *line, int begin, int end) {
    return cr_starts(line, begin, end, "```") || cr_starts(line, begin, end, "~~~");
}

/* Codex's compact diff gutter is "3 +text", including for non-code text.
 * The colored row supplies the missing separator's provenance; ordinary
 * arithmetic such as "3 +14" must not become a numbered diff. */
static int cr_diff_background(const Cell *line, int begin, int end) {
    uint32_t bg = line[begin].bg;
    if (!bg) return 0;
    for (int x = begin+1; x < end; x++) if (line[x].bg != bg) return 0;
    return 1;
}

static int cr_url_start(const Cell *line, int begin, int end) {
    return cr_starts(line, begin, end, "https://") ||
           cr_starts(line, begin, end, "http://") ||
           cr_starts(line, begin, end, "file://");
}

/* A tool tree can contain literal search expressions and boolean operators.
 * Do not grant the same treatment to arbitrary pipe-separated output: the
 * inherited tree indentation alone is not evidence that a table may wrap. */
static int cr_tree_literal_pipes(const Cell *line, const ChromeReflowRow *r) {
    if (cr_starts(line, r->body, r->end, "Search ") ||
        cr_starts(line, r->body, r->end, "Ran ")) return 1;
    int syntax = 0, pipe = 0;
    for (int x = r->body; x < r->end; x++) {
        uint32_t c = line[x].ch;
        if (c && c < 128 && strchr("();{}=", (int)c)) syntax = 1;
        if (c == '|') {
            pipe = 1;
            if (!(x > r->body && line[x-1].ch == '|') &&
                !(x+1 < r->end && line[x+1].ch == '|')) return 0;
        }
    }
    return syntax && pipe;
}

/* Explicit line identities permit literal wrapping without treating code as
 * prose. A numbered diff keeps its number/sign on its own first row; a tool
 * tree keeps its prefix there. Tables and complete box frames stay protected. */
static int cr_record_shape(const Cell *line, ChromeReflowRow *r) {
    int x = r->indent, digits = x, gap, dot = 0;
    while (digits < r->end && line[digits].ch >= '0' && line[digits].ch <= '9') digits++;
    gap = digits;
    while (gap < r->end && cr_space(line[gap].ch)) gap++;
    if (digits > x && gap > digits && gap + 1 < r->end &&
        (line[gap].ch == '+' || line[gap].ch == '-') &&
        (cr_space(line[gap+1].ch) || cr_diff_background(line, x, r->end))) {
        r->body = gap+1;
        while (r->body < r->end && cr_space(line[r->body].ch)) r->body++;
        return CR_RECORD_CODE;
    }
    if (digits > x && gap-digits >= 2 && gap < r->end) {
        for (int k = gap; k < r->end; k++) if (line[k].ch < 128 && strchr("();{}=", (int)line[k].ch)) {
            r->body = gap;
            return CR_RECORD_CODE;
        }
    }
    if (x+1 < r->end && line[x].ch == 0x2514 && cr_space(line[x+1].ch)) {
        r->body = x+2;
        while (r->body < r->end && cr_space(line[r->body].ch)) r->body++;
        return CR_RECORD_TREE;
    }
    if (r->bullet && (cr_starts(line, r->body, r->end, "Ran ") ||
        cr_starts(line, r->body, r->end, "You ran ") || cr_starts(line, r->body, r->end, "Failed (exit ")))
        return CR_RECORD_COMMAND;
    if ((r->bullet && cr_starts(line, r->body, r->end, "Opened ") &&
         cr_url_start(line, r->body+7, r->end)) ||
        (cr_starts(line, r->body, r->end, "Saved to: ") &&
         cr_url_start(line, r->body+10, r->end))) return CR_RECORD_PATH;
    if (x < r->end && (line[x].ch == '/' || cr_starts(line, x, r->end, "~/") ||
        cr_starts(line, x, r->end, "./") || cr_starts(line, x, r->end, "../") ||
        cr_url_start(line, x, r->end))) {
        r->body = x+2;
        return CR_RECORD_PATH;
    }
    /* rg's filename:line: result is one record, including any following code. */
    for (int k = x; k < r->end && !cr_space(line[k].ch); k++) {
        if (line[k].ch == '.' || line[k].ch == '/') dot = 1;
        if (dot && line[k].ch == ':') {
            int n = k+1;
            while (n < r->end && line[n].ch >= '0' && line[n].ch <= '9') n++;
            if (n > k+1 && n < r->end && line[n].ch == ':') {
                r->body = r->indent+2; /* Avoid a very wide filename gutter. */
                return CR_RECORD_PATH;
            }
        }
    }
    if (x+2 < r->end && line[x].ch == '+' && cr_space(line[x+1].ch)) {
        int n = x+2;
        if (cr_starts(line, n, r->end, "Show details")) return CR_RECORD_FOOTER;
        int begin = n;
        while (n < r->end && line[n].ch >= '0' && line[n].ch <= '9') n++;
        if (n > begin && (cr_starts(line, n, r->end, " line (") || cr_starts(line, n, r->end, " lines (")))
            return CR_RECORD_FOOTER;
    }
    return CR_RECORD_NONE;
}

/* Foreground colors and bold/dim/italic attributes travel with each cell.
 * Meaningful backgrounds, other effects, links and uncertain widths can
 * convey structure; protect them even in trailing spaces. Native callers may
 * recognize explicitly encoded background colors that equal the canvas. */
static ChromeReflowRow cr_classify(const Cell *line, int cols, int protected_row, int fenced,
                                   ChromeReflowBackgroundSafe background_safe, int tree_indent) {
    static const char *const code_words[] = {
        "const", "let", "var", "function", "class", "def", "import", "from",
        "return", "export", "if", "else", "for", "while", "switch", "case",
        "try", "catch", "throw", "print", "printf"
    };
    static const char *const title_joiners[] = {
        "a", "an", "and", "or", "the", "of", "to", "for", "in", "with"
    };
    ChromeReflowRow r;
    int x, first_end, lexical = 0, title_words = 0, all_title = 1;
    size_t k;
    memset(&r, 0, sizeof(r));
    r.type = CR_PROTECTED;
    r.end = cols;
    while (r.end > 0 && cr_space(line[r.end - 1].ch)) r.end--;
    while (r.indent < r.end && cr_space(line[r.indent].ch)) r.indent++;
    r.body = r.indent;
    /* A Unicode bullet is a fixed first-row prefix. Wrapped prose beneath it
     * must have the matching hanging indent; shell/list markers stay literal. */
    if (r.indent + 2 < r.end && line[r.indent].ch == 0x2022 &&
        cr_space(line[r.indent + 1].ch) && !cr_space(line[r.indent + 2].ch)) {
        r.bullet = 1;
        r.reason = CR_REASON_LIST;
        r.body += 2;
    }
    if (protected_row) { r.reason = CR_REASON_UNSAFE; return r; }
    if (fenced || cr_fence(line, r.indent, r.end)) return r;
    r.record = cr_record_shape(line, &r);
    if (!r.record && tree_indent >= 0 && r.indent >= tree_indent && r.end > r.indent && !r.bullet) {
        r.record = CR_RECORD_TREE;
        r.body = r.indent;
    }
    if (r.record) {
        int literal_pipes = r.record != CR_RECORD_TREE || cr_tree_literal_pipes(line, &r);
        for (x = 0; x < cols; x++) {
            const Cell *c = line+x;
            if ((c->attr & (16u | 64u)) || c->wflag || (!literal_pipes && c->ch == '|') ||
                (!cr_space(c->ch) && !cr_prose_character(c->ch) && !(x == r.indent && c->ch == 0x2514) &&
                 !(r.record == CR_RECORD_FOOTER && c->ch == 0x2303))) {
                r.reason = CR_REASON_UNSAFE;
                return r;
            }
        }
        r.type = CR_RECORD;
        r.reason = CR_REASON_LIST;
        return r;
    }
    for (x = 0; x < cols; x++) {
        const Cell *c = line + x;
        if ((c->bg && (!background_safe || !background_safe(c))) ||
            (c->attr & ~(1u | 2u | 4u)) || c->link || c->wflag ||
            (!cr_space(c->ch) && !cr_prose_character(c->ch))) {
            r.reason = CR_REASON_UNSAFE;
            return r;
        }
    }
    if (!r.end) { r.type = CR_BLANK; return r; }
    x = r.body;
    if (line[x].ch == '$' || line[x].ch == '%' || line[x].ch == '>' ||
        line[x].ch == '#' || line[x].ch == '!') return r;
    if (((line[x].ch == '-' || line[x].ch == '*' || line[x].ch == '+') &&
         x + 1 < r.end && cr_space(line[x + 1].ch)) ||
        (x + 3 < r.end && line[x].ch == '[' && line[x + 2].ch == ']' &&
         (cr_space(line[x + 1].ch) || line[x + 1].ch == 'x' || line[x + 1].ch == 'X') &&
         cr_space(line[x + 3].ch))) {
        r.reason = CR_REASON_LIST;
        return r;
    }
    while (x < r.end && line[x].ch >= '0' && line[x].ch <= '9') x++;
    if (x > r.body && x + 1 < r.end &&
        (line[x].ch == '.' || line[x].ch == ')') && cr_space(line[x + 1].ch)) {
        r.reason = CR_REASON_LIST;
        return r;
    }
    if (line[r.body].ch == '/' || cr_starts(line, r.body, r.end, "~/") ||
        cr_starts(line, r.body, r.end, "./") || cr_starts(line, r.body, r.end, "../") ||
        cr_starts(line, r.body, r.end, "console.")) return r;
    /* Recognize the literal command in a Codex tool summary. It may wrap at
     * existing single spaces, but never joins neighboring output rows. */
    r.command = r.bullet && cr_starts(line, r.body, r.end, "Ran ");
    if (!r.command && line[r.end - 1].ch == ';') return r;
    first_end = r.body;
    while (first_end < r.end && !cr_space(line[first_end].ch)) first_end++;
    for (k = 0; k < sizeof(code_words) / sizeof(code_words[0]); k++)
        if (cr_word_is(line, r.body, first_end, code_words[k])) return r;
    for (x = r.body; x < r.end; x++) {
        uint32_t c = line[x].ch;
        if (cr_space(c) && x + 1 < r.end && cr_space(line[x + 1].ch)) return r;
        if (r.command) continue;
        if (c == 0x2022 || c == '|' || c == '{' || c == '}' || c == '=' || c == '`' ||
            c == '_' || c == '\\' || c == '<' || c == '>') return r;
        if (c == ':' && x + 1 < r.end && line[x + 1].ch == ':') return r;
        if (c == ':' && x + 2 < r.end && line[x + 1].ch == '/' && line[x + 2].ch == '/') return r;
        if (x < first_end && (c == '@' || c == '$' || c == '%' || c == '#')) return r;
        /* foo(arguments) is a call/literal; a separated parenthesis may be prose. */
        if (c == '(' && x > r.body && !cr_space(line[x - 1].ch)) return r;
    }
    for (x = r.body; x < r.end;) {
        int begin = x, has_letter = 0, is_title, ignore_title = 0;
        while (x < r.end && !cr_space(line[x].ch)) {
            if (cr_letter(line[x].ch)) has_letter = 1;
            x++;
        }
        r.words++;
        lexical += has_letter;
        for (k = 0; k < sizeof(title_joiners) / sizeof(title_joiners[0]); k++)
            if (cr_word_is(line, begin, x, title_joiners[k])) { ignore_title = 1; break; }
        if (!ignore_title) {
            int p;
            title_words++;
            is_title = line[begin].ch >= 'A' && line[begin].ch <= 'Z';
            for (p = begin + 1; p < x; p++)
                if (!cr_letter(line[p].ch) && line[p].ch != '\'' && line[p].ch != '-') is_title = 0;
            if (!is_title) all_title = 0;
        }
        while (x < r.end && cr_space(line[x].ch)) x++;
    }
    if (!r.command && (!lexical || lexical * 5 < r.words * 3)) return r;
    if (r.words <= 8 && r.end - r.body <= 60 &&
        ((title_words && all_title) || line[r.end - 1].ch == ':')) return r;
    r.reason = r.command ? CR_REASON_LIST : CR_REASON_OTHER;
    r.words += r.bullet; /* Include the fixed bullet cell in copied words. */
    r.type = CR_PROSE;
    return r;
}

static int cr_in_band(int row, int y, int height) {
    return (long long)row >= y && (long long)row < (long long)y + height;
}

static int cr_bound(long long value, int maximum) {
    return value <= 0 ? 0 : value >= maximum ? maximum : (int)value;
}

static int cr_overlap(const Cell *input, int cols, int first, int last,
                      int dock_x, int dock_y, int dock_width, int dock_height) {
    int row, x, begin = cr_bound(dock_x, cols);
    int end = cr_bound((long long)dock_x + dock_width, cols);
    for (row = first; row < last; row++) {
        if (!cr_in_band(row, dock_y, dock_height)) continue;
        for (x = begin; x < end; x++)
            if (!cr_space(input[row * cols + x].ch)) return 1;
    }
    return 0;
}

/* Also consider text touching the preferred gap, even if it already clears
 * the menu itself. Bound before narrowing to int for offscreen geometry. */
static int cr_needs_layout(const Cell *input, int cols, int first, int last, int side,
                           int dock_x, int dock_y, int dock_width, int dock_height) {
    int begin = cr_bound((long long)dock_x-(side == 1 ? 2 : 0), cols);
    int end = cr_bound((long long)dock_x+dock_width+(side == 2 ? 2 : 0), cols);
    return cr_overlap(input, cols, first, last, begin, dock_y, end-begin, dock_height);
}

static int cr_source_after(const ChromeReflowWord *word, int cols) {
    int last = word->source + word->length - 1;
    return last % cols + 1 < cols ? last + 1 : last;
}

/* Blank source rows can already contain an earlier block's continuation.
 * Only borrow untouched, safe rows; never erase previously placed output. */
static int cr_available_blank(const ChromeReflow *out, const ChromeReflowRow *info, int row) {
    if (info[row].type != CR_BLANK) return 0;
    for (int x = 0; x < out->cols; x++) {
        int i = row*out->cols+x;
        if (!cr_space(out->cells[i].ch) || out->source[i] != i) return 0;
    }
    return 1;
}

static void cr_lane(int row, int cols, int side, int dock_x, int dock_y,
                    int dock_width, int dock_height, int gap, int *offset, int *limit) {
    *offset = 0; *limit = cols;
    if (!cr_in_band(row, dock_y, dock_height)) return;
    if (side == 2) *offset = cr_bound((long long)dock_x+dock_width+gap, cols);
    else *limit = cr_bound((long long)dock_x-gap, cols);
}

#include "hardcore_structured_reflow.h"

static int chrome_reflow_build_checked(ChromeReflow *out, const Cell *input,
                               int cols, int rows, const unsigned char *protected_rows,
                               int side, int dock_x, int dock_y, int dock_width,
                               int dock_height, int max_blank,
                               ChromeReflowBackgroundSafe background_safe) {
    ChromeReflowRow *info = NULL;
    ChromeReflowWord *words = NULL;
    ChromeReflowPlacement *placement = NULL;
    size_t count, total_words = 0;
    int row, fenced = 0;
    Cell blank;
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!input || cols <= 0 || rows <= 0 || cols > INT_MAX / rows) return 0;
    count = (size_t)cols * (size_t)rows;
    if (count > SIZE_MAX / sizeof(Cell) || count > SIZE_MAX / sizeof(int)) return 0;
    out->cells = (Cell *)malloc(count * sizeof(Cell));
    out->source = (int *)malloc(count * sizeof(int));
    out->markers = (int *)malloc((size_t)rows * sizeof(int));
    if (!out->cells || !out->source || !out->markers) goto fail;
    out->cols = cols;
    out->rows = rows;
    memcpy(out->cells, input, count * sizeof(Cell));
    for (row = 0; row < (int)count; row++) out->source[row] = row;
    for (row = 0; row < rows; row++) out->markers[row] = -1;
    if ((side != 1 && side != 2) || dock_width <= 0 || dock_height <= 0 ||
        !cr_needs_layout(input, cols, 0, rows, side, dock_x, dock_y, dock_width, dock_height)) return 1;
    info = (ChromeReflowRow *)malloc((size_t)rows * sizeof(*info));
    placement = (ChromeReflowPlacement *)malloc((size_t)rows * sizeof(*placement));
    if (!info || !placement) goto fail;
    int tree_indent = -1;
    for (row = 0; row < rows; row++) {
        const Cell *line = input + row * cols;
        info[row] = cr_classify(line, cols, protected_rows && protected_rows[row], fenced, background_safe, tree_indent);
        if (info[row].type == CR_RECORD && info[row].record == CR_RECORD_TREE) {
            if (tree_indent < 0) tree_indent = info[row].body;
        } else if (!(info[row].type == CR_RECORD && info[row].record != CR_RECORD_FOOTER &&
                     tree_indent >= 0 && info[row].indent >= tree_indent)) tree_indent = -1;
        if (cr_fence(line, info[row].indent, info[row].end)) fenced = !fenced;
        if (info[row].type == CR_PROSE) total_words += (size_t)info[row].words;
    }
    if (total_words) {
        if (total_words > SIZE_MAX / sizeof(*words)) goto fail;
        words = (ChromeReflowWord *)malloc(total_words * sizeof(*words));
        if (!words) goto fail;
    }
    memset(&blank, 0, sizeof(blank));
    blank.ch = ' ';
    if (max_blank < 0) max_blank = 0;
    if (max_blank > rows) max_blank = rows;
    for (row = 0; row < rows;) {
        int start, end, indent, hanging, bullet, source_rows, blank_rows, above, top, word_count = 0;
        int y, x, word, used_rows = 0, fit, affected;
        if (info[row].type == CR_RECORD && info[row].record != CR_RECORD_FOOTER) {
            row = cr_reflow_records(out, input, info, row, cols, rows, side,
                                    dock_x, dock_y, dock_width, dock_height, max_blank);
            continue;
        }
        if (info[row].type != CR_PROSE || info[row].words < 2) { row++; continue; }
        start = row;
        indent = info[row].indent;
        bullet = info[row].bullet;
        hanging = bullet ? info[row].body : indent;
        row++;
        while (!info[start].command && row < rows && info[row].type == CR_PROSE && !info[row].bullet &&
               info[row].indent == hanging) {
            const Cell *previous = input + (row - 1) * cols;
            uint32_t next_first = input[row * cols + info[row].indent].ch;
            uint32_t previous_last = previous[info[row - 1].end - 1].ch;
            if (!bullet && next_first >= 'A' && next_first <= 'Z' &&
                (long long)(info[row - 1].end - indent) * 100 < (long long)cols * 72) break;
            if (previous_last == ':') break;
            if (info[row].words == 1 && (previous_last == '.' || previous_last == '!' ||
                                       previous_last == '?' || previous_last == ':')) break;
            row++;
        }
        end = row;
        source_rows = end - start;
        if (start && info[start - 1].reason == CR_REASON_LIST && !bullet) continue;
        /* Do not detach a plain-looking part of a styled/interactive
         * paragraph. Its unsafe row can carry formatting or a cursor that
         * makes the whole same-indent run ambiguous. */
        if (start && info[start - 1].reason == CR_REASON_UNSAFE &&
            (info[start - 1].indent == indent ||
             (info[start - 1].bullet && info[start - 1].body == indent)) &&
            info[start - 1].end > info[start - 1].indent) continue;
        if (end < rows && info[end].reason == CR_REASON_UNSAFE &&
            info[end].indent == hanging && info[end].end > hanging) continue;
        if (!cr_needs_layout(input, cols, start, end, side, dock_x, dock_y, dock_width, dock_height)) continue;
        blank_rows = 0;
        while (blank_rows < max_blank && end + blank_rows < rows &&
               cr_available_blank(out, info, end + blank_rows)) blank_rows++;
        above = 0;
        while (above < max_blank && start-above > 0 && cr_available_blank(out, info, start-above-1)) above++;
        for (y = start; y < end; y++) {
            for (x = info[y].indent; x < info[y].end;) {
                ChromeReflowWord *w = words + word_count++;
                w->source = y * cols + x;
                while (x < info[y].end && !cr_space(input[y * cols + x].ch)) x++;
                w->length = y * cols + x - w->source;
                while (x < info[y].end && cr_space(input[y * cols + x].ch)) x++;
            }
        }
        fit = 0; top = start;
        /* Prefer a two-cell gap, allowing a flush fit before borrowing above.
         * Replan at each earlier start: crossing the dock's top/bottom changes
         * the width, so merely shifting a previous plan is not sufficient. */
        for (int up = 0; up <= above && !fit; up++)
        for (int gap = 2; gap >= 0 && !fit; gap -= 2) {
            word = used_rows = 0;
            top = start-up;
            while (word < word_count && used_rows < source_rows + blank_rows + up) {
                int offset = 0, limit = cols, used = 0, first = word;
                int row_indent = used_rows ? hanging : indent;
                ChromeReflowPlacement *p = placement + used_rows;
                y = top + used_rows;
                cr_lane(y, cols, side, dock_x, dock_y, dock_width, dock_height, gap, &offset, &limit);
                while (word < word_count) {
                    int next = words[word].length + (word != first);
                    if (next > limit - offset - row_indent - used) break;
                    used += next;
                    word++;
                }
                /* Keep the bullet and at least its first word together. */
                if (word == first || (bullet && !used_rows && word == 1)) break;
                p->first = first;
                p->count = word - first;
                p->offset = offset;
                p->indent = row_indent;
                used_rows++;
            }
            fit = word == word_count && used_rows > 0;
        }
        if (!fit) continue;
        affected = end-top > used_rows ? end-top : used_rows;
        for (y = 0; y < affected; y++) {
            int base = (top + y) * cols;
            int endpoint = cr_source_after(words + word_count - 1, cols);
            out->markers[top+y] = -1;
            for (x = 0; x < cols; x++) {
                out->cells[base + x] = blank;
                out->source[base + x] = endpoint;
            }
        }
        for (y = 0; y < used_rows; y++) {
            const ChromeReflowPlacement *p = placement + y;
            int base = (top + y) * cols, first_source = words[p->first].source;
            int last_word = p->first + p->count - 1;
            int endpoint = cr_source_after(words + last_word, cols);
            int begin = p->offset + p->indent;
            int source_col = first_source % cols;
            int source_indent = info[first_source / cols].indent;
            /* Preserve real indentation at an original row start. For a
             * fragment starting mid-row, synthetic indentation points at the
             * preceding word separator. Space beneath a left dock maps to
             * that same boundary, never unrelated original hidden content. */
            int boundary = source_col == source_indent ? first_source - source_indent :
                           source_col ? first_source - 1 : first_source;
            for (x = 0; x < begin; x++) {
                int mapped = boundary;
                if (source_col == source_indent && x >= p->offset && x - p->offset < source_indent)
                    mapped += x - p->offset;
                out->source[base + x] = mapped;
            }
            x = begin;
            for (word = p->first; word <= last_word; word++) {
                const ChromeReflowWord *w = words + word;
                int n;
                if (word != p->first) {
                    int separator = w->source % cols ? w->source - 1 : w->source;
                    /* Copy a real separating space when one exists, retaining
                     * its foreground. Otherwise this is a line join. */
                    if (cr_space(input[separator].ch)) out->cells[base + x] = input[separator];
                    out->source[base + x++] = separator;
                }
                for (n = 0; n < w->length; n++, x++) {
                    out->cells[base + x] = input[w->source + n];
                    out->source[base + x] = w->source + n;
                }
            }
            while (x < cols) out->source[base + x++] = endpoint;
            if (p->indent && memcmp(out->cells + base, input + base, (size_t)cols * sizeof(Cell)))
                out->markers[top + y] = begin - 1;
        }
        out->changed++;
        if (top+used_rows > row) row = top+used_rows;
    }
    free(info);
    free(words);
    free(placement);
    return 1;
fail:
    free(info);
    free(words);
    free(placement);
    chrome_reflow_free(out);
    return 0;
}

static inline int chrome_reflow_build(ChromeReflow *out, const Cell *input,
                               int cols, int rows, const unsigned char *protected_rows,
                               int side, int dock_x, int dock_y, int dock_width,
                               int dock_height, int max_blank) {
    return chrome_reflow_build_checked(out, input, cols, rows, protected_rows, side,
                                       dock_x, dock_y, dock_width, dock_height, max_blank, NULL);
}

#endif /* HARDCORE_REFLOW_H */
