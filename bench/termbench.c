/*
 * termbench.c — measures a terminal window from the outside, the way a person
 * experiences it: synthetic keypress in, pixels on screen out.
 *
 *   termbench latency --class C [--title T] [--n 200]
 *       Needs bench/echoprobe.py running in the terminal. Each keypress
 *       toggles a solid block; we time SendInput → first changed pixel.
 *   termbench newtab  --class C [--title T] [--n 10] [--top PX]
 *       Ctrl+Shift+T → time until the old content is gone (tab shown) and
 *       until a prompt has been drawn (tab usable). Closes it with Ctrl+Shift+W.
 *   termbench type    --class C [--title T] TEXT     (\n = Enter)
 *   termbench shot    --class C [--title T] OUT.bmp
 *   termbench key     --class C [--title T] ctrl+shift+tab
 *   termbench click   --class C [--title T] X,Y      (client coordinates)
 *   termbench resize  --class C [--title T] W,H      (outer window size)
 *   termbench launch  --class C --top PX "COMMAND LINE"
 *       Starts the command, waits for a new window of class C, then for a
 *       prompt on its first text row. Leaves the window open.
 *
 * Window is found by class name, optionally by a substring of its title.
 * Every keystroke is preceded by a foreground check; if the target isn't the
 * foreground window we abort rather than type into something else.
 *
 * Pixels come from DXGI Desktop Duplication: every frame the compositor
 * presents arrives with its QPC present time, so "key → pixels" is measured
 * to the frame that actually showed the change, not to when we looked.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o termbench.exe termbench.c -ld3d11 -ldxgi -lgdi32 -luser32
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static HWND g_target;
static const char *g_class, *g_title;
static LARGE_INTEGER g_freq;

static double now_ms(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)g_freq.QuadPart;
}

static BOOL CALLBACK find_cb(HWND h, LPARAM lp) {
    char cls[256], title[512];
    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, sizeof(cls));
    if (strcmp(cls, g_class)) return TRUE;
    GetWindowTextA(h, title, sizeof(title));
    if (g_title && !strstr(title, g_title)) return TRUE;
    g_target = h;
    return FALSE;
}

static void focus(void) {
    if (GetForegroundWindow() == g_target) return;
    /* an Alt tap lifts the foreground lock for SetForegroundWindow */
    keybd_event(VK_MENU, 0, 0, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    SetForegroundWindow(g_target);
    Sleep(150);
}

static void must_be_foreground(void) {
    if (GetForegroundWindow() != g_target) {
        focus();
        if (GetForegroundWindow() != g_target) {
            fprintf(stderr, "termbench: target lost focus, aborting before typing anywhere else\n");
            exit(3);
        }
    }
}

static void send_vk(WORD vk, int ctrl, int shift) {
    INPUT in[6];
    int n = 0;
    memset(in, 0, sizeof(in));
    must_be_foreground();
    if (ctrl) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; n++; }
    if (shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT; n++; }
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; n++;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    if (shift) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_SHIFT; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    if (ctrl) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = VK_CONTROL; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++; }
    SendInput(n, in, sizeof(INPUT));
}

static void send_text(const char *s) {
    for (; *s; s++) {
        INPUT in[2];
        if (*s == '\n') { send_vk(VK_RETURN, 0, 0); Sleep(5); continue; }
        must_be_foreground();
        memset(in, 0, sizeof(in));
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wScan = in[1].ki.wScan = (WORD)(unsigned char)*s;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(2);
    }
}

/* ── capture: DXGI desktop duplication ─────────────────────────────── */

typedef struct {
    int x, y, w, h;              /* client-area rectangle in desktop coords */
    unsigned int *px;            /* w*h copy of the region from the latest frame */
    double present_ms;           /* QPC time that frame was presented */
} Cap;

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGIOutputDuplication *g_dup;
static ID3D11Texture2D *g_stage;
static int g_desk_w, g_desk_h;

static void dup_init(void) {
    IDXGIDevice *dxdev;
    IDXGIAdapter *ad;
    IDXGIOutput *out;
    IDXGIOutput1 *out1;
    DXGI_OUTPUT_DESC od;
    D3D11_TEXTURE2D_DESC td;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx))) {
        fprintf(stderr, "termbench: D3D11CreateDevice failed\n"); exit(4);
    }
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxdev);
    IDXGIDevice_GetAdapter(dxdev, &ad);
    IDXGIAdapter_EnumOutputs(ad, 0, &out);
    IDXGIOutput_GetDesc(out, &od);
    g_desk_w = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
    g_desk_h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&out1);
    if (FAILED(IDXGIOutput1_DuplicateOutput(out1, (IUnknown *)g_dev, &g_dup))) {
        fprintf(stderr, "termbench: DuplicateOutput failed\n"); exit(4);
    }
    memset(&td, 0, sizeof(td));
    td.Width = g_desk_w; td.Height = g_desk_h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_stage);
}

static void cap_init(Cap *c, int top_skip) {
    RECT r;
    POINT p = {0, 0};
    if (!g_dup) dup_init();
    GetClientRect(g_target, &r);
    ClientToScreen(g_target, &p);
    c->x = p.x;
    c->y = p.y + top_skip;
    c->w = r.right;
    c->h = r.bottom - top_skip;
    if (c->x < 0) c->x = 0;
    if (c->y < 0) c->y = 0;
    if (c->x + c->w > g_desk_w) c->w = g_desk_w - c->x;
    if (c->y + c->h > g_desk_h) c->h = g_desk_h - c->y;
    c->px = (unsigned int *)calloc((size_t)c->w * c->h, 4);
    c->present_ms = 0;
}

/* Waits up to timeout_ms for the next presented frame. Returns 1 and updates
   c->px / c->present_ms when one arrived. */
static int cap_next(Cap *c, int timeout_ms) {
    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource *res = NULL;
    ID3D11Texture2D *tex;
    D3D11_MAPPED_SUBRESOURCE m;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(g_dup, timeout_ms, &fi, &res);
    int y, got = 0;
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (FAILED(hr)) { fprintf(stderr, "termbench: AcquireNextFrame 0x%lx\n", (unsigned long)hr); exit(4); }
    if (fi.LastPresentTime.QuadPart) {
        IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex);
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_stage, (ID3D11Resource *)tex);
        ID3D11Texture2D_Release(tex);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_stage, 0, D3D11_MAP_READ, 0, &m))) {
            for (y = 0; y < c->h; y++)
                memcpy(c->px + (size_t)y * c->w, (char *)m.pData + (size_t)(c->y + y) * m.RowPitch + (size_t)c->x * 4, (size_t)c->w * 4);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_stage, 0);
            c->present_ms = (double)fi.LastPresentTime.QuadPart * 1000.0 / (double)g_freq.QuadPart;
            got = 1;
        }
    }
    IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(g_dup);
    return got;
}

static int cap_diff(const unsigned int *a, const unsigned int *b, int n);

/* Returns once our region has gone quiet_ms without changing (other windows
   may keep animating), or after 3 s regardless. */
static void cap_settle(Cap *c, int quiet_ms) {
    unsigned int *prev = (unsigned int *)malloc((size_t)c->w * c->h * 4);
    double start = now_ms(), last_change = now_ms();
    memcpy(prev, c->px, (size_t)c->w * c->h * 4);
    while (now_ms() - last_change < quiet_ms && now_ms() - start < 3000) {
        if (cap_next(c, 20) && cap_diff(prev, c->px, c->w * c->h)) {
            memcpy(prev, c->px, (size_t)c->w * c->h * 4);
            last_change = now_ms();
        }
    }
    free(prev);
}

static int cap_diff(const unsigned int *a, const unsigned int *b, int n) {
    int i, d = 0;
    for (i = 0; i < n; i++) d += (a[i] & 0xFFFFFF) != (b[i] & 0xFFFFFF);
    return d;
}

static int color_far(unsigned int a, unsigned int b) {
    int dr = (int)((a >> 16) & 255) - (int)((b >> 16) & 255);
    int dg = (int)((a >> 8) & 255) - (int)((b >> 8) & 255);
    int db = (int)(a & 255) - (int)(b & 255);
    return abs(dr) + abs(dg) + abs(db) > 60;
}

/* columns with ink in pixel rows [y0, y0+band); background = top-right of the region */
static int ink_columns_at(const Cap *c, int y0, int band) {
    unsigned int bg = c->px[2 * c->w + c->w - 40];
    int x, y, cols = 0;
    if (y0 + band > c->h) band = c->h - y0;
    for (x = 0; x < c->w - 20; x++)
        for (y = y0; y < y0 + band; y++)
            if (color_far(c->px[y * c->w + x], bg)) { cols++; break; }
    return cols;
}

static int ink_columns(const Cap *c, int band) { return ink_columns_at(c, 0, band); }

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *name, double *v, int n) {
    double sum = 0;
    int i;
    if (!n) { printf("%-14s no samples\n", name); return; }
    qsort(v, n, sizeof(double), cmp_d);
    for (i = 0; i < n; i++) sum += v[i];
    printf("%-14s n=%-4d min %7.1f  p50 %7.1f  p90 %7.1f  p99 %7.1f  max %7.1f  mean %7.1f ms\n", name, n, v[0],
           v[n / 2], v[(int)(n * 0.9)], v[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1], v[n - 1], sum / n);
}

/* ── commands ───────────────────────────────────────────────────────── */

static int cmd_latency(int n, int top) {
    Cap c;
    unsigned int *base;
    double *lat = (double *)malloc(sizeof(double) * n);
    int i, got = 0, misses = 0;
    focus();
    cap_init(&c, top);
    base = (unsigned int *)malloc((size_t)c.w * c.h * 4);
    cap_settle(&c, 200);
    for (i = 0; i < n; i++) {
        double t0;
        int changed = 0;
        /* the screen must be still: no frame for 60 ms, then use the last one */
        cap_settle(&c, 60 + rand() % 40);
        memcpy(base, c.px, (size_t)c.w * c.h * 4);
        send_vk('A', 0, 0);
        t0 = now_ms();
        while (now_ms() - t0 < 2000) {
            if (!cap_next(&c, 100)) continue;
            if (cap_diff(base, c.px, c.w * c.h) > 20) { changed = 1; break; }
        }
        if (changed && c.present_ms >= t0) lat[got++] = c.present_ms - t0;
        else misses++;
    }
    if (misses) printf("(%d samples with no visible change)\n", misses);
    report("key→frame", lat, got);
    return 0;
}

/* The old tab is prepared with its prompt pushed down to about row 12
   (bench/run-newtab.sh does that), so the two events are unambiguous:
   shown = the old prompt is gone from rows ~8-15, ready = a prompt has been
   drawn on row 0 of the new tab. Both can land in the same frame. */
static int cmd_newtab(int n, int top) {
    Cap c;
    double *shown = (double *)malloc(sizeof(double) * n), *ready = (double *)malloc(sizeof(double) * n);
    int i, ns = 0, nr = 0, band = 40, low = 150, lowband = 120;
    focus();
    cap_init(&c, top);
    for (i = 0; i < n; i++) {
        double t0, t, t_shown = -1, t_ready = -1;
        cap_settle(&c, 400);
        if (ink_columns(&c, band) > 20 || ink_columns_at(&c, low, lowband) < 40)
            fprintf(stderr, "iteration %d: old tab doesn't look prepared (prompt should sit near row 12)\n", i);
        send_vk('T', 1, 1);
        t0 = now_ms();
        while ((t = now_ms()) - t0 < 10000) {
            if (!cap_next(&c, 100)) continue;
            t = c.present_ms;
            if (getenv("TERMBENCH_TRACE")) fprintf(stderr, "  t=%7.1f top=%d low=%d\n", t - t0, ink_columns(&c, band), ink_columns_at(&c, low, lowband));
            if (t_shown < 0 && ink_columns_at(&c, low, lowband) < 20) t_shown = t - t0;
            if (t_shown >= 0 && ink_columns(&c, band) >= 40) { t_ready = t - t0; break; }
        }
        if (t_shown >= 0) shown[ns++] = t_shown;
        if (t_ready >= 0) ready[nr++] = t_ready;
        Sleep(300);
        send_vk('W', 1, 1);
        Sleep(1500);             /* let a pre-warming terminal get its spare back */
    }
    report("tab shown", shown, ns);
    report("prompt ready", ready, nr);
    return 0;
}

static HWND g_known[256];
static int g_nknown;

static BOOL CALLBACK known_cb(HWND h, LPARAM lp) {
    char cls[256];
    (void)lp;
    GetClassNameA(h, cls, sizeof(cls));
    if (!strcmp(cls, g_class) && g_nknown < 256) g_known[g_nknown++] = h;
    return TRUE;
}

static BOOL CALLBACK fresh_cb(HWND h, LPARAM lp) {
    char cls[256];
    int i;
    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameA(h, cls, sizeof(cls));
    if (strcmp(cls, g_class)) return TRUE;
    for (i = 0; i < g_nknown; i++) if (g_known[i] == h) return TRUE;
    g_target = h;
    return FALSE;
}

/* true once the window is actually composited: its terminal background
   (#0C0C0C in both terminals) shows at three sample points */
static int window_on_screen(const Cap *c) {
    const unsigned int pts[3] = {c->px[2 * c->w + c->w - 40], c->px[(c->h - 10) * c->w + 10], c->px[(c->h / 2) * c->w + c->w - 40]};
    int i;
    for (i = 0; i < 3; i++) {
        int r = (pts[i] >> 16) & 255, g = (pts[i] >> 8) & 255, b = pts[i] & 255;
        if (r > 30 || g > 30 || b > 30) return 0;
    }
    return 1;
}

static int cmd_launch(const char *cmdline, int top) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char buf[2048];
    double t0, t_hwnd, t_visible = -1, t_ready = -1;
    Cap c;
    g_target = NULL;
    EnumWindows(known_cb, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    snprintf(buf, sizeof(buf), "%s", cmdline);
    if (!g_dup) dup_init();
    t0 = now_ms();
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { fprintf(stderr, "launch failed\n"); return 1; }
    while (!g_target && now_ms() - t0 < 10000) { EnumWindows(fresh_cb, 0); if (!g_target) Sleep(1); }
    if (!g_target) { fprintf(stderr, "no window appeared\n"); return 1; }
    t_hwnd = now_ms() - t0;
    Sleep(20);                   /* let it reach its first size before we fix the capture rect */
    cap_init(&c, top);
    while (now_ms() - t0 < 15000) {
        if (!cap_next(&c, 100)) continue;
        if (!window_on_screen(&c)) continue;
        if (t_visible < 0) t_visible = c.present_ms - t0;
        if (ink_columns(&c, 40) >= 40) {
            t_ready = c.present_ms - t0;
            if (getenv("TERMBENCH_SHOT")) {   /* keep the frame we counted, for checking */
                FILE *f = fopen(getenv("TERMBENCH_SHOT"), "wb");
                BITMAPFILEHEADER fh;
                BITMAPINFOHEADER ih;
                memset(&fh, 0, sizeof(fh));
                memset(&ih, 0, sizeof(ih));
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                ih.biSize = sizeof(ih); ih.biWidth = c.w; ih.biHeight = -c.h; ih.biPlanes = 1; ih.biBitCount = 32;
                if (f) { fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f); fwrite(c.px, 4, (size_t)c.w * c.h, f); fclose(f); }
            }
            break;
        }
    }
    printf("hwnd %.1f ms, on screen %.1f ms, prompt %.1f ms\n", t_hwnd, t_visible, t_ready);
    return 0;
}

static int cmd_shot(const char *path) {
    Cap c;
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    focus();
    cap_init(&c, 0);
    {   /* a fresh duplication's first frames can carry no image; wait for a real one */
        double t0 = now_ms();
        InvalidateRect(g_target, NULL, FALSE);
        while (!cap_next(&c, 100) && now_ms() - t0 < 2000) InvalidateRect(g_target, NULL, FALSE);
    }
    f = fopen(path, "wb");
    if (!f) return 1;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + c.w * c.h * 4;
    ih.biSize = sizeof(ih);
    ih.biWidth = c.w;
    ih.biHeight = -c.h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(c.px, 4, (size_t)c.w * c.h, f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "";
    const char *arg = NULL;
    int n = 0, top = -1, i;
    typedef BOOL (WINAPI *PFN)(HANDLE);
    PFN dpi = (PFN)(void *)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
    if (dpi) dpi((HANDLE)(INT_PTR)-4);
    QueryPerformanceFrequency(&g_freq);
    srand(GetTickCount());
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--class") && i + 1 < argc) g_class = argv[++i];
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) g_title = argv[++i];
        else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--top") && i + 1 < argc) top = atoi(argv[++i]);
        else arg = argv[i];
    }
    if (!g_class) { fprintf(stderr, "need --class\n"); return 2; }
    if (!strcmp(cmd, "launch") && arg) return cmd_launch(arg, top < 0 ? 0 : top);
    EnumWindows(find_cb, 0);
    if (!g_target) { fprintf(stderr, "termbench: no window of class %s%s%s\n", g_class, g_title ? " titled *" : "", g_title ? g_title : ""); return 2; }
    if (top < 0) top = 0;
    if (!strcmp(cmd, "latency")) return cmd_latency(n ? n : 200, top);
    if (!strcmp(cmd, "newtab")) return cmd_newtab(n ? n : 10, top);
    if (!strcmp(cmd, "type") && arg) {
        char buf[4096], *o = buf;
        const char *s;
        focus();
        for (s = arg; *s && o < buf + sizeof(buf) - 1; s++) {
            if (s[0] == '\\' && s[1] == 'n') { *o++ = '\n'; s++; }
            else *o++ = *s;
        }
        *o = 0;
        send_text(buf);
        return 0;
    }
    if (!strcmp(cmd, "shot") && arg) return cmd_shot(arg);
    if (!strcmp(cmd, "resize") && arg) {
        int w, h;
        if (sscanf(arg, "%d,%d", &w, &h) != 2) return 2;
        SetWindowPos(g_target, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
        return 0;
    }
    if (!strcmp(cmd, "click") && arg) {
        POINT p;
        if (sscanf(arg, "%ld,%ld", &p.x, &p.y) != 2) return 2;
        focus();
        must_be_foreground();
        ClientToScreen(g_target, &p);
        SetCursorPos(p.x, p.y);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        return 0;
    }
    if (!strcmp(cmd, "key") && arg) {
        int ctrl = strstr(arg, "ctrl+") != NULL, shift = strstr(arg, "shift+") != NULL;
        const char *k = strrchr(arg, '+') ? strrchr(arg, '+') + 1 : arg;
        WORD vk = !strcmp(k, "tab") ? VK_TAB : !strcmp(k, "enter") ? VK_RETURN : !strcmp(k, "esc") ? VK_ESCAPE
                : !strcmp(k, "pgup") ? VK_PRIOR : !strcmp(k, "pgdn") ? VK_NEXT : (WORD)toupper((unsigned char)k[0]);
        focus();
        send_vk(vk, ctrl, shift);
        return 0;
    }
    fprintf(stderr, "usage: termbench latency|newtab|type|shot --class C [--title T] [--n N] [--top PX] [ARG]\n");
    return 2;
}
