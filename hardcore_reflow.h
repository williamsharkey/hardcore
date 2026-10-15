/* Display-only local prose reflow. Include after the terminal's Cell type.
 *
 * This deliberately recognizes a small, safe subset of terminal output. A
 * paragraph keeps its own rows and may borrow immediately following empty
 * rows; subsequent text never moves. Text outside the dock's vertical band
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

enum { CR_BLANK, CR_PROSE, CR_PROTECTED };
enum { CR_REASON_OTHER, CR_REASON_LIST, CR_REASON_UNSAFE };
typedef struct {
    int type, reason, indent, end, words;
} ChromeReflowRow;
typedef struct { int source, length; } ChromeReflowWord;
typedef struct { int first, count, offset; } ChromeReflowPlacement;

static void chrome_reflow_free(ChromeReflow *out) {
    if (!out) return;
    free(out->cells);
    free(out->source);
    free(out->markers);
    memset(out, 0, sizeof(*out));
}

static int cr_letter(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int cr_space(uint32_t c) { return c == 0 || c == ' '; }

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

/* Foreground colors travel with the actual cells. Backgrounds, text effects,
 * hyperlinks and uncertain cell widths may convey structure: leave them
 * literal, including when they occur only in a row's trailing spaces. */
static ChromeReflowRow cr_classify(const Cell *line, int cols, int protected_row, int fenced) {
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
    if (protected_row) { r.reason = CR_REASON_UNSAFE; return r; }
    if (fenced || cr_fence(line, r.indent, r.end)) return r;
    for (x = 0; x < cols; x++) {
        const Cell *c = line + x;
        if (c->bg || c->attr || c->link || c->wflag ||
            (!cr_space(c->ch) && (c->ch < 0x21 || c->ch > 0x7e))) {
            r.reason = CR_REASON_UNSAFE;
            return r;
        }
    }
    if (!r.end) { r.type = CR_BLANK; return r; }
    x = r.indent;
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
    if (x > r.indent && x + 1 < r.end &&
        (line[x].ch == '.' || line[x].ch == ')') && cr_space(line[x + 1].ch)) {
        r.reason = CR_REASON_LIST;
        return r;
    }
    if (line[r.indent].ch == '/' || cr_starts(line, r.indent, r.end, "~/") ||
        cr_starts(line, r.indent, r.end, "./") || cr_starts(line, r.indent, r.end, "../") ||
        cr_starts(line, r.indent, r.end, "console.")) return r;
    if (line[r.end - 1].ch == ';') return r;
    first_end = r.indent;
    while (first_end < r.end && !cr_space(line[first_end].ch)) first_end++;
    for (k = 0; k < sizeof(code_words) / sizeof(code_words[0]); k++)
        if (cr_word_is(line, r.indent, first_end, code_words[k])) return r;
    for (x = r.indent; x < r.end; x++) {
        uint32_t c = line[x].ch;
        if (c == '|' || c == '{' || c == '}' || c == '=' || c == '`' ||
            c == '_' || c == '\\' || c == '<' || c == '>') return r;
        if (c == ':' && x + 1 < r.end && line[x + 1].ch == ':') return r;
        if (c == ':' && x + 2 < r.end && line[x + 1].ch == '/' && line[x + 2].ch == '/') return r;
        if (x < first_end && (c == '@' || c == '$' || c == '%' || c == '#')) return r;
        if (cr_space(c) && x + 1 < r.end && cr_space(line[x + 1].ch)) return r;
        /* foo(arguments) is a call/literal; a separated parenthesis may be prose. */
        if (c == '(' && x > r.indent && !cr_space(line[x - 1].ch)) return r;
    }
    for (x = r.indent; x < r.end;) {
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
    if (!lexical || lexical * 5 < r.words * 3) return r;
    if (r.words <= 8 && r.end - r.indent <= 60 &&
        ((title_words && all_title) || line[r.end - 1].ch == ':')) return r;
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

static int cr_source_after(const ChromeReflowWord *word, int cols) {
    int last = word->source + word->length - 1;
    return last % cols + 1 < cols ? last + 1 : last;
}

static int chrome_reflow_build(ChromeReflow *out, const Cell *input,
                               int cols, int rows, const unsigned char *protected_rows,
                               int side, int dock_x, int dock_y, int dock_width,
                               int dock_height, int max_blank) {
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
        !cr_overlap(input, cols, 0, rows, dock_x, dock_y, dock_width, dock_height)) return 1;
    info = (ChromeReflowRow *)malloc((size_t)rows * sizeof(*info));
    placement = (ChromeReflowPlacement *)malloc((size_t)rows * sizeof(*placement));
    if (!info || !placement) goto fail;
    for (row = 0; row < rows; row++) {
        const Cell *line = input + row * cols;
        info[row] = cr_classify(line, cols, protected_rows && protected_rows[row], fenced);
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
        int start, end, indent, source_rows, blank_rows, word_count = 0;
        int y, x, word, used_rows, fit, affected;
        if (info[row].type != CR_PROSE || info[row].words < 2) { row++; continue; }
        start = row;
        indent = info[row].indent;
        row++;
        while (row < rows && info[row].type == CR_PROSE && info[row].indent == indent) {
            const Cell *previous = input + (row - 1) * cols;
            uint32_t next_first = input[row * cols + info[row].indent].ch;
            uint32_t previous_last = previous[info[row - 1].end - 1].ch;
            if (next_first >= 'A' && next_first <= 'Z' &&
                (long long)(info[row - 1].end - indent) * 100 < (long long)cols * 72) break;
            if (previous_last == ':') break;
            if (info[row].words == 1 && (previous_last == '.' || previous_last == '!' ||
                                       previous_last == '?' || previous_last == ':')) break;
            row++;
        }
        end = row;
        source_rows = end - start;
        if (start && info[start - 1].reason == CR_REASON_LIST) continue;
        /* Do not detach a plain-looking part of a styled/interactive
         * paragraph. Its unsafe row can carry formatting or a cursor that
         * makes the whole same-indent run ambiguous. */
        if (start && info[start - 1].reason == CR_REASON_UNSAFE &&
            info[start - 1].indent == indent && info[start - 1].end > indent) continue;
        if (end < rows && info[end].reason == CR_REASON_UNSAFE &&
            info[end].indent == indent && info[end].end > indent) continue;
        if (!cr_overlap(input, cols, start, end, dock_x, dock_y, dock_width, dock_height)) continue;
        blank_rows = 0;
        while (blank_rows < max_blank && end + blank_rows < rows &&
               info[end + blank_rows].type == CR_BLANK) blank_rows++;
        for (y = start; y < end; y++) {
            for (x = info[y].indent; x < info[y].end;) {
                ChromeReflowWord *w = words + word_count++;
                w->source = y * cols + x;
                while (x < info[y].end && !cr_space(input[y * cols + x].ch)) x++;
                w->length = y * cols + x - w->source;
                while (x < info[y].end && cr_space(input[y * cols + x].ch)) x++;
            }
        }
        word = used_rows = 0;
        fit = 1;
        /* Greedy wrapping is identical for each larger local row budget, so
         * one pass finds the smallest number of following blanks needed. */
        while (word < word_count && used_rows < source_rows + blank_rows) {
            int offset = 0, limit = cols, used = 0, first = word;
            ChromeReflowPlacement *p = placement + used_rows;
            y = start + used_rows;
            if (cr_in_band(y, dock_y, dock_height)) {
                if (side == 2) offset = cr_bound((long long)dock_x + dock_width, cols);
                else limit = cr_bound(dock_x, cols);
            }
            while (word < word_count) {
                int next = words[word].length + (word != first);
                if (next > limit - offset - indent - used) break;
                used += next;
                word++;
            }
            if (word == first) { fit = 0; break; }
            p->first = first;
            p->count = word - first;
            p->offset = offset;
            used_rows++;
        }
        if (!fit || word != word_count) continue;
        affected = source_rows > used_rows ? source_rows : used_rows;
        for (y = 0; y < affected; y++) {
            int base = (start + y) * cols;
            int endpoint = cr_source_after(words + word_count - 1, cols);
            for (x = 0; x < cols; x++) {
                out->cells[base + x] = blank;
                out->source[base + x] = endpoint;
            }
        }
        for (y = 0; y < used_rows; y++) {
            const ChromeReflowPlacement *p = placement + y;
            int base = (start + y) * cols, first_source = words[p->first].source;
            int last_word = p->first + p->count - 1;
            int endpoint = cr_source_after(words + last_word, cols);
            int begin = p->offset + indent;
            int source_col = first_source % cols;
            /* Preserve real indentation at an original row start. For a
             * fragment starting mid-row, synthetic indentation points at the
             * preceding word separator. Space beneath a left dock maps to
             * that same boundary, never unrelated original hidden content. */
            int boundary = source_col == indent ? first_source - indent :
                           source_col ? first_source - 1 : first_source;
            for (x = 0; x < begin; x++) {
                int mapped = boundary;
                if (source_col == indent && x >= p->offset) mapped += x - p->offset;
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
            if (indent && memcmp(out->cells + base, input + base, (size_t)cols * sizeof(Cell)))
                out->markers[start + y] = begin - 1;
        }
        out->changed++;
        if (used_rows > source_rows) row += used_rows - source_rows;
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

#endif /* HARDCORE_REFLOW_H */
