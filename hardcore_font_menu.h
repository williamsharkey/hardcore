/* Font settings share this geometry for drawing, hit testing and pagination.
   The list reads as a table. Each family has a name row at the left edge and
   its sizes on indented rows below it; a family with one size keeps that size
   on its name row, right-aligned. When widths share heights (Hairline), every
   width is a row ("13×") and every height a column, so equal heights line up.
   Otherwise the sizes are full W×H labels on a fixed column grid. A page
   never starts with sizes: it repeats the family name instead. */
#define FONT_MENU_ROWS (6 * NFONTS)
#define FONT_MENU_INDENT 3
typedef struct {
    int font, left, right;
    wchar_t label[24];
} FontMenuSize;
typedef struct {
    int family, count, show_name, name_left, name_right, prefix_left;
    wchar_t prefix[8];
    FontMenuSize sizes[NFONTS];
} FontMenuRow;
static FontMenuRow g_font_menu_rows[FONT_MENU_ROWS];
static int g_font_menu_count;

static void font_menu_label(int font, wchar_t *label) {
    _snwprintf(label, 24, L"%d×%d", cell_w(&FONTS[font]), cell_h(&FONTS[font]));
    label[23] = 0;
}
static int font_menu_before(int a, int b) {
    int ah = cell_h(&FONTS[a]), bh = cell_h(&FONTS[b]);
    return ah != bh ? ah < bh : cell_w(&FONTS[a]) < cell_w(&FONTS[b]);
}
/* One family's rows before pagination: its name row, then rows of sizes at
   their preferred columns for a full-width row. */
static int font_menu_family(const int *fonts, int count, FontMenuRow *lines) {
    int heights[NFONTS], widths[NFONTS], nh = 0, nw = 0, i, j, shared = 0, n = 0, longest = 0;
    int right = SB_COLS - 1;
    FontMenuRow *line;
    memset(lines, 0, sizeof(*lines));
    lines->family = fonts[0]; lines->show_name = 1; lines->prefix_left = -1;
    if (count == 1) {
        FontMenuSize *size = &lines->sizes[lines->count++];
        size->font = fonts[0]; font_menu_label(fonts[0], size->label);
        return 1;
    }
    n = 1;
    for (i = 0; i < count; i++) {
        int h = cell_h(&FONTS[fonts[i]]), w = cell_w(&FONTS[fonts[i]]), seen_h = 0, seen_w = 0;
        wchar_t label[24];
        for (j = 0; j < nh; j++) if (heights[j] == h) { seen_h = 1; shared = 1; }
        for (j = 0; j < nw; j++) seen_w |= widths[j] == w;
        if (!seen_h) heights[nh++] = h;
        if (!seen_w) widths[nw++] = w;
        font_menu_label(fonts[i], label);
        longest = chrome_max(longest, (int)wcslen(label));
    }
    for (i = 1; i < nw; i++) for (j = i; j > 0 && widths[j] < widths[j - 1]; j--) {
        int t = widths[j]; widths[j] = widths[j - 1]; widths[j - 1] = t;
    }
    if (shared) {
        /* heights[] follows the sorted fonts, so it is already ascending. */
        int prefix = 0, cell = 0, first;
        for (i = 0; i < nw; i++) {
            wchar_t text[8]; _snwprintf(text, 8, L"%d×", widths[i]); text[7] = 0;
            prefix = chrome_max(prefix, (int)wcslen(text));
        }
        for (i = 0; i < nh; i++) {
            wchar_t text[8]; _snwprintf(text, 8, L"%d", heights[i]); text[7] = 0;
            cell = chrome_max(cell, (int)wcslen(text));
        }
        first = FONT_MENU_INDENT + prefix + 1;
        if (first + (nh - 1) * (cell + 1) + cell <= right) {
            for (i = 0; i < nw; i++) {
                line = &lines[n++];
                memset(line, 0, sizeof(*line));
                line->family = fonts[0]; line->prefix_left = FONT_MENU_INDENT;
                _snwprintf(line->prefix, 8, L"%d×", widths[i]); line->prefix[7] = 0;
                for (j = 0; j < count; j++) if (cell_w(&FONTS[fonts[j]]) == widths[i]) {
                    FontMenuSize *size = &line->sizes[line->count++];
                    int column = 0, length;
                    while (heights[column] != cell_h(&FONTS[fonts[j]])) column++;
                    size->font = fonts[j];
                    _snwprintf(size->label, 24, L"%d", heights[column]); size->label[23] = 0;
                    length = (int)wcslen(size->label);
                    /* Right-align within the column, like numbers in a table. */
                    size->right = first + column * (cell + 1) + cell;
                    size->left = size->right - length;
                }
            }
            return n;
        }
    }
    {
        int stride = longest + 1, per = chrome_max(1, (right - FONT_MENU_INDENT - longest) / stride + 1);
        for (i = 0; i < count; i++) {
            FontMenuSize *size;
            if (i % per == 0) {
                line = &lines[n++];
                memset(line, 0, sizeof(*line));
                line->family = fonts[0]; line->prefix_left = -1;
            }
            size = &line->sizes[line->count++];
            size->font = fonts[i]; font_menu_label(fonts[i], size->label);
            size->left = FONT_MENU_INDENT + (i % per) * stride;
            size->right = size->left + (int)wcslen(size->label);
        }
    }
    return n;
}
static int font_menu_build(int capacity, int total_hint) {
    int first, seen[NFONTS] = {0};
    g_font_menu_count = 0;
    memset(g_font_menu_rows, 0, sizeof(g_font_menu_rows));
    for (first = 0; first < NFONTS; first++) {
        static FontMenuRow lines[NFONTS + 1];
        int fonts[NFONTS], count = 0, i, j, nlines, at = 0, done = 0;
        if (seen[first]) continue;
        for (i = first; i < NFONTS; i++) if (!strcmp(FONTS[first].name, FONTS[i].name)) {
            seen[i] = 1; fonts[count++] = i;
        }
        for (i = 1; i < count; i++) {
            int fi = fonts[i];
            for (j = i; j > 0 && font_menu_before(fi, fonts[j - 1]); j--) fonts[j] = fonts[j - 1];
            fonts[j] = fi;
        }
        nlines = font_menu_family(fonts, count, lines);
        while (at < nlines && g_font_menu_count < FONT_MENU_ROWS) {
            const FontMenuRow *line = &lines[at];
            FontMenuRow *row = &g_font_menu_rows[g_font_menu_count];
            int left = 1, right = SB_COLS - 1, next_col, counter_col, slot = capacity > 0 ? g_font_menu_count % capacity : -1;
            int name_width = (int)strlen(FONTS[first].name), col, fits = 1;
            if (slot == 0 && chrome_pager_geometry(g_font_menu_count, capacity, total_hint, 0, &next_col, &counter_col)) {
                left = 5; right = next_col;
            }
            memset(row, 0, sizeof(*row));
            row->family = first; row->prefix_left = -1;
            row->name_left = row->name_right = left;
            g_font_menu_count++;
            if (capacity >= 2 && !line->show_name && slot == 0) {
                /* Sizes continue on a new page under their family's name. */
                row->show_name = 1; row->name_right = chrome_min(left + name_width, right);
                continue;
            }
            if (capacity >= 2 && line->show_name && nlines > 1 && slot == capacity - 1) {
                row->family = row[-1].family;   /* a blank row, not an orphaned name */
                continue;
            }
            if (line->show_name) {
                row->show_name = 1;
                row->name_right = chrome_min(left + name_width, right);
                if (line->count) {
                    FontMenuSize *size = &row->sizes[row->count++];
                    *size = line->sizes[0];
                    size->right = right; size->left = right - (int)wcslen(size->label);
                    row->name_right = chrome_min(row->name_right, size->left - 1);
                }
                at++;
                continue;
            }
            /* A size row at its own columns, or packed into a narrower row. */
            if (line->prefix_left >= 0 && line->prefix_left < left) fits = 0;
            for (i = done; i < line->count; i++)
                if (line->sizes[i].left < left || line->sizes[i].right > right) fits = 0;
            row->name_right = left - 1;
            col = left;
            if (line->prefix[0]) {
                row->prefix_left = fits ? line->prefix_left : left;
                wcscpy(row->prefix, line->prefix);
                col = row->prefix_left + (int)wcslen(row->prefix) + 1;
            }
            while (done < line->count) {
                FontMenuSize size = line->sizes[done];
                int length = (int)wcslen(size.label);
                if (!fits) {
                    if (row->count && col + length > right) break;
                    size.left = col; size.right = col + length;
                }
                row->sizes[row->count++] = size;
                col = size.right + 1; done++;
            }
            if (done == line->count) { at++; done = 0; }
        }
    }
    return g_font_menu_count;
}
static int font_menu_hit(int item, int col) {
    int i;
    if (item < 0 || item >= g_font_menu_count) return -1;
    for (i = 0; i < g_font_menu_rows[item].count; i++) {
        const FontMenuSize *size = &g_font_menu_rows[item].sizes[i];
        if (col >= size->left && col < size->right) return size->font;
    }
    return -1;
}
static int unit_font_menu(FILE *f) {
    int fails = 0, scale, capacity, saved_scale = g_scale;
    for (scale = 1; scale <= 3; scale++) for (capacity = 0; capacity <= FONT_MENU_ROWS; capacity++) {
        int count, pass, row, coverage[NFONTS] = {0}, family_seen[NFONTS] = {0};
        int prior_family = -1, named = -1, left_of[NFONTS], row_of[NFONTS];
        g_scale = scale;
        count = font_menu_build(0, 0);
        for (pass = 0; pass < FONT_MENU_ROWS; pass++) {
            int previous = count;
            count = font_menu_build(capacity, count);
            if (count == previous) break;
        }
        fails += pass == FONT_MENU_ROWS || count <= 0 || count > FONT_MENU_ROWS;
        for (row = 0; row < count; row++) {
            const FontMenuRow *r = &g_font_menu_rows[row];
            int k, col, left = 1, right = SB_COLS - 1, next_col, counter_col;
            int page_first = capacity > 0 && row % capacity == 0;
            if (page_first && chrome_pager_geometry(row, capacity, count, 0, &next_col, &counter_col)) {
                left = 5; right = next_col;
            }
            if (page_first) named = -1;
            fails += r->name_left < left || r->name_right > right;
            if (r->family != prior_family) {
                /* Families stay together and start with their name. */
                fails += family_seen[r->family]++ != 0 || !r->show_name;
            }
            if (r->show_name) named = r->family;
            /* With room for a name above them, sizes never start a page. */
            if (capacity != 1 && r->count && !r->show_name) fails += named != r->family;
            if (r->prefix[0]) fails += r->prefix_left < left || r->prefix_left + (int)wcslen(r->prefix) >= (r->count ? r->sizes[0].left + 1 : right);
            for (k = 0; k < r->count; k++) {
                const FontMenuSize *size = &r->sizes[k];
                int j;
                fails += size->font < 0 || size->font >= NFONTS;
                if (size->font < 0 || size->font >= NFONTS) continue;
                coverage[size->font]++;
                left_of[size->font] = size->left; row_of[size->font] = row;
                fails += strcmp(FONTS[size->font].name, FONTS[r->family].name) != 0;
                fails += (r->show_name && size->left <= r->name_right) || size->right > right || size->left < left;
                fails += size->right - size->left != (int)wcslen(size->label);
                fails += k > 0 && size->left <= r->sizes[k - 1].right;
                fails += k > 0 && font_menu_before(size->font, r->sizes[k - 1].font);
                for (j = 0; j < k; j++) fails += !wcscmp(size->label, r->sizes[j].label);
                for (col = size->left; col < size->right; col++) fails += font_menu_hit(row, col) != size->font;
            }
            for (col = 0; col < SB_COLS; col++) {
                int expected = -1;
                for (k = 0; k < r->count; k++)
                    if (col >= r->sizes[k].left && col < r->sizes[k].right) expected = r->sizes[k].font;
                fails += font_menu_hit(row, col) != expected;
            }
            prior_family = r->family;
        }
        for (row = 0; row < NFONTS; row++) fails += coverage[row] != 1;
        /* In an unpaged list, widths sharing a height form a table: one width
           per row, and each height in one column. */
        if (!capacity) {
            int a, b, k, shared[NFONTS] = {0};
            for (a = 0; a < NFONTS; a++) for (k = 0; k < NFONTS; k++)
                shared[a] |= !strcmp(FONTS[k].name, FONTS[a].name) &&
                             cell_h(&FONTS[k]) == cell_h(&FONTS[a]) && cell_w(&FONTS[k]) != cell_w(&FONTS[a]);
            for (a = 0; a < NFONTS; a++) for (k = 0; k < NFONTS; k++)
                if (!strcmp(FONTS[k].name, FONTS[a].name)) shared[a] |= shared[k];
            for (a = 0; a < NFONTS; a++) for (b = 0; b < NFONTS; b++) {
                if (a == b || strcmp(FONTS[a].name, FONTS[b].name) || !shared[a]) continue;
                fails += (cell_w(&FONTS[a]) == cell_w(&FONTS[b])) != (row_of[a] == row_of[b]);
                if (cell_h(&FONTS[a]) == cell_h(&FONTS[b])) fails += left_of[a] != left_of[b];
            }
        }
    }
    g_scale = saved_scale;
    font_menu_build(0, 0);
    fprintf(f, "%s font menu: family tables, aligned size columns, complete pages and independent size targets\n", fails ? "FAIL" : "ok  ");
    return fails;
}
