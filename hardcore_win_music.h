/* Music For Programming on Windows: the public RSS feed is the catalog
   (WinHTTP, on a worker thread) and Media Foundation's MFPlay streams an
   episode only after Play. No browser, no download to disk. The panel is
   hardcore_music_panel.m in C: same font, theme and TV placement. */
#include <mfplay.h>
#include <mferror.h>

#define WM_APP_MUSIC (WM_APP + 11)
#define MUSIC_FEED_LIMIT (2u * 1024u * 1024u)
#define MUSIC_ITEM_LIMIT 512

typedef struct {
    char audio[512];
    char title[184], short_title[184];
    int episode;
    double duration;
} MusicItem;

static MusicItem *g_music_items;
static int g_music_nitems;
static struct {
    int loading, wants, buffering, index, seek_pending, ended;
    double position, duration;
    char error[96], catalog_error[96];
    unsigned generation;
    IMFPMediaPlayer *player;
} g_mus = {.index = -1};
static void music_start_player(void);
static void mac_music_next(int delta);
static void ui_music_resume(void);

static int music_count(void) { return g_music_nitems; }
static MusicItem *music_current(void) { return g_mus.index >= 0 && g_mus.index < g_music_nitems ? &g_music_items[g_mus.index] : NULL; }
static void music_changed(void) { update_busy(0); plat_invalidate(); }

/* ── the catalog ── */
static void music_entities(char *s) {
    static const struct { const char *name; char c; } map[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&#39;", '\''}};
    char *w = s;
    for (const char *r = s; *r;) {
        int done = 0;
        if (*r == '&')
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
                size_t n = strlen(map[i].name);
                if (!strncmp(r, map[i].name, n)) { *w++ = map[i].c; r += n; done = 1; break; }
            }
        if (!done) { unsigned char c = (unsigned char)*r++; *w++ = c < 32 ? ' ' : (char)c; }
    }
    *w = 0;
    while (w > s && w[-1] == ' ') *--w = 0;
    char *start = s;
    while (*start == ' ') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}
/* The text of <tag>…</tag> inside [from, to), CDATA unwrapped, bounded. */
static int music_field(const char *from, const char *to, const char *tag, char *out, size_t cap) {
    char open[48], close[48];
    snprintf(open, sizeof(open), "<%s>", tag);
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *a = strstr(from, open);
    if (!a || a >= to) return 0;
    a += strlen(open);
    const char *b = strstr(a, close);
    if (!b || b > to) return 0;
    if (!strncmp(a, "<![CDATA[", 9) && b - a >= 12 && !strncmp(b - 3, "]]>", 3)) { a += 9; b -= 3; }
    size_t n = (size_t)(b - a);
    if (n >= cap) n = cap - 1;
    memcpy(out, a, n);
    out[n] = 0;
    music_entities(out);
    return 1;
}
static double music_duration(const char *s) {
    double total = 0;
    int parts = 0;
    if (!*s || strlen(s) > 16) return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        double n = 0;
        while (isdigit((unsigned char)*s)) n = n * 10 + (*s++ - '0');
        if (parts && n >= 60) return 0;
        total = total * 60 + n;
        if (++parts > 3) return 0;
        if (*s == ':') s++;
        else if (*s) return 0;
    }
    return total <= 7 * 24 * 3600 ? total : 0;
}
/* Only https streams from the two hosts the feed uses, never credentials or ports. */
static int music_audio_url(const char *in, char *out, size_t cap) {
    const char *rest = !strncmp(in, "https://", 8) ? in + 8 : !strncmp(in, "http://", 7) ? in + 7 : NULL;
    if (!rest || strlen(in) > 400) return 0;
    const char *slash = strchr(rest, '/');
    if (!slash) return 0;
    size_t host = (size_t)(slash - rest);
    if (!((host == 12 && !_strnicmp(rest, "datashat.net", 12)) || (host == 23 && !_strnicmp(rest, "musicforprogramming.net", 23)))) return 0;
    size_t len = strlen(slash);
    if (len < 5 || _stricmp(slash + len - 4, ".mp3") || strpbrk(slash, "\"'<> #?")) return 0;
    snprintf(out, cap, "https://%.*s%s", (int)host, rest, slash);
    return 1;
}
static int music_parse(const char *xml, MusicItem **items_out) {
    MusicItem *items = NULL;
    int n = 0;
    if (strstr(xml, "<!DOCTYPE") || strstr(xml, "<!ENTITY")) return 0;
    for (const char *p = strstr(xml, "<item>"); p && n < MUSIC_ITEM_LIMIT; p = strstr(p, "<item>")) {
        const char *end = strstr(p, "</item>");
        if (!end) break;
        MusicItem it;
        char raw[600], dur[32] = "";
        memset(&it, 0, sizeof(it));
        const char *enc = strstr(p, "<enclosure");
        const char *url = enc && enc < end ? strstr(enc, "url=\"") : NULL;
        if (url && url < end && music_field(p, end, "title", it.title, sizeof(it.title)) && it.title[0]) {
            url += 5;
            const char *q = strchr(url, '"');
            size_t len = q && q < end ? (size_t)(q - url) : 0;
            if (len && len < sizeof(raw)) {
                memcpy(raw, url, len);
                raw[len] = 0;
                music_entities(raw);
                if (music_audio_url(raw, it.audio, sizeof(it.audio))) {
                    int dup = 0;
                    for (int i = 0; i < n && !dup; i++) dup = !strcmp(items[i].audio, it.audio);
                    music_field(p, end, "itunes:duration", dur, sizeof(dur));
                    it.duration = music_duration(dur);
                    snprintf(it.short_title, sizeof(it.short_title), "%s", !strncmp(it.title, "Episode ", 8) ? it.title + 8 : it.title);
                    it.episode = atoi(it.short_title);
                    if (it.episode < 1 || it.episode > 100000) it.episode = 0;
                    MusicItem *more = dup ? items : (MusicItem *)realloc(items, (size_t)(n + 1) * sizeof(*items));
                    if (!more) break;
                    items = more;
                    if (!dup) items[n++] = it;
                }
            }
        }
        p = end + 7;
    }
    if (!n) { free(items); return 0; }
    *items_out = items;
    return n;
}

typedef struct { unsigned generation; MusicItem *items; int count; } MusicCatalog;
static DWORD WINAPI music_fetch_thread(LPVOID arg) {
    MusicCatalog *c = (MusicCatalog *)arg;
    HINTERNET s = WinHttpOpen(L"Hardcore Music/1.0 (+https://github.com/williamsharkey/hardcore)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0), h = NULL, r = NULL;
    char *buf = NULL;
    DWORD used = 0, status = 0, size = sizeof(status), redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!s) s = WinHttpOpen(L"Hardcore Music/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (s) {
        WinHttpSetTimeouts(s, 10000, 10000, 15000, 25000);
        WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        h = WinHttpConnect(s, L"musicforprogramming.net", INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (h) r = WinHttpOpenRequest(h, L"GET", L"/rss.xml", NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (r && WinHttpSendRequest(r, L"Accept: application/rss+xml, application/xml, text/xml\r\n", (DWORD)-1, NULL, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL) &&
        status >= 200 && status < 300 && (buf = (char *)malloc(MUSIC_FEED_LIMIT + 1))) {
        DWORD got;
        while (used < MUSIC_FEED_LIMIT && WinHttpReadData(r, buf + used, MUSIC_FEED_LIMIT - used, &got) && got) used += got;
        buf[used] = 0;
        if (used < MUSIC_FEED_LIMIT && strlen(buf) == used) c->count = music_parse(buf, &c->items);
    }
    free(buf);
    if (r) WinHttpCloseHandle(r);
    if (h) WinHttpCloseHandle(h);
    if (s) WinHttpCloseHandle(s);
    if (!PostMessageW(g_hwnd, WM_APP_MUSIC, 0, (LPARAM)c)) { free(c->items); free(c); }
    return 0;
}
static void mac_music_load(void) {
    if (g_mus.loading) return;
    MusicCatalog *c = (MusicCatalog *)calloc(1, sizeof(*c));
    if (!c) return;
    g_mus.loading = 1;
    g_mus.catalog_error[0] = 0;
    c->generation = ++g_mus.generation;
    HANDLE th = CreateThread(NULL, 0, music_fetch_thread, c, 0, NULL);
    if (th) CloseHandle(th);
    else { free(c); g_mus.loading = 0; snprintf(g_mus.catalog_error, sizeof(g_mus.catalog_error), "Cannot refresh Music For Programming; choose Refresh to retry."); }
    music_changed();
}
/* WM_APP_MUSIC: the catalog arrived (or didn't). Keep the playing stream if
   a refreshed feed drops an older episode. */
static void music_catalog_done(MusicCatalog *c) {
    if (c->generation == g_mus.generation) {
        g_mus.loading = 0;
        if (!c->count) snprintf(g_mus.catalog_error, sizeof(g_mus.catalog_error), "Cannot refresh Music For Programming; choose Refresh to retry.");
        else {
            MusicItem *cur = music_current(), keep;
            int index = -1;
            if (cur) keep = *cur;
            for (int i = 0; cur && i < c->count; i++) if (!strcmp(c->items[i].audio, keep.audio)) { index = i; break; }
            if (cur && index < 0) {
                MusicItem *more = (MusicItem *)realloc(c->items, (size_t)(c->count + 1) * sizeof(*more));
                if (more) { c->items = more; c->items[c->count] = keep; index = c->count++; }
            }
            free(g_music_items);
            g_music_items = c->items; g_music_nitems = c->count; c->items = NULL;
            g_mus.index = index >= 0 ? index : 0;
            if (!g_mus.duration) g_mus.duration = music_current()->duration;
            if (g_mus.wants && !g_mus.player) music_start_player();
            ui_music_resume();
        }
        if (!g_music_nitems) g_mus.wants = 0;
        music_changed();
    }
    free(c->items);
    free(c);
}

/* ── playback: MFPlay, loaded at run time (N editions have no Media Foundation) ── */
typedef HRESULT (WINAPI *PFN_MFPCreate)(LPCWSTR, BOOL, MFP_CREATION_OPTIONS, IMFPMediaPlayerCallback *, HWND, IMFPMediaPlayer **);
static PFN_MFPCreate music_mfplay(void) {
    static PFN_MFPCreate create;
    static int tried;
    if (!tried) {
        HMODULE m;
        tried = 1;
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if ((m = LoadLibraryW(L"mfplay.dll"))) create = (PFN_MFPCreate)(void *)GetProcAddress(m, "MFPCreateMediaPlayer");
    }
    return create;
}
static const GUID IID_IMFPMediaPlayerCallback_ = {0x766C8FFB, 0x5FDB, 0x4fea, {0xA2, 0x8D, 0xB9, 0x12, 0x99, 0x6F, 0x51, 0xBD}};
/* MFPlay posts its events to the thread that made the player: this one. */
static HRESULT STDMETHODCALLTYPE mcb_qi(IMFPMediaPlayerCallback *self, REFIID riid, void **out) {
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFPMediaPlayerCallback_)) { *out = self; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE mcb_ref(IMFPMediaPlayerCallback *self) { (void)self; return 1; }   /* static object */
static void STDMETHODCALLTYPE mcb_event(IMFPMediaPlayerCallback *self, MFP_EVENT_HEADER *e) {
    (void)self;
    if (!e || e->pMediaPlayer != g_mus.player) return;
    if (FAILED(e->hrEvent) && e->eEventType != MFP_EVENT_TYPE_ERROR) e->eEventType = MFP_EVENT_TYPE_ERROR;
    if (e->eEventType == MFP_EVENT_TYPE_ERROR) {
        g_mus.wants = g_mus.buffering = 0;
        snprintf(g_mus.error, sizeof(g_mus.error), "Playback unavailable; press Play to retry.");
        IMFPMediaPlayer *p = g_mus.player;
        g_mus.player = NULL;
        IMFPMediaPlayer_Shutdown(p);
        IMFPMediaPlayer_Release(p);
    } else if (e->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) {
        if (g_mus.seek_pending) {
            PROPVARIANT v;
            PropVariantInit(&v);
            v.vt = VT_I8;
            v.hVal.QuadPart = (LONGLONG)(g_mus.position * 1e7);
            IMFPMediaPlayer_SetPosition(g_mus.player, &MFP_POSITIONTYPE_100NS, &v);
            g_mus.seek_pending = 0;
        }
        if (g_mus.wants) IMFPMediaPlayer_Play(g_mus.player);
    } else if (e->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
        g_mus.ended = 1;
    }
    media_schedule();
    music_changed();
}
static IMFPMediaPlayerCallbackVtbl g_mcb_vtbl = {mcb_qi, mcb_ref, mcb_ref, mcb_event};
static IMFPMediaPlayerCallback g_mcb = {&g_mcb_vtbl};

static void music_release_player(void) {
    if (g_mus.player) {
        IMFPMediaPlayer *p = g_mus.player;
        g_mus.player = NULL;
        IMFPMediaPlayer_Shutdown(p);
        IMFPMediaPlayer_Release(p);
    }
    g_mus.buffering = 0;
}
static void music_start_player(void) {
    MusicItem *it = music_current();
    PFN_MFPCreate create = music_mfplay();
    wchar_t url[512];
    if (!it) return;
    music_release_player();
    g_mus.error[0] = 0;
    if (!create) {
        g_mus.wants = 0;
        snprintf(g_mus.error, sizeof(g_mus.error), "Media Foundation isn't installed on this Windows.");
        return;
    }
    utf8_to_wide(it->audio, url, 512);
    g_mus.seek_pending = g_mus.position > 0;
    g_mus.buffering = g_mus.wants;
    if (FAILED(create(url, FALSE, 0, &g_mcb, NULL, &g_mus.player))) {
        g_mus.player = NULL; g_mus.wants = g_mus.buffering = 0;
        snprintf(g_mus.error, sizeof(g_mus.error), "Playback unavailable; press Play to retry.");
    }
    media_schedule();
}
static void mac_music_select(int index, int play) {
    if (index < 0 || index >= g_music_nitems) return;
    int same = index == g_mus.index;
    g_mus.wants = play;
    if (!same) { music_release_player(); g_mus.index = index; g_mus.position = 0; g_mus.duration = g_music_items[index].duration; g_mus.seek_pending = 0; }
    g_mus.error[0] = 0;
    if (play) {
        if (!g_mus.player) music_start_player();
        else IMFPMediaPlayer_Play(g_mus.player);
    } else if (g_mus.player) { IMFPMediaPlayer_Pause(g_mus.player); g_mus.buffering = 0; }
    media_schedule();
    music_changed();
}
static void mac_music_play_pause(void) {
    if (g_mus.wants) {
        g_mus.wants = g_mus.buffering = 0;
        if (g_mus.player) IMFPMediaPlayer_Pause(g_mus.player);
        music_changed();
        return;
    }
    g_mus.wants = 1;
    if (!g_music_nitems) { mac_music_load(); music_changed(); return; }
    mac_music_select(g_mus.index >= 0 ? g_mus.index : 0, 1);
}
static void mac_music_next(int delta) {
    if (!g_music_nitems) { mac_music_load(); return; }
    int n = g_music_nitems, index = g_mus.index >= 0 ? g_mus.index : 0;
    index = ((index + delta % n) + n) % n;
    mac_music_select(index, g_mus.wants);
}
static void mac_music_seek(double seconds) {
    if (!music_current() || !isfinite(seconds)) return;
    g_mus.position = fmax(0, fmin(seconds, g_mus.duration > 0 ? g_mus.duration : 7 * 24 * 3600));
    if (g_mus.player) {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_I8;
        v.hVal.QuadPart = (LONGLONG)(g_mus.position * 1e7);
        IMFPMediaPlayer_SetPosition(g_mus.player, &MFP_POSITIONTYPE_100NS, &v);
    }
    music_changed();
}
static void mac_music_stop(void) {
    g_mus.wants = 0; g_mus.position = 0; g_mus.seek_pending = 0;
    music_release_player();
    g_mus.error[0] = 0;
    music_changed();
}
/* Every 250 ms while a stream is open: position, duration, buffering, the end. */
static void music_tick(void) {
    if (g_mus.ended) {
        g_mus.ended = 0;
        if (g_mus.wants) {
            if (g_music_nitems == 1) { g_mus.position = 0; music_start_player(); }
            else mac_music_next(1);
        }
    }
    if (!g_mus.player) return;
    PROPVARIANT v;
    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    IMFPMediaPlayer_GetState(g_mus.player, &state);
    PropVariantInit(&v);
    if (SUCCEEDED(IMFPMediaPlayer_GetDuration(g_mus.player, &MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_UI8 && v.uhVal.QuadPart)
        g_mus.duration = (double)v.uhVal.QuadPart / 1e7;
    PropVariantClear(&v);
    if (!g_mus.seek_pending && SUCCEEDED(IMFPMediaPlayer_GetPosition(g_mus.player, &MFP_POSITIONTYPE_100NS, &v)) &&
        (v.vt == VT_I8 || v.vt == VT_UI8))
        g_mus.position = (double)v.hVal.QuadPart / 1e7;
    PropVariantClear(&v);
    g_mus.buffering = g_mus.wants && state != MFP_MEDIAPLAYER_STATE_PLAYING;
}

/* ── the panel ── */
static int g_music_enabled, g_music_visible, g_music_placed, g_music_manual;
static int g_music_offset, g_music_down, g_music_pending, g_music_drag;
static int g_music_drag_x, g_music_drag_y;
static char g_music_pending_id[512];
static HcTvRect g_music_rect, g_music_requested, g_music_drag_origin;
static HcTvPlacement g_music_placement;
enum { MUSIC_NONE, MUSIC_BODY, MUSIC_MOVE, MUSIC_MINIMIZE, MUSIC_RESIZE,
    MUSIC_PREVIOUS, MUSIC_PLAY, MUSIC_NEXT, MUSIC_STOP, MUSIC_REFRESH, MUSIC_SITE,
    MUSIC_SEEK, MUSIC_PAGE_UP, MUSIC_PAGE_DOWN, MUSIC_ITEM = 1000 };

static int mac_music_menu_visible(void) { return g_music_enabled; }
static int mac_music_playing(void) { return g_music_enabled && g_mus.wants; }
static int mac_music_loading(void) { return g_music_enabled && (g_mus.loading || g_mus.buffering); }
static void mac_music_compact_label(wchar_t *out, int cap) {
    MusicItem *it = music_current();
    unsigned seconds = (unsigned)fmax(0, g_mus.position);
    if (g_mus.error[0] || g_mus.catalog_error[0]) _snwprintf(out, cap, L"Retry");
    else if (it && it->episode) _snwprintf(out, cap, L"%d %u:%02u", it->episode, seconds / 60, seconds % 60);
    else _snwprintf(out, cap, g_mus.loading ? L"Loading" : L"Music");
    out[cap - 1] = 0;
}
static int mac_music_capacity(void) { return chrome_max(1, g_music_rect.h - 8); }
static void mac_music_scroll(int delta) {
    g_music_offset = chrome_min(chrome_max(0, g_music_offset + delta), chrome_max(0, music_count() - mac_music_capacity()));
    plat_invalidate();
}
static void mac_music_cancel_press(void) {
    g_music_pending = g_music_drag = 0;
    g_music_pending_id[0] = 0;
}
static void mac_music_toggle(void) {
    g_music_enabled = 1;
    g_music_visible = !g_music_visible;
    mac_music_cancel_press();
    if (g_music_visible) {
        if (!g_music_nitems && !g_mus.loading) mac_music_load();
        if (!g_music_requested.w) g_music_requested = (HcTvRect){0, 0, 48, 16};
        if (g_mus.index >= 0) g_music_offset = g_mus.index;
    }
    tab_layout();
    plat_invalidate();
}
static int mac_music_rect_available(HcTvRect r, Term *t) {
    return mac_camera_rect_available(r, t) && (!g_camera_visible || !g_camera_placed || !hc_tv_rect_overlap(r, g_camera_rect));
}
static int mac_music_cost(HcTvRect r, HcTvCost *cost, void *context) {
    if (g_camera_visible && g_camera_placed && hc_tv_rect_overlap(r, g_camera_rect)) return 0;
    return chrome_tv_cost(r, cost, context);
}
static void mac_music_layout(Term *term) {
    g_music_placed = 0;
    if (!g_music_visible || !term || term->cols < 32 || term->rows < 12) return;
    UI_FONT;
    HcTvRect r = hc_tv_requested_rect(g_music_requested, term->cols, term->rows);
    r.w = chrome_max(32, r.w); r.h = chrome_max(12, r.h);
    if (g_music_manual && mac_music_rect_available(r, term)) {
        g_music_rect = r; g_music_placement = (HcTvPlacement){.rect = r, .valid = 1}; g_music_placed = 1;
    } else if (g_music_placement.valid && g_music_rect.w == r.w && g_music_rect.h == r.h && mac_music_rect_available(g_music_rect, term))
        g_music_placed = 1;
    else {
        ChromeTvCostContext context = {.term = term, .primary = mac_camera_primary()};
        HcTvPlacement placement = hc_tv_place(term->cols, term->rows, context.primary, r.w, r.h,
            g_music_placement.valid ? &g_music_placement : NULL, mac_music_cost, &context);
        if (placement.valid) { g_music_rect = placement.rect; g_music_placement = placement; g_music_placed = 1; }
    }
    if (g_music_placed) {
        HcTvRect prior[HC_TV_COUNT + 2];
        int count = 0;
        prior[count++] = mac_camera_primary();
        for (int i = 0; i < HC_TV_COUNT; i++) if (g_tvs[i].state.open && g_tvs[i].state.placed) prior[count++] = g_tvs[i].state.rect;
        if (g_camera_visible && g_camera_placed) prior[count++] = g_camera_rect;
        chrome_tv_reflow_apply(term, g_music_rect, prior, count);
        g_music_offset = chrome_min(chrome_max(0, g_music_offset), chrome_max(0, music_count() - mac_music_capacity()));
    }
    UI_DONE;
}
/* Transport buttons: {column, width}. Grid rails are as narrow as the tab
   menu, so narrow panels pack the same four buttons tighter. */
static const int g_music_transport_wide[4][2] = {{1, 7}, {9, 7}, {17, 7}, {25, 5}};
static const int g_music_transport_narrow[4][2] = {{1, 5}, {7, 7}, {15, 5}, {21, 5}};
static const int (*mac_music_transport(void))[2] {
    return g_music_rect.w < 32 ? g_music_transport_narrow : g_music_transport_wide;
}
static int mac_music_hit(int x, int y) {
    if (!g_music_visible || !g_music_placed || x < 0 || y < 0) return MUSIC_NONE;
    UI_FONT;
    int col = x / CW - g_music_rect.x, row = y / CH - g_music_rect.y;
    UI_DONE;
    HcTvRect r = g_music_rect;
    if (col < 0 || col >= r.w || row < 0 || row >= r.h) return MUSIC_NONE;
    if (!row) return col == r.w - 2 ? MUSIC_MINIMIZE : MUSIC_MOVE;
    if (row == r.h - 1) {
        if (col >= r.w - 3) return MUSIC_RESIZE;
        if (col >= 1 && col < 5) return MUSIC_PAGE_UP;
        if (col >= 6 && col < 10) return MUSIC_PAGE_DOWN;
    }
    if (row == 3 && col >= 1 && col < r.w - 1) return MUSIC_SEEK;
    if (row == 4) {
        const int (*b)[2] = mac_music_transport();
        static const int actions[4] = {MUSIC_PREVIOUS, MUSIC_PLAY, MUSIC_NEXT, MUSIC_STOP};
        for (int i = 0; i < 4; i++) if (col >= b[i][0] && col < b[i][0] + b[i][1]) return actions[i];
    }
    if (row == 5) {
        if (col >= r.w - 15 && col < r.w - 7) return MUSIC_REFRESH;
        if (col >= r.w - 6 && col < r.w - 1) return MUSIC_SITE;
    }
    if (row >= 6 && row < r.h - 2 && col > 0 && col < r.w - 1) {
        int index = g_music_offset + row - 6;
        if (index < music_count()) return MUSIC_ITEM + index;
    }
    return MUSIC_BODY;
}
static void mac_music_action(int hit, int x) {
    if (hit == MUSIC_MINIMIZE) g_music_visible = 0;
    else if (hit == MUSIC_PREVIOUS) mac_music_next(-1);
    else if (hit == MUSIC_PLAY) mac_music_play_pause();
    else if (hit == MUSIC_NEXT) mac_music_next(1);
    else if (hit == MUSIC_STOP) mac_music_stop();
    else if (hit == MUSIC_REFRESH) mac_music_load();
    else if (hit == MUSIC_SITE) mac_browser_open_url("https://musicforprogramming.net/");
    else if (hit == MUSIC_PAGE_UP) mac_music_scroll(-mac_music_capacity());
    else if (hit == MUSIC_PAGE_DOWN) mac_music_scroll(mac_music_capacity());
    else if (hit >= MUSIC_ITEM) mac_music_select(hit - MUSIC_ITEM, 1);
    else if (hit == MUSIC_SEEK) {
        UI_FONT;
        double fraction = (double)(x - (g_music_rect.x + 1) * CW) / ((g_music_rect.w - 2) * CW);
        UI_DONE;
        mac_music_seek(fmax(0, fmin(1, fraction)) * g_mus.duration);
    }
    plat_invalidate();
}
static void mac_music_text(const char *text, int col, int end, int row, uint32_t color) {
    wchar_t s[512];
    utf8_to_wide(text ? text : "", s, 512);
    chrome_label(s, col, end, row, color);
}
static void mac_music_button(const wchar_t *text, int col, int width, int row, int action) {
    if (action == MUSIC_PREVIOUS || action == MUSIC_PLAY || action == MUSIC_NEXT || action == MUSIC_STOP) chrome_button_outline(col, width, row);
    if (mac_music_hit(g_mouse_x, g_mouse_y) == action) fill_rect((float)(col * CW), (float)(row * CH), (float)((col + width) * CW), (float)((row + 1) * CH), TAB_HOVER, 1);
    chrome_label(text, col, col + width, row, g_look.text);
}
static void mac_music_draw(void) {
    if (!g_music_visible || !g_music_placed) return;
    UI_FONT;
    int origin = g_chrome_x, surface = g_chrome_surface_cols, capture = g_chrome_capture;
    float alpha = g_draw_alpha;
    HcTvRect r = g_music_rect;
    int y = r.y;
    char text[600];
    g_chrome_x = r.x * CW; g_chrome_surface_cols = r.w; g_draw_alpha = 1; g_chrome_capture = 0;
    fill_rect(0, (float)(y * CH), (float)(r.w * CW), (float)((y + r.h) * CH), BAR_BG, 1);
    fill_rect(0, (float)(y * CH), (float)(r.w * CW), (float)(y * CH + 1), g_look.dim, 1);
    fill_rect(0, (float)((y + r.h) * CH - 1), (float)(r.w * CW), (float)((y + r.h) * CH), g_look.dim, 1);
    fill_rect(0, (float)(y * CH), 1, (float)((y + r.h) * CH), g_look.dim, 1);
    fill_rect((float)(r.w * CW - 1), (float)(y * CH), (float)(r.w * CW), (float)((y + r.h) * CH), g_look.dim, 1);
    chrome_label(g_grid_on ? L"Music for Programming" : L"↕ Music for Programming", 1, r.w - 3, y, g_look.text);
    mac_music_button(L"−", r.w - 2, 1, y, MUSIC_MINIMIZE);
    MusicItem *cur = music_current();
    mac_music_text(cur ? cur->title : "Choose a mix below", 1, r.w - 1, y + 1, AMBER);
    unsigned pos = (unsigned)fmax(0, g_mus.position), duration = (unsigned)fmax(0, g_mus.duration);
    snprintf(text, sizeof(text), "%s · %u:%02u / %u:%02u", g_mus.buffering ? "Buffering" : g_mus.wants ? "Playing" : "Paused",
             pos / 60, pos % 60, duration / 60, duration % 60);
    mac_music_text(text, 1, r.w - 1, y + 2, g_look.detail);
    wchar_t bar[128];
    double progress = g_mus.duration > 0 ? fmax(0, fmin(1, g_mus.position / g_mus.duration)) : 0;
    int n = chrome_min(r.w - 4, 120), fill = (int)(progress * n);
    bar[0] = '[';
    for (int i = 0; i < n; i++) bar[i + 1] = i < fill ? '=' : ' ';
    bar[n + 1] = ']'; bar[n + 2] = 0;
    chrome_label(bar, 1, r.w - 1, y + 3, g_look.detail);
    const int (*b)[2] = mac_music_transport();
    mac_music_button(L"Prev", b[0][0], b[0][1], y + 4, MUSIC_PREVIOUS);
    mac_music_button(g_mus.wants ? L"Ⅱ Pause" : L"▶ Play", b[1][0], b[1][1], y + 4, MUSIC_PLAY);
    mac_music_button(L"Next", b[2][0], b[2][1], y + 4, MUSIC_NEXT);
    mac_music_button(L"Stop", b[3][0], b[3][1], y + 4, MUSIC_STOP);
    chrome_label(L"Episodes", 1, r.w - 16, y + 5, g_look.dim);
    mac_music_button(L"Refresh", r.w - 15, 8, y + 5, MUSIC_REFRESH);
    mac_music_button(L"Site", r.w - 6, 5, y + 5, MUSIC_SITE);
    for (int i = 0; i < mac_music_capacity() && g_music_offset + i < music_count(); i++) {
        int at = g_music_offset + i, row = y + 6 + i;
        if (g_mus.index == at || mac_music_hit(g_mouse_x, g_mouse_y) == MUSIC_ITEM + at)
            fill_rect((float)CW, (float)(row * CH), (float)((r.w - 1) * CW), (float)((row + 1) * CH), TAB_HOVER, 1);
        snprintf(text, sizeof(text), "%s %s", g_mus.index == at ? ">" : " ", g_music_items[at].title);
        mac_music_text(text, 1, r.w - 1, row, g_mus.index == at ? AMBER : g_look.text_lo);
    }
    const char *message = g_mus.error[0] ? g_mus.error : g_mus.catalog_error[0] ? g_mus.catalog_error :
                          g_mus.loading ? "Loading episode list…" : "musicforprogramming.net · click a mix to play";
    mac_music_text(message, 1, r.w - 1, y + r.h - 2, g_mus.error[0] ? g_look.red : g_look.dim);
    mac_music_button(L"↑", 1, 4, y + r.h - 1, MUSIC_PAGE_UP);
    mac_music_button(L"↓", 6, 4, y + r.h - 1, MUSIC_PAGE_DOWN);
    snprintf(text, sizeof(text), "%d–%d / %d", music_count() ? g_music_offset + 1 : 0,
             chrome_min(g_music_offset + mac_music_capacity(), music_count()), music_count());
    mac_music_text(text, 11, r.w - 3, y + r.h - 1, g_look.detail);
    int size = chrome_min(CW - 3, CH - 5), bottom = (y + r.h) * CH;
    for (int len = size; len >= 2; len -= 3)
        for (int k = 0; k < len; k++) {
            int x = r.w * CW - 3 - k, py = bottom - 3 - (len - 1 - k);
            fill_rect((float)x, (float)py, (float)(x + 1), (float)(py + 1), g_look.detail, 1);
        }
    g_chrome_x = origin; g_chrome_surface_cols = surface; g_chrome_capture = capture; g_draw_alpha = alpha;
    UI_DONE;
}
static void mac_music_drag_move(int x, int y) {
    Tab *tb = active_tab();
    if (!tb) return;
    UI_FONT;
    HcTvRect r = g_music_drag_origin;
    int dx = (int)lround((double)(x - g_music_drag_x) / CW), dy = (int)lround((double)(y - g_music_drag_y) / CH);
    if (g_music_drag == MUSIC_RESIZE) {
        r.w = chrome_min(chrome_max(32, r.w + dx), chrome_min(HC_TV_MAX_WIDTH, tb->t.cols - r.x));
        r.h = chrome_min(chrome_max(12, r.h + dy), chrome_min(HC_TV_MAX_HEIGHT, tb->t.rows - r.y));
    } else {
        r.x = chrome_min(chrome_max(0, r.x + dx), chrome_max(0, tb->t.cols - r.w));
        r.y = chrome_min(chrome_max(0, r.y + dy), chrome_max(0, tb->t.rows - r.h));
    }
    if (mac_music_rect_available(r, &tb->t)) {
        g_music_manual = 1; g_music_requested = g_music_rect = r;
        g_music_placement = (HcTvPlacement){.rect = r, .valid = 1};
        plat_invalidate();
    }
    UI_DONE;
}
static void mac_cutie_blur(void);
/* Returns 1 when the panel took the event. wheel: notches, up positive. */
static int mac_music_mouse(int type, int x, int y, int wheel) {
    if (type == MEV_DRAG && g_music_down) { if (g_music_drag) mac_music_drag_move(x, y); return 1; }
    if (type == MEV_UP && g_music_down) {
        int action = g_music_pending, go = action && mac_music_hit(x, y) == action;
        if (go && action >= MUSIC_ITEM) {
            int index = action - MUSIC_ITEM;
            go = index < music_count() && !strcmp(g_music_pending_id, g_music_items[index].audio);
        }
        mac_music_cancel_press();
        g_music_down = 0;
        if (go) mac_music_action(action, x);
        return 1;
    }
    int hit = mac_music_hit(x, y);
    if (!hit) return 0;
    if (type == MEV_WHEEL) { if (wheel) mac_music_scroll(wheel > 0 ? -1 : 1); return 1; }
    if (type == MEV_MOVE) { g_mouse_x = x; g_mouse_y = y; plat_invalidate(); return 0; }
    if (type == MEV_OTHER) return 1;
    if (type != MEV_DOWN) return 0;
    mac_cutie_blur(); chrome_preview_cancel(); mac_music_cancel_press();
    g_music_down = 1; g_sel_drag = g_sb_drag = 0;
    if (g_grid_on && (hit == MUSIC_MOVE || hit == MUSIC_RESIZE)) return 1;
    if (hit == MUSIC_MOVE || hit == MUSIC_RESIZE) { g_music_drag = hit; g_music_drag_origin = g_music_rect; g_music_drag_x = x; g_music_drag_y = y; }
    else if (hit != MUSIC_BODY) {
        g_music_pending = hit;
        if (hit >= MUSIC_ITEM) {
            int index = hit - MUSIC_ITEM;
            if (index < music_count()) snprintf(g_music_pending_id, sizeof(g_music_pending_id), "%s", g_music_items[index].audio);
            if (!g_music_pending_id[0]) g_music_pending = 0;
        }
    }
    plat_invalidate();
    return 1;
}
static int mac_music_cursor(int x, int y) {
    int hit = mac_music_hit(x, y);
    if (!hit && !g_music_drag) return 0;
    win_cursor((g_music_drag == MUSIC_RESIZE || hit == MUSIC_RESIZE) ? IDC_SIZENWSE : (g_music_drag || hit == MUSIC_MOVE) ? IDC_SIZEALL :
               hit == MUSIC_BODY ? IDC_ARROW : IDC_HAND);
    return 1;
}
