/* Literal records for hardcore_reflow.h. Classifier-proven physical diff
 * continuations form one logical record; all other rows keep independent
 * gutters. A failed logical record restores every original physical row.
 * Layout only borrows bounded neighboring blank rows and, after a footer,
 * may move one intact short status row into those blanks. */
typedef struct {
    int record, row, from, to, begin, offset, limit, continuation;
} ChromeRecordFragment;
typedef struct {
    int row, last, length, body;
    Cell *cells;
    int *source;
} ChromeLiteralRecord;

/* The classifier supplies join provenance. Flattening retains the source of
 * every cell; a physical mid-word wrap introduces no synthetic whitespace.
 * Storage is bounded by the original physical rows, including the one real
 * separating space used by an explicitly classified word-boundary join. */
static int cr_literal_records(ChromeLiteralRecord *records, Cell *cells, int *source,
                              const Cell *input, const ChromeReflowRow *info,
                              int start, int end, int cols) {
    int count = 0, used = 0;
    for (int row = start; row < end;) {
        ChromeLiteralRecord *record = records + count++;
        record->row = row; record->body = info[row].body;
        record->cells = cells + used; record->source = source + used;
        for (int x = 0; x < info[row].end; x++) {
            cells[used] = input[row*cols+x]; source[used++] = row*cols+x;
        }
        row++;
        while (row < end && info[row].record == CR_RECORD_CONTINUATION && info[row].join) {
            if (info[row].join == 2 && info[row-1].end < cols) {
                int previous = (row-1)*cols + info[row-1].end;
                cells[used] = input[previous]; source[used++] = previous;
            }
            for (int x = info[row].indent; x < info[row].end; x++) {
                cells[used] = input[row*cols+x]; source[used++] = row*cols+x;
            }
            row++;
        }
        record->last = row;
        record->length = (int)(cells + used - record->cells);
    }
    return count;
}

static int cr_path_token(const Cell *line, int x, int end, int *token_end) {
    int begin = x, last = x+1, slashes = 0, path = 0;
    while (begin && !cr_space(line[begin-1].ch)) begin--;
    while (last < end && !cr_space(line[last].ch)) last++;
    *token_end = last;
    for (int k = begin; k < last; k++) {
        if (line[k].ch == '/') slashes++;
        if (line[k].ch == '.' || line[k].ch == '~') path = 1;
    }
    return path || slashes >= 2 || line[begin].ch == '/' ||
           ((line[begin].ch == '\'' || line[begin].ch == '"') && begin+1 < end && line[begin+1].ch == '/');
}

/* Return a source boundary, never a rewritten token. Whitespace is preferred
 * naturally by its position; paths can also break after a slash, and literal
 * argument lists after an unquoted comma/semicolon. */
static int cr_record_cut(const Cell *line, int from, int end, int capacity) {
    int boundary = -1, quote = 0, escape = 0, path_end = 0, path = 0;
    if (capacity <= 0) return -1;
    if (end-from <= capacity) return end;
    for (int x = 0; x < end && x < from+capacity; x++) {
        uint32_t c = line[x].ch;
        if (x >= from) {
            if (cr_space(c) && x > from && !cr_space(line[x-1].ch)) boundary = x;
            if (c == '/') {
                if (x >= path_end) path = cr_path_token(line, x, end, &path_end);
                if (path && x+1 < end && line[x+1].ch != '/' && (!x || line[x-1].ch != '/')) boundary = x+1;
            }
            if (!quote && (c == ',' || c == ';')) boundary = x+1;
        }
        if (escape) escape = 0;
        else if (c == '\\') escape = 1;
        else if (quote) { if (c == (uint32_t)quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = (int)c;
    }
    /* A word ending exactly at the limit fits; its following separator does
       not need to consume a display column. */
    if (from+capacity < end && cr_space(line[from+capacity].ch)) boundary = from+capacity;
    return boundary > from ? boundary : -1;
}

/* Plan before changing output. Try the original start with a two-cell gap,
 * then flush to the menu. Only an incomplete fit warrants borrowing above.
 * A failed record remains literal; compare complete plans so padding cannot
 * hide another record that would have fitted without it. */
static int cr_plan_records(ChromeRecordFragment *plan, const Cell *input, const ChromeLiteralRecord *records,
                           int count, const ChromeReflowRow *info, int start, int end,
                           int top, int bottom, int cols, int side, int dock_x, int dock_y,
                           int dock_width, int dock_height, int gap, int *failed, int *changes) {
    int used = 0, budget = bottom-top;
    *failed = *changes = 0;
    for (int index = 0; index < count; index++) {
        const ChromeLiteralRecord *record = records + index;
        const Cell *line = record->cells;
        const ChromeReflowRow *r = info + record->row;
        int first = used, from = 0, fit = 1;
        /* Reserve all physical rows of later records. A failed wrapped diff
           must have room to restore its complete original continuation chain. */
        int available = budget-(end-record->last);
        while (fit && from < record->length && used < available) {
            int y = top+used, offset, limit;
            int indent = used == first ? 0 : record->body;
            cr_lane_below(input, y, cols, side, dock_x, dock_y, dock_width, dock_height, gap, &offset, &limit);
            int cut = cr_record_cut(line, from, record->length, limit-offset-indent);
            if (cut < 0 || (used == first && cut <= record->body) ||
                ((r->record == CR_RECORD_FOOTER || r->record == CR_RECORD_STATUS) && cut != record->length)) {
                fit = 0; break;
            }
            plan[used] = (ChromeRecordFragment){index, record->row, from, cut, offset+indent, offset, limit, used != first};
            used++;
            from = cut;
            while (from < record->length && cr_space(line[from].ch)) from++;
        }
        if (!fit || from < record->length) {
            used = first;
            for (int row = record->row; row < record->last; row++) {
                int offset, limit;
                /* Raw fallback may cross the menu, but must not overwrite a
                   badge in a partially usable borrowed row. Reject this plan
                   rather than restore the badge over original terminal ink. */
                cr_lane_below(input, top+used, cols, side, dock_x, 0, dock_width, 0, 0, &offset, &limit);
                for (int x = limit > 0 ? limit : 0; x < cols; x++) {
                    const Cell *c = input+row*cols+x;
                    if (!cr_space(c->ch) || (c->attr & (8u | 32u)) || c->link || c->wflag) return -1;
                }
                plan[used++] = (ChromeRecordFragment){-1, row, 0, cols, 0, 0, cols, 0};
            }
            (*failed)++;
        } else if (used-first > 1 || plan[first].offset || record->last-record->row > 1) (*changes)++;
    }
    if (top != start && !*changes) *changes = 1;
    return used;
}

static int cr_reflow_records(ChromeReflow *out, const Cell *input, const ChromeReflowRow *info,
                             int start, int cols, int rows, int side, int dock_x, int dock_y,
                             int dock_width, int dock_height, int max_blank) {
    int end = start, below = 0, above = 0, used = 0, changes = 0, top = start;
    while (end < rows && info[end].type == CR_RECORD) {
        if (info[end].record == CR_RECORD_STATUS) { if (end == start) end++; break; }
        int footer = info[end++].record == CR_RECORD_FOOTER;
        if (footer) break;
    }
    /* One status between a footer and spare rows may move as a complete
       row, but never pull a second unrelated section into this local plan. */
    if (max_blank > 0 && end > start && info[end-1].record == CR_RECORD_FOOTER &&
        end+1 < rows && info[end].type == CR_RECORD && info[end].record == CR_RECORD_STATUS &&
        cr_available_blank(out, info, end+1)) end++;
    if (!cr_needs_layout(input, cols, start, end, side, dock_x, dock_y, dock_width, dock_height)) return end;
    while (below < max_blank && end+below < rows && cr_available_below(out, info, end+below)) below++;
    while (above < max_blank && start-above > 0 && cr_available_blank(out, info, start-above-1)) above++;
    int budget = end-start+below+above, best_failed = INT_MAX;
    ChromeRecordFragment *plan = (ChromeRecordFragment *)malloc((size_t)budget*sizeof(*plan));
    ChromeRecordFragment *candidate = (ChromeRecordFragment *)malloc((size_t)budget*sizeof(*candidate));
    ChromeLiteralRecord *records = (ChromeLiteralRecord *)calloc((size_t)(end-start), sizeof(*records));
    Cell *literal = (Cell *)malloc((size_t)(end-start)*cols*sizeof(*literal));
    int *sources = (int *)malloc((size_t)(end-start)*cols*sizeof(*sources));
    if (!plan || !candidate || !records || !literal || !sources) {
        free(plan); free(candidate); free(records); free(literal); free(sources); return end;
    }
    int record_count = cr_literal_records(records, literal, sources, input, info, start, end, cols);
    for (int up = 0; up <= above && best_failed; up++) {
        for (int gap = 2; gap >= 0 && best_failed; gap -= 2) {
            int failed, changed;
            int n = cr_plan_records(candidate, input, records, record_count, info, start, end, start-up, end+below,
                                    cols, side, dock_x, dock_y, dock_width, dock_height, gap, &failed, &changed);
            if (n < 0) continue;
            if (failed < best_failed) {
                memcpy(plan, candidate, (size_t)n*sizeof(*plan));
                used = n; top = start-up; changes = changed; best_failed = failed;
            }
        }
    }
    free(candidate);
    if (!changes) { free(plan); free(records); free(literal); free(sources); return end; }
    Cell empty;
    memset(&empty, 0, sizeof(empty)); empty.ch = ' ';
    int bottom = top+used > end ? top+used : end;
    int endpoint = (end-1)*cols + info[end-1].end;
    if (endpoint >= rows*cols) endpoint = rows*cols-1;
    for (int y = top; y < bottom; y++) {
        out->markers[y] = -1;
        for (int x = 0; x < cols; x++) {
            out->cells[y*cols+x] = empty;
            out->source[y*cols+x] = endpoint;
        }
    }
    for (int i = 0; i < used; i++) {
        const ChromeRecordFragment *p = plan+i;
        const ChromeReflowRow *r = info+p->row;
        const ChromeLiteralRecord *record = p->record >= 0 ? records + p->record : NULL;
        const Cell *line = record ? record->cells : input+p->row*cols;
        int base = (top+i)*cols;
        int boundary = record ? record->source[p->from] : p->row*cols+p->from;
        /* A diff's background is row styling, so it follows continuations.
           Underline/link/strike belong only to their actual source cells. */
        Cell background = line[r->body < r->end ? r->body : r->indent];
        background.ch = ' '; background.attr &= 32u; background.link = 0; background.wflag = 0;
        for (int x = 0; x < cols; x++) {
            if (x >= p->offset && x < p->limit) out->cells[base+x] = background;
            out->source[base+x] = boundary;
        }
        for (int x = p->from; x < p->to; x++) {
            int dest = base+p->begin+x-p->from;
            out->cells[dest] = line[x];
            out->source[dest] = record ? record->source[x] : p->row*cols+x;
        }
        int after;
        if (record) {
            after = record->source[p->to-1];
            after = p->to < record->length ? record->source[p->to] :
                    after%cols+1 < cols ? after+1 : after;
        } else after = p->row*cols+(p->to < cols ? p->to : cols-1);
        for (int x = p->begin+p->to-p->from; x < cols; x++) out->source[base+x] = after;
        int marker = p->continuation ? p->begin-1 : p->offset+r->indent-1;
        if (marker >= 0 && marker < cols && cr_space(out->cells[base+marker].ch) &&
            memcmp(out->cells+base, input+base, (size_t)cols*sizeof(Cell))) out->markers[top+i] = marker;
    }
    for (int y = top; y < bottom; y++) cr_restore_badge(out, input, y);
    out->changed += changes;
    free(plan); free(records); free(literal); free(sources);
    return bottom;
}
