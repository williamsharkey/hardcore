/* One Chrome DevTools connection per window, to a headless Chrome (or the
   Edge every Windows has) with hardcore's own profile beside config.ini. The
   browser is shared by every hardcore window: it's found again through its
   DevToolsActivePort file, and the last window out closes it. Cookies and
   profiles are never read or copied; pages are driven only through CDP.
   The WebSocket is WinHTTP's: a worker thread connects and receives, and
   posts each message to the window; sends go out from the UI thread. */
#include "hardcore_json.h"

#define WM_APP_CDP (WM_APP + 13)   /* wp: CDP_* below; lp: a heap string for CDP_MESSAGE/CDP_FAILED */
enum { CDP_MESSAGE, CDP_CONNECTED, CDP_FAILED, CDP_CLOSED };
enum { CDP_IDLE, CDP_CONNECTING, CDP_READY };

/* reply: the parsed reply (freed after the call); result: its "result" node, -1 on failure */
typedef void (*CdpDone)(JDoc *reply, int result, const char *error, void *ctx);
typedef void (*CdpReady)(const char *error, void *ctx);
typedef struct { int id; CdpDone done; void *ctx; } CdpPending;

static struct {
    int state, next_id;
    HINTERNET session, connection, ws;
    HANDLE thread;
    CdpPending *pending;
    int npending, cap;
    struct { CdpReady fn; void *ctx; } waiting[64];
    int nwaiting;
    char error[300];
} g_cdp;

static void browser_cdp_event(JDoc *d, const char *method, int params, const char *session);
static void browser_cdp_lost(const char *why);

static void cdp_profile(wchar_t *out, int cap) {
    _snwprintf(out, cap, L"%ls\\Chrome", g_cfg_dir);
    out[cap - 1] = 0;
}
/* Chrome if it's installed, else Edge, which comes with Windows. */
static int cdp_executable(wchar_t *out, int cap) {
    static const wchar_t *const tails[] = {
        L"\\Google\\Chrome\\Application\\chrome.exe", L"\\Microsoft\\Edge\\Application\\msedge.exe",
    };
    const wchar_t *roots[] = {_wgetenv(L"ProgramFiles"), _wgetenv(L"ProgramFiles(x86)"), _wgetenv(L"LOCALAPPDATA")};
    wchar_t *override = _wgetenv(L"HARDCORE_CHROME_EXECUTABLE");
    if (override && *override) { _snwprintf(out, cap, L"%ls", override); return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES; }
    for (int t = 0; t < 2; t++)
        for (int r = 0; r < 3; r++) {
            if (!roots[r]) continue;
            _snwprintf(out, cap, L"%ls%ls", roots[r], tails[t]);
            out[cap - 1] = 0;
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    return 0;
}
/* DevToolsActivePort: the port on line one, the browser's WebSocket path on line two. */
static int cdp_endpoint(const wchar_t *profile, int *port, wchar_t *path, int cap) {
    wchar_t file[MAX_PATH];
    char text[512] = "", *nl;
    FILE *f;
    _snwprintf(file, MAX_PATH, L"%ls\\DevToolsActivePort", profile);
    if (!(f = _wfopen(file, L"rb"))) return 0;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    *port = atoi(text);
    if (!(nl = strchr(text, '\n')) || *port <= 0 || *port > 65535) return 0;
    nl++;
    nl[strcspn(nl, "\r\n")] = 0;
    if (strncmp(nl, "/devtools/browser/", 18) || strpbrk(nl, " \"<>")) return 0;
    MultiByteToWideChar(CP_UTF8, 0, nl, -1, path, cap);
    return 1;
}
static int cdp_open_socket(int port, const wchar_t *path) {
    HINTERNET s = WinHttpOpen(L"hardcore", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0), c = NULL, r = NULL, ws = NULL;
    if (s) {
        WinHttpSetTimeouts(s, 2000, 2000, 5000, 0);
        c = WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)port, 0);
    }
    if (c) r = WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, 0);
    if (r && WinHttpSetOption(r, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
        WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL))
        ws = WinHttpWebSocketCompleteUpgrade(r, 0);
    if (r) WinHttpCloseHandle(r);
    if (!ws) {
        if (c) WinHttpCloseHandle(c);
        if (s) WinHttpCloseHandle(s);
        return 0;
    }
    g_cdp.session = s; g_cdp.connection = c; g_cdp.ws = ws;
    return 1;
}
static void cdp_post_failure(const char *message) {
    char *copy = _strdup(message);
    if (!copy || !PostMessageW(g_hwnd, WM_APP_CDP, CDP_FAILED, (LPARAM)copy)) free(copy);
}
static DWORD WINAPI cdp_thread(LPVOID unused) {
    wchar_t profile[MAX_PATH], path[256], exe[MAX_PATH], file[MAX_PATH];
    int port = 0;
    (void)unused;
    cdp_profile(profile, MAX_PATH);
    if (!(cdp_endpoint(profile, &port, path, 256) && cdp_open_socket(port, path))) {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        wchar_t cmd[2048];
        if (!cdp_executable(exe, MAX_PATH)) { cdp_post_failure("Install Google Chrome or Microsoft Edge to browse inside hardcore."); return 0; }
        SHCreateDirectoryExW(NULL, profile, NULL);
        _snwprintf(file, MAX_PATH, L"%ls\\DevToolsActivePort", profile);
        DeleteFileW(file);
        _snwprintf(cmd, 2048, L"\"%ls\" --headless=new --user-data-dir=\"%ls\" --remote-debugging-address=127.0.0.1 --remote-debugging-port=0 "
                   L"--no-first-run --no-default-browser-check --disable-features=Translate --hide-scrollbars about:blank", exe, profile);
        cmd[2047] = 0;
        memset(&si, 0, sizeof(si));
        si.cb = sizeof(si);
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL, profile, &si, &pi)) {
            cdp_post_failure("Chrome could not be started.");
            return 0;
        }
        CloseHandle(pi.hThread);
        int ok = 0;
        /* up to 20 s for its debugging port (the process started may hand over to another and exit) */
        for (int i = 0; i < 200 && !ok; i++) {
            Sleep(100);
            ok = cdp_endpoint(profile, &port, path, 256) && cdp_open_socket(port, path);
        }
        CloseHandle(pi.hProcess);
        if (!ok) { cdp_post_failure("Chrome started but didn't open its debugging port. Another Chrome may be using hardcore's profile."); return 0; }
    }
    PostMessageW(g_hwnd, WM_APP_CDP, CDP_CONNECTED, 0);
    /* receive: a message may arrive in fragments */
    size_t cap = 1 << 16, used = 0;
    char *buf = (char *)malloc(cap + 1);
    while (buf) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (used == cap) {
            char *more = cap >= (64u << 20) ? NULL : (char *)realloc(buf, cap * 2 + 1);
            if (!more) break;
            buf = more; cap *= 2;
        }
        if (WinHttpWebSocketReceive(g_cdp.ws, buf + used, (DWORD)(cap - used), &got, &type) != NO_ERROR) break;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
        used += got;
        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) continue;
        char *message = (char *)malloc(used + 1);
        if (message) {
            memcpy(message, buf, used);
            message[used] = 0;
            if (!PostMessageW(g_hwnd, WM_APP_CDP, CDP_MESSAGE, (LPARAM)message)) free(message);
        }
        used = 0;
    }
    free(buf);
    PostMessageW(g_hwnd, WM_APP_CDP, CDP_CLOSED, 0);
    return 0;
}

static void cdp_close_socket(void) {
    if (g_cdp.ws) { WinHttpWebSocketClose(g_cdp.ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0); WinHttpCloseHandle(g_cdp.ws); g_cdp.ws = NULL; }
    if (g_cdp.connection) { WinHttpCloseHandle(g_cdp.connection); g_cdp.connection = NULL; }
    if (g_cdp.session) { WinHttpCloseHandle(g_cdp.session); g_cdp.session = NULL; }
}
static void cdp_connect(CdpReady fn, void *ctx) {
    if (g_cdp.state == CDP_READY) { fn(NULL, ctx); return; }
    if (g_cdp.nwaiting == 64) { fn("Too many pages are waiting for Chrome.", ctx); return; }
    g_cdp.waiting[g_cdp.nwaiting].fn = fn;
    g_cdp.waiting[g_cdp.nwaiting++].ctx = ctx;
    if (g_cdp.state == CDP_CONNECTING) return;
    g_cdp.state = CDP_CONNECTING;
    if (g_cdp.thread) { CloseHandle(g_cdp.thread); g_cdp.thread = NULL; }
    g_cdp.thread = CreateThread(NULL, 0, cdp_thread, NULL, 0, NULL);
    if (!g_cdp.thread) cdp_post_failure("Chrome could not be started.");
}
static void cdp_wake(const char *error) {
    int n = g_cdp.nwaiting;
    g_cdp.nwaiting = 0;
    for (int i = 0; i < n; i++) g_cdp.waiting[i].fn(error, g_cdp.waiting[i].ctx);
}
/* params: a JSON object's text, or NULL for {}. done may be NULL. Returns the id, 0 if nothing went. */
static int cdp_send(const char *session, const char *method, const char *params, CdpDone done, void *ctx) {
    SBuf b = {0};
    int id;
    if (g_cdp.state != CDP_READY || !g_cdp.ws) { if (done) done(NULL, -1, "Chrome isn't connected.", ctx); return 0; }
    if (g_cdp.npending == g_cdp.cap) {
        int cap = g_cdp.cap ? g_cdp.cap * 2 : 64;
        CdpPending *more = (CdpPending *)realloc(g_cdp.pending, (size_t)cap * sizeof(*more));
        if (!more) { if (done) done(NULL, -1, "Out of memory.", ctx); return 0; }
        g_cdp.pending = more; g_cdp.cap = cap;
    }
    id = ++g_cdp.next_id;
    sb_printf(&b, "{\"id\":%d,\"method\":", id);
    sb_json(&b, method);
    sb_put(&b, ",\"params\":", 10);
    sb_put(&b, params ? params : "{}", strlen(params ? params : "{}"));
    if (session && *session) { sb_put(&b, ",\"sessionId\":", 13); sb_json(&b, session); }
    sb_put(&b, "}", 1);
    if (!b.p || WinHttpWebSocketSend(g_cdp.ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, b.p, (DWORD)b.n) != NO_ERROR) {
        free(b.p);
        if (done) done(NULL, -1, "Chrome connection failed.", ctx);
        return 0;
    }
    free(b.p);
    g_cdp.pending[g_cdp.npending].id = id;
    g_cdp.pending[g_cdp.npending].done = done;
    g_cdp.pending[g_cdp.npending++].ctx = ctx;
    return id;
}
static void cdp_fail_all(const char *why) {
    while (g_cdp.npending) {
        CdpPending p = g_cdp.pending[--g_cdp.npending];
        if (p.done) p.done(NULL, -1, why, p.ctx);
    }
}
/* WM_APP_CDP */
static void cdp_message(WPARAM kind, char *text) {
    if (kind == CDP_CONNECTED) {
        g_cdp.state = CDP_READY;
        cdp_send(NULL, "Target.setDiscoverTargets", "{\"discover\":true}", NULL, NULL);
        cdp_wake(NULL);
        return;
    }
    if (kind == CDP_FAILED || kind == CDP_CLOSED) {
        const char *why = kind == CDP_FAILED && text ? text : "The connection to Chrome closed.";
        cdp_close_socket();
        g_cdp.state = CDP_IDLE;
        snprintf(g_cdp.error, sizeof(g_cdp.error), "%s", why);
        cdp_fail_all(why);
        cdp_wake(why);
        browser_cdp_lost(why);
        free(text);
        return;
    }
    JDoc *d = json_parse(text, strlen(text));
    free(text);
    if (!d) return;
    int idn = jget(d, 0, "id");
    if (idn >= 0) {
        int id = jint(d, idn);
        for (int i = 0; i < g_cdp.npending; i++)
            if (g_cdp.pending[i].id == id) {
                CdpPending p = g_cdp.pending[i];
                g_cdp.pending[i] = g_cdp.pending[--g_cdp.npending];
                int err = jget(d, 0, "error");
                if (p.done) p.done(d, err >= 0 ? -1 : jget(d, 0, "result"), err >= 0 ? jstr(d, jget(d, err, "message")) : NULL, p.ctx);
                break;
            }
    } else {
        int params = jget(d, 0, "params");
        browser_cdp_event(d, jstr(d, jget(d, 0, "method")), params, jstr(d, jget(d, 0, "sessionId")));
    }
    json_free(d);
}
