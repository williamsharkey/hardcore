/* Pixel-edge frames share the glyph row. A boundary belongs to the following
   row, so adjacent rows share one device-pixel stroke. The right dock paints
   past the drawable edge, with no outside vertical rule. Its controls remain
   on the original grid and retain their full visible width. */
static int chrome_draw_width(void) { return sidebar_w() + (g_tab_underlay == 1 ? CW : 0); }
static void chrome_sides(int row, uint32_t color) {
    float y = row_y(row), w = (float)chrome_draw_width();
    fill_rect(0, y, 1, y + CH, color, 1);
    if (g_tab_underlay != 1) fill_rect(w - 1, y, w, y + CH, color, 1);
}
static void chrome_rule(int row, uint32_t color) {
    fill_rect(0, row_y(row), (float)chrome_draw_width(), row_y(row) + 1, color, 1);
}
static void chrome_frame(int row, uint32_t color) {
    chrome_sides(row, color);
    chrome_rule(row, color);
}
static void chrome_row_fill(int row, int selected, int hovered) {
    if (hovered || selected) {
        fill_rect(0, row_y(row), (float)chrome_draw_width(), row_y(row + 1), hovered ? TAB_HOVER : DEF_BG, 1);
#ifndef _WIN32
        plat_render_title_background(0, row_y(row), (float)chrome_draw_width(), row_y(row + 1),
                                     hovered ? TAB_HOVER : DEF_BG, plat_title_mix(BAR_BG, DEF_BG, g_draw_alpha));
#endif
    }
}
static void chrome_label(const wchar_t *s, int left, int right, int row, uint32_t color) {
    if (right > left) draw_label(s, (float)(left * CW), row_y(row), (float)((right - left) * CW), color);
}
static void chrome_pager_draw(int row, int offset, int capacity, int total, int tab,
                              int hover, int direction, int *left, int *right) {
    int next_col, counter_col;
    wchar_t counter[24];
    *left = 1; *right = SB_COLS - (tab ? 3 : 1);
    if (!chrome_pager_geometry(offset, capacity, total, tab, &next_col, &counter_col)) return;
    *left = 5; *right = next_col;
    if (hover && direction < 0) fill_rect((float)CW, row_y(row), (float)(5 * CW), row_y(row + 1), TAB_HOVER, 1);
    if (hover && direction > 0) fill_rect((float)(next_col * CW), row_y(row), (float)(counter_col * CW), row_y(row + 1), TAB_HOVER, 1);
    grid_text(L"[<-]", 1, row, offset ? g_look.text_lo : g_look.dim);
    grid_text(L"[->]", next_col, row, offset + capacity < total ? g_look.text_lo : g_look.dim);
    _snwprintf(counter, 24, L"%d/%d", offset / capacity + 1, (total + capacity - 1) / capacity);
    counter[23] = 0;
    grid_text(counter, counter_col, row, g_look.detail);
}
static void render_settings(int hover, int hidx) {
    int n, row = g_chrome.section_row, offset = g_setting_offset[g_settings_tab];
    wchar_t buf[96];
    uint32_t on = g_look.text, off = g_look.text_lo;
    if (row < 0) return;
    chrome_row_fill(row, 0, hover == HIT_STAB);
    grid_text(g_settings_tab == 0 ? L"● Font" : L"  Font", 1, row, g_settings_tab == 0 ? AMBER : off);
    grid_text(g_settings_tab == 1 ? L"● Theme" : L"  Theme", 8, row, g_settings_tab == 1 ? AMBER : off);
    grid_text(g_settings_tab == 2 ? L"● Layout" : L"  Layout", 16, row, g_settings_tab == 2 ? AMBER : off);
    chrome_frame(row, g_look.dim);
    fill_rect((float)(8 * CW), row_y(row), (float)(8 * CW + 1), row_y(row + 1), g_look.dim, 1);
    fill_rect((float)(16 * CW), row_y(row), (float)(16 * CW + 1), row_y(row + 1), g_look.dim, 1);
    for (n = 0; n < g_chrome.list_count; n++) {
        int item = offset + n, left = 1, right = SB_COLS - 1, selected = 0, pointed = 0;
        row = g_chrome.list_row + n;
        if (!g_settings_tab) { selected = &FONTS[item] == g_font; pointed = hover == HIT_FONT && hidx == item; }
        else if (g_settings_tab == 1) {
            selected = g_scheme->key == FAMILIES[item].dark || g_scheme->key == FAMILIES[item].light;
            pointed = hover == HIT_SCHEME && (SCHEMES[hidx].key == FAMILIES[item].dark || SCHEMES[hidx].key == FAMILIES[item].light);
        } else pointed = (item == 0 && hover == HIT_UNDERLAY) || (item == 1 && hover == HIT_REFLOW) ||
                          (item == 2 && hover == HIT_REFLOW_DEBUG) || (item == 3 && hover == HIT_DOCK_ROW);
        chrome_row_fill(row, selected, pointed);
        if (!n) chrome_pager_draw(row, offset, g_chrome.list_capacity, chrome_item_total(), 0,
                                  hover == HIT_SETPAGE, hidx, &left, &right);
        if (!g_settings_tab) {
            const Font *font = &FONTS[item];
            wchar_t size[24];
            int size_col;
            _snwprintf(size, 24, L"%d×%d", cell_w(font), cell_h(font)); size[23] = 0;
            size_col = right - (int)wcslen(size);
            grid_text(selected ? L"●" : L" ", left, row, AMBER);
            _snwprintf(buf, 96, L"%hs", font->name); buf[95] = 0;
            chrome_label(buf, left + 1, size_col - 1, row, selected ? AMBER : pointed ? on : off);
            if (size_col > left) grid_text(size, size_col, row, selected ? AMBER : g_look.detail);
        } else if (g_settings_tab == 1) {
            int k;
            utf8_to_wide(FAMILIES[item].name, buf, 96);
            chrome_label(buf, left, right - 5, row, selected ? AMBER : pointed ? on : off);
            for (k = 0; k < 2; k++) {
                char key = k ? FAMILIES[item].light : FAMILIES[item].dark;
                int col = right - 4 + k * 2, cur = key == g_scheme->key;
                if (!key) continue;
                if (cur) fill_rect((float)(col * CW), row_y(row), (float)((col + 1) * CW), row_y(row + 1), AMBER, 1);
                grid_text(k ? L"L" : L"D", col, row, cur ? DEF_BG : off);
            }
        } else if (item == 0) {
            static const wchar_t *mode[] = {L"Sidebar", L"Right underlay", L"Left underlay"};
            _snwprintf(buf, 96, L"Dock: %ls ›", mode[g_tab_underlay]); buf[95] = 0;
            chrome_label(buf, left, right, row, pointed ? on : off);
        } else if (item == 1) chrome_label(g_chrome_reflow ? L"[x] Local reflow" : L"[ ] Local reflow", left, right, row, pointed ? on : off);
        else if (item == 2) chrome_label(g_chrome_debug ? L"[x] Reflow markers" : L"[ ] Reflow markers", left, right, row, pointed ? on : off);
        else {
            _snwprintf(buf, 96, L"Row %d", g_dock_row); buf[95] = 0;
            chrome_label(buf, left, right - 8, row, pointed ? on : off);
            grid_text(L"[↑] [↓]", right - 8, row, g_tab_underlay ? off : g_look.dim);
        }
        chrome_sides(row, g_look.dim);
        if (!n) chrome_rule(row, g_look.dim);
    }
}

static void render_tabbar(int win_w, int win_h) {
    int i, idx = -1, hover = HIT_NONE;
    int overlay = g_tab_underlay || (active_tab() && active_tab()->kind == TK_BROWSER);
    uint16_t drip = g_ndrip ? g_drip[(GetTickCount() / DRIP_MS) % g_ndrip] : 0x2800;
    UI_FONT;
    if (g_mouse_y >= 0) hover = hit_tabbar(g_mouse_x, g_mouse_y, &idx);
    tab_layout();
    g_chrome_x = sidebar_x();
    fill_rect(0, overlay ? row_y(g_chrome.top) : 0, (float)chrome_draw_width(),
              overlay ? row_y(g_chrome.bottom) : (float)win_h, BAR_BG, 1);
#ifndef _WIN32
    plat_render_title_background(0, overlay ? row_y(g_chrome.top) : 0, (float)chrome_draw_width(),
                                 overlay ? row_y(g_chrome.bottom) : (float)win_h, BAR_BG, DEF_BG);
#endif
    for (i = g_chrome.top; i < g_chrome.top + g_chrome.grip_rows; i++) {
        int hovered = hover == HIT_BAR && g_mouse_y >= i * CH && g_mouse_y < (i + 1) * CH;
        chrome_row_fill(i, 0, hovered);
        if (i == g_chrome.top + g_chrome.grip_rows - 1) grid_text(L"⠿⠿ hardcore ⠿⠿", 1, i, g_look.detail);
        else {
            int col;
            for (col = 1; col < SB_COLS - 1; col++) grid_text(col & 1 ? L"⠿" : L" ", col, i, g_look.dim);
        }
        chrome_sides(i, g_look.dim);
        if (i == g_chrome.top) chrome_rule(i, g_look.dim);
    }
    for (i = 0; i < g_chrome.tab_count; i++) {
        int tab = g_tab_offset + i, row = g_chrome.tab_row + i, left = 1, right = SB_COLS - 3;
        int active = tab == g_active, hovered = idx == tab && (hover == HIT_TAB || hover == HIT_CLOSE);
        Tab *tb = g_tabs[tab];
        wchar_t label[256];
        uint32_t fg = active ? AMBER : g_look.text_lo;
        chrome_row_fill(row, active, hovered);
        if (!i) chrome_pager_draw(row, g_tab_offset, g_chrome.tab_capacity, g_ntabs, 1,
                                  hover == HIT_TABPAGE, idx, &left, &right);
        tab_label_fit(tb, label, 256, right-left-1);
        chrome_label(label, left + 1, right, row, hovered ? g_look.text : fg);
        r_color(tb->t.ps_state == PS_ERROR ? g_look.red : tb->attention || tb->t.ps_state == PS_BLOCKED || tb->busy ? AMBER : fg, 1);
        draw_glyph(tb->busy ? drip : tb->t.ps_state == PS_BLOCKED || tb->t.ps_state == PS_ERROR || tb->attention ? '!' : active ? 0x25CF : tb->activity ? 0x00B7 : ' ',
                   (float)(left * CW), row_y(row), 1, g_scale);
        grid_text(L"×", SB_COLS - 2, row, hover == HIT_CLOSE && idx == tab ? g_look.red : g_look.dim);
        chrome_frame(row, active ? AMBER : g_look.dim);
    }
    if (g_chrome.settings_row >= 0) {
        int row = g_chrome.settings_row;
        chrome_row_fill(row, 0, hover == HIT_SETTINGS || hover == HIT_PLUS);
        grid_text(g_settings_open ? L"Settings −" : L"Settings +", 1, row, hover == HIT_SETTINGS ? g_look.text : g_look.text_lo);
        grid_text(L"+", SB_COLS - 2, row, hover == HIT_PLUS ? AMBER : g_look.text);
        chrome_frame(row, g_look.dim);
    }
    if (g_settings_open) render_settings(hover, idx);
    /* Only the final row owns a bottom edge. All interior horizontal strokes
       are drawn by the following tab or section, never doubled. */
    if (g_chrome.bottom > g_chrome.top)
        fill_rect(0, row_y(g_chrome.bottom) - 1, (float)chrome_draw_width(), row_y(g_chrome.bottom), g_look.dim, 1);
#ifdef _WIN32
    render_disk_warning(win_h);
#endif
    g_chrome_x = 0;
    UI_DONE;
    (void)win_w;
}
