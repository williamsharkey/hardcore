/* Native chrome: one grid row per tab, independent item-row pagination.
   Included after the terminal/tab model and before platform caption drawing. */
enum {
    HIT_NONE, HIT_TAB, HIT_CLOSE, HIT_PLUS, HIT_BAR, HIT_SETTINGS, HIT_FONT,
    HIT_SCHEME, HIT_STAB, HIT_SCLOSE, HIT_UNDERLAY, HIT_TABPAGE, HIT_SETPAGE,
    HIT_REFLOW, HIT_REFLOW_DEBUG, HIT_DOCK_ROW, HIT_CHROME_INERT
};
static int plat_lights_bottom(void);
static void plat_settings_shown(void);
static void plat_settings_tell(void);
#ifdef _WIN32
static int sb_row_y(int k) { return k * CH; }
#endif
static float row_y(int r) { return (float)(r * CH); }
static int first_row(void) {
    int b = plat_lights_bottom();
#ifdef _WIN32
    b = caption_h();
#endif
    return b > CH ? (b + CH - 1) / CH : 1;
}

static int g_row_y[MAX_TABS + 2]; /* invisible tabs are -1; visible values are pixels */
static int g_settings_open, g_settings_tab; /* Font, Theme, Layout */
static int g_tab_offset, g_setting_offset[3];
static int g_layout_active = -1, g_layout_ntabs = -1;
static const struct { const char *name; char dark, light; } FAMILIES[] = {
    {"hardcore", 'h', 0}, {"Dracula", 'd', 0}, {"Monokai", 'm', 0}, {"Nord", 'n', 0},
    {"Gruvbox", 'g', 'G'}, {"Solarized", 's', 'S'}, {"One", 'o', 'O'}, {"Catppuccin", 'c', 'C'},
    {"Tokyo Night", 'k', 'K'}, {"Rose Pine", 'r', 'R'},
};
#define NFAMILIES ((int)(sizeof(FAMILIES) / sizeof(FAMILIES[0])))
#define CHROME_LAYOUT_ITEMS 4

typedef struct {
    int top, grip_rows, tab_row, tab_capacity, tab_count;
    int settings_row, section_row, list_row, list_capacity, list_count, bottom;
} ChromeLayout;
static ChromeLayout g_chrome;

static int chrome_min(int a, int b) { return a < b ? a : b; }
static int chrome_max(int a, int b) { return a > b ? a : b; }
static int chrome_page_offset(int offset, int total, int capacity) {
    if (total <= 0 || capacity <= 0) return 0;
    if (offset < 0) offset = 0;
    return chrome_min(offset / capacity, (total - 1) / capacity) * capacity;
}
static int chrome_item_total(void) {
    return g_settings_tab == 0 ? NFONTS : g_settings_tab == 1 ? NFAMILIES : CHROME_LAYOUT_ITEMS;
}
/* Capacity depends on the viewport and complete lists, never the short last
   page. A resize re-aligns each list's offset to the new page boundary. */
static ChromeLayout chrome_measure(int rows, int lights, int tabs, int open,
                                   int underlay, int dock, int items) {
    ChromeLayout a;
    int minimum_height, available, budget;
    memset(&a, 0, sizeof(a));
    a.settings_row = a.section_row = a.list_row = -1;
    if (rows < 1) return a;
    /* Opening settings may use all rows below the grip. Do not move a dock
       upward just to fit the whole list: its chosen row is stable, and pages
       handle actual overflow. Keep enough space for one of each control. */
    minimum_height = 1 + (tabs > 0) + 1 + (open ? 1 + (items > 0) : 0);
    a.top = underlay ? chrome_min(chrome_max(dock, 0), chrome_max(0, rows - minimum_height)) : 0;
    a.grip_rows = chrome_min(rows - a.top, chrome_max(1, lights - a.top));
    available = rows - a.top - a.grip_rows;
    a.tab_capacity = chrome_min(tabs, chrome_max(0, available - 1));
    if (open) {
        /* Give both lists room when they overflow, then lend any unused rows
           to the longer list. Capacity depends on the complete lists, never
           on the number of entries left on a short final page. */
        budget = chrome_max(0, available - 2);
        a.tab_capacity = chrome_min(tabs, (budget + 1) / 2);
        a.list_capacity = chrome_min(items, budget - a.tab_capacity);
        a.tab_capacity = chrome_min(tabs, budget - a.list_capacity);
    }
    a.tab_row = a.top + a.grip_rows;
    a.tab_count = chrome_min(tabs, a.tab_capacity);
    a.settings_row = a.tab_row + a.tab_count;
    if (a.settings_row >= rows) a.settings_row = -1;
    a.bottom = a.tab_row + a.tab_count;
    if (a.settings_row >= 0) a.bottom++;
    if (open && a.bottom < rows) {
        a.section_row = a.bottom++;
        a.list_row = a.bottom;
        a.list_count = chrome_min(items, a.list_capacity);
        a.bottom += a.list_count;
    }
    return a;
}
static int scheme_index(int key) {
    const Scheme *s = scheme_by_key(key);
    return s ? (int)(s - SCHEMES) : 0;
}
static void tab_layout(void) {
    int i, win_w, win_h, items = chrome_item_total();
    UI_FONT;
    plat_client_size(&win_w, &win_h);
#ifndef _WIN32
    if(active_tab() && active_tab()->kind==TK_BROWSER)win_h=mac_browser_page_rows(active_tab())*CH;
#endif
    g_chrome = chrome_measure(win_h / CH, first_row(), g_ntabs, g_settings_open,
                              g_tab_underlay, g_dock_row, items);
    if (g_tab_underlay) g_dock_row = g_chrome.top;
    if (g_active != g_layout_active || g_ntabs != g_layout_ntabs) {
        if (g_active >= 0) g_tab_offset = g_active;
        g_layout_active = g_active; g_layout_ntabs = g_ntabs;
    }
    g_tab_offset = chrome_page_offset(g_tab_offset, g_ntabs, g_chrome.tab_capacity);
    g_setting_offset[g_settings_tab] = chrome_page_offset(g_setting_offset[g_settings_tab], items, g_chrome.list_capacity);
    g_chrome.tab_count = chrome_min(g_chrome.tab_capacity, g_ntabs - g_tab_offset);
    if (g_chrome.settings_row >= 0) g_chrome.settings_row = g_chrome.tab_row + g_chrome.tab_count;
    g_chrome.bottom = g_chrome.tab_row + g_chrome.tab_count + (g_chrome.settings_row >= 0);
    if (g_chrome.section_row >= 0) {
        g_chrome.section_row = g_chrome.bottom++;
        g_chrome.list_row = g_chrome.bottom;
        g_chrome.list_count = chrome_min(g_chrome.list_capacity, items - g_setting_offset[g_settings_tab]);
        g_chrome.bottom += g_chrome.list_count;
    }
    for (i = 0; i < MAX_TABS + 2; i++) g_row_y[i] = -1;
    for (i = 0; i < g_chrome.tab_count; i++) g_row_y[g_tab_offset + i] = (g_chrome.tab_row + i) * CH;
    g_row_y[g_ntabs] = g_chrome.settings_row < 0 ? -1 : g_chrome.settings_row * CH;
    g_row_y[g_ntabs + 1] = g_chrome.bottom * CH;
    UI_DONE;
    (void)win_w;
}
static int chrome_top_row(void) { tab_layout(); return g_chrome.top; }
static int chrome_bottom_row(void) { tab_layout(); return g_chrome.bottom; }
static int chrome_hovered(void) {
    if (g_sel_drag) return 0;
    int x = sidebar_x(), cw = cell_w(g_font), ch = cell_h(g_font);
    tab_layout();
    return g_mouse_x >= x && g_mouse_x < x + SB_COLS * cw &&
           g_mouse_y >= g_chrome.top * ch && g_mouse_y < g_chrome.bottom * ch;
}
/* The page arrows occupy separate targets on the first selectable row.
   The counter is inert; tabs keep a close button after it. */
static int chrome_pager_geometry(int offset, int capacity, int total, int tab,
                                 int *next_col, int *counter_col) {
    wchar_t counter[24];
    if (capacity <= 0 || total <= capacity) return 0;
    _snwprintf(counter, 24, L"%d/%d", offset / capacity + 1, (total + capacity - 1) / capacity);
    counter[23] = 0;
    *counter_col = SB_COLS - (tab ? 3 : 1) - (int)wcslen(counter);
    *next_col = *counter_col - 4;
    return 1;
}
static int chrome_page_hit(int col, int offset, int capacity, int total, int tab, int *direction) {
    int next_col, counter_col;
    if (!chrome_pager_geometry(offset, capacity, total, tab, &next_col, &counter_col)) return 0;
    if (col >= 1 && col < 5) { *direction = -1; return 1; }
    if (col >= next_col && col < counter_col) { *direction = 1; return 1; }
    return col >= counter_col && col < SB_COLS - (tab ? 3 : 1) ? 2 : 0;
}
static int hit_tabbar(int mx, int my, int *idx) {
    int col, row, i, page_hit;
    UI_FONT;
    mx -= sidebar_x();
    if (mx < 0 || mx >= sidebar_w() || my < 0) { UI_DONE; return HIT_NONE; }
    tab_layout();
    col = mx / CW; row = my / CH;
    if ((g_tab_underlay || (active_tab() && active_tab()->kind == TK_BROWSER)) && (row < g_chrome.top || row >= g_chrome.bottom)) { UI_DONE; return HIT_NONE; }
    for (i = 0; i < g_chrome.tab_count; i++) {
        if (row != g_chrome.tab_row + i) continue;
        if (col == SB_COLS - 2) { *idx = g_tab_offset + i; UI_DONE; return HIT_CLOSE; }
        if (!i && (page_hit = chrome_page_hit(col, g_tab_offset, g_chrome.tab_capacity, g_ntabs, 1, idx))) {
            UI_DONE; return page_hit == 1 ? HIT_TABPAGE : HIT_CHROME_INERT;
        }
        *idx = g_tab_offset + i; UI_DONE; return HIT_TAB;
    }
    if (row == g_chrome.settings_row) { UI_DONE; return col >= SB_COLS - 3 ? HIT_PLUS : HIT_SETTINGS; }
    if (g_settings_open && row == g_chrome.section_row) {
        *idx = col < 8 ? 0 : col < 16 ? 1 : 2;
        UI_DONE; return HIT_STAB;
    }
    if (g_settings_open && g_chrome.list_row >= 0 && row >= g_chrome.list_row && row < g_chrome.list_row + g_chrome.list_count) {
        int item = g_setting_offset[g_settings_tab] + row - g_chrome.list_row;
        if (row == g_chrome.list_row && (page_hit = chrome_page_hit(col, g_setting_offset[g_settings_tab], g_chrome.list_capacity, chrome_item_total(), 0, idx))) {
            UI_DONE; return page_hit == 1 ? HIT_SETPAGE : HIT_CHROME_INERT;
        }
        if (!g_settings_tab) { *idx = item; UI_DONE; return HIT_FONT; }
        if (g_settings_tab == 1) {
            /* The last two label cells select the light variant. On a page's
               first row those cells precede the next-page button. */
            int next_col, counter_col, right = SB_COLS - 1;
            if (row == g_chrome.list_row && chrome_pager_geometry(g_setting_offset[1], g_chrome.list_capacity, NFAMILIES, 0, &next_col, &counter_col)) right = next_col;
            *idx = scheme_index(FAMILIES[item].light && col >= right - 2 ? FAMILIES[item].light : FAMILIES[item].dark);
            UI_DONE; return HIT_SCHEME;
        }
        if (!item) { *idx = (g_tab_underlay + 1) % 3; UI_DONE; return HIT_UNDERLAY; }
        if (item == 1) { UI_DONE; return HIT_REFLOW; }
        if (item == 2) { UI_DONE; return HIT_REFLOW_DEBUG; }
        { int next_col, counter_col, right = SB_COLS - 1;
          if (row == g_chrome.list_row && chrome_pager_geometry(g_setting_offset[2], g_chrome.list_capacity, CHROME_LAYOUT_ITEMS, 0, &next_col, &counter_col)) right = next_col;
          *idx = col >= right - 4 ? 1 : -1; UI_DONE; return HIT_DOCK_ROW; }
    }
    UI_DONE;
    return HIT_BAR;
}
static int over_chrome(int x, int y) { int idx = -1; return hit_tabbar(x, y, &idx) != HIT_NONE; }
static int chrome_setting_hit(int hit) {
    return hit == HIT_SETTINGS || hit == HIT_STAB || hit == HIT_SCLOSE || hit == HIT_FONT || hit == HIT_SCHEME ||
           hit == HIT_UNDERLAY || hit == HIT_TABPAGE || hit == HIT_SETPAGE || hit == HIT_REFLOW || hit == HIT_REFLOW_DEBUG || hit == HIT_DOCK_ROW || hit == HIT_CHROME_INERT;
}
static int tabbar_action(int mx, int my) {
    int idx = -1, hit = hit_tabbar(mx, my, &idx);
    return (chrome_setting_hit(hit) && hit != HIT_CHROME_INERT) || hit == HIT_PLUS || hit == HIT_CLOSE;
}
static void resize_all(void);
static void settings_point(void) {
    int idx = -1, hit = g_mouse_y >= 0 && g_settings_open ? hit_tabbar(g_mouse_x, g_mouse_y, &idx) : HIT_NONE;
    const Scheme *s = hit == HIT_SCHEME ? &SCHEMES[idx] : g_scheme;
    if (s != g_shown) { look_set(s); plat_settings_shown(); }
}
static void settings_apply(void) {
    chrome_reflow_free(&g_display); g_display_term = NULL;
    look_set(g_scheme);
    g_tfont = g_cf = g_font;
    resize_all();
    plat_settings_shown();
    settings_point();
}
static void settings_pick(int hit, int idx) {
    if (hit == HIT_FONT) g_font = &FONTS[idx];
    else if (hit == HIT_SCHEME) g_scheme = &SCHEMES[idx];
    else return;
    config_save(); plat_settings_tell(); settings_apply();
}
static void settings_click(int mx, int my) {
    int idx = -1, hit = hit_tabbar(mx, my, &idx), save = 0;
    if (hit == HIT_SETTINGS) g_settings_open = !g_settings_open;
    else if (hit == HIT_SCLOSE) g_settings_open = 0;
    else if (hit == HIT_STAB) g_settings_tab = idx;
    else if (hit == HIT_TABPAGE) g_tab_offset = chrome_page_offset(g_tab_offset + idx * g_chrome.tab_capacity, g_ntabs, g_chrome.tab_capacity);
    else if (hit == HIT_SETPAGE) g_setting_offset[g_settings_tab] = chrome_page_offset(g_setting_offset[g_settings_tab] + idx * g_chrome.list_capacity, chrome_item_total(), g_chrome.list_capacity);
    else if (hit == HIT_UNDERLAY) { g_tab_underlay = idx; g_mouse_x = g_mouse_y = -1; save = 1; }
    else if (hit == HIT_REFLOW) { g_chrome_reflow = !g_chrome_reflow; save = 1; }
    else if (hit == HIT_REFLOW_DEBUG) { g_chrome_debug = !g_chrome_debug; save = 1; }
    else if (hit == HIT_DOCK_ROW) { g_dock_row = chrome_max(0, g_dock_row + idx); tab_layout(); save = 1; }
    else if (hit == HIT_FONT || hit == HIT_SCHEME) settings_pick(hit, idx);
    if (save) { config_save(); plat_settings_tell(); settings_apply(); }
    settings_point(); plat_invalidate();
}
static int unit_chrome(FILE *f) {
    int fails = 0, rows, open, tabs, underlay, next_col, counter_col, direction;
    for (rows = 1; rows <= 48; rows++) for (open = 0; open <= 1; open++)
        for (tabs = 1; tabs <= MAX_TABS; tabs += 7) for (underlay = 0; underlay <= 2; underlay++) {
            ChromeLayout a = chrome_measure(rows, 2, tabs, open, underlay, 8, NFONTS);
            fails += a.top < 0 || a.bottom > rows || a.bottom < a.top;
            fails += a.tab_count < 0 || a.tab_count > a.tab_capacity;
            fails += a.list_count < 0 || a.list_count > a.list_capacity;
            if (a.settings_row >= 0) fails += a.settings_row < a.tab_row + a.tab_count;
            if (a.list_count) fails += a.list_row <= a.section_row || a.list_row + a.list_count > rows;
        }
    {
        ChromeLayout a = chrome_measure(48, 1, 2, 1, 1, 8, NFONTS);
        fails += a.top != 8 || a.tab_capacity != 2 || a.list_capacity != NFONTS;
        a = chrome_measure(48, 1, 2, 1, 2, 8, NFAMILIES);
        fails += a.top != 8 || a.list_capacity != NFAMILIES;
        a = chrome_measure(48, 1, 12, 0, 1, 8, 0);
        fails += a.tab_capacity != 12; /* no arbitrary closed-underlay cap */
        a = chrome_measure(16, 1, 2, 1, 1, 8, NFAMILIES);
        fails += a.top != 8 || a.tab_capacity != 2 || a.list_capacity != 3;
        a = chrome_measure(16, 1, 12, 1, 1, 0, NFAMILIES);
        fails += a.tab_capacity != 7 || a.list_capacity != 6;
        a = chrome_measure(16, 1, 12, 1, 1, 0, 2);
        fails += a.tab_capacity != 11 || a.list_capacity != 2;
    }
    /* Last-page navigation must retain the full capacity, including after a resize. */
    fails += chrome_page_offset(8, 10, 4) != 8;
    fails += chrome_page_offset(12, 10, 4) != 8;
    fails += chrome_page_offset(8 - 4, 10, 4) != 4;
    fails += chrome_page_offset(8, 10, 3) != 6;
    fails += chrome_page_offset(-4, 10, 4) != 0;
    fails += !chrome_pager_geometry(8, 4, 10, 0, &next_col, &counter_col);
    fails += chrome_page_hit(2, 8, 4, 10, 0, &direction) != 1 || direction != -1;
    fails += chrome_page_hit(next_col, 8, 4, 10, 0, &direction) != 1 || direction != 1;
    fails += chrome_page_hit(6, 8, 4, 10, 0, &direction) != 0;
    fails += chrome_page_hit(counter_col, 8, 4, 10, 0, &direction) != 2;
    fprintf(f, "%s chrome: bounded rows, full-height lists, stable dock and pages, independent arrow targets\n", fails ? "FAIL" : "ok  ");
    return fails;
}
