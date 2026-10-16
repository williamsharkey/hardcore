/* Literal records for hardcore_reflow.h. Each source row remains an independent
 * record. Continuations borrow local blank rows; later records keep separate
 * gutters, and an unfittable record retains its complete original text. */
typedef struct {
    int row, from, to, begin, offset, limit, continuation;
} ChromeRecordFragment;

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

static int cr_reflow_records(ChromeReflow *out, const Cell *input, const ChromeReflowRow *info,
                             int start, int cols, int rows, int side, int dock_x, int dock_y,
                             int dock_width, int dock_height, int max_blank) {
    int end = start, blanks = 0, used = 0, changes = 0;
    while (end < rows && info[end].type == CR_RECORD) {
        int footer = info[end++].record == CR_RECORD_FOOTER;
        if (footer) break;
    }
    if (!cr_overlap(input, cols, start, end, dock_x, dock_y, dock_width, dock_height)) return end;
    while (blanks < max_blank && end+blanks < rows && info[end+blanks].type == CR_BLANK) blanks++;
    int budget = end-start+blanks;
    ChromeRecordFragment *plan = (ChromeRecordFragment *)malloc((size_t)budget*sizeof(*plan));
    if (!plan) return end;
    for (int row = start; row < end; row++) {
        const Cell *line = input+row*cols;
        const ChromeReflowRow *r = info+row;
        int first = used, from = 0, fit = 1;
        /* Reserve one independent first row for every remaining record.
           A single long record cannot consume the next number or diff sign. */
        int available = budget-(end-row-1);
        while (fit && from < r->end && used < available) {
            int y = start+used, offset = 0, limit = cols;
            int indent = used == first ? 0 : r->body;
            if (cr_in_band(y, dock_y, dock_height)) {
                if (side == 2) offset = cr_bound((long long)dock_x+dock_width, cols);
                else limit = cr_bound(dock_x, cols);
            }
            int cut = cr_record_cut(line, from, r->end, limit-offset-indent);
            if (cut < 0 || (used == first && cut <= r->body) ||
                (r->record == CR_RECORD_FOOTER && cut != r->end)) { fit = 0; break; }
            plan[used] = (ChromeRecordFragment){row, from, cut, offset+indent, offset, limit, used != first};
            used++;
            from = cut;
            while (from < r->end && cr_space(line[from].ch)) from++;
        }
        if (!fit || from < r->end) {
            /* Best effort is per record. Earlier records may still fit even
               when this one has a long identifier or no continuation room. */
            used = first;
            plan[used++] = (ChromeRecordFragment){row, 0, cols, 0, 0, cols, 0};
        } else if (used-first > 1 || plan[first].offset) changes++;
    }
    if (!changes) { free(plan); return end; }
    Cell empty;
    memset(&empty, 0, sizeof(empty)); empty.ch = ' ';
    int affected = used > end-start ? used : end-start;
    int endpoint = (end-1)*cols + info[end-1].end;
    if (endpoint >= rows*cols) endpoint = rows*cols-1;
    for (int y = start; y < start+affected; y++) {
        out->markers[y] = -1;
        for (int x = 0; x < cols; x++) {
            out->cells[y*cols+x] = empty;
            out->source[y*cols+x] = endpoint;
        }
    }
    for (int i = 0; i < used; i++) {
        const ChromeRecordFragment *p = plan+i;
        const ChromeReflowRow *r = info+p->row;
        const Cell *line = input+p->row*cols;
        int base = (start+i)*cols;
        int boundary = p->row*cols+p->from;
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
            out->source[dest] = p->row*cols+x;
        }
        for (int x = p->begin+p->to-p->from; x < cols; x++)
            out->source[base+x] = p->row*cols+(p->to < cols ? p->to : cols-1);
        int marker = p->continuation ? p->begin-1 : p->offset+r->indent-1;
        if (marker >= 0 && marker < cols && cr_space(out->cells[base+marker].ch) &&
            memcmp(out->cells+base, input+base, (size_t)cols*sizeof(Cell))) out->markers[start+i] = marker;
    }
    out->changed += changes;
    free(plan);
    return start+affected;
}
