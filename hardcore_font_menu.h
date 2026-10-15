/* Font settings share this geometry for drawing, hit testing and pagination.
   A family keeps its native-size choices together. Narrow rows use heights;
   continuation rows repeat the family name only at a new page boundary. */
typedef struct {
    int font, left, right;
    wchar_t label[24];
} FontMenuSize;
typedef struct {
    int family, count, show_name, name_left, name_right;
    FontMenuSize sizes[NFONTS];
} FontMenuRow;
static FontMenuRow g_font_menu_rows[NFONTS];
static int g_font_menu_count;

static void font_menu_label(int font, int compact, wchar_t *label) {
    if (compact) _snwprintf(label, 24, L"%d", cell_h(&FONTS[font]));
    else _snwprintf(label, 24, L"%d×%d", cell_w(&FONTS[font]), cell_h(&FONTS[font]));
    label[23] = 0;
}
static int font_menu_before(int a, int b) {
    int ah = cell_h(&FONTS[a]), bh = cell_h(&FONTS[b]);
    return ah != bh ? ah < bh : cell_w(&FONTS[a]) < cell_w(&FONTS[b]);
}
static int font_menu_build(int capacity, int total_hint) {
    int first, seen[NFONTS] = {0};
    g_font_menu_count = 0;
    memset(g_font_menu_rows, 0, sizeof(g_font_menu_rows));
    for (first = 0; first < NFONTS; first++) {
        int fonts[NFONTS], count = 0, i, j, used = 0, full, compact, name_width;
        if (seen[first]) continue;
        for (i = first; i < NFONTS; i++) if (!strcmp(FONTS[first].name, FONTS[i].name)) {
            seen[i] = 1; fonts[count++] = i;
        }
        for (i = 1; i < count; i++) {
            int fi = fonts[i];
            for (j = i; j > 0 && font_menu_before(fi, fonts[j - 1]); j--) fonts[j] = fonts[j - 1];
            fonts[j] = fi;
        }
        name_width = (int)strlen(FONTS[first].name);
        full = name_width;
        for (i = 0; i < count; i++) {
            wchar_t label[24]; font_menu_label(fonts[i], 0, label);
            full += 1 + (int)wcslen(label);
        }
        compact = full > SB_COLS - 2;
        while (used < count && g_font_menu_count < NFONTS) {
            int left = 1, right = SB_COLS - 1, next_col, counter_col, col;
            int page_first = capacity > 0 && g_font_menu_count % capacity == 0;
            FontMenuRow *row = &g_font_menu_rows[g_font_menu_count];
            if (page_first && chrome_pager_geometry(g_font_menu_count, capacity, total_hint, 0, &next_col, &counter_col)) {
                left = 5; right = next_col;
            }
            row->family = first; row->show_name = used == 0 || page_first;
            row->name_left = left;
            /* Keep room for at least one height, even with a wide page count. */
            row->name_right = chrome_min(left + name_width, right - 4);
            col = row->name_right + 1;
            while (used < count) {
                FontMenuSize size;
                int length;
                memset(&size, 0, sizeof(size)); size.font = fonts[used];
                font_menu_label(size.font, compact, size.label);
                length = (int)wcslen(size.label);
                if (!row->count && col + length > right) {
                    font_menu_label(size.font, 1, size.label);
                    length = (int)wcslen(size.label);
                }
                if (col + length > right) break;
                size.left = col; size.right = col + length;
                row->sizes[row->count++] = size;
                col += length + 1; used++;
            }
            /* Current families fit even the narrowest pager row. Keep an
               explicit fallback so a future long name cannot drop a font. */
            if (!row->count) {
                FontMenuSize *size = &row->sizes[row->count++];
                size->font = fonts[used++]; font_menu_label(size->font, 1, size->label);
                size->right = right; size->left = right - (int)wcslen(size->label);
                row->name_right = size->left - 1;
            }
            g_font_menu_count++;
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
    for (scale = 1; scale <= 3; scale++) for (capacity = 0; capacity <= NFONTS; capacity++) {
        int count, pass, row, coverage[NFONTS] = {0}, family_seen[NFONTS] = {0};
        int prior_family = -1, prior_font = -1;
        g_scale = scale;
        count = font_menu_build(0, 0);
        for (pass = 0; pass < NFONTS; pass++) {
            int previous = count;
            count = font_menu_build(capacity, count);
            if (count == previous) break;
        }
        fails += pass == NFONTS || count <= 0 || count > NFONTS;
        for (row = 0; row < count; row++) {
            const FontMenuRow *r = &g_font_menu_rows[row];
            int k, col, left = 1, right = SB_COLS - 1, next_col, counter_col;
            int page_first = capacity > 0 && row % capacity == 0;
            if (page_first && chrome_pager_geometry(row, capacity, count, 0, &next_col, &counter_col)) {
                left = 5; right = next_col;
            }
            fails += r->count <= 0 || r->name_left < left || r->name_right > right;
            if (r->family != prior_family) {
                fails += family_seen[r->family]++ != 0 || !r->show_name;
                prior_font = -1;
            } else fails += r->show_name != page_first;
            for (k = 0; k < r->count; k++) {
                const FontMenuSize *size = &r->sizes[k];
                fails += size->font < 0 || size->font >= NFONTS;
                if (size->font < 0 || size->font >= NFONTS) continue;
                coverage[size->font]++;
                fails += strcmp(FONTS[size->font].name, FONTS[r->family].name) != 0;
                fails += size->left <= r->name_right || size->right > right || size->left < left;
                fails += size->right - size->left != (int)wcslen(size->label);
                fails += prior_font >= 0 && font_menu_before(size->font, prior_font);
                fails += k > 0 && size->left <= r->sizes[k - 1].right;
                for (col = size->left; col < size->right; col++) fails += font_menu_hit(row, col) != size->font;
                prior_font = size->font;
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
    }
    g_scale = saved_scale;
    font_menu_build(0, 0);
    fprintf(f, "%s font menu: grouped families, sorted sizes, complete pages and independent size targets\n", fails ? "FAIL" : "ok  ");
    return fails;
}
