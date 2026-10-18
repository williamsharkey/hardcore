/* The Mac tab menu's behavior on Win32: header actions, TV dragging, Cuties,
   grid panes and their utility rail. Included in the Win32 window procedure's
   translation unit, after the shared chrome, TV, Cutie and grid code. The
   names keep their mac_ prefix because the shared code calls them by it. */

#include <math.h>
#include <shlobj.h>

static void win_cursor(LPCWSTR id) { SetCursor(LoadCursorW(NULL, id)); }

/* ── keep awake (CAF) ── */
/* SetThreadExecutionState is per thread: the UI thread lives as long as the
   window, so the request lasts until it's turned off or the window closes. */
static int g_caf_on;
static uint64_t g_caf_began;
static int mac_caf_active(void) { return g_caf_on; }
static int mac_caf_toggle(void) {
    if (g_caf_on) {
        SetThreadExecutionState(ES_CONTINUOUS);
        g_caf_on = 0;
        return 0;
    }
    if (!SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)) return -1;
    g_caf_on = 1;
    g_caf_began = GetTickCount64();
    return 1;
}
static double mac_caf_elapsed(void) { return g_caf_on ? (double)(GetTickCount64() - g_caf_began) / 1000.0 : 0; }
static const char *mac_caf_power(void) {
    static char text[96];
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s) || s.ACLineStatus == 255) return "Power status unavailable";
    const char *supply = s.ACLineStatus == 1 ? "Plugged in" : "On battery";
    if (s.BatteryFlag == 128 || s.BatteryLifePercent > 100) snprintf(text, sizeof(text), "%s", supply);
    else snprintf(text, sizeof(text), "%s · %d%%%s", supply, s.BatteryLifePercent, (s.BatteryFlag & 8) ? " · charging" : "");
    return text;
}

/* Input held while a Claude adapter restores its draft: a Mac-only adapter. */
static int mac_input_held(Tab *tb) { (void)tb; return 0; }

#define TIMER_MEDIA 9                /* music position and camera clock, while either needs it */
/* The panels' share of the window's mouse events */
enum { MEV_MOVE, MEV_DOWN, MEV_DRAG, MEV_UP, MEV_WHEEL, MEV_OTHER };
static void media_schedule(void);
static void mac_cutie_blur(void);
#include "hardcore_win_browser.h"
#include "hardcore_win_camera.h"
#include "hardcore_win_music.h"

/* ── files: the bundled browser, run by WSL's python3 ── */
static Tab *files_tab_new(const char *dir) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) { free(tb); return NULL; }
    tb->id = g_next_id++;
    InitializeCriticalSection(&tb->lock);
    tb->kind = TK_WSL;
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", dir);
    snprintf(tb->run, sizeof(tb->run), "files:");
    if (dir[0]) set_cwd(&tb->t, dir);
    if (!g_have_distro) show_help(tb, HELP_NO_DISTRO, 0);
    else tab_spawn(tb);
    return tb;
}
static void mac_files_at(const char *requested) {
    if (g_ntabs >= MAX_TABS) return;
    Tab *cur = active_tab();
    Tab *tb = files_tab_new(requested ? requested : cur && cur->t.cwd[0] ? cur->t.cwd : cur ? cur->start_dir : "");
    if (!tb) return;
    g_tabs[g_ntabs++] = tb;
    activate(g_ntabs - 1);
}
static void mac_files_new(void) { mac_files_at(NULL); }

/* ── grid panes ── */
static int g_grid_mouse_consumed, g_grid_drag_grab;
static int mac_grid_utility_click(int x, int y);
static int mac_grid_down(int mx, int my) {
    if (!g_grid_on) return 0;
    if (mac_grid_utility_click(mx, my)) { g_grid_mouse_consumed = 1; return 1; }
    int boundary, axis = grid_divider_at(mx, my, &boundary);
    if (axis) {
        g_grid_drag_axis = axis; g_grid_drag_boundary = boundary; g_grid_mouse_consumed = 1; g_sel_on = g_sel_drag = 0;
        /* Grabbed a few pixels off the line: a click without a move leaves it where it is */
        g_grid_drag_grab = axis == 1 ? mx - (g_grid_left + g_grid.x[boundary]) * CW : my - (g_grid_top + g_grid.y[boundary]) * CH;
        return 1;
    }
    int index = grid_pane_at(mx, my);
    if (index < 0) return 0;
    Tab *tb = g_tabs[index];
    HcGridRect r = grid_rect(tb->id);
    if (my < (r.y + 1) * CH) {
        int action = grid_title_action(r, mx);
        g_grid_mouse_consumed = 1;
        if (action == 2) { grid_hide_tab(index); return 1; }
        activate(index);
        if (action == 1) grid_toggle();
        else if (action == 3) { g_grid_expand_id = tb->id; grid_refit(); }
        return 1;
    }
    if (index != g_active) activate(index);
    if (tb->kind == TK_BROWSER) {
        g_grid_mouse_consumed = 1;
        int col = mx / CW - r.x, row = my / CH - r.y - 1, at = row * r.w + col;
        if (g_grid_browser_id[index] == tb->id && g_grid_browser_generation[index] == mac_browser_cutie_generation(tb) &&
            at >= 0 && at < g_grid_browser_capacity[index] && g_grid_browser_cells[index])
            mac_browser_cutie_action(tb, g_grid_browser_cells[index][at].link);
        return 1;
    }
    return 0;
}
static void mac_grid_drag(int mx, int my) {
    if (!g_grid_drag_axis) return;
    int vertical = g_grid_drag_axis == 1, at = (vertical ? mx : my) - g_grid_drag_grab, cell = vertical ? CW : CH;
    hc_grid_move(&g_grid, vertical, g_grid_drag_boundary, (at + cell / 2) / cell - (vertical ? g_grid_left : g_grid_top));
    grid_refit();
}
/* Right and middle clicks: the pane under the pointer, as a left click picks it */
static void mac_grid_point(int mx, int my) {
    int index = grid_pane_at(mx, my);
    if (index >= 0 && index != g_active) activate(index);
}
static int mac_grid_cursor(int mx, int my) {
    if (!g_grid_on) return 0;
    int boundary, axis = g_grid_drag_axis ? g_grid_drag_axis : grid_divider_at(mx, my, &boundary);
    if (axis) { win_cursor(axis == 1 ? IDC_SIZEWE : IDC_SIZENS); return 1; }
    int index = grid_pane_at(mx, my);
    if (index >= 0 && my < (grid_rect(g_tabs[index]->id).y + 1) * CH) { win_cursor(IDC_HAND); return 1; }
    return 0;
}

static void mac_grid_transition(int entering) {
    static HcTvRect rects[HC_TV_COUNT + 2];
    static HcTvPlacement placements[HC_TV_COUNT + 2];
    static int saved;
    if (entering) {
        for (int i = 0; i < HC_TV_COUNT; i++) { rects[i] = g_tvs[i].state.rect; placements[i] = g_tvs[i].placement; }
        rects[HC_TV_COUNT] = g_music_rect; placements[HC_TV_COUNT] = g_music_placement;
        rects[HC_TV_COUNT + 1] = g_camera_rect; placements[HC_TV_COUNT + 1] = g_camera_placement;
        saved = 1;
    } else if (saved) {
        for (int i = 0; i < HC_TV_COUNT; i++) { g_tvs[i].state.rect = rects[i]; g_tvs[i].placement = placements[i]; }
        g_music_rect = rects[HC_TV_COUNT]; g_music_placement = placements[HC_TV_COUNT];
        g_camera_rect = rects[HC_TV_COUNT + 1]; g_camera_placement = placements[HC_TV_COUNT + 1];
        saved = 0;
    }
}
/* Grid utilities occupy a stable side rail as wide as the tab menu; opening them never resizes panes. */
static void mac_grid_browser_sync(void) { mac_browser_sync(); }
static int g_grid_utility_choice, g_grid_utility_mask, g_grid_utility_row = -1, g_grid_utility_x;
static const wchar_t *g_grid_utility_names[] = {L"Config", L"Music", L"Cam", L"CAF"};
static const int g_grid_utility_cols[] = {0, 8, 16, 22};
static int mac_grid_utility_click(int x, int y) {
    if (!g_grid_on || g_grid_utility_row < 0 || y / CH != g_grid_utility_row) return 0;
    int col = x / CW - g_grid_utility_x;
    for (int i = 0; i < 4; i++)
        if ((g_grid_utility_mask & (1 << i)) && col >= g_grid_utility_cols[i] && col < g_grid_utility_cols[i] + 6) {
            g_grid_utility_choice = i; plat_invalidate(); return 1;
        }
    return 0;
}
static void mac_grid_utility_draw(void) {
    if (g_grid_utility_row < 0) return;
    for (int i = 0; i < 4; i++)
        if (g_grid_utility_mask & (1 << i))
            draw_label(g_grid_utility_names[i], (float)((g_grid_utility_x + g_grid_utility_cols[i]) * CW), (float)(g_grid_utility_row * CH),
                       (float)(6 * CW), g_grid_utility_choice == i ? AMBER : g_look.detail);
}
static void mac_grid_utilities(void) {
    int x = g_grid_left ? 0 : g_cols - g_grid_rail, y = g_chrome.bottom + 1, room = g_rows - y;
    int music = g_music_visible, camera = g_camera_visible, settings = g_tvs[HC_TV_SETTINGS].state.open;
    int caf = g_tvs[HC_TV_CAF].state.open;
    int minimum = (music ? 9 : 0) + (camera ? 9 : 0) + (settings ? 3 : 0) + (caf ? 2 : 0);
    int mask = settings | (music << 1) | (camera << 2) | (caf << 3), opened = mask & ~g_grid_utility_mask;
    if (opened) for (int i = 0; i < 4; i++) if (opened & (1 << i)) g_grid_utility_choice = i;
    g_grid_utility_mask = mask; g_grid_utility_row = -1;
    if (minimum > room && mask && room > 3) {
        if (!(mask & (1 << g_grid_utility_choice))) for (int i = 0; i < 4; i++) if (mask & (1 << i)) { g_grid_utility_choice = i; break; }
        g_grid_utility_x = x; g_grid_utility_row = y++; room--;
        settings = g_grid_utility_choice == 0; music = g_grid_utility_choice == 1;
        camera = g_grid_utility_choice == 2; caf = g_grid_utility_choice == 3;
        minimum = (music ? 9 : 0) + (camera ? 9 : 0) + (settings ? 3 : 0) + (caf ? 2 : 0);
    }
    int extra = chrome_max(0, room - minimum);
    for (int i = 0; i < HC_TV_COUNT; i++) g_tvs[i].state.placed = 0;
    g_music_placed = g_camera_placed = 0;
    if (settings && room >= 3) {
        int items = chrome_item_total(), height = chrome_min(items + 2, 3 + chrome_min(extra, 10));
        extra -= height - 3;
        int capacity = height - 2;
        if (!g_settings_tab) {
            items = font_menu_build(capacity, items);
            height = chrome_min(height, items + 2); capacity = height - 2;
        }
        g_setting_offset[g_settings_tab] = chrome_page_offset(g_setting_offset[g_settings_tab], items, capacity);
        ChromeTv *tv = g_tvs + HC_TV_SETTINGS;
        tv->state.rect = (HcTvRect){x, y, SB_COLS, height}; tv->state.placed = 1;
        tv->state.alpha = tv->motion.alpha = 1; hc_tv_interact(&tv->state, GetTickCount());
        tv->layout = (ChromeLayout){.top = y, .bottom = y + height, .settings_row = -1, .section_row = y + 1,
            .list_row = y + 2, .list_capacity = capacity, .list_count = chrome_min(capacity, items - g_setting_offset[g_settings_tab])};
        y += height; room -= height;
    }
    if (music && room >= 9) {
        int height = 9 + chrome_min(extra, 7);
        extra -= height - 9;
        g_music_rect = (HcTvRect){x, y, g_grid_rail, height}; g_music_placed = 1; y += height; room -= height;
        g_music_offset = chrome_min(g_music_offset, chrome_max(0, music_count() - mac_music_capacity()));
    }
    if (camera && room >= 9) {
        int height = 9 + chrome_min(extra, 3);
        extra -= height - 9;
        g_camera_rect = (HcTvRect){x, y, g_grid_rail, height}; g_camera_placed = 1; y += height; room -= height;
    }
    if (caf && room >= 2) {
        int height = chrome_min(room, 6);
        ChromeTv *tv = g_tvs + HC_TV_CAF;
        tv->state.rect = (HcTvRect){x, y, SB_COLS, height}; tv->state.placed = 1;
        tv->state.alpha = tv->motion.alpha = 1; hc_tv_interact(&tv->state, GetTickCount());
        tv->layout = (ChromeLayout){.top = y, .bottom = y + height, .settings_row = -1, .section_row = -1, .list_row = -1};
    }
    mac_camera_preview_sync();
}

/* ── the header row's buttons ── */
static int mac_header_action(int hit) {
    if (hit == HIT_GRID) grid_toggle();
    else if (hit == HIT_GRID_EXPAND) grid_expand_next();
    else if (hit == HIT_PLUS) new_tab_here();
    else if (hit == HIT_WEB) mac_browser_open_welcome();
    else if (hit == HIT_FILES) mac_files_new();
    else if (hit == HIT_CAMERA) mac_camera_toggle();
    else if (hit == HIT_MUSIC) mac_music_toggle();
    else if (hit == HIT_MUSIC_PLAY) mac_music_play_pause();
    else if (hit == HIT_MUSIC_PREV) mac_music_next(-1);
    else if (hit == HIT_MUSIC_NEXT) mac_music_next(1);
    else if (hit == HIT_MUSIC_STOP) mac_music_stop();
    else if (hit == HIT_CAF) {
        int on = mac_caf_toggle();
        if (on > 0) hc_tv_open(&g_tvs[HC_TV_CAF].state, GetTickCount());
        else hc_tv_close(&g_tvs[HC_TV_CAF].state);
        plat_invalidate();
    } else return 0;
    return 1;
}

/* ── Cuties: live previews of other tabs, which can take the keyboard ── */
static int g_cutie_mouse_consumed, g_cutie_mouse_id, g_cutie_mouse_col, g_cutie_mouse_row, g_cutie_mouse_mods, g_cutie_aux_buttons;
static void mac_cutie_release(void) {
    Tab *tb = tab_by_id(g_cutie_mouse_id);
    if (tb && !mac_input_held(tb) && tb->t.m_mouse) {
        char buf[64];
        int n;
        if (tb->t.m_mouse_sgr) n = snprintf(buf, sizeof(buf), "\033[<%d;%d;%dm", g_cutie_mouse_mods, g_cutie_mouse_col + 1, g_cutie_mouse_row + 1);
        else { n = 6; memcpy(buf, "\033[M", 3); buf[3] = 35; buf[4] = (char)(33 + g_cutie_mouse_col); buf[5] = (char)(33 + g_cutie_mouse_row); }
        tab_write(tb, buf, n);
    }
    g_cutie_mouse_consumed = g_cutie_mouse_id = 0;
}
static void mac_cutie_blur(void) {
    if (!g_cutie_focus_id) return;
    Tab *tb = tab_by_id(g_cutie_focus_id);
    g_cutie_focus_id = 0;
    mac_cutie_release();
    Tab *selected = active_tab();
    if (tb && tb != selected) {
        if (tb->t.m_focus && !mac_input_held(tb)) tab_write(tb, "\033[O", 3);
        if (g_focused && selected && selected->t.m_focus && !mac_input_held(selected)) tab_write(selected, "\033[I", 3);
    }
    mac_browser_sync();
    plat_invalidate();
}

/* Pointer capture belongs to the TV chrome, never its terminal. Manual
   geometry is grid-aligned and only affects the independent projection. */
static HcTvRect g_tv_drag_origin;
static int g_tv_drag_x, g_tv_drag_y;
static void win_tv_drag_cursor(void) { win_cursor(g_tv_drag_mode ? IDC_SIZENWSE : IDC_SIZEALL); }
static void mac_tv_drag_end(void) {
    if (g_tv_drag_slot < 0) return;
    g_tv_drag_slot = -1;
    /* Keep the matching mouse-up consumed even when focus loss ends capture. */
    g_cutie_mouse_consumed = 1; g_cutie_mouse_id = 0;
    win_cursor(IDC_ARROW);
    plat_invalidate();
}
static int mac_tv_drag_begin(int mx, int my) {
    if (g_grid_on) return 0;
    int slot = -1, hit = chrome_tv_hit(mx, my, &slot);
    if (slot < 0 || slot >= HC_TV_COUNT || (hit != HIT_TV_GRIP && hit != HIT_TV_RESIZE)) return 0;
    mac_cutie_blur(); chrome_preview_cancel();
    g_tv_drag_slot = slot; g_tv_drag_mode = hit == HIT_TV_RESIZE;
    g_tv_drag_origin = g_tvs[slot].state.rect; g_tv_drag_x = mx; g_tv_drag_y = my;
    g_cutie_mouse_consumed = 1; g_cutie_mouse_id = 0;
    g_mouse_x = mx; g_mouse_y = my; g_sel_drag = g_sb_drag = 0;
    hc_tv_motion_reset(&g_tvs[slot].motion);
    chrome_tv_interaction(slot);
    win_tv_drag_cursor();
    plat_invalidate();
    return 1;
}
static void mac_tv_drag_move(int mx, int my) {
    if (g_tv_drag_slot < 0 || g_tv_drag_slot >= HC_TV_COUNT) return;
    ChromeTv *tv = g_tvs + g_tv_drag_slot;
    Tab *tb = active_tab();
    if (!tv->state.open || !tb) { mac_tv_drag_end(); return; }
    int cols = tb->t.cols, rows = tb->kind == TK_BROWSER ? mac_browser_page_rows(tb) : tb->t.rows;
    HcTvRect r = g_tv_drag_origin;
    int dx = (int)lround((double)(mx - g_tv_drag_x) / CW), dy = (int)lround((double)(my - g_tv_drag_y) / CH);
    if (g_tv_drag_mode) {
        r.w = chrome_min(chrome_max(HC_TV_MIN_WIDTH, r.w + dx), chrome_min(HC_TV_MAX_WIDTH, cols - r.x));
        r.h = chrome_min(chrome_max(HC_TV_MIN_HEIGHT, r.h + dy), chrome_min(HC_TV_MAX_HEIGHT, rows - r.y));
    } else {
        r.x = chrome_min(chrome_max(0, r.x + dx), chrome_max(0, cols - r.w));
        r.y = chrome_min(chrome_max(0, r.y + dy), chrome_max(0, rows - r.h));
    }
    g_mouse_x = mx; g_mouse_y = my;
    HcTvRect primary = {sidebar_x() / CW, g_chrome.top, SB_COLS, g_chrome.bottom - g_chrome.top};
    int fits = hc_tv_rect_fits(r, cols, rows) && !hc_tv_rect_overlap(r, primary);
    for (int i = 0; i < HC_TV_COUNT && fits; i++)
        if (i != g_tv_drag_slot && g_tvs[i].state.placed && hc_tv_rect_overlap(r, g_tvs[i].state.rect)) fits = 0;
    if (fits && !hc_tv_rect_equal(r, tv->state.rect)) {
        tv->manual = 1; tv->requested = r; tv->state.rect = r;
        tv->placement = (HcTvPlacement){.rect = r, .valid = 1};
        tv->frame.count = 0;
        if (g_tv_drag_slot >= HC_TV_CUTIE_FIRST) g_cuties[g_tv_drag_slot].valid = 0;
        hc_tv_motion_reset(&tv->motion); chrome_tv_interaction(g_tv_drag_slot); plat_invalidate();
    }
    win_tv_drag_cursor();
}
static int mac_cutie_click(int mx, int my, int mods) {
    int slot = cutie_at(mx, my);
    if (slot < 0) return 0;
    g_cutie_mouse_consumed = 1; g_cutie_mouse_id = 0;
    ChromeTv *tv = g_tvs + slot;
    Tab *tb = tab_by_id(tv->tab_id);
    if (!tb) return 1;
    int idx = -1, hit = chrome_tv_hit(mx, my, &idx);
    if (hit == HIT_NONE) return 1;
    if (hit == HIT_TV_CLOSE) { mac_cutie_blur(); chrome_tv_close_slot(slot); g_cutie_mouse_consumed = 1; return 1; }
    HcTvRect r = tv->state.rect;
    if (my / CH == r.y) {
        if (hit == HIT_TV_EXPAND) for (int i = 0; i < g_ntabs; i++) if (g_tabs[i] == tb) { activate(i); break; }
        g_cutie_mouse_consumed = 1;
        return 1;
    }
    int col = 0, row = 0, node = 0, mapped = cutie_source_at(slot, mx, my, &col, &row, &node);
    if (tb->kind == TK_BROWSER) { mac_cutie_blur(); g_cutie_mouse_consumed = 1; if (mapped) mac_browser_cutie_action(tb, node); return 1; }
    if (mac_input_held(tb)) { MessageBeep(MB_OK); return 1; }
    if (g_cutie_focus_id != tb->id) {
        mac_cutie_blur(); g_cutie_focus_id = tb->id;
        SetFocus(g_hwnd); g_sel_drag = g_sel_on = 0;
        Tab *selected = active_tab();
        if (tb != selected) {
            if (selected && selected->t.m_focus && !mac_input_held(selected)) tab_write(selected, "\033[O", 3);
            if (tb->t.m_focus) tab_write(tb, "\033[I", 3);
        }
    }
    g_cutie_mouse_consumed = 1;
    if (mapped && tb->t.m_mouse && !(mods & M_SHIFT) && (tb->t.m_mouse_sgr || (col < 223 && row < 223))) {
        int flags = (mods & M_SHIFT ? 4 : 0) | (mods & M_ALT ? 8 : 0) | (mods & M_CTRL ? 16 : 0);
        char buf[64];
        int n;
        if (tb->t.m_mouse_sgr) n = snprintf(buf, sizeof(buf), "\033[<%d;%d;%dM", flags, col + 1, row + 1);
        else { n = 6; memcpy(buf, "\033[M", 3); buf[3] = (char)(32 + flags); buf[4] = (char)(33 + col); buf[5] = (char)(33 + row); }
        tab_write(tb, buf, n);
        g_cutie_mouse_id = tb->id; g_cutie_mouse_col = col; g_cutie_mouse_row = row; g_cutie_mouse_mods = flags;
    }
    tv->scroll = 0;
    plat_invalidate();
    return 1;
}

/* The pointer's shape, from the same geometry the clicks use. */
static void win_pointer_cursor_at(int x, int y) {
    if (mac_grid_cursor(x, y)) return;
    if (mac_music_cursor(x, y)) return;
    if (mac_camera_cursor(x, y)) return;
    if (g_tv_drag_slot >= 0) { win_tv_drag_cursor(); return; }
    if (g_chrome_drag) { win_cursor(IDC_SIZEALL); return; }
    if (over_chrome(x, y) || chrome_any_tv_contains(x, y)) {
        int idx = -1, hit = hit_tabbar(x, y, &idx);
        if (g_grid_on && (hit == HIT_GRIP || hit == HIT_TV_GRIP || hit == HIT_TV_RESIZE)) { win_cursor(IDC_ARROW); return; }
        win_cursor(hit == HIT_TV_RESIZE ? IDC_SIZENWSE : (hit == HIT_TV_GRIP || hit == HIT_GRIP) ? IDC_SIZEALL :
                   tabbar_action(x, y) ? IDC_HAND : IDC_ARROW);
        return;
    }
    int col, row;
    char url[2048];
    mouse_cell(x, y, &col, &row);
    if ((GetKeyState(VK_CONTROL) & 0x8000) && link_at(col, row, url, sizeof(url))) win_cursor(IDC_HAND);
    else win_cursor(IDC_IBEAM);
}

/* One 250 ms clock for the music position and the recording time, running
   only while one of them is moving. */
static void media_schedule(void) {
    if (!g_hwnd) return;
    if (g_mus.player || g_mus.ended || mac_record_active()) SetTimer(g_hwnd, TIMER_MEDIA, 250, NULL);
    else KillTimer(g_hwnd, TIMER_MEDIA);
}
static void media_tick(void) {
    music_tick();
    media_schedule();
    plat_invalidate();
}
/* The window is going: finish a recording's file, let go of the camera. */
static void media_quit(void) {
    if (mac_record_active()) {
        cam_record_stop(0);
        if (g_cam.record_thread) WaitForSingleObject(g_cam.record_thread, 15000);
    }
    InterlockedExchange(&g_cam.quit, 1);
    if (g_cam.video_thread) WaitForSingleObject(g_cam.video_thread, 3000);
    music_release_player();
}

/* What the window had going besides its tabs: grid, CAF, music. Carried over
   by an in-place update and kept in the session. One line of key=value. */
static struct { int set, grid, caf, music, visible, playing; double position; char audio[512]; } g_ui_saved;
static void ui_pack(char *out, size_t cap) {
    MusicItem *it = music_current();
    snprintf(out, cap, "grid=%d caf=%d music=%d visible=%d playing=%d position=%.0f audio=%s",
             g_grid_on, mac_caf_active(), g_music_enabled, g_music_visible, g_mus.wants, g_mus.position, it ? it->audio : "");
}
static void ui_restore(const char *v) {
    const char *a;
    memset(&g_ui_saved, 0, sizeof(g_ui_saved));
    g_ui_saved.set = sscanf(v, "grid=%d caf=%d music=%d visible=%d playing=%d position=%lf", &g_ui_saved.grid, &g_ui_saved.caf,
                            &g_ui_saved.music, &g_ui_saved.visible, &g_ui_saved.playing, &g_ui_saved.position) == 6;
    if (g_ui_saved.set && (a = strstr(v, " audio=")) && strlen(a + 7) < sizeof(g_ui_saved.audio))
        snprintf(g_ui_saved.audio, sizeof(g_ui_saved.audio), "%s", a + 7);
}
/* After the tabs are back. The episode plays on once the catalog arrives. */
static void ui_apply(void) {
    if (!g_ui_saved.set) return;
    if (g_ui_saved.grid && !g_grid_on) grid_toggle();
    if (g_ui_saved.caf && !mac_caf_active() && mac_caf_toggle() > 0) hc_tv_open(&g_tvs[HC_TV_CAF].state, GetTickCount());
    if (g_ui_saved.music) {
        g_music_enabled = 1;
        if (g_ui_saved.visible && !g_music_visible) { g_music_visible = 0; mac_music_toggle(); }
        else if (!g_music_nitems && !g_mus.loading) mac_music_load();
    }
    if (!g_ui_saved.audio[0]) g_ui_saved.set = 0;
}
/* From music_catalog_done: the episode that was playing, where it was. */
static void ui_music_resume(void) {
    if (!g_ui_saved.set || !g_ui_saved.audio[0]) return;
    for (int i = 0; i < g_music_nitems; i++)
        if (!strcmp(g_music_items[i].audio, g_ui_saved.audio)) {
            mac_music_select(i, 0);
            mac_music_seek(g_ui_saved.position);
            if (g_ui_saved.playing) mac_music_play_pause();
            break;
        }
    g_ui_saved.set = 0;
}
