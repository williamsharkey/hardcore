/* Camera TV on Windows: hardcore_camera_panel.m's grid-drawn controls around
   a Media Foundation preview (hardcore_win_capture.h). Recordings are MP4s
   in Videos\Hardcore; Clips opens that folder. */
#include "hardcore_win_capture.h"

static int g_camera_visible, g_camera_placed, g_camera_manual, g_camera_drag, g_camera_down;
static int g_camera_pending_action;
static HcTvRect g_camera_rect, g_camera_requested, g_camera_drag_origin;
static int g_camera_drag_x, g_camera_drag_y;
static HcTvPlacement g_camera_placement;
static int g_camera_library_count;

enum { MAC_CAMERA_NONE, MAC_CAMERA_MOVE, MAC_CAMERA_MINIMIZE, MAC_CAMERA_RECORD,
    MAC_CAMERA_PAUSE, MAC_CAMERA_SAVE, MAC_CAMERA_DISCARD, MAC_CAMERA_CAPTIONS,
    MAC_CAMERA_LIBRARY, MAC_CAMERA_RESIZE, MAC_CAMERA_BODY };

static int mac_record_active(void) { return cam_recording(); }
static int mac_record_exports_active(void) { return cam_state() == CAM_FINISHING; }
static HcTvRect mac_camera_primary(void) {
    return (HcTvRect){sidebar_x() / CW, g_chrome.top, SB_COLS, g_chrome.bottom - g_chrome.top};
}
static int mac_camera_rect_available(HcTvRect rect, Term *term) {
    if (!hc_tv_rect_fits(rect, term->cols, term->rows) || hc_tv_rect_overlap(rect, mac_camera_primary())) return 0;
    for (int i = 0; i < HC_TV_COUNT; i++)
        if (g_tvs[i].state.open && g_tvs[i].state.placed && hc_tv_rect_overlap(rect, g_tvs[i].state.rect)) return 0;
    return 1;
}
/* The preview is drawn with the frame, so there's no native view to move. */
static void mac_camera_preview_sync(void) {}
static void mac_camera_library_refresh(void) {
    wchar_t dir[MAX_PATH], pattern[MAX_PATH + 8];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0;
    cam_videos_dir(dir, MAX_PATH);
    _snwprintf(pattern, MAX_PATH + 8, L"%ls\\*.mp4", dir);
    pattern[MAX_PATH + 7] = 0;
    if ((h = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE) {
        do count++; while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_camera_library_count = count;
}
static void mac_record_library(void) {
    wchar_t dir[MAX_PATH];
    cam_videos_dir(dir, MAX_PATH);
    SHCreateDirectoryExW(NULL, dir, NULL);
    ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}
static void mac_camera_cancel_press(void) { g_camera_pending_action = g_camera_drag = 0; }
static void mac_camera_toggle(void) {
    g_camera_visible = !g_camera_visible;
    g_camera_drag = g_camera_down = g_camera_pending_action = 0;
    if (g_camera_visible) { mac_camera_library_refresh(); cam_prepare(); }
    else if (!mac_record_active()) cam_shutdown();
    media_schedule();
    tab_layout();
    plat_invalidate();
}
/* WM_APP_CAMERA */
static void camera_changed(void) {
    cam_reap();
    if (!g_camera_visible && !mac_record_active() && cam_state() != CAM_IDLE) cam_shutdown();
    mac_camera_library_refresh();
    media_schedule();
    update_busy(0);
    plat_invalidate();
}
static void mac_camera_layout(Term *term) {
    g_camera_placed = 0;
    if (!g_camera_visible || !term || term->cols < 32 || term->rows < 9) return;
    UI_FONT;
    if (!g_camera_requested.w) {
        int width = 32;
        int body = chrome_max(4, (int)ceil((double)(width - 2) * CW * 9 / (16 * CH)));
        g_camera_requested = (HcTvRect){0, 0, width, body + 5};
    }
    HcTvRect requested = hc_tv_requested_rect(g_camera_requested, term->cols, term->rows);
    requested.w = chrome_max(32, requested.w); requested.h = chrome_max(9, requested.h);
    if (g_camera_manual && mac_camera_rect_available(requested, term)) {
        g_camera_rect = requested; g_camera_placement = (HcTvPlacement){.rect = requested, .valid = 1}; g_camera_placed = 1;
    } else if (g_camera_placement.valid && g_camera_rect.w == requested.w && g_camera_rect.h == requested.h &&
               mac_camera_rect_available(g_camera_rect, term)) {
        g_camera_placed = 1;
    } else {
        ChromeTvCostContext context = {.term = term, .primary = mac_camera_primary()};
        HcTvPlacement placement = hc_tv_place(term->cols, term->rows, context.primary, requested.w, requested.h,
            g_camera_placement.valid ? &g_camera_placement : NULL, chrome_tv_cost, &context);
        if (placement.valid) { g_camera_rect = placement.rect; g_camera_placement = placement; g_camera_placed = 1; }
    }
    if (g_camera_placed) {
        HcTvRect prior[HC_TV_COUNT + 1];
        int count = 0;
        prior[count++] = mac_camera_primary();
        for (int i = 0; i < HC_TV_COUNT; i++) if (g_tvs[i].state.open && g_tvs[i].state.placed) prior[count++] = g_tvs[i].state.rect;
        chrome_tv_reflow_apply(term, g_camera_rect, prior, count);
    }
    UI_DONE;
}
/* Recording buttons: {column, width}, packed tighter in a narrow grid rail. */
static const int g_camera_controls_wide[4][2] = {{1, 7}, {9, 7}, {17, 5}, {23, 7}};
static const int g_camera_controls_narrow[4][2] = {{1, 5}, {7, 6}, {14, 4}, {19, 7}};
static const int (*mac_camera_controls(void))[2] {
    return g_camera_rect.w < 32 ? g_camera_controls_narrow : g_camera_controls_wide;
}
static int mac_camera_hit(int x, int y) {
    if (!g_camera_visible || !g_camera_placed || x < 0 || y < 0) return MAC_CAMERA_NONE;
    UI_FONT;
    HcTvRect r = g_camera_rect;
    int col = x / CW - r.x, row = y / CH - r.y;
    UI_DONE;
    if (col < 0 || col >= r.w || row < 0 || row >= r.h) return MAC_CAMERA_NONE;
    if (row == 0) return col == r.w - 2 ? MAC_CAMERA_MINIMIZE : MAC_CAMERA_MOVE;
    if (row == r.h - 1 && col >= r.w - 3) return MAC_CAMERA_RESIZE;
    if (row == r.h - 4) {
        const int (*b)[2] = mac_camera_controls();
        static const int actions[4] = {MAC_CAMERA_RECORD, MAC_CAMERA_PAUSE, MAC_CAMERA_SAVE, MAC_CAMERA_DISCARD};
        for (int i = 0; i < 4; i++) if (col >= b[i][0] && col < b[i][0] + b[i][1]) return actions[i];
    }
    if (row == r.h - 3 && col >= 1 && col < 10) return MAC_CAMERA_LIBRARY;
    return MAC_CAMERA_BODY;
}
static void mac_camera_label(const wchar_t *label, int col, int width, int row, uint32_t color, int hit) {
    if (mac_camera_hit(g_mouse_x, g_mouse_y) == hit)
        fill_rect((float)(col * CW), (float)(row * CH), (float)((col + width) * CW), (float)((row + 1) * CH), TAB_HOVER, 1);
    chrome_label(label, col, col + width, row, color);
}
static void mac_camera_draw(void) {
    if (!g_camera_visible || !g_camera_placed) return;
    UI_FONT;
    int origin = g_chrome_x, surface = g_chrome_surface_cols, capture = g_chrome_capture;
    float alpha = g_draw_alpha;
    HcTvRect r = g_camera_rect;
    g_chrome_x = r.x * CW; g_chrome_surface_cols = r.w; g_draw_alpha = 1; g_chrome_capture = 0;
    fill_rect(0, (float)(r.y * CH), (float)(r.w * CW), (float)((r.y + r.h) * CH), BAR_BG, 1);
    fill_rect(0, (float)(r.y * CH), (float)(r.w * CW), (float)(r.y * CH + 1), g_look.dim, 1);
    fill_rect(0, (float)((r.y + r.h) * CH - 1), (float)(r.w * CW), (float)((r.y + r.h) * CH), g_look.dim, 1);
    fill_rect(0, (float)(r.y * CH), 1, (float)((r.y + r.h) * CH), g_look.dim, 1);
    fill_rect((float)(r.w * CW - 1), (float)(r.y * CH), (float)(r.w * CW), (float)((r.y + r.h) * CH), g_look.dim, 1);
    int state = cam_state(), busy = mac_record_active(), recording = state == CAM_RECORDING, paused = state == CAM_PAUSED;
    wchar_t title[96], text[512];
    if (busy) {
        unsigned elapsed = (unsigned)cam_elapsed();
        _snwprintf(title, 96, L"%lsCamera %02u:%02u:%02u", g_grid_on ? L"" : L"↕ ", elapsed / 3600, elapsed / 60 % 60, elapsed % 60);
    } else _snwprintf(title, 96, L"%lsCamera", g_grid_on ? L"" : L"↕ ");
    chrome_label(title, 1, r.w - 3, r.y, recording ? g_look.red : g_look.text);
    mac_camera_label(L"−", r.w - 2, 1, r.y, g_look.text, MAC_CAMERA_MINIMIZE);
    fill_rect((float)CW, (float)((r.y + 1) * CH), (float)((r.w - 1) * CW), (float)((r.y + r.h - 4) * CH), 0x000000, 1);
    if (state == CAM_PREPARING || state == CAM_FAILED || state == CAM_IDLE ||
        !cam_draw_preview(CW, (r.y + 1) * CH, (r.w - 1) * CW, (r.y + r.h - 4) * CH)) {
        const char *message = state == CAM_FAILED ? "Camera unavailable" : state == CAM_IDLE ? "Camera closed" : "Opening camera…";
        utf8_to_wide(message, text, 512);
        chrome_label(text, 2, r.w - 2, r.y + 2, g_look.detail);
        if (state == CAM_FAILED) chrome_label(L"Close and reopen cam to retry", 2, r.w - 2, r.y + 3, g_look.dim);
    }
    int row = r.y + r.h - 4;
    const int (*b)[2] = mac_camera_controls();
    mac_camera_label(L"● Rec", b[0][0], b[0][1], row, !busy && state == CAM_READY ? g_look.red : g_look.dim, MAC_CAMERA_RECORD);
    mac_camera_label(paused ? L"Resume" : L"Pause", b[1][0], b[1][1], row, recording || paused ? g_look.text : g_look.dim, MAC_CAMERA_PAUSE);
    mac_camera_label(L"Save", b[2][0], b[2][1], row, recording || paused ? AMBER : g_look.dim, MAC_CAMERA_SAVE);
    mac_camera_label(L"Discard", b[3][0], b[3][1], row, recording || paused ? g_look.text_lo : g_look.dim, MAC_CAMERA_DISCARD);
    row++;
    wchar_t library[32];
    _snwprintf(library, 32, L"Clips %d", chrome_min(g_camera_library_count, 999));
    mac_camera_label(library, 1, 9, row, g_look.text_lo, MAC_CAMERA_LIBRARY);
    if (mac_record_exports_active()) {
        wchar_t drip[2] = {g_ndrip ? g_drip[(GetTickCount() / DRIP_MS) % g_ndrip] : 0x2800, 0};
        chrome_label(drip, 11, 12, row, AMBER);
    }
    cam_lock();
    int failed = g_cam.error[0] != 0;
    utf8_to_wide(failed ? g_cam.error : g_cam.status[0] ? g_cam.status :
                 state == CAM_RECORDING ? "Recording" : state == CAM_PAUSED ? "Paused" : state == CAM_FINISHING ? "Saving…" :
                 state == CAM_READY ? (g_cam.name[0] ? g_cam.name : "Ready") : "", text, 512);
    cam_unlock();
    chrome_label(text, 1, r.w - 1, row + 1, failed ? g_look.red : g_look.detail);
    chrome_label(g_cam.has_mic ? L"Camera + microphone" : L"Camera only · no microphone", 1, r.w - 3, row + 2, g_look.dim);
    int grip = chrome_min(CW - 3, CH - 5), bottom = (r.y + r.h) * CH;
    for (int length = grip; length >= 2; length -= 3)
        for (int step = 0; step < length; step++) {
            int x = r.w * CW - 3 - step, y = bottom - 3 - (length - 1 - step);
            fill_rect((float)x, (float)y, (float)(x + 1), (float)(y + 1), g_look.detail, 1);
        }
    g_chrome_x = origin; g_chrome_surface_cols = surface; g_draw_alpha = alpha; g_chrome_capture = capture;
    UI_DONE;
}
static void mac_camera_drag_move(int x, int y) {
    Tab *tab = active_tab();
    if (!tab) return;
    UI_FONT;
    HcTvRect r = g_camera_drag_origin;
    int dx = (int)lround((double)(x - g_camera_drag_x) / CW), dy = (int)lround((double)(y - g_camera_drag_y) / CH);
    if (g_camera_drag == MAC_CAMERA_RESIZE) {
        r.w = chrome_min(chrome_max(32, r.w + dx), chrome_min(HC_TV_MAX_WIDTH, tab->t.cols - r.x));
        r.h = chrome_min(chrome_max(9, r.h + dy), chrome_min(HC_TV_MAX_HEIGHT, tab->t.rows - r.y));
    } else {
        r.x = chrome_min(chrome_max(0, r.x + dx), chrome_max(0, tab->t.cols - r.w));
        r.y = chrome_min(chrome_max(0, r.y + dy), chrome_max(0, tab->t.rows - r.h));
    }
    if (mac_camera_rect_available(r, &tab->t)) {
        g_camera_manual = 1; g_camera_requested = g_camera_rect = r;
        g_camera_placement = (HcTvPlacement){.rect = r, .valid = 1};
        plat_invalidate();
    }
    UI_DONE;
    win_cursor(g_camera_drag == MAC_CAMERA_RESIZE ? IDC_SIZENWSE : IDC_SIZEALL);
}
static int mac_camera_mouse(int type, int x, int y, int wheel) {
    (void)wheel;
    if (g_camera_drag && (type == MEV_DRAG || type == MEV_UP)) {
        mac_camera_drag_move(x, y);
        if (type == MEV_UP) { g_camera_drag = g_camera_down = 0; win_cursor(IDC_ARROW); }
        return 1;
    }
    if (g_camera_down && (type == MEV_DRAG || type == MEV_UP)) {
        if (type == MEV_UP) {
            int action = g_camera_pending_action;
            g_camera_down = g_camera_pending_action = 0;
            /* Open Explorer on the release, so the click can't bring hardcore back over it. */
            if (action == MAC_CAMERA_LIBRARY && mac_camera_hit(x, y) == action) { mac_camera_library_refresh(); mac_record_library(); }
        }
        return 1;
    }
    int hit = mac_camera_hit(x, y);
    if (!hit) return 0;
    if (type == MEV_MOVE) { g_mouse_x = x; g_mouse_y = y; plat_invalidate(); return 0; }
    if (type != MEV_DOWN) return type == MEV_WHEEL || type == MEV_OTHER;
    g_mouse_x = x; g_mouse_y = y; g_camera_down = 1; g_camera_pending_action = 0;
    int state = cam_state(), busy = mac_record_active();
    if (g_grid_on && (hit == MAC_CAMERA_MOVE || hit == MAC_CAMERA_RESIZE)) return 1;
    if (hit == MAC_CAMERA_MOVE || hit == MAC_CAMERA_RESIZE) {
        mac_cutie_blur(); chrome_preview_cancel(); g_sel_drag = g_sb_drag = 0;
        g_camera_drag = hit; g_camera_drag_origin = g_camera_rect; g_camera_drag_x = x; g_camera_drag_y = y;
        win_cursor(hit == MAC_CAMERA_RESIZE ? IDC_SIZENWSE : IDC_SIZEALL);
    } else if (hit == MAC_CAMERA_MINIMIZE) { mac_camera_toggle(); g_camera_down = 1; }
    else if (hit == MAC_CAMERA_RECORD && !busy && state == CAM_READY) cam_record_start();
    else if (hit == MAC_CAMERA_PAUSE && (state == CAM_RECORDING || state == CAM_PAUSED)) cam_record_pause();
    else if (hit == MAC_CAMERA_SAVE && (state == CAM_RECORDING || state == CAM_PAUSED)) cam_record_stop(0);
    else if (hit == MAC_CAMERA_DISCARD && (state == CAM_RECORDING || state == CAM_PAUSED)) cam_record_stop(1);
    else if (hit == MAC_CAMERA_LIBRARY) g_camera_pending_action = hit;
    media_schedule();
    plat_invalidate();
    return 1;
}
static int mac_camera_cursor(int x, int y) {
    if (g_camera_drag) { win_cursor(g_camera_drag == MAC_CAMERA_RESIZE ? IDC_SIZENWSE : IDC_SIZEALL); return 1; }
    int hit = mac_camera_hit(x, y);
    if (!hit) return 0;
    win_cursor(hit == MAC_CAMERA_MOVE ? IDC_SIZEALL : hit == MAC_CAMERA_RESIZE ? IDC_SIZENWSE : hit == MAC_CAMERA_BODY ? IDC_ARROW : IDC_HAND);
    return 1;
}
