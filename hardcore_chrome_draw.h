/* Pixel-edge frames share the glyph row. A boundary belongs to the following
   row, so adjacent rows share one device-pixel stroke. The right dock paints
   past the drawable edge, with no outside vertical rule. Its controls remain
   on the original grid and retain their full visible width. */
static float g_chrome_base_alpha;
static void chrome_begin_row(int row) { g_draw_alpha = g_chrome_base_alpha * g_chrome_motion_alpha; (void)row; }

/* Stage ordinary chrome geometry, then move complete glyph groups into the
   unobstructed part of each row. Backgrounds remain opaque; the terminal's
   complete occupied span, including its spaces, is cut out as one notch. */
static void chrome_capture_begin(void) {
    g_chrome_quad_count = 0; g_chrome_capture_failed = 0; g_chrome_capture = 1;
}
static int chrome_capture_plan(int raised) {
    int rows = g_chrome.bottom - g_chrome.top, cols = g_chrome_surface_cols, i, x;
    unsigned char *ink;
    /* Failure is fail-closed for hit testing: no invisible menu targets. */
    g_chrome_menu_count = -1; g_chrome_menu_raised = 0; g_chrome_menu_cols = 0;
    if (g_chrome_capture_failed || rows <= 0 || cols < 1 || cols > CHROME_PACK_MAX) return 0;
    if (rows > g_chrome_menu_capacity) {
        ChromePackRow *more = (ChromePackRow *)realloc(g_chrome_menu_rows, (size_t)rows * sizeof(*more));
        if (!more) return 0;
        g_chrome_menu_rows = more; g_chrome_menu_capacity = rows;
    }
    ink = (unsigned char *)calloc((size_t)rows * cols, 1);
    if (!ink) return 0;
    for (i = 0; i < g_chrome_quad_count; i++) {
        const Inst *q = &g_chrome_quads[i];
        int row = q->y0 / CH - g_chrome.top;
        if (q->u0 == 0xffff || row < 0 || row >= rows) continue;
        int left = (q->x0 - g_chrome_x) / CW;
        int right = (q->x1 - g_chrome_x + CW - 1) / CW;
        for (x = chrome_max(left, 0); x < chrome_min(right, cols); x++) ink[row * cols + x] = 1;
    }
    for (i = 0; i < rows; i++) {
        int left = 0, right = 0;
        if (!raised && g_tab_underlay && chrome_intrusion_span_at(g_chrome.top+i,g_chrome_x/CW,g_chrome.top,g_chrome.bottom,&left,&right)) {
            left -= g_chrome_x / CW; right -= g_chrome_x / CW;
        } else left = right = 0;
        chrome_pack_row(&g_chrome_menu_rows[i], ink + i * cols, cols, left, right);
    }
    free(ink);
    g_chrome_menu_top = g_chrome.top; g_chrome_menu_count = rows;
    g_chrome_menu_cols = cols;
    g_chrome_menu_x = g_chrome_x; g_chrome_menu_cw = CW; g_chrome_menu_ch = CH;
    g_chrome_menu_raised = raised;
    g_chrome_menu_alpha = g_chrome_motion_alpha;
    return 1;
}
static void chrome_emit_raw(const Inst *q, int x0, int x1, int y0, int y1) {
    if (x0 >= x1 || y0 >= y1) return;
    g_rgba = q->rgba;
    r_quad_uv((float)x0, (float)y0, (float)x1, (float)y1, q->u0, q->v0, q->u1, q->v1);
}
static int g_chrome_round_x, g_chrome_round_w, g_chrome_round_right;
static int g_chrome_join_top, g_chrome_join_bottom;
/* Equal-width panels on the same edge share the upper panel's last pixel.
 * Exposed corners stay round; touching corners form a straight joint. */
static int chrome_join_mask(int x,int cols,int top,int bottom,int other_x,int other_cols,int other_top,int other_bottom) {
    if(x!=other_x || cols!=other_cols || cols<1 || top>=bottom || other_top>=other_bottom)return 0;
    return (top==other_bottom?1:0) | (bottom==other_top?2:0);
}
static void chrome_prepare_edges(ChromeLayout primary,int primary_x,float primary_alpha) {
    int mask=0;
#ifndef _WIN32
    if(primary_alpha>0)
        mask|=chrome_join_mask(g_chrome_x,g_chrome_surface_cols,g_chrome.top,g_chrome.bottom,
                               primary_x,SB_COLS,primary.top,primary.bottom);
    for(int i=0;i<HC_TV_COUNT;i++) {
        const ChromeTv *tv=g_tvs+i;HcTvRect r=tv->state.rect;
        if(tv->state.open && tv->state.placed && tv->state.alpha*tv->motion.alpha>0)
            mask|=chrome_join_mask(g_chrome_x,g_chrome_surface_cols,g_chrome.top,g_chrome.bottom,r.x*CW,r.w,r.y,r.y+r.h);
    }
#else
    (void)primary;(void)primary_x;(void)primary_alpha;
#endif
    g_chrome_join_top=mask&1;g_chrome_join_bottom=(mask&2)!=0;
}
static int chrome_corner_inset(int y) {
    int radius=chrome_min(6,chrome_min(CW/2,CH/3));
    int top=g_chrome.top*CH,bottom=g_chrome.bottom*CH;
    int d=chrome_min(g_chrome_join_top?radius:y-top,
                    g_chrome_join_bottom?radius:bottom-1-y),inset=0;
    if(d<0)return radius;
    if(d>=radius)return 0;
    while(inset<radius && (radius-inset)*(radius-inset)+(radius-d-1)*(radius-d-1)>radius*radius)inset++;
    return inset;
}
static void chrome_emit_inst(const Inst *q,int x0,int x1,int y0,int y1) {
    int r=chrome_min(6,chrome_min(CW/2,CH/3));
    if(q->u0!=0xffff || (y0>=g_chrome.top*CH+r && y1<=g_chrome.bottom*CH-r)) {
        chrome_emit_raw(q,x0,x1,y0,y1);return;
    }
    for(int y=y0;y<y1;y++) {
        int inset=chrome_corner_inset(y);
        int left=g_chrome_round_x+(g_chrome_round_x>0?inset:0);
        int right=g_chrome_round_x+g_chrome_round_w-(g_chrome_round_right?inset:0);
        chrome_emit_raw(q,chrome_max(x0,left),chrome_min(x1,right),y,y+1);
    }
}
static int chrome_pack_translation(const ChromePackRow *p, int left, int right, int *shift) {
    int x, d;
    if (left < 0 || right > g_chrome_menu_cols || left >= right || p->dst[left] < 0) return 0;
    d = p->dst[left] - left;
    for (x = left + 1; x < right; x++) if (p->dst[x] != x + d) return 0;
    *shift = d * CW;
    return 1;
}
static void chrome_capture_end(int raised) {
    int i, x, origin = g_chrome_x;
    g_chrome_capture = 0;
    if (!chrome_capture_plan(raised)) return;
    g_chrome_round_x=origin;
    int width=g_chrome_surface_cols*CW;
    g_chrome_round_w=origin+width>g_clip_w-CW?g_clip_w-origin:width;
    g_chrome_round_right=origin+g_chrome_round_w<g_clip_w;
    g_chrome_x = 0;
    for (i = 0; i < g_chrome_quad_count; i++) {
        const Inst *q = &g_chrome_quads[i];
        int row = q->y0 / CH - g_chrome.top;
        if (row < 0 || row >= g_chrome_menu_count) continue;
        const ChromePackRow *p = &g_chrome_menu_rows[row];
        int left = (q->x0 - origin) / CW, right = (q->x1 - origin + CW - 1) / CW;
        int shift = 0, x0 = q->x0, x1 = q->x1;
        if (q->u0 != 0xffff) {
            /* A wide glyph stays whole and carries its original atlas and
               foreground color. Partial clipping never creates half glyphs. */
            if (chrome_pack_translation(p, left, right, &shift))
                chrome_emit_inst(q, x0 + shift, x1 + shift, q->y0, q->y1);
            continue;
        }
        /* Move selected font-size/theme-key backgrounds with their glyphs.
           Full-row fills and section rules instead keep their original span. */
        if (p->blocked_left != p->blocked_right && x1-x0 >= CW && x1-x0 <= 8*CW &&
            q->y1-q->y0 == CH && (x0-origin)%CW == 0 && (x1-origin)%CW == 0) {
            if (!chrome_pack_translation(p, left, right, &shift)) continue;
            x0 += shift; x1 += shift;
        }
        if (p->blocked_left == p->blocked_right) chrome_emit_inst(q, x0, x1, q->y0, q->y1);
        else {
            int cut0 = origin + p->blocked_left*CW, cut1 = origin + p->blocked_right*CW;
            chrome_emit_inst(q, x0, chrome_min(x1, cut0), q->y0, q->y1);
            chrome_emit_inst(q, chrome_max(x0, cut1), x1, q->y0, q->y1);
        }
    }
    /* Outline the actual remaining shape, not individual terminal letters.
       Each edge occupies exactly one physical pixel, independent of scale.
       Horizontal steps lie on the menu side of the boundary, so terminal
       text and its background can be drawn afterward without erasing them. */
    r_color(g_look.dim, 1);
    for (i = 0; i < g_chrome_menu_count; i++) {
        const ChromePackRow *p = &g_chrome_menu_rows[i];
        int y = (g_chrome.top + i) * CH;
        if (p->blocked_left == p->blocked_right) continue;
        if (p->blocked_left > 0) {
            int edge = origin + p->blocked_left * CW;
            quad((float)(edge-1), (float)y, (float)edge, (float)(y+CH));
        }
        if (p->blocked_right < g_chrome_menu_cols) {
            int edge = origin + p->blocked_right * CW;
            quad((float)edge, (float)y, (float)(edge+1), (float)(y+CH));
        }
    }
    for (i = 1; i < g_chrome_menu_count; i++) {
        const ChromePackRow *a = &g_chrome_menu_rows[i-1], *b = &g_chrome_menu_rows[i];
        int y = (g_chrome.top + i) * CH;
        for (x = 0; x < g_chrome_menu_cols; x++) {
            int above = x >= a->blocked_left && x < a->blocked_right;
            int below = x >= b->blocked_left && x < b->blocked_right;
            if (above != below) {
                int yy = y - below;
                quad((float)(origin+x*CW), (float)yy, (float)(origin+(x+1)*CW), (float)(yy+1));
            }
        }
    }
    g_chrome_x = origin;
}
static int chrome_draw_width(void) {
    int width=g_chrome_surface_cols*CW;
    /* Fill only a partial edge column, never an intentional full-cell gap. */
    return g_chrome_x+width>g_clip_w-CW?g_clip_w-g_chrome_x:width;
}
static void chrome_round_edges(void) {
    int width=chrome_draw_width(),radius=chrome_min(6,chrome_min(CW/2,CH/3));
    for(int i=0;i<radius;i++)for(int edge=0;edge<2;edge++) {
        if(edge?g_chrome_join_bottom:g_chrome_join_top)continue;
        int y=edge?g_chrome.bottom*CH-1-i:g_chrome.top*CH+i,inset=chrome_corner_inset(y);
        if(g_chrome_x>0)fill_rect((float)inset,(float)y,(float)(inset+1),(float)(y+1),g_look.dim,1);
        if(g_chrome_x+width<g_clip_w)fill_rect((float)(width-inset-1),(float)y,(float)(width-inset),(float)(y+1),g_look.dim,1);
    }
}
#ifndef _WIN32
/* A second one-device-pixel outline leaves one untouched pixel between the
 * strokes. Its smaller corner radius shares the outer outline's center. */
static void chrome_inner_frame(uint32_t color) {
    int width=chrome_draw_width(),left=2,right=width-2;
    int top=g_chrome.top*CH+2,bottom=g_chrome.bottom*CH-2;
    int radius=chrome_max(0,chrome_min(6,chrome_min(CW/2,CH/3))-2);
    int round_left=g_chrome_x>0,round_right=g_chrome_x+width<g_clip_w;
    int tr=g_chrome_join_top?0:radius,br=g_chrome_join_bottom?0:radius;
    int ti=0,bi=0;
    if(left>=right || top>=bottom)return;
    while(ti<tr && (tr-ti)*(tr-ti)+(tr-1)*(tr-1)>tr*tr)ti++;
    while(bi<br && (br-bi)*(br-bi)+(br-1)*(br-1)>br*br)bi++;
    fill_rect((float)(left+(round_left?ti:0)),(float)top,
              (float)(right-(round_right?ti:0)),(float)(top+1),color,1);
    fill_rect((float)(left+(round_left?bi:0)),(float)(bottom-1),
              (float)(right-(round_right?bi:0)),(float)bottom,color,1);
    fill_rect((float)left,(float)(top+(round_left?tr:0)),
              (float)(left+1),(float)(bottom-(round_left?br:0)),color,1);
    fill_rect((float)(right-1),(float)(top+(round_right?tr:0)),
              (float)right,(float)(bottom-(round_right?br:0)),color,1);
    for(int edge=0;edge<2;edge++) {
        int r=edge?br:tr;
        for(int d=0;d<r;d++) {
            int inset=0,y=edge?bottom-1-d:top+d;
            while(inset<r && (r-inset)*(r-inset)+(r-d-1)*(r-d-1)>r*r)inset++;
            if(round_left)fill_rect((float)(left+inset),(float)y,(float)(left+inset+1),(float)(y+1),color,1);
            if(round_right)fill_rect((float)(right-inset-1),(float)y,(float)(right-inset),(float)(y+1),color,1);
        }
    }
}
#endif
static void chrome_sides(int row, uint32_t color) {
    float y = row_y(row), w = (float)chrome_draw_width();
    fill_rect(0, y, 1, y + CH, color, 1);
    if (g_chrome_x+(int)w<g_clip_w) fill_rect(w - 1, y, w, y + CH, color, 1);
}
static void chrome_rule(int row, uint32_t color) {
    if(row==g_chrome.top && g_chrome_join_top)return;
    fill_rect(0, row_y(row), (float)chrome_draw_width(), row_y(row) + 1, color, 1);
}
static void chrome_frame(int row, uint32_t color) {
    chrome_sides(row, color);
    chrome_rule(row, color);
}
static void chrome_row_fill(int row, int selected, int hovered) {
    chrome_begin_row(row);
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
    chrome_row_fill(row, 0, 0);
    for (n = 0; n < 2; n++) {
        int left = n * (SB_COLS / 2), right = (n + 1) * (SB_COLS / 2);
        int active = g_settings_tab == n, pointed = hover == HIT_STAB && hidx == n;
        if (pointed) fill_rect((float)(left * CW), row_y(row), (float)(right * CW), row_y(row + 1), TAB_HOVER, 1);
        grid_text(n ? L"Theme" : L"Font", left + (right - left - (n ? 5 : 4)) / 2, row,
                  active || pointed ? on : off);
        if (active) fill_rect((float)(left * CW + 1), row_y(row + 1) - 1,
                              (float)(right * CW - 1), row_y(row + 1), g_look.detail, .55f);
    }
    chrome_frame(row, g_look.dim);
    for (n = 0; n < g_chrome.list_count; n++) {
        int item = offset + n, left = 1, right = SB_COLS - 1, selected = 0, pointed = 0;
        row = g_chrome.list_row + n;
        if (!g_settings_tab) {
            const FontMenuRow *fonts = &g_font_menu_rows[item];
            int k;
            selected = !strcmp(FONTS[fonts->family].name, g_font->name);
            for (k = 0; k < fonts->count; k++)
                if (hover == HIT_FONT && hidx == fonts->sizes[k].font) pointed = 1;
        }
        else if (g_settings_tab == 1) {
            selected = g_scheme->key == FAMILIES[item].dark || g_scheme->key == FAMILIES[item].light;
            pointed = hover == HIT_SCHEME && (SCHEMES[hidx].key == FAMILIES[item].dark || SCHEMES[hidx].key == FAMILIES[item].light);
        }
        chrome_row_fill(row, g_settings_tab ? selected : 0, g_settings_tab ? pointed : 0);
        if (!n) chrome_pager_draw(row, offset, g_chrome.list_capacity, chrome_item_total(), 0,
                                  hover == HIT_SETPAGE, hidx, &left, &right);
        if (!g_settings_tab) {
            const FontMenuRow *fonts = &g_font_menu_rows[item];
            int k;
            if (fonts->show_name) {
                utf8_to_wide(FONTS[fonts->family].name, buf, 96);
                chrome_label(buf, fonts->name_left, fonts->name_right, row, selected ? AMBER : off);
            }
            for (k = 0; k < fonts->count; k++) {
                const FontMenuSize *size = &fonts->sizes[k];
                int current = &FONTS[size->font] == g_font;
                int hovered = hover == HIT_FONT && hidx == size->font;
                if (current || hovered)
                    fill_rect((float)(size->left * CW), row_y(row), (float)(size->right * CW), row_y(row + 1), current ? AMBER : TAB_HOVER, 1);
                grid_text(size->label, size->left, row, current ? DEF_BG : hovered ? on : g_look.detail);
            }
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
        }
        chrome_sides(row, g_look.dim);
    }
}

#include "hardcore_tv_draw.h"

static int chrome_tab_hover(int hit,int row) {
    return (hit==HIT_TAB || hit==HIT_CLOSE || hit==HIT_CUTIE || hit==HIT_TABPAGE || hit==HIT_CHROME_INERT) &&
        g_mouse_x>=g_chrome_x && g_mouse_x<g_chrome_x+sidebar_w() && g_mouse_y/CH==row;
}
static void chrome_render_header(int hover) {
    if (!g_chrome.grip_rows) return;
#ifdef _WIN32
    chrome_row_fill(g_chrome.top, 0, 0);
    if (hover == HIT_GRIP || g_chrome_drag)
        fill_rect((float)CW, row_y(g_chrome.top), (float)(2 * CW), row_y(g_chrome.top + 1), TAB_HOVER, 1);
    grid_text(L"↕", 1, g_chrome.top, hover == HIT_GRIP || g_chrome_drag ? g_look.text : g_look.detail);
    grid_text(L"hardcore", 3, g_chrome.top, g_look.detail);
    chrome_frame(g_chrome.top, g_look.dim);
#else
    for (int row = 0; row < g_chrome.grip_rows; row++) {
        int y = g_chrome.top + row;
        chrome_row_fill(y, 0, 0);
        for (int i = 0; i < CHROME_HEADER_CONTROL_COUNT; i++) {
            const ChromeHeaderControl *c = CHROME_HEADER_CONTROLS + i;
            if (c->row != row) continue;
            int highlighted = hover == c->hit || (c->hit == HIT_GRIP && g_chrome_drag);
            if (highlighted) fill_rect((float)(c->col * CW), row_y(y),
                (float)((c->col + c->width) * CW), row_y(y + 1), TAB_HOVER, 1);
            uint32_t color = highlighted ? g_look.text : c->hit == HIT_CAF ?
                (mac_caf_active() ? AMBER : g_look.dim) : g_look.detail;
            const wchar_t *label = c->hit == HIT_CAF && !mac_caf_active() ? L"caf" : c->label;
            if(c->hit==HIT_CAMERA && mac_record_active()){label=L"REC";color=g_look.red;}
            grid_text(label, c->col, y, color);
        }
        if (!row) chrome_frame(y, g_look.dim);
        else chrome_sides(y, g_look.dim);
    }
#endif
}
static void render_tabbar(int win_w, int win_h) {
    int i, idx = -1, hover = HIT_NONE;
    uint16_t drip = g_ndrip ? g_drip[(GetTickCount() / DRIP_MS) % g_ndrip] : 0x2800;
    float saved_alpha = g_draw_alpha;
    int clip=g_clip_w;g_clip_w=win_w;
    g_chrome_base_alpha = saved_alpha;
    UI_FONT;
    if (g_mouse_y >= 0) hover = hit_tabbar(g_mouse_x, g_mouse_y, &idx);
    tab_layout();
    g_chrome_surface_cols=SB_COLS;
    g_chrome_x = sidebar_x();
    chrome_prepare_edges(g_chrome,g_chrome_x,g_chrome_motion_alpha);
    chrome_capture_begin();
    for (i = g_chrome.top; i < g_chrome.bottom; i++) {
        chrome_begin_row(i);
        fill_rect(0, row_y(i), (float)chrome_draw_width(), row_y(i + 1), BAR_BG, 1);
#ifndef _WIN32
        plat_render_title_background(0, row_y(i), (float)chrome_draw_width(), row_y(i + 1), BAR_BG, DEF_BG);
#endif
    }
    chrome_render_header(hover);
    for (i = 0; i < g_chrome.tab_count; i++) {
        int tab = g_tab_offset + i, row = g_chrome.tab_row + i, left = 1, right = SB_COLS - 5;
        int active = tab == g_active, hovered = chrome_tab_hover(hover,row);
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
        #ifndef _WIN32
        int cutie_open=chrome_tv_slot_for(tb->id)>=0;
        if (hovered || cutie_open)
            grid_text(hovered && cutie_open?L"−":L"□", SB_COLS - 4, row,
                      hover == HIT_CUTIE && idx == tab ? AMBER : g_look.text_lo);
        #endif
        if (hovered) grid_text(L"×", SB_COLS - 2, row, hover == HIT_CLOSE && idx == tab ? g_look.red : g_look.dim);
        chrome_sides(row, g_look.dim);
        if (!i) chrome_rule(row, g_look.dim);
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
    if (g_chrome.bottom > g_chrome.top) {
        chrome_begin_row(g_chrome.bottom - 1);
        fill_rect(0, row_y(g_chrome.bottom) - 1, (float)chrome_draw_width(), row_y(g_chrome.bottom), g_look.dim, 1);
    }
    chrome_round_edges();
    chrome_capture_end(chrome_hovered() || !g_tab_underlay);
#ifndef _WIN32
    render_tvs();
#endif
    g_clip_w=clip;
    g_draw_alpha = saved_alpha;
#ifdef _WIN32
    render_disk_warning(win_h);
#endif
    g_chrome_x = 0;
    UI_DONE;
    (void)win_w; (void)win_h;
}
