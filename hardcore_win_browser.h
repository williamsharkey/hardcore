/* Browser tabs on Windows: hardcore_browser_mac.m's Chrome documents in C.
   Each tab is a page in the shared headless Chrome (hardcore_win_cdp.h).
   Grid mode ("Mono") runs browser/project.js in an isolated world and lays
   the page out as terminal cells; Native mode shows Chrome's own pixels
   (a screencast) and forwards the mouse and keys. The page script, welcome
   page and grid layout are the Mac's, compiled in. */
#include <wincodec.h>
#include "build/browser.inc"
#include "hardcore_browser_grid.h"
#include "hardcore_browser_pixels.h"
#include "hardcore_win_cdp.h"

#define WB_IMAGES 32
typedef struct {
    char *source;
    unsigned char *rgba;         /* premultiplied RGBA, at most BROWSER_IMAGE_DECODE_LIMIT a side */
    int sw, sh, source_width, source_height, cols, rows;
    int failed, loading, attempts;
    DWORD retry_at;
    unsigned char *indexed;
    int qw, qh;
    uint32_t palette[18], background;
} WbImage;
typedef struct { char label[160]; char value[512]; int op; } WbItem;
typedef struct { char *text; int id; } WbLine;
typedef struct WinBrowser {
    int tab_id, serial, disposed;
    char target[128], session[128], frame[128], adopt[128];
    int started, ready, owns_target, metadata_pending;
    int context_id, prepared_context;
    struct { int id; char frame[128]; } contexts[16];
    int epoch, generation, scroll, total, dirty, requesting, changed_again, project_serial, focused_action, cutie_generation;
    int can_back, can_forward;
    char url[4096], title[1024];
    BrowserBlock *blocks;
    int block_count;
    int anchors[4001];           /* anchor id -> block + 1 */
    WbImage cache[WB_IMAGES];
    int ncache;
    WbImage *shown[WB_IMAGES];
    int nshown;
    uint16_t *node_grid;
    int node_cols, node_rows, page_rows;
    int last_cols, last_rows, last_x, last_y, last_height, last_side, last_preview;
    /* the grid's own editors: 1 address, 2 a text field, 3 a text area */
    uint32_t edit[4096];
    int edit_len, edit_kind, edit_node, edit_revision, edit_epoch, edit_caret, edit_all, edit_secret;
    int edit_x, edit_y, edit_w, edit_h, edit_view;
    /* menus: 1 context, 2 a <select> */
    WbItem *menu;
    int menu_count, menu_kind, menu_selected, menu_node, menu_revision, menu_x, menu_y, menu_w;
    char menu_url[4096];
    /* the inspector: one element's DOM, style and text, a panel below the page */
    WbLine *insp[3];
    int insp_n[3], inspect_open, inspect_tab, inspect_scroll, inspect_revision, inspect_id, inspect_parent;
    /* native */
    int casting, buttons, sent_w, sent_h;
    double sent_scale;
    unsigned char *frame_px;
    int frame_w, frame_h, frame_seq;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11Device *tex_dev;
    int tex_w, tex_h, tex_seq;
} WinBrowser;

static int g_browser_nav_hover, g_browser_native_drag, g_browser_serial;
static WinBrowser *browser_for(Tab *tb) { return tb && tb->kind == TK_BROWSER ? (WinBrowser *)tb->browser : NULL; }
static Tab *wb_new_tab(const char *address, int parent, int native, const char *target);
static void mac_browser_sync(void);
static void wb_project(WinBrowser *b);
static void wb_load(WinBrowser *b, const char *url);
static void wb_inspect_request(WinBrowser *b, int node);

/* ── small helpers ── */
static uint32_t wb_utf8(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp = *s++;
    int need = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    if (cp >= 0x80 && !need) cp = 0xFFFD;
    else if (need) {
        cp &= need == 3 ? 0x07 : need == 2 ? 0x0F : 0x1F;
        while (need-- > 0) {
            if ((*s & 0xC0) != 0x80) { cp = 0xFFFD; break; }
            cp = (cp << 6) | (*s++ & 0x3F);
        }
    }
    *p = (const char *)s;
    return cp;
}
static int wb_base64_value(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}
static unsigned char *wb_base64_decode(const char *s, size_t *out_n) {
    size_t n = strlen(s), o = 0;
    unsigned char *out = (unsigned char *)malloc(n / 4 * 3 + 4);
    unsigned v = 0;
    int bits = 0;
    if (!out) return NULL;
    for (; *s; s++) {
        int d = wb_base64_value((unsigned char)*s);
        if (d < 0) continue;
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(v >> bits); }
    }
    *out_n = o;
    return out;
}
static void sb_base64(SBuf *b, const unsigned char *p, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char q[4];
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        q[0] = t[v >> 18 & 63]; q[1] = t[v >> 12 & 63];
        q[2] = i + 1 < n ? t[v >> 6 & 63] : '='; q[3] = i + 2 < n ? t[v & 63] : '=';
        sb_put(b, q, 4);
    }
}

/* What the address bar accepts, normalized; "" when it isn't a place to go. */
static int wb_url(const char *input, char *out, size_t cap) {
    char s[4096];
    size_t n;
    while (*input == ' ' || *input == '\t') input++;
    snprintf(s, sizeof(s), "%s", input);
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    out[0] = 0;
    if (!n || !strcmp(s, "hardcore:welcome")) { snprintf(out, cap, "hardcore:welcome"); return 1; }
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 32 || s[i] == 127) return 0;
    if (s[0] == '/' || (isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'))) {   /* a file */
        char path[4096];
        size_t k = 0;
        for (size_t i = 0; s[i] && k < sizeof(path) - 4; i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '\\') c = '/';
            if (c == ' ' || c == '#' || c == '?' || c == '%') k += (size_t)snprintf(path + k, sizeof(path) - k, "%%%02X", c);
            else path[k++] = (char)c;
        }
        path[k] = 0;
        snprintf(out, cap, "file://%s%s", path[0] == '/' ? "" : "/", path);
        return 1;
    }
    if (!strstr(s, "://") && strncmp(s, "about:", 6) && strncmp(s, "chrome:", 7) && strncmp(s, "data:", 5)) {
        char authority[512];
        size_t a = strcspn(s, "/?#");
        snprintf(authority, sizeof(authority), "%.*s", (int)(a < sizeof(authority) - 1 ? a : sizeof(authority) - 1), s);
        char *colon = strrchr(authority, ':');
        int host_port = colon && colon[1] && strspn(colon + 1, "0123456789") == strlen(colon + 1);
        if (!colon || host_port) {
            int local = !strcmp(authority, "localhost") || !strncmp(authority, "localhost:", 10) || !strncmp(authority, "127.0.0.1", 9);
            if (!strchr(authority, '.') && !local && !host_port) {   /* a search, not a host */
                SBuf q = {0};
                sb_printf(&q, "https://duckduckgo.com/html/?q=");
                for (const char *p = s; *p; p++) {
                    unsigned char c = (unsigned char)*p;
                    if (isalnum(c) || strchr("-_.~", c)) sb_put(&q, (const char *)&c, 1);
                    else if (c == ' ') sb_put(&q, "+", 1);
                    else sb_printf(&q, "%%%02X", c);
                }
                snprintf(out, cap, "%s", q.p ? q.p : "");
                free(q.p);
                return out[0] != 0;
            }
            char with[4200];
            snprintf(with, sizeof(with), "%s%s", local ? "http://" : "https://", s);
            snprintf(s, sizeof(s), "%s", with);
        }
    }
    if (!_strnicmp(s, "https://", 8) || !_strnicmp(s, "http://", 7)) {
        const char *host = s + (s[4] == 's' || s[4] == 'S' ? 8 : 7);
        if (!*host || *host == '/') return 0;
        snprintf(out, cap, "%s", s);
        return 1;
    }
    if (!_strnicmp(s, "file:///", 8) || !strcmp(s, "about:blank") || !_strnicmp(s, "data:text/html", 14)) { snprintf(out, cap, "%s", s); return 1; }
    if (!_strnicmp(s, "chrome://history", 16) || !_strnicmp(s, "chrome://settings", 17) || !_strnicmp(s, "chrome://extensions", 19)) {
        snprintf(out, cap, "%s", s);
        return 1;
    }
    return 0;
}

/* ── the shared page script ── */
static const char *wb_source(void) {
    static char *source;
    if (!source) {
        static const char shim[] = "globalThis.webkit={messageHandlers:{hardcoreBrowser:{postMessage:(message)=>globalThis.hardcoreChromeNotify(JSON.stringify(message))}}};\n";
        size_t n = strlen((const char *)browser_script);
        source = (char *)malloc(sizeof(shim) + n);
        if (source) { memcpy(source, shim, sizeof(shim) - 1); memcpy(source + sizeof(shim) - 1, browser_script, n + 1); }
    }
    return source ? source : "";
}

/* ── WIC: image bytes to pixels, at most `limit` a side ── */
static const GUID CLSID_WICImagingFactory_ = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
static const GUID IID_IWICImagingFactory_ = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
static const GUID GUID_WICPixelFormat32bppBGRA_ = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
static const GUID GUID_WICPixelFormat32bppPRGBA_ = {0x3cc4a650, 0xa527, 0x4d37, {0xa9, 0x16, 0x31, 0x42, 0xc7, 0xeb, 0xed, 0xba}};
static IWICImagingFactory *wic(void) {
    static IWICImagingFactory *factory;
    static int tried;
    if (!tried) {
        tried = 1;
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory_, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory_, (void **)&factory))) factory = NULL;
    }
    return factory;
}
static unsigned char *wic_decode(const unsigned char *bytes, size_t n, int limit, int premultiplied_rgba, int *w, int *h, int *ow, int *oh) {
    IWICImagingFactory *f = wic();
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *convert = NULL;
    unsigned char *px = NULL;
    UINT fw = 0, fh = 0;
    if (!f || !n || n > 32u * 1024 * 1024) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)bytes, (DWORD)n)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &fw, &fh)) && fw && fh) {
        double scale = limit > 0 && (fw > (UINT)limit || fh > (UINT)limit) ? fmin((double)limit / fw, (double)limit / fh) : 1;
        UINT tw = (UINT)fmax(1, fw * scale + .5), th = (UINT)fmax(1, fh * scale + .5);
        IWICBitmapSource *src = (IWICBitmapSource *)frame;
        if (scale < 1 && SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &scaler)) &&
            SUCCEEDED(IWICBitmapScaler_Initialize(scaler, src, tw, th, WICBitmapInterpolationModeFant)))
            src = (IWICBitmapSource *)scaler;
        else { tw = fw; th = fh; }
        if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &convert)) &&
            SUCCEEDED(IWICFormatConverter_Initialize(convert, src, premultiplied_rgba ? &GUID_WICPixelFormat32bppPRGBA_ : &GUID_WICPixelFormat32bppBGRA_,
                                                     WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) &&
            (px = (unsigned char *)malloc((size_t)tw * th * 4)) &&
            FAILED(IWICFormatConverter_CopyPixels(convert, NULL, tw * 4, tw * th * 4, px))) {
            free(px);
            px = NULL;
        }
        if (px) { *w = (int)tw; *h = (int)th; if (ow) *ow = (int)fw; if (oh) *oh = (int)fh; }
    }
    if (convert) IWICFormatConverter_Release(convert);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (decoder) IWICBitmapDecoder_Release(decoder);
    if (stream) IWICStream_Release(stream);
    return px;
}

/* ── the page's blocks ── */
static void wb_cutie_changed(WinBrowser *b) { b->cutie_generation = b->cutie_generation == 0x7fffffff ? 1 : b->cutie_generation + 1; }
static void wb_free_blocks(WinBrowser *b) {
    for (int i = 0; i < b->block_count; i++) { free(b->blocks[i].cells); free(b->blocks[i].nodes); free(b->blocks[i].ends); }
    free(b->blocks);
    b->blocks = NULL;
    b->block_count = 0;
    memset(b->anchors, 0, sizeof(b->anchors));
}
static void wb_free_images(WinBrowser *b) {
    for (int i = 0; i < b->ncache; i++) { free(b->cache[i].source); free(b->cache[i].rgba); free(b->cache[i].indexed); }
    memset(b->cache, 0, sizeof(b->cache));
    b->ncache = b->nshown = 0;
}
static int wb_reserve(BrowserBlock *block, int *capacity, int needed) {
    if (needed <= *capacity) return 1;
    int next = chrome_max(*capacity * 2, needed + 128);
    Cell *cells = (Cell *)realloc(block->cells, (size_t)next * sizeof(Cell));
    if (!cells) return 0;
    block->cells = cells;
    uint16_t *nodes = (uint16_t *)realloc(block->nodes, (size_t)next * sizeof(uint16_t));
    if (!nodes) return 0;
    block->nodes = nodes;
    *capacity = next;
    return 1;
}
static void wb_load_image(WinBrowser *b, WbImage *image);
static void wb_decode_runs(WinBrowser *b, JDoc *d, BrowserBlock *block, int runs, int heading, int *capacity, int *remaining) {
    for (int run = jtype(d, runs) == JN_ARR ? d->n[runs].first : -1; run >= 0 && *remaining > 0; run = d->n[run].next) {
        if (jtype(d, run) != JN_OBJ) continue;
        const char *text = jstr(d, jget(d, run, "text"));
        int node = jint(d, jget(d, run, "node")), action = jint(d, jget(d, run, "action"));
        char alt[300];
        if (node < 0 || node > 4000) node = 0;
        if (action < 0 || action > 4000) action = 0;
        int asset = jget(d, run, "image");
        if (jtype(d, asset) == JN_OBJ && b->nshown < WB_IMAGES) {
            const char *src = jstr(d, jget(d, asset, "src"));
            WbImage *image = NULL;
            if (strlen(src) > 3 * 1024 * 1024) src = "";
            for (int i = 0; i < b->ncache && src[0]; i++) if (!strcmp(b->cache[i].source, src)) image = &b->cache[i];
            if (!image && src[0] && b->ncache < WB_IMAGES) {
                image = &b->cache[b->ncache++];
                memset(image, 0, sizeof(*image));
                image->source = _strdup(src);
                if (!image->source) { b->ncache--; image = NULL; }
            }
            if (image) {
                /* Chrome may expose the DOM before this resource is in its cache;
                   the image's load event projects again later. */
                int loaded = jget(d, asset, "loaded") < 0 || jtruthy(d, jget(d, asset, "loaded"));
                if (image->failed && image->attempts < 3 && GetTickCount() >= image->retry_at) image->failed = 0;
                if (loaded && !image->rgba && !image->failed && !image->loading) wb_load_image(b, image);
                b->shown[b->nshown++] = image;
                int width = chrome_max(1, chrome_min(20000, jint(d, jget(d, asset, "width"))));
                int height = chrome_max(1, chrome_min(20000, jint(d, jget(d, asset, "height"))));
                if (image->source_width < 1) image->source_width = width;
                if (image->source_height < 1) image->source_height = height;
                browser_image_size(image->source_width, image->source_height, cell_w(g_font) / chrome_max(1, g_scale),
                                   cell_h(g_font) / chrome_max(1, g_scale), &image->cols, &image->rows);
                if (!wb_reserve(block, capacity, block->count + 1)) { *remaining = 0; break; }
                Cell cell = {0};
                cell.ch = (uint32_t)b->nshown; cell.wflag = BROWSER_IMAGE;
                cell.fg = (uint32_t)image->cols; cell.bg = (uint32_t)image->rows; cell.link = (uint16_t)action;
                block->nodes[block->count] = (uint16_t)node;
                block->cells[block->count++] = cell;
                (*remaining)--;
                continue;
            }
            snprintf(alt, sizeof(alt), "[%s]", jstr(d, jget(d, asset, "alt")));
            text = alt;
        }
        int attr = (jtruthy(d, jget(d, run, "bold")) || heading ? A_BOLD : 0) | (jtruthy(d, jget(d, run, "dim")) ? A_DIM : 0) |
                   (jtruthy(d, jget(d, run, "italic")) ? A_ITALIC : 0);
        for (const char *p = text; *p && *remaining > 0;) {
            uint32_t cp = wb_utf8(&p);
            if ((cp < 32 && cp != '\n' && cp != '\t') || (cp >= 0x7F && cp < 0xA0) || (cp >= 0xD800 && cp <= 0xDFFF)) continue;
            if (cp == '\t' || cp == 0xA0) cp = ' ';
            if (cp == '\n' && !block->literal && !block->columns) cp = ' ';
            int width = cp == '\n' ? 1 : cp_width(cp);
            if (width < 1) continue;
            if (!wb_reserve(block, capacity, block->count + width)) { *remaining = 0; break; }
            Cell cell = {0};
            cell.ch = cp; cell.link = (uint16_t)action;
            cell.fg = action ? C_PAL(4) : 0; cell.attr = (uint8_t)attr;
            if (width == 2) cell.wflag = W_WIDE;
            block->nodes[block->count] = (uint16_t)node;
            block->cells[block->count++] = cell;
            if (width == 2) { cell.ch = ' '; cell.wflag = W_CONT; block->nodes[block->count] = (uint16_t)node; block->cells[block->count++] = cell; }
            (*remaining)--;
        }
    }
}
static void wb_decode(WinBrowser *b, JDoc *d, int snapshot) {
    wb_free_blocks(b);
    b->nshown = 0;
    int items = jget(d, snapshot, "blocks");
    if (jtype(d, items) != JN_ARR || !(b->blocks = (BrowserBlock *)calloc(2000, sizeof(BrowserBlock)))) return;
    int remaining = 100000;
    for (int item = d->n[items].first; item >= 0 && b->block_count < 2000 && remaining > 0; item = d->n[item].next) {
        if (jtype(d, item) != JN_OBJ) continue;
        int columns = jget(d, item, "columns"), ncols = jtype(d, columns) == JN_ARR ? jlen(d, columns) : 0;
        if (ncols < 1 || ncols > 16) { columns = -1; ncols = 0; }
        if (columns < 0 && jtype(d, jget(d, item, "runs")) != JN_ARR) continue;
        BrowserBlock *block = &b->blocks[b->block_count];
        const char *kind = jstr(d, jget(d, item, "kind"));
        block->indent = chrome_max(0, chrome_min(12, jint(d, jget(d, item, "indent"))));
        block->hanging = jget(d, item, "hang") >= 0 ? chrome_max(0, chrome_min(12, jint(d, jget(d, item, "hang")))) : 2;
        block->literal = !strcmp(kind, "code");
        block->gap = (!strcmp(kind, "list") || !strcmp(kind, "table") || !strcmp(kind, "caption")) ? 0 : 1;
        if (jtype(d, jget(d, item, "gap")) == JN_NUM) block->gap = chrome_max(0, chrome_min(2, jint(d, jget(d, item, "gap"))));
        block->rule = jtruthy(d, jget(d, item, "rule"));
        block->table = chrome_max(0, chrome_min(2000, jint(d, jget(d, item, "table"))));
        block->header = jtruthy(d, jget(d, item, "header"));
        block->columns = ncols;
        block->numeric = (unsigned)jint(d, jget(d, item, "numeric")) & ((1u << block->columns) - 1u);
        block->decoration.attr = A_DIM;
        if (block->columns && !(block->ends = (int *)calloc((size_t)block->columns, sizeof(int)))) break;
        int anchor = jint(d, jget(d, item, "anchor"));
        if (anchor > 0 && anchor <= 4000) b->anchors[anchor] = b->block_count + 1;
        b->block_count++;
        int capacity = 0, heading = !strcmp(kind, "heading");
        if (columns >= 0) {
            int column = 0;
            for (int c = d->n[columns].first; c >= 0 && column < block->columns; c = d->n[c].next, column++) {
                if (jtype(d, c) == JN_ARR) wb_decode_runs(b, d, block, c, heading, &capacity, &remaining);
                block->ends[column] = block->count;
            }
        } else wb_decode_runs(b, d, block, jget(d, item, "runs"), heading, &capacity, &remaining);
    }
}
static void wb_set_metadata(Tab *tb, const char *title, const char *site, const char *url) {
    if (title) utf8_to_wide(title, tb->t.title, 256);
    if (site) snprintf(tb->browser_site, sizeof(tb->browser_site), "%s", site);
    if (url && url[0]) snprintf(tb->browser_url, sizeof(tb->browser_url), "%s", url);
    tb->browser_label_cols = 0;
}
static void wb_accept(WinBrowser *b, JDoc *d, int snapshot) {
    Tab *tb = tab_by_id(b->tab_id);
    char url[4096];
    if (!tb) return;
    wb_decode(b, d, snapshot);
    wb_cutie_changed(b);
    b->generation = jint(d, jget(d, snapshot, "generation"));
    b->focused_action = 0;
    b->dirty = 1;
    const char *raw = jstr(d, jget(d, snapshot, "url"));
    if (raw[0] && wb_url(raw, url, sizeof(url)) && strcmp(url, "about:blank") && strncmp(url, "data:", 5))
        snprintf(tb->browser_url, sizeof(tb->browser_url), "%s", url);
    wb_set_metadata(tb, jstr(d, jget(d, snapshot, "title")), jstr(d, jget(d, snapshot, "site")), NULL);
    if (b->inspect_open && b->insp_n[0]) wb_inspect_request(b, b->inspect_id);   /* the inspected node, again */
    if (tb == active_tab()) { g_sel_on = 0; update_title(); }
    else tb->activity = 1;
    plat_invalidate();
}
/* A page of hardcore's own: a heading and lines, through the same decoder. */
static void wb_message_page(WinBrowser *b, const char *title, const char *line1, const char *line2) {
    SBuf s = {0};
    sb_printf(&s, "{\"generation\":0,\"title\":");
    sb_json(&s, title);
    sb_printf(&s, ",\"blocks\":[{\"kind\":\"heading\",\"runs\":[{\"text\":");
    sb_json(&s, title);
    sb_printf(&s, "}]},{\"runs\":[{\"text\":");
    sb_json(&s, line1 ? line1 : "");
    sb_printf(&s, "}]},{\"runs\":[{\"text\":");
    sb_json(&s, line2 ? line2 : "");
    sb_printf(&s, "}]}]}");
    JDoc *d = s.p ? json_parse(s.p, s.n) : NULL;
    if (d) { wb_accept(b, d, 0); json_free(d); }
    free(s.p);
}
static void wb_failure(WinBrowser *b, const char *message) {
    Tab *tb = tab_by_id(b->tab_id);
    if (!tb) return;
    tb->busy = 0;
    tb->browser_native = 0;
    b->casting = 0;
    wb_message_page(b, "Chrome browsing unavailable", message && *message ? message : "Could not connect to Chrome.",
                    "Install Google Chrome (or use the Edge that comes with Windows), then press F5 to retry.");
    update_busy(0);
    plat_invalidate();
}

/* ── talking to the page: each reply finds its browser again by tab id and serial ── */
typedef struct { int tab, serial, epoch, op, stage, a, b2, c, d2; char *text; } WbCtx;
static WbCtx *wb_ctx(WinBrowser *b, int op) {
    WbCtx *x = (WbCtx *)calloc(1, sizeof(WbCtx));
    if (x) { x->tab = b->tab_id; x->serial = b->serial; x->epoch = b->epoch; x->op = op; }
    return x;
}
static void wb_ctx_free(WbCtx *x) { if (x) { free(x->text); free(x); } }
static WinBrowser *wb_live(WbCtx *x) {
    WinBrowser *b = browser_for(tab_by_id(x->tab));
    return b && b->serial == x->serial && !b->disposed ? b : NULL;
}
static void wb_command(WinBrowser *b, const char *method, const char *params, CdpDone done, void *ctx) {
    if (!b->session[0]) { if (done) done(NULL, -1, "Chrome page is not ready.", ctx); return; }
    cdp_send(b->session, method, params, done, ctx);
}

/* evaluation in the "hardcore.browser" isolated world; the first one in a world loads the script */
enum { EV_NONE, EV_PROJECT, EV_INFO_ACTION, EV_INFO_MENU, EV_COPY, EV_INSPECT };
static void wb_eval_done(WinBrowser *b, WbCtx *x, JDoc *d, int value);
static void wb_eval_cb(JDoc *d, int result, const char *error, void *ctx) {
    WbCtx *x = (WbCtx *)ctx;
    WinBrowser *b = wb_live(x);
    if (!b) { wb_ctx_free(x); return; }
    if (result < 0) {
        if (x->op == EV_PROJECT && x->d2 == b->project_serial) { b->requesting = 0; if (b->changed_again) { b->changed_again = 0; wb_project(b); } }
        (void)error;
        wb_ctx_free(x);
        return;
    }
    if (x->stage == 0) {            /* the frame tree: the main frame's id */
        const char *frame = jstr(d, jpath(d, result, "frameTree", "frame", "id", NULL));
        SBuf p = {0};
        if (!frame[0]) { wb_eval_cb(NULL, -1, "no document", x); return; }
        snprintf(b->frame, sizeof(b->frame), "%s", frame);
        sb_printf(&p, "{\"frameId\":");
        sb_json(&p, frame);
        sb_printf(&p, ",\"worldName\":\"hardcore.browser\"}");
        x->stage = 1;
        wb_command(b, "Page.createIsolatedWorld", p.p, wb_eval_cb, x);
        free(p.p);
        return;
    }
    if (x->stage == 1) {            /* the world: evaluate in it */
        int context = jint(d, jget(d, result, "executionContextId"));
        if (!context) { wb_eval_cb(NULL, -1, "document changed", x); return; }
        b->context_id = context;
        x->stage = 2;
    } else if (x->stage == 3) {     /* the evaluation's answer */
        if (jget(d, result, "exceptionDetails") < 0) b->prepared_context = x->c;
        wb_eval_done(b, x, d, jget(d, result, "exceptionDetails") >= 0 ? -1 : jpath(d, result, "result", "value", NULL));
        wb_ctx_free(x);
        return;
    }
    /* stage 2: run it */
    SBuf p = {0};
    int prepare = b->prepared_context != b->context_id;
    sb_put(&p, "{\"expression\":\"", 15);
    {
        SBuf e = {0};
        if (prepare) { sb_put(&e, wb_source(), strlen(wb_source())); sb_put(&e, ";\n", 2); }
        sb_put(&e, x->text, strlen(x->text));
        if (e.p) {
            sb_put(&e, "", 1);   /* NUL, without counting it */
            e.n--;
            SBuf q = {0};
            sb_json(&q, e.p);
            if (q.p) sb_put(&p, q.p + 1, q.n - 2);
            free(q.p);
        }
        free(e.p);
    }
    sb_printf(&p, "\",\"contextId\":%d,\"returnByValue\":true,\"awaitPromise\":true,\"userGesture\":true}", b->context_id);
    x->stage = 3;
    x->c = b->context_id;
    wb_command(b, "Runtime.evaluate", p.p, wb_eval_cb, x);
    free(p.p);
}
static void wb_eval(WinBrowser *b, const char *js, int op, int a, int b2) {
    WbCtx *x;
    if (!b->ready || !(x = wb_ctx(b, op))) return;
    x->text = _strdup(js);
    x->a = a; x->b2 = b2; x->d2 = op == EV_PROJECT ? b->project_serial : 0;
    if (!x->text) { wb_ctx_free(x); return; }
    if (b->context_id) { x->stage = 2; wb_eval_cb(NULL, 0, NULL, x); return; }
    x->stage = 0;
    wb_command(b, "Page.getFrameTree", NULL, wb_eval_cb, x);
}
static void wb_project(WinBrowser *b) {
    if (b->requesting) { b->changed_again = 1; return; }
    b->requesting = 1;
    b->project_serial++;
    wb_eval(b, "globalThis.hardcoreBrowser?.project()", EV_PROJECT, 0, 0);
}

/* ── metadata: history position, address, title ── */
static void wb_history_cb(JDoc *d, int result, const char *error, void *ctx) {
    WbCtx *x = (WbCtx *)ctx;
    WinBrowser *b = wb_live(x);
    (void)error;
    if (b) {
        b->metadata_pending = 0;
        if (result >= 0) {
            int entries = jget(d, result, "entries"), current = jint(d, jget(d, result, "currentIndex"));
            b->can_back = current > 0;
            b->can_forward = current + 1 < jlen(d, entries);
            int entry = jat(d, entries, current);
            if (x->op == 1) {       /* go: one step through history */
                int to = jat(d, entries, current + x->a);
                if (to >= 0) {
                    char p[64];
                    snprintf(p, sizeof(p), "{\"entryId\":%d}", jint(d, jget(d, to, "id")));
                    wb_command(b, "Page.navigateToHistoryEntry", p, NULL, NULL);
                }
            } else if (entry >= 0) {
                snprintf(b->url, sizeof(b->url), "%s", jstr(d, jget(d, entry, "url")));
                snprintf(b->title, sizeof(b->title), "%s", jstr(d, jget(d, entry, "title")));
            }
            plat_invalidate();
        }
    }
    wb_ctx_free(x);
}
static void wb_refresh_metadata(WinBrowser *b) {
    WbCtx *x;
    if (b->metadata_pending || !b->ready || !(x = wb_ctx(b, 0))) return;
    b->metadata_pending = 1;
    wb_command(b, "Page.getNavigationHistory", NULL, wb_history_cb, x);
}
static void wb_go(WinBrowser *b, int delta) {
    WbCtx *x = wb_ctx(b, 1);
    if (!x) return;
    x->a = delta;
    wb_command(b, "Page.getNavigationHistory", NULL, wb_history_cb, x);
}

/* ── the viewport and the screencast ── */
static double wb_scale(void) { UINT dpi = g_hwnd ? GetDpiForWindow(g_hwnd) : 96; return dpi ? dpi / 96.0 : 1; }
static void wb_viewport(WinBrowser *b) {
    int w, h;
    double s = wb_scale();
    plat_client_size(&w, &h);
    if (b->inspect_open) { Tab *tb = tab_by_id(b->tab_id); if (tb) h = chrome_min(h, mac_browser_page_rows(tb) * CH); }
    int cw = chrome_max(1, chrome_min(8192, (int)(w / s))), chh = chrome_max(1, chrome_min(8192, (int)(h / s)));
    if (!b->ready || (b->sent_w == cw && b->sent_h == chh && b->sent_scale == s)) return;
    char p[160];
    b->sent_w = cw; b->sent_h = chh; b->sent_scale = s;
    snprintf(p, sizeof(p), "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%.3f,\"mobile\":false}", cw, chh, s);
    wb_command(b, "Emulation.setDeviceMetricsOverride", p, NULL, NULL);
    b->dirty = 1;
}
static void wb_cast(WinBrowser *b, int on) {
    if (!b->ready || b->casting == on) return;
    b->casting = on;
    if (on) wb_command(b, "Page.startScreencast", "{\"format\":\"jpeg\",\"quality\":85,\"everyNthFrame\":1}", NULL, NULL);
    else wb_command(b, "Page.stopScreencast", NULL, NULL, NULL);
}

/* ── starting a page: create (or adopt) a target, attach, enable, the script ── */
static void wb_setup_cb(JDoc *d, int result, const char *error, void *ctx);
static void wb_setup_next(WinBrowser *b, WbCtx *x) {
    SBuf p = {0};
    switch (x->stage) {
    case 1:
        sb_printf(&p, "{\"targetId\":");
        sb_json(&p, b->target);
        sb_printf(&p, ",\"flatten\":true}");
        cdp_send(NULL, "Target.attachToTarget", p.p, wb_setup_cb, x);
        break;
    case 2: wb_command(b, "Page.enable", NULL, wb_setup_cb, x); break;
    case 3: wb_command(b, "Runtime.enable", NULL, wb_setup_cb, x); break;
    case 4: wb_command(b, "Runtime.addBinding", "{\"name\":\"hardcoreChromeNotify\",\"executionContextName\":\"hardcore.browser\"}", wb_setup_cb, x); break;
    case 5:
        sb_printf(&p, "{\"source\":");
        sb_json(&p, wb_source());
        sb_printf(&p, ",\"worldName\":\"hardcore.browser\"}");
        wb_command(b, "Page.addScriptToEvaluateOnNewDocument", p.p, wb_setup_cb, x);
        break;
    case 6:
        b->ready = 1;
        b->sent_w = b->sent_h = 0;
        wb_viewport(b);
        if (x->text) {
            sb_printf(&p, "{\"url\":");
            sb_json(&p, x->text);
            sb_put(&p, "}", 1);
            wb_command(b, "Page.navigate", p.p, wb_setup_cb, x);
            break;
        }
        /* an adopted page is already where it was */
        /* fall through */
    default: {
        Tab *tb = tab_by_id(b->tab_id);
        if (tb) snprintf(tb->browser_target, sizeof(tb->browser_target), "%s", b->target);
        wb_refresh_metadata(b);
        wb_project(b);
        mac_browser_sync();
        wb_ctx_free(x);
        break;
    }
    }
    free(p.p);
}
static void wb_setup_cb(JDoc *d, int result, const char *error, void *ctx) {
    WbCtx *x = (WbCtx *)ctx;
    WinBrowser *b = wb_live(x);
    if (!b) {
        if (x->stage == 0 && result >= 0) {   /* the tab closed while its page was made */
            SBuf p = {0};
            sb_printf(&p, "{\"targetId\":");
            sb_json(&p, jstr(d, jget(d, result, "targetId")));
            sb_put(&p, "}", 1);
            cdp_send(NULL, "Target.closeTarget", p.p, NULL, NULL);
            free(p.p);
        }
        wb_ctx_free(x);
        return;
    }
    if (result < 0 || (x->stage == 6 && jstr(d, jget(d, result, "errorText"))[0])) {
        char why[300];
        snprintf(why, sizeof(why), "%s", result < 0 ? (error ? error : "Chrome failed.") : jstr(d, jget(d, result, "errorText")));
        if (x->stage == 1 && b->adopt[0]) {      /* a saved target is gone: a fresh page for the same address */
            Tab *tb = tab_by_id(b->tab_id);
            b->adopt[0] = b->target[0] = 0;
            if (tb) tb->browser_target[0] = 0;
            b->started = 0;
            wb_ctx_free(x);
            if (tb) wb_load(b, tb->browser_url[0] ? tb->browser_url : "hardcore:welcome");
            return;
        }
        if (x->stage == 6) {   /* a navigation error: the page stays usable */
            Tab *tb = tab_by_id(b->tab_id);
            if (tb) tb->busy = 0;
            wb_message_page(b, "Page failed to load", why, "Use Ctrl+L to change the address or F5 to retry.");
            wb_ctx_free(x);
            return;
        }
        b->started = 0;
        wb_failure(b, why);
        wb_ctx_free(x);
        return;
    }
    if (x->stage == 0) {
        snprintf(b->target, sizeof(b->target), "%s", jstr(d, jget(d, result, "targetId")));
        b->owns_target = 1;
    } else if (x->stage == 1) {
        snprintf(b->session, sizeof(b->session), "%s", jstr(d, jget(d, result, "sessionId")));
        b->owns_target = 1;
    }
    x->stage++;
    wb_setup_next(b, x);
}
static void wb_connected(const char *error, void *ctx) {
    WbCtx *x = (WbCtx *)ctx;
    WinBrowser *b = wb_live(x);
    if (!b) { wb_ctx_free(x); return; }
    if (error) { b->started = 0; wb_failure(b, error); wb_ctx_free(x); return; }
    if (b->adopt[0]) {
        snprintf(b->target, sizeof(b->target), "%s", b->adopt);
        x->stage = 1;
        wb_setup_next(b, x);
        return;
    }
    x->stage = 0;
    cdp_send(NULL, "Target.createTarget", "{\"url\":\"about:blank\"}", wb_setup_cb, x);   /* not background: a hidden page never loads its lazy images */
}
/* The welcome page: compiled in, handed over as a data: URL */
static char *wb_welcome_url(void) {
    SBuf s = {0};
    sb_printf(&s, "data:text/html;base64,");
    sb_base64(&s, browser_welcome, strlen((const char *)browser_welcome));
    sb_put(&s, "", 1);
    return s.p;
}
static void wb_inspect_free(WinBrowser *b) {
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < b->insp_n[t]; i++) free(b->insp[t][i].text);
        free(b->insp[t]);
        b->insp[t] = NULL;
        b->insp_n[t] = 0;
    }
}
static void wb_reset_document(WinBrowser *b) {
    wb_inspect_free(b);
    b->inspect_scroll = 0;
    if (b->edit_kind > 1) b->edit_kind = 0;
    free(b->menu); b->menu = NULL; b->menu_kind = b->menu_count = 0;
    b->epoch++; b->generation = 0; b->scroll = 0; b->dirty = 1;
    wb_free_blocks(b);
    wb_free_images(b);
    wb_cutie_changed(b);
}
static void wb_load(WinBrowser *b, const char *url) {
    Tab *tb = tab_by_id(b->tab_id);
    char *address = !strcmp(url, "hardcore:welcome") ? wb_welcome_url() : _strdup(url);
    if (!address) return;
    wb_cutie_changed(b);
    b->scroll = 0; b->generation = 0; b->dirty = 1; b->edit_kind = 0;
    wb_free_blocks(b);
    if (tb) {
        tb->busy = 1;
        wb_set_metadata(tb, NULL, NULL, url);
        update_busy(0);
        plat_invalidate();
    }
    if (b->started && b->ready) {
        SBuf p = {0};
        WbCtx *x = wb_ctx(b, 0);
        sb_printf(&p, "{\"url\":");
        sb_json(&p, address);
        sb_put(&p, "}", 1);
        if (x) { x->stage = 6; x->text = NULL; }
        wb_command(b, "Page.navigate", p.p, x ? wb_setup_cb : NULL, x);
        free(p.p);
        free(address);
        return;
    }
    if (b->started) { free(address); return; }   /* still connecting: the first address wins */
    WbCtx *x = wb_ctx(b, 0);
    if (!x) { free(address); return; }
    b->started = 1;
    x->text = address;
    cdp_connect(wb_connected, x);
}

/* ── images: the bytes Chrome already has, never fetched again ── */
static void wb_image_done(WinBrowser *b, WbImage *image, const unsigned char *bytes, size_t n) {
    int w = 0, h = 0, ow = 0, oh = 0;
    image->loading = 0;
    free(image->rgba);
    image->rgba = n ? wic_decode(bytes, n, BROWSER_IMAGE_DECODE_LIMIT, 1, &w, &h, &ow, &oh) : NULL;
    if (image->rgba) {
        image->sw = w; image->sh = h;
        if (ow > 0 && oh > 0) { image->source_width = ow; image->source_height = oh; }
        free(image->indexed); image->indexed = NULL; image->qw = image->qh = 0;
    }
    image->failed = !image->rgba;
    if (image->failed) image->retry_at = GetTickCount() + (DWORD)(300 * image->attempts * image->attempts);
    b->dirty = 1;
    plat_invalidate();
}
static void wb_image_cb(JDoc *d, int result, const char *error, void *ctx) {
    WbCtx *x = (WbCtx *)ctx;
    WinBrowser *b = wb_live(x);
    WbImage *image = b && x->epoch == b->epoch && x->a >= 0 && x->a < b->ncache ? &b->cache[x->a] : NULL;
    (void)error;
    if (!image || !image->source || strcmp(image->source, x->text)) { wb_ctx_free(x); return; }
    if (result < 0) { wb_image_done(b, image, NULL, 0); wb_ctx_free(x); return; }
    if (x->stage == 0) {
        const char *frame = jstr(d, jpath(d, result, "frameTree", "frame", "id", NULL));
        SBuf p = {0};
        if (!frame[0]) { wb_image_done(b, image, NULL, 0); wb_ctx_free(x); return; }
        sb_printf(&p, "{\"frameId\":");
        sb_json(&p, frame);
        sb_printf(&p, ",\"url\":");
        sb_json(&p, image->source);
        sb_put(&p, "}", 1);
        x->stage = 1;
        wb_command(b, "Page.getResourceContent", p.p, wb_image_cb, x);
        free(p.p);
        return;
    }
    const char *content = jstr(d, jget(d, result, "content"));
    size_t n = 0;
    unsigned char *bytes = NULL;
    if (strlen(content) <= 4u * 1024 * 1024) {
        if (jtruthy(d, jget(d, result, "base64Encoded"))) bytes = wb_base64_decode(content, &n);
        else { n = strlen(content); bytes = (unsigned char *)_strdup(content); }
    }
    wb_image_done(b, image, bytes, bytes ? n : 0);
    free(bytes);
    wb_ctx_free(x);
}
static void wb_load_image(WinBrowser *b, WbImage *image) {
    if (image->loading || image->attempts >= 3) return;
    image->attempts++;
    if (!strncmp(image->source, "data:image/", 11)) {
        const char *comma = strchr(image->source, ',');
        size_t n = 0;
        unsigned char *bytes = comma && comma - image->source >= 7 && !strncmp(comma - 7, ";base64", 7) ? wb_base64_decode(comma + 1, &n) : NULL;
        wb_image_done(b, image, bytes, bytes ? n : 0);
        free(bytes);
        return;
    }
    WbCtx *x = wb_ctx(b, 0);
    if (!x || !(x->text = _strdup(image->source))) { wb_ctx_free(x); return; }
    image->loading = 1;
    x->a = (int)(image - b->cache);
    wb_command(b, "Page.getFrameTree", NULL, wb_image_cb, x);
}

/* ── events from Chrome ── */
static WinBrowser *wb_by_session(const char *session) {
    for (int i = 0; i < g_ntabs; i++) {
        WinBrowser *b = browser_for(g_tabs[i]);
        if (b && b->session[0] && !strcmp(b->session, session)) return b;
    }
    return NULL;
}
static WinBrowser *wb_by_target(const char *target) {
    for (int i = 0; i < g_ntabs; i++) {
        WinBrowser *b = browser_for(g_tabs[i]);
        if (b && b->target[0] && !strcmp(b->target, target)) return b;
    }
    return NULL;
}
static void mac_browser_context_native(WinBrowser *b, JDoc *d, int message);
static void wb_notify(WinBrowser *b, JDoc *d, int message) {
    const char *type = jstr(d, jget(d, message, "type"));
    Tab *tb = tab_by_id(b->tab_id);
    if (!tb) return;
    if (!strcmp(type, "context")) { if (tb == active_tab()) mac_browser_context_native(b, d, message); return; }
    if (!strcmp(type, "navigation")) {
        wb_reset_document(b);
        if (b->url[0] && strcmp(b->url, "about:blank") && strncmp(b->url, "data:", 5)) wb_set_metadata(tb, b->title, NULL, b->url);
        tb->busy = !b->ready;
        update_busy(0);
    }
    if (b->ready) { tb->busy = 0; update_busy(0); wb_project(b); }
    plat_invalidate();
}
static void wb_simple_notify(WinBrowser *b, const char *type) {
    char text[64];
    snprintf(text, sizeof(text), "{\"type\":\"%s\"}", type);
    JDoc *d = json_parse(text, strlen(text));
    if (d) { wb_notify(b, d, 0); json_free(d); }
}
static void wb_frame(WinBrowser *b, JDoc *d, int params) {
    char ack[64];
    size_t n = 0;
    snprintf(ack, sizeof(ack), "{\"sessionId\":%d}", jint(d, jget(d, params, "sessionId")));
    wb_command(b, "Page.screencastFrameAck", ack, NULL, NULL);
    unsigned char *jpeg = wb_base64_decode(jstr(d, jget(d, params, "data")), &n);
    int w = 0, h = 0;
    unsigned char *px = jpeg ? wic_decode(jpeg, n, 0, 0, &w, &h, NULL, NULL) : NULL;
    free(jpeg);
    if (!px) return;
    free(b->frame_px);
    b->frame_px = px; b->frame_w = w; b->frame_h = h; b->frame_seq++;
    plat_invalidate();
}
static void wb_page_event(WinBrowser *b, JDoc *d, const char *method, int params) {
    if (!strcmp(method, "Runtime.bindingCalled") && !strcmp(jstr(d, jget(d, params, "name")), "hardcoreChromeNotify")) {
        /* only our world in the main frame, even if a page finds the binding's name */
        int context = jint(d, jget(d, params, "executionContextId")), ok = 0;
        for (int i = 0; i < 16; i++) if (b->contexts[i].id == context && b->frame[0] && !strcmp(b->contexts[i].frame, b->frame)) ok = 1;
        const char *payload = jstr(d, jget(d, params, "payload"));
        if (ok && strlen(payload) < 1024 * 1024) {
            JDoc *e = json_parse(payload, strlen(payload));
            if (e && jtype(e, 0) == JN_OBJ) wb_notify(b, e, 0);
            json_free(e);
        }
    } else if (!strcmp(method, "Runtime.executionContextCreated")) {
        int context = jget(d, params, "context");
        if (!strcmp(jstr(d, jget(d, context, "name")), "hardcore.browser")) {
            const char *frame = jstr(d, jpath(d, context, "auxData", "frameId", NULL));
            int slot = 0;
            for (int i = 0; i < 16; i++) if (!b->contexts[i].id) { slot = i; break; }
            b->contexts[slot].id = jint(d, jget(d, context, "id"));
            snprintf(b->contexts[slot].frame, sizeof(b->contexts[slot].frame), "%s", frame);
        }
    } else if (!strcmp(method, "Runtime.executionContextsCleared")) {
        b->context_id = b->prepared_context = 0;
        memset(b->contexts, 0, sizeof(b->contexts));
    } else if (!strcmp(method, "Runtime.executionContextDestroyed")) {
        int context = jint(d, jget(d, params, "executionContextId"));
        for (int i = 0; i < 16; i++) if (b->contexts[i].id == context) b->contexts[i].id = 0;
        if (context == b->context_id) b->context_id = b->prepared_context = 0;
    } else if (!strcmp(method, "Page.frameNavigated")) {
        int frame = jget(d, params, "frame");
        if (jget(d, frame, "parentId") >= 0) return;
        b->context_id = b->prepared_context = 0;
        snprintf(b->frame, sizeof(b->frame), "%s", jstr(d, jget(d, frame, "id")));
        snprintf(b->url, sizeof(b->url), "%s", jstr(d, jget(d, frame, "url")));
        wb_simple_notify(b, "navigation");
        wb_refresh_metadata(b);
    } else if (!strcmp(method, "Page.loadEventFired") || !strcmp(method, "Page.domContentEventFired") || !strcmp(method, "Page.navigatedWithinDocument")) {
        wb_refresh_metadata(b);
        wb_simple_notify(b, "changed");
    } else if (!strcmp(method, "Page.screencastFrame")) wb_frame(b, d, params);
    else if (!strcmp(method, "Inspector.targetCrashed")) { b->ready = 0; wb_failure(b, "Chrome page crashed. Open the page again."); }
}
static void browser_cdp_event(JDoc *d, const char *method, int params, const char *session) {
    WinBrowser *b;
    if (session[0] && (b = wb_by_session(session))) wb_page_event(b, d, method, params);
    if (!strcmp(method, "Target.targetInfoChanged")) {
        int info = jget(d, params, "targetInfo");
        if ((b = wb_by_target(jstr(d, jget(d, info, "targetId"))))) {
            snprintf(b->url, sizeof(b->url), "%s", jstr(d, jget(d, info, "url")));
            snprintf(b->title, sizeof(b->title), "%s", jstr(d, jget(d, info, "title")));
            Tab *tb = tab_by_id(b->tab_id);
            if (tb && b->title[0]) { wb_set_metadata(tb, b->title, NULL, NULL); if (tb == active_tab()) update_title(); }
            plat_invalidate();
        }
    } else if (!strcmp(method, "Target.targetCreated")) {
        int info = jget(d, params, "targetInfo");
        const char *opener = jstr(d, jget(d, info, "openerId"));
        if (opener[0] && !strcmp(jstr(d, jget(d, info, "type")), "page") && (b = wb_by_target(opener))) {
            Tab *tb = tab_by_id(b->tab_id);
            char url[4096];
            if (!wb_url(jstr(d, jget(d, info, "url")), url, sizeof(url))) snprintf(url, sizeof(url), "about:blank");
            wb_new_tab(url, tb ? tb->parent_id : 0, tb ? tb->browser_native : 0, jstr(d, jget(d, info, "targetId")));
        }
    } else if (!strcmp(method, "Target.targetDestroyed")) {
        if ((b = wb_by_target(jstr(d, jget(d, params, "targetId"))))) {
            b->ready = 0; b->session[0] = 0; b->owns_target = 0; b->started = 0; b->target[0] = 0;
            wb_failure(b, "This Chrome tab was closed. Press F5 to open it again.");
        }
    }
}
static void browser_cdp_lost(const char *why) {
    for (int i = 0; i < g_ntabs; i++) {
        WinBrowser *b = browser_for(g_tabs[i]);
        if (!b || !b->started) continue;
        b->ready = 0; b->started = 0; b->session[0] = b->target[0] = 0; b->casting = 0;
        g_tabs[i]->browser_target[0] = 0;
        wb_failure(b, why);
    }
}

/* ── tabs ── */
static WinBrowser *wb_create(Tab *tb) {
    WinBrowser *b = (WinBrowser *)calloc(1, sizeof(WinBrowser));
    if (!b) return NULL;
    b->tab_id = tb->id;
    b->serial = ++g_browser_serial;
    b->dirty = 1;
    if (tb->browser_target[0]) snprintf(b->adopt, sizeof(b->adopt), "%s", tb->browser_target);
    tb->browser = b;
    return b;
}
static void wb_texture_free(WinBrowser *b) {
    if (b->srv) ID3D11ShaderResourceView_Release(b->srv);
    if (b->tex) ID3D11Texture2D_Release(b->tex);
    b->srv = NULL; b->tex = NULL; b->tex_dev = NULL; b->tex_w = b->tex_h = 0;
}
static void mac_browser_destroy(Tab *tb) {
    WinBrowser *b = browser_for(tb);
    if (!b) return;
    tb->browser = NULL;
    b->disposed = 1;
    if (b->target[0] && b->owns_target) {
        SBuf p = {0};
        sb_printf(&p, "{\"targetId\":");
        sb_json(&p, b->target);
        sb_put(&p, "}", 1);
        cdp_send(NULL, "Target.closeTarget", p.p, NULL, NULL);
        free(p.p);
    }
    wb_free_blocks(b);
    wb_free_images(b);
    wb_inspect_free(b);
    free(b->node_grid);
    free(b->menu);
    free(b->frame_px);
    wb_texture_free(b);
    free(b);
}
static Tab *wb_new_tab(const char *address, int parent, int native, const char *target) {
    char url[4096];
    if (!wb_url(address, url, sizeof(url)) || g_ntabs >= MAX_TABS) return NULL;
    Tab *owner = tab_by_id(parent);
    if (!owner) owner = active_tab();
    if (owner && owner->kind == TK_BROWSER) owner = tab_by_id(owner->parent_id);
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) { free(tb); return NULL; }
    tb->id = g_next_id++;
    InitializeCriticalSection(&tb->lock);
    tb->kind = TK_BROWSER;
    tb->browser_native = !!native;
    tb->parent_id = owner ? owner->id : 0;
    tb->t.m_cursor_vis = 0;
    tb->born = GetTickCount();
    InterlockedExchange(&tb->exited, 0);
    wb_set_metadata(tb, NULL, NULL, url);
    if (target && *target) snprintf(tb->browser_target, sizeof(tb->browser_target), "%s", target);
    int at = g_ntabs;
    if (owner) for (int i = 0; i < g_ntabs; i++) if (g_tabs[i] == owner) {
        at = i + 1;
        while (at < g_ntabs && g_tabs[at]->parent_id == owner->id) at++;
        break;
    }
    memmove(g_tabs + at + 1, g_tabs + at, (size_t)(g_ntabs - at) * sizeof(Tab *));
    g_tabs[at] = tb;
    g_ntabs++;
    activate(at);
    return tb;
}
static void mac_browser_open_url(const char *url) { wb_new_tab(url, active_tab() ? active_tab()->id : 0, 0, NULL); }
static void mac_browser_open_welcome(void) { mac_browser_open_url("hardcore:welcome"); }

/* The active browser tab gets its page; every one shows or stops its screencast. */
static void mac_browser_sync(void) {
    Tab *active = active_tab();
    g_browser_native_drag = 0;
    for (int i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tb->kind != TK_BROWSER) continue;
        if ((tb == active || (g_grid_on && grid_index(tb->id) >= 0)) && !tb->browser && wb_create(tb))
            wb_load(browser_for(tb), tb->browser_url[0] ? tb->browser_url : "hardcore:welcome");
        WinBrowser *b = browser_for(tb);
        if (b) {
            wb_cast(b, !g_grid_on && tb == active && tb->browser_native && !chrome_preview_active());
            if (tb == active) { b->dirty = 1; wb_viewport(b); }
        }
    }
    plat_invalidate();
}
static void mac_browser_resize(Tab *tb) { WinBrowser *b = browser_for(tb); if (b) wb_viewport(b); }
static int mac_browser_page_rows(Tab *tb) {
    WinBrowser *b = browser_for(tb);
    if (!b || !b->inspect_open) return tb ? tb->t.rows : 0;
    return chrome_max(3, tb->t.rows - chrome_max(5, tb->t.rows / 3));
}

/* Grid mode: the blocks laid out into the tab's own cells, each frame they change. */
static void mac_browser_prepare(Tab *tb) {
    int preview = chrome_preview_active() && tb == display_tab();
    WinBrowser *b = browser_for(tb);
    if (!b) return;
    if (tb == active_tab()) wb_viewport(b);
    if (tb->browser_native && !preview) return;
    tab_layout();
    int cols = tb->t.cols, rows = preview ? tb->t.rows : mac_browser_page_rows(tb), x = sidebar_x() / CW, y = g_chrome.top;
    b->page_rows = rows;
    int height = g_chrome.bottom - y, side = g_tab_underlay == 1 ? 1 : 2;
    if (!b->dirty && b->last_cols == cols && b->last_rows == rows && b->last_x == x && b->last_y == y && b->last_height == height &&
        b->last_side == side && b->last_preview == preview) return;
    Cell *cells = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
    if (!cells) return;
    if (b->node_cols != cols || b->node_rows != rows) {
        free(b->node_grid);
        b->node_grid = (uint16_t *)calloc((size_t)cols * rows, sizeof(uint16_t));
        b->node_cols = cols; b->node_rows = rows;
    }
    for (int i = 0; i < b->block_count; i++)
        for (int j = 0; j < b->blocks[i].count; j++)
            if (b->blocks[i].cells[j].wflag & BROWSER_IMAGE) {
                Cell *c = b->blocks[i].cells + j;
                int id = (int)c->ch;
                if (id > 0 && id <= b->nshown) {
                    WbImage *image = b->shown[id - 1];
                    int w, h;
                    browser_image_size(image->source_width, image->source_height, CW / chrome_max(1, g_scale), CH / chrome_max(1, g_scale), &w, &h);
                    c->fg = (uint32_t)w; c->bg = (uint32_t)h;
                }
            }
    int scroll = b->scroll;
    b->total = browser_grid_layout_nodes(b->blocks, b->block_count, cells, b->node_grid, cols, rows, scroll, side, x, y, SB_COLS, height);
    int limit = chrome_max(0, b->total - rows);
    if (scroll > limit) {
        scroll = limit;
        if (!preview) b->scroll = scroll;
        b->total = browser_grid_layout_nodes(b->blocks, b->block_count, cells, b->node_grid, cols, rows, scroll, side, x, y, SB_COLS, height);
    }
    if (b->focused_action)
        for (int i = 0; i < cols * rows; i++) if (cells[i].link == b->focused_action && !(cells[i].wflag & BROWSER_IMAGE)) cells[i].attr |= A_INV;
    for (int row = 0; row < rows; row++) {
        memcpy(tb->t.row[0][row], cells + row * cols, (size_t)cols * sizeof(Cell));
        tb->t.rlen[0][row] = cols;
    }
    for (int row = rows; row < tb->t.rows; row++) { memset(tb->t.row[0][row], 0, (size_t)cols * sizeof(Cell)); tb->t.rlen[0][row] = 0; }
    tb->t.m_cursor_vis = 0;
    tb->t.view = 0;
    free(cells);
    b->dirty = 0; b->last_cols = cols; b->last_rows = rows; b->last_x = x; b->last_y = y; b->last_height = height; b->last_side = side; b->last_preview = preview;
}
static int mac_browser_ready(Tab *tb) { WinBrowser *b = browser_for(tb); return b && b->ready; }
static int mac_browser_cutie_generation(Tab *tb) { WinBrowser *b = browser_for(tb); return b ? b->cutie_generation : 0; }
static int mac_browser_cutie_project(Tab *tb, Cell *output, int cols, int rows, int scroll) {
    if (!output || cols < 4 || cols > 1000 || rows < 1 || rows > 500) return 0;
    WinBrowser *b = browser_for(tb);
    int offset = chrome_max(0, chrome_min(BROWSER_DOCUMENT_ROWS, scroll));
    int total = browser_grid_layout_nodes(b ? b->blocks : NULL, b ? b->block_count : 0, output, NULL, cols, rows, offset, 0, 0, 0, 0, 0);
    int limit = chrome_max(0, total - rows);
    if (offset > limit) browser_grid_layout_nodes(b ? b->blocks : NULL, b ? b->block_count : 0, output, NULL, cols, rows, limit, 0, 0, 0, 0, 0);
    /* The little grid has no image atlas: each image keeps its action as a quiet marker */
    for (int i = 0; i < cols * rows; i++)
        if (output[i].wflag & BROWSER_IMAGE) {
            int first = !output[i].attr;
            output[i].ch = first ? 0x25a7 : ' ';
            output[i].wflag = 0; output[i].attr = 0; output[i].fg = output[i].bg = 0;
        }
    return total;
}
static void mac_browser_scroll(int lines) {
    WinBrowser *b = browser_for(active_tab());
    if (!b) return;
    b->scroll = chrome_max(0, chrome_min(chrome_max(0, b->total - mac_browser_page_rows(active_tab())), b->scroll + lines));
    b->dirty = 1;
    g_sel_on = 0;
    plat_invalidate();
}
static void mac_browser_command(int command) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b) return;
    if (command == 0) { tb->browser_native = !tb->browser_native; b->dirty = 1; mac_browser_sync(); }
    else if (command == 1) wb_go(b, -1);
    else if (command == 2) wb_go(b, 1);
    else if (command == 3) {
        if (!strcmp(tb->browser_url, "hardcore:welcome") || !b->started || !b->ready) { b->started = b->ready = 0; b->session[0] = 0; wb_load(b, tb->browser_url[0] ? tb->browser_url : "hardcore:welcome"); }
        else wb_command(b, "Page.reload", NULL, NULL, NULL);
    }
    plat_invalidate();
}

/* ── links, anchors and the page's controls ── */
static int wb_anchor_row(WinBrowser *b, int anchor, int cols, int side, int x, int y, int width, int height) {
    if (anchor < 1 || anchor > 4000 || !b->anchors[anchor] || cols < 4) return -1;
    int block = b->anchors[anchor] - 1;
    if (block < 0 || block >= b->block_count) return -1;
    while (block > 0 && b->blocks[block].table && b->blocks[block - 1].table == b->blocks[block].table) block--;
    Cell *scratch = (Cell *)calloc((size_t)cols, sizeof(Cell));
    if (!scratch) return -1;
    int offset = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        int next = browser_grid_layout(b->blocks, block, scratch, cols, 1, offset, side, x, y, width, height);
        if (next == offset) break;
        offset = next;
    }
    free(scratch);
    return offset;
}
static int wb_follow_anchor(Tab *tb, WinBrowser *b, int anchor) {
    if (!anchor || tb->browser_native) return 0;
    tab_layout();
    int row = wb_anchor_row(b, anchor, tb->t.cols, g_tab_underlay == 1 ? 1 : 2, sidebar_x() / CW, g_chrome.top, SB_COLS, g_chrome.bottom - g_chrome.top);
    if (row < 0) return 0;
    b->scroll = row; b->focused_action = 0; b->dirty = 1; g_sel_on = 0;
    plat_invalidate();
    return 1;
}
static void wb_edit_set(WinBrowser *b, const char *utf8) {
    b->edit_len = 0;
    for (const char *p = utf8 ? utf8 : ""; *p && b->edit_len < 4095;) b->edit[b->edit_len++] = wb_utf8(&p);
}
static char *wb_edit_utf8(WinBrowser *b) {
    char *out = (char *)malloc((size_t)b->edit_len * 4 + 1);
    int n = 0;
    if (!out) return NULL;
    for (int i = 0; i < b->edit_len; i++) n += utf8_encode(b->edit[i], out + n);
    out[n] = 0;
    return out;
}
static void wb_begin_edit(WinBrowser *b, int kind, const char *value) {
    wb_edit_set(b, value);
    b->edit_kind = kind;
    b->edit_caret = b->edit_len;
    b->edit_all = 1;
    free(b->menu); b->menu = NULL; b->menu_kind = b->menu_count = 0;
    SetFocus(g_hwnd);
    plat_invalidate();
}
static void wb_activate_value(WinBrowser *b, int node, int epoch, const char *value) {
    SBuf p = {0};
    if (b->epoch != epoch) return;
    sb_printf(&p, "globalThis.hardcoreBrowser?.activate(%d,%d", node, b->generation);
    if (value) { sb_put(&p, ",", 1); sb_json(&p, value); }
    sb_put(&p, ")", 1);
    sb_put(&p, "", 1);
    if (p.p) wb_eval(b, p.p, EV_NONE, 0, 0);
    free(p.p);
}
static void wb_end_edit(WinBrowser *b) { b->edit_kind = 0; b->edit_all = 0; plat_invalidate(); }
static void wb_commit(WinBrowser *b) {
    char *value, url[4096];
    if (!b->edit_kind || !(value = wb_edit_utf8(b))) return;
    int kind = b->edit_kind;
    if (kind == 1) {
        if (!wb_url(value, url, sizeof(url))) { MessageBeep(MB_OK); free(value); return; }
        wb_end_edit(b);
        wb_load(b, url);
    } else {
        wb_end_edit(b);
        wb_activate_value(b, b->edit_node, b->edit_epoch, value);
    }
    free(value);
    mac_browser_sync();
}
static void wb_menu_set(WinBrowser *b, int kind, int count) {
    free(b->menu);
    b->menu = (WbItem *)calloc((size_t)chrome_max(1, count), sizeof(WbItem));
    b->menu_kind = b->menu ? kind : 0;
    b->menu_count = 0;
    b->menu_selected = 0;
}
static void wb_menu_add(WinBrowser *b, const char *label, const char *value, int op) {
    WbItem *it = &b->menu[b->menu_count++];
    snprintf(it->label, sizeof(it->label), "%s", label);
    snprintf(it->value, sizeof(it->value), "%s", value ? value : "");
    it->op = op;
}
static void wb_context_items(WinBrowser *b, int with_link) {
    wb_menu_set(b, 1, 7);
    if (!b->menu) return;
    if (with_link) { wb_menu_add(b, "Copy link location", NULL, 1); wb_menu_add(b, "Open in new browser tab", NULL, 2); }
    wb_menu_add(b, "Copy selection", NULL, 3);
    wb_menu_add(b, "Inspect element", NULL, 4);
    wb_menu_add(b, "Back", NULL, 5);
    wb_menu_add(b, "Reload", NULL, 6);
}
static void wb_begin_control(Tab *tb, WinBrowser *b, int node, int revision, int epoch, JDoc *d, int info) {
    const char *tag = jstr(d, jget(d, info, "tag")), *type = jstr(d, jget(d, info, "type"));
    if (!strcmp(tag, "SELECT")) {
        int options = jget(d, info, "options"), count = jlen(d, options);
        wb_menu_set(b, 2, count);
        if (!b->menu) return;
        b->menu_node = node; b->menu_revision = revision;
        b->menu_w = chrome_min(tb->t.cols, 36);
        b->menu_x = chrome_max(0, chrome_min(tb->t.cols - b->menu_w, g_mouse_x / CW));
        b->menu_y = chrome_max(0, chrome_min(tb->t.rows - 4, g_mouse_y / CH));
        for (int o = d->n[options].first; o >= 0 && b->menu_count < count; o = d->n[o].next) {
            const char *value = jstr(d, jget(d, o, "value"));
            if (!strcmp(value, jstr(d, jget(d, info, "value")))) b->menu_selected = b->menu_count;
            wb_menu_add(b, jstr(d, jget(d, o, "label")), value, 0);
        }
        plat_invalidate();
        return;
    }
    static const char *const not_text[] = {"checkbox", "radio", "button", "submit", "reset", "file", "image", "hidden"};
    int text = !strcmp(tag, "TEXTAREA");
    if (!strcmp(tag, "INPUT")) {
        text = 1;
        for (int i = 0; i < 8; i++) if (!strcmp(type, not_text[i])) text = 0;
    }
    if (text) {
        b->edit_node = node; b->edit_revision = revision; b->edit_epoch = epoch; b->edit_secret = !strcmp(type, "password");
        b->edit_w = chrome_min(tb->t.cols, chrome_max(16, chrome_min(50, tb->t.cols - 2)));
        b->edit_h = !strcmp(tag, "TEXTAREA") ? chrome_min(7, tb->t.rows) : 4;
        b->edit_x = chrome_max(0, chrome_min(tb->t.cols - b->edit_w, g_mouse_x / CW));
        b->edit_y = chrome_max(0, chrome_min(tb->t.rows - b->edit_h, g_mouse_y / CH));
        wb_begin_edit(b, !strcmp(tag, "TEXTAREA") ? 3 : 2, jstr(d, jget(d, info, "value")));
        return;
    }
    wb_activate_value(b, node, epoch, NULL);
}
static void wb_eval_done(WinBrowser *b, WbCtx *x, JDoc *d, int value) {
    Tab *tb = tab_by_id(b->tab_id);
    if (!tb) return;
    if (x->op == EV_PROJECT) {
        if (x->d2 != b->project_serial) return;
        b->requesting = 0;
        if (b->epoch == x->epoch && jtype(d, value) == JN_OBJ) wb_accept(b, d, value);
        if (b->changed_again) { b->changed_again = 0; wb_project(b); }
    } else if (x->op == EV_INFO_ACTION) {
        if (b->generation != x->b2 || b->epoch != x->epoch || jtype(d, value) != JN_OBJ) return;
        if (!wb_follow_anchor(tb, b, jint(d, jget(d, value, "target")))) wb_begin_control(tb, b, x->a, x->b2, x->epoch, d, value);
    } else if (x->op == EV_INFO_MENU) {
        if (b->generation != x->b2 || b->epoch != x->epoch || b->menu_kind != 1 || b->menu_node != x->a || jtype(d, value) != JN_OBJ) return;
        const char *url = jstr(d, jget(d, value, "href"));
        if (!url[0]) url = jstr(d, jget(d, value, "src"));
        if (url[0]) {
            int mx = b->menu_x, my = b->menu_y, mw = b->menu_w, node = b->menu_node, rev = b->menu_revision;
            snprintf(b->menu_url, sizeof(b->menu_url), "%s", url);
            wb_context_items(b, 1);
            b->menu_x = mx; b->menu_y = my; b->menu_w = mw; b->menu_node = node; b->menu_revision = rev;
        }
        plat_invalidate();
    } else if (x->op == EV_INSPECT) {
        if (b->generation != x->b2 || b->epoch != x->epoch || jtype(d, value) != JN_OBJ) return;
        int id = jint(d, jget(d, value, "id"));
        int keep = id == b->inspect_id ? b->inspect_scroll : 0;
        wb_inspect_free(b);
        for (int t = 0; t < 3; t++) {
            int cap = 64;
            WbLine *lines = (WbLine *)calloc((size_t)cap, sizeof(WbLine));
            int n = 0;
            char text[1200];
            #define WB_LINE(str, ident) do { if (n == cap) { WbLine *more = (WbLine *)realloc(lines, (size_t)(cap *= 2) * sizeof(WbLine)); if (!more) break; lines = more; } \
                                             lines[n].text = _strdup(str); lines[n++].id = (ident); } while (0)
            if (!lines) continue;
            snprintf(text, sizeof(text), "<%s>", jstr(d, jget(d, value, "tag")));
            WB_LINE(text, 0);
            if (t == 2) {
                const char *all = jstr(d, jget(d, value, "text"));
                while (*all) {
                    size_t k = strcspn(all, "\n");
                    snprintf(text, sizeof(text), "%.*s", (int)(k < 1000 ? k : 1000), all);
                    WB_LINE(text, 0);
                    all += k + (all[k] == '\n');
                }
            } else {
                int items = jget(d, value, t == 1 ? "style" : "attributes");
                for (int it = jtype(d, items) == JN_ARR ? d->n[items].first : -1; it >= 0; it = d->n[it].next) {
                    snprintf(text, sizeof(text), "  %s = %s", jstr(d, jget(d, it, "name")), jstr(d, jget(d, it, "value")));
                    WB_LINE(text, 0);
                }
                int kids = t == 0 ? jget(d, value, "children") : -1;
                for (int c = jtype(d, kids) == JN_ARR ? d->n[kids].first : -1; c >= 0; c = d->n[c].next) {
                    snprintf(text, sizeof(text), "  <%s> %s", jstr(d, jget(d, c, "tag")), jstr(d, jget(d, c, "text")));
                    WB_LINE(text, jint(d, jget(d, c, "id")));
                }
            }
            #undef WB_LINE
            b->insp[t] = lines;
            b->insp_n[t] = n;
        }
        b->inspect_id = id;
        b->inspect_parent = jint(d, jget(d, value, "parent"));
        b->inspect_revision = x->b2;
        b->inspect_open = 1;
        b->inspect_scroll = keep;
        b->dirty = 1;
        wb_viewport(b);
        plat_invalidate();
    } else if (x->op == EV_COPY) {
        const char *s = jstr(d, value);
        if (s[0]) set_clipboard_utf8(s, (int)strlen(s));
    }
}
static void wb_inspect_request(WinBrowser *b, int node) {
    char js[96];
    snprintf(js, sizeof(js), "globalThis.hardcoreBrowser?.inspect(%d,%d)", node, b->generation);
    wb_eval(b, js, EV_INSPECT, node, b->generation);
}
/* the inspector's lines on its current tab, with a note when the page has moved on */
static int wb_inspect_count(WinBrowser *b) { return b->insp_n[b->inspect_tab] ? b->insp_n[b->inspect_tab] + (b->inspect_revision != b->generation) : 1; }
static const char *wb_inspect_line(WinBrowser *b, int i, int *id) {
    int stale = b->inspect_revision != b->generation;
    *id = 0;
    if (!b->insp_n[b->inspect_tab]) return "Choose Inspect element from the page context menu.";
    if (stale && i == 1) return "Page changed; inspect again to refresh this node.";
    if (stale && i > 1) i--;
    if (i < 0 || i >= b->insp_n[b->inspect_tab]) return "";
    *id = b->insp[b->inspect_tab][i].id;
    return b->insp[b->inspect_tab][i].text ? b->insp[b->inspect_tab][i].text : "";
}
static void mac_browser_action(Tab *tb, int action) {
    WinBrowser *b = browser_for(tb);
    char js[96];
    if (!b || action < 1 || action > 4000) return;
    snprintf(js, sizeof(js), "globalThis.hardcoreBrowser?.info(%d,%d)", action, b->generation);
    wb_eval(b, js, EV_INFO_ACTION, action, b->generation);
}
static void mac_browser_cutie_action(Tab *tb, int action) {
    if (!tb || action < 1) return;
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i] == tb) { activate(i); break; }
    mac_browser_action(tb, action);
}
static void mac_browser_focus(int direction) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    static int ids[4001];
    unsigned char seen[4001] = {0};
    int count = 0;
    if (!b) return;
    mac_browser_prepare(tb);
    for (int y = 0; y < tb->t.rows; y++)
        for (int x = 0; x < tb->t.cols; x++) {
            int action = tb->t.row[0][y][x].link;
            if (action > 0 && action <= 4000 && !seen[action]) { seen[action] = 1; ids[count++] = action; }
        }
    if (!count) return;
    int at = direction < 0 ? 0 : -1;
    for (int i = 0; i < count; i++) if (ids[i] == b->focused_action) { at = i; break; }
    b->focused_action = ids[(at + direction + count) % count];
    b->dirty = 1;
    plat_invalidate();
}

/* ── the browser's own chrome, in character coordinates ── */
typedef struct { int back, forward, url, url_w, go, close, add, mode, controls_row, url_row, rows; } BrowserNav;
static BrowserNav browser_nav(void) {
    int w, h, cols, right;
    BrowserNav n;
    plat_client_size(&w, &h);
    cols = chrome_max(10, w / CW);
    right = cols - (3 * caption_btn_w() + CW - 1) / CW - 1;   /* the top line, left of the caption buttons */
    memset(&n, 0, sizeof(n));
    if (right < 70) { n.controls_row = 0; n.url_row = 1; }
    n.back = 0; n.forward = 3;
    n.close = right - 14; n.add = right - 11; n.mode = right - 8;
    n.url = right < 70 ? 1 : 6; n.go = right < 70 ? cols - 5 : right - 18;
    n.url_w = n.go - n.url - 1;
    n.rows = n.url_row + 1;
    return n;
}
static int mac_browser_nav_contains(int x, int y) {
    int w, h;
    if (g_grid_on || !browser_for(active_tab())) return 0;
    plat_client_size(&w, &h);
    return x >= 0 && x < w && y >= 0 && y < browser_nav().rows * CH;
}
static void mac_browser_nav_hover_at(int x, int y) {
    WinBrowser *b = browser_for(active_tab());
    int hover = !g_chrome_drag && g_tv_drag_slot < 0 && !over_chrome(x, y) && !g_sel_drag && !g_sb_drag && !g_browser_native_drag && mac_browser_nav_contains(x, y);
    if (!g_grid_on && b && b->edit_kind == 1 && !g_chrome_drag && g_tv_drag_slot < 0) hover = 1;
    if (hover == g_browser_nav_hover) return;
    g_browser_nav_hover = hover;
    plat_invalidate();
}
static void wb_text(const char *utf8, int x, int y, int width, uint32_t color) {
    wchar_t s[4096];
    if (width < 1 || y < 0) return;
    utf8_to_wide(utf8, s, 4096);
    draw_label(s, (float)(x * CW), (float)(y * CH), (float)(width * CW), color);
}
static void wb_box(int x, int y, int width, int height) {
    fill_rect((float)(x * CW), (float)(y * CH), (float)((x + width) * CW), (float)((y + height) * CH), BAR_BG, 1);
    wb_text("┌", x, y, 1, g_look.dim); wb_text("┐", x + width - 1, y, 1, g_look.dim);
    wb_text("└", x, y + height - 1, 1, g_look.dim); wb_text("┘", x + width - 1, y + height - 1, 1, g_look.dim);
    for (int i = 1; i < width - 1; i++) { wb_text("─", x + i, y, 1, g_look.dim); wb_text("─", x + i, y + height - 1, 1, g_look.dim); }
    for (int j = 1; j < height - 1; j++) { wb_text("│", x, y + j, 1, g_look.dim); wb_text("│", x + width - 1, y + j, 1, g_look.dim); }
}
static void wb_draw_editor(WinBrowser *b, int x, int y, int width, int height) {
    int caret_x = 0, caret_y = 0;
    for (int at = 0; at < b->edit_caret && at < b->edit_len; at++) {
        uint32_t cp = b->edit_secret ? 0x2022 : b->edit[at];
        int w = cp_width(cp);
        if (cp == '\n') { caret_x = 0; caret_y++; }
        else { if (height > 1 && caret_x + w > width) { caret_x = 0; caret_y++; } caret_x += w; }
    }
    int origin = height == 1 ? chrome_max(0, caret_x - width + 2) : chrome_max(0, caret_y - height + 1);
    b->edit_view = origin;
    if (b->edit_all) fill_rect((float)(x * CW), (float)(y * CH), (float)((x + width) * CW), (float)((y + height) * CH), SEL_BG, 1);
    int cx = 0, cy = 0;
    for (int at = 0; at < b->edit_len; at++) {
        uint32_t cp = b->edit_secret ? 0x2022 : b->edit[at];
        int w = cp_width(cp);
        if (cp == '\n') { cx = 0; cy++; continue; }
        if (height > 1 && cx + w > width) { cx = 0; cy++; }
        int dx = height == 1 ? cx - origin : cx, dy = height == 1 ? 0 : cy - origin;
        if (w > 0 && dx >= 0 && dx + w <= width && dy >= 0 && dy < height) {
            char one[8];
            one[utf8_encode(cp, one)] = 0;
            wb_text(one, x + dx, y + dy, w, DEF_FG);
        }
        cx += w;
    }
    int dx = height == 1 ? caret_x - origin : caret_x, dy = height == 1 ? 0 : caret_y - origin;
    if (dx >= width) { dx = 0; dy++; }
    dy = chrome_min(height - 1, chrome_max(0, dy));
    dx = chrome_min(width - 1, chrome_max(0, dx));
    fill_rect((float)((x + dx) * CW), (float)((y + dy) * CH), (float)((x + dx) * CW + 1), (float)((y + dy + 1) * CH), AMBER, 1);
}
static void mac_browser_render_nav_overlay(void) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b || !g_browser_nav_hover || g_grid_on || g_sel_drag || g_sb_drag || g_chrome_drag || g_tv_drag_slot >= 0 || chrome_preview_active()) return;
    int w, h, clip = g_clip_w;
    BrowserNav n = browser_nav();
    plat_client_size(&w, &h);
    g_clip_w = w;
    fill_rect(0, 0, (float)w, (float)(n.rows * CH), BAR_BG, .91f);
    g_clip_w = clip;
    wb_text("[←]", n.back, n.controls_row, 3, b->can_back ? DEF_FG : g_look.dim);
    wb_text("[→]", n.forward, n.controls_row, 3, b->can_forward ? DEF_FG : g_look.dim);
    wb_text("[×]", n.close, n.controls_row, 3, DEF_FG);
    wb_text("[+]", n.add, n.controls_row, 3, DEF_FG);
    wb_text(tb->browser_native ? "[Native]" : "[ Mono ]", n.mode, n.controls_row, 8, AMBER);
    wb_text("│", n.url - 1, n.url_row, 1, g_look.dim);
    wb_text("│", n.go - 1, n.url_row, 1, g_look.dim);
    if (b->edit_kind == 1) wb_draw_editor(b, n.url, n.url_row, n.url_w, 1);
    else wb_text(tb->browser_url, n.url, n.url_row, n.url_w, DEF_FG);
    wb_text("[Go]", n.go, n.url_row, 4, DEF_FG);
    fill_rect(0, (float)(n.rows * CH - 1), (float)(tb->t.cols * CW), (float)(n.rows * CH), g_look.dim, 1);
}
static int wb_menu_offset(WinBrowser *b, int capacity) { return capacity > 0 ? (b->menu_selected / capacity) * capacity : 0; }
static void mac_browser_draw_overlay(void) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (chrome_preview_active() || !b) return;
    if (b->inspect_open) {
        int top = mac_browser_page_rows(tb), height = tb->t.rows - top;
        fill_rect(0, (float)(top * CH), (float)(tb->t.cols * CW), (float)(tb->t.rows * CH), BAR_BG, 1);
        fill_rect(0, (float)(top * CH), (float)(tb->t.cols * CW), (float)(top * CH + 1), g_look.dim, 1);
        wb_text("[↑ parent]", 0, top, 10, DEF_FG);
        wb_text("[DOM]", 11, top, 6, b->inspect_tab == 0 ? AMBER : DEF_FG);
        wb_text("[Style]", 18, top, 8, b->inspect_tab == 1 ? AMBER : DEF_FG);
        wb_text("[Text]", 27, top, 7, b->inspect_tab == 2 ? AMBER : DEF_FG);
        wb_text("[×]", tb->t.cols - 3, top, 3, DEF_FG);
        for (int i = 0; i < height - 1 && i + b->inspect_scroll < wb_inspect_count(b); i++) {
            int id;
            const char *line = wb_inspect_line(b, i + b->inspect_scroll, &id);
            wb_text(line, 0, top + i + 1, tb->t.cols, id ? g_shown->pal[4] : DEF_FG);
        }
    }
    if (b->edit_kind > 1) {
        wb_box(b->edit_x, b->edit_y, b->edit_w, b->edit_h);
        wb_text(b->edit_kind == 3 ? " Ctrl+Enter applies " : " Enter applies ", b->edit_x + 1, b->edit_y, b->edit_w - 7, g_look.detail);
        wb_text("[OK]", b->edit_x + b->edit_w - 5, b->edit_y, 4, AMBER);
        wb_draw_editor(b, b->edit_x + 1, b->edit_y + 1, b->edit_w - 2, b->edit_h - 2);
    }
    if (b->menu_kind && b->menu) {
        int cap = chrome_max(1, chrome_min(10, tb->t.rows - b->menu_y - 2)), visible = chrome_min(cap, b->menu_count), offset = wb_menu_offset(b, cap);
        wb_box(b->menu_x, b->menu_y, b->menu_w, visible + 2);
        for (int i = 0; i < visible && offset + i < b->menu_count; i++) {
            int selected = offset + i == b->menu_selected;
            if (selected) fill_rect((float)((b->menu_x + 1) * CW), (float)((b->menu_y + i + 1) * CH), (float)((b->menu_x + b->menu_w - 1) * CW),
                                    (float)((b->menu_y + i + 2) * CH), TAB_HOVER, 1);
            wb_text(b->menu[offset + i].label, b->menu_x + 1, b->menu_y + i + 1, b->menu_w - 2, selected ? AMBER : DEF_FG);
        }
    }
}
static int mac_browser_hover_link(void) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (chrome_preview_active() || !b || tb->browser_native || g_browser_nav_hover || b->menu_kind || b->edit_kind || over_chrome(g_mouse_x, g_mouse_y)) return 0;
    int x = g_mouse_x / CW, y = g_mouse_y / CH;
    if (g_mouse_x < 0 || g_mouse_y < 0 || x >= tb->t.cols || y >= b->page_rows) return 0;
    return tb->t.row[0][y][x].link;
}
static void wb_image_palette(WbImage *image, int w, int h) {
    uint32_t colors[18];
    for (int i = 0; i < 16; i++) colors[i] = g_shown->pal[i];
    colors[16] = DEF_BG; colors[17] = DEF_FG;
    if (image->qw == w && image->qh == h && image->background == DEF_BG && !memcmp(colors, image->palette, sizeof(colors))) return;
    free(image->indexed);
    image->indexed = NULL;
    if (!image->rgba || !(image->indexed = (unsigned char *)malloc((size_t)w * h))) return;
    memcpy(image->palette, colors, sizeof(colors));
    image->background = DEF_BG; image->qw = w; image->qh = h;
    browser_image_quantize(image->rgba, image->sw, image->sh, image->indexed, w, h, colors, 18, DEF_BG);
}
static void mac_browser_draw_tile(Tab *tb, const Cell *cell, int x, int y) {
    WinBrowser *b = browser_for(tb);
    int id = cell->ch & 255;
    if (!b || id < 1 || id > b->nshown) return;
    WbImage *image = b->shown[id - 1];
    int width = (cell->ch >> 8) & 255, height = (cell->ch >> 16) & 255;
    if (width < 1 || width > BROWSER_IMAGE_COLUMNS || height < 1 || height > BROWSER_IMAGE_ROWS) return;
    int scale = chrome_max(1, g_scale), cw = CW / scale, ch = CH / scale;
    wb_image_palette(image, width * cw, height * ch);
    if (!image->indexed) {
        if (!cell->attr) wb_text(image->failed ? "[img]" : "[…]", x, y, chrome_min(width, 5), g_look.dim);
        return;
    }
    int tx = cell->attr & BROWSER_IMAGE_TILE_MASK, ty = cell->attr >> BROWSER_IMAGE_TILE_SHIFT;
    if (tx >= width || ty >= height) return;
    for (int py = 0; py < ch; py++) {
        int px = 0;
        while (px < cw) {
            unsigned char color = image->indexed[(ty * ch + py) * image->qw + tx * cw + px];
            int end = px + 1;
            while (end < cw && image->indexed[(ty * ch + py) * image->qw + tx * cw + end] == color) end++;
            fill_rect((float)(x * CW + px * scale), (float)(y * CH + py * scale), (float)(x * CW + end * scale), (float)(y * CH + (py + 1) * scale),
                      image->palette[color], 1);
            px = end;
        }
    }
}

/* ── native: Chrome's pixels, with our menus over them ── */
static int mac_browser_draw_native(Tab *tb, int w, int h) {
    WinBrowser *b = browser_for(tb);
    if (!b || !b->frame_px || !g_dev || !imgpipe_ready()) return 0;
    if (b->tex_dev != g_dev) wb_texture_free(b);
    if (!b->tex || b->tex_w != b->frame_w || b->tex_h != b->frame_h) {
        D3D11_TEXTURE2D_DESC td;
        wb_texture_free(b);
        memset(&td, 0, sizeof(td));
        td.Width = (UINT)b->frame_w; td.Height = (UINT)b->frame_h; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &b->tex))) return 0;
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)b->tex, NULL, &b->srv);
        b->tex_dev = g_dev; b->tex_w = b->frame_w; b->tex_h = b->frame_h; b->tex_seq = -1;
    }
    if (b->tex_seq != b->frame_seq) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b->tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            for (int y = 0; y < b->frame_h; y++)
                memcpy((unsigned char *)m.pData + (size_t)y * m.RowPitch, b->frame_px + (size_t)y * b->frame_w * 4, (size_t)b->frame_w * 4);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)b->tex, 0);
        }
        b->tex_seq = b->frame_seq;
    }
    /* the frame is the viewport at device pixels: drawn 1:1 from the top left */
    img_draw(b->srv, 0, 0, b->frame_w < w ? b->frame_w : w, b->frame_h < h ? b->frame_h : h, 0,
             b->frame_w < w ? 4096 : (int)(4096.0 * w / b->frame_w), b->frame_h < h ? 4096 : (int)(4096.0 * h / b->frame_h));
    return 1;
}
static int wb_cdp_mods(void) {
    return ((GetKeyState(VK_MENU) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 8 : 0);
}
/* type: mousePressed/mouseReleased/mouseMoved/mouseWheel; button: 0 left 1 middle 2 right, -1 none */
static void wb_native_mouse(WinBrowser *b, const char *type, int button, int mx, int my, int clicks, int wheel) {
    static const char *const names[] = {"left", "middle", "right"};
    static const int masks[] = {1, 4, 2};
    char p[300];
    double s = wb_scale();
    if (!b->ready) return;
    if (button >= 0 && !strcmp(type, "mousePressed")) b->buttons |= masks[button];
    if (button >= 0 && !strcmp(type, "mouseReleased")) b->buttons &= ~masks[button];
    if (wheel) snprintf(p, sizeof(p), "{\"type\":\"mouseWheel\",\"x\":%.1f,\"y\":%.1f,\"deltaX\":0,\"deltaY\":%d,\"modifiers\":%d}", mx / s, my / s, -wheel * 100, wb_cdp_mods());
    else snprintf(p, sizeof(p), "{\"type\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"button\":\"%s\",\"buttons\":%d,\"clickCount\":%d,\"modifiers\":%d}",
                  type, mx / s, my / s, button >= 0 ? names[button] : "none", b->buttons, clicks, wb_cdp_mods());
    wb_command(b, "Input.dispatchMouseEvent", p, NULL, NULL);
}
static int wb_key_name(WPARAM vk, const char **key, const char **code) {
    static const struct { WPARAM vk; const char *key, *code; } map[] = {
        {VK_RETURN, "Enter", "Enter"}, {VK_TAB, "Tab", "Tab"}, {VK_BACK, "Backspace", "Backspace"}, {VK_DELETE, "Delete", "Delete"},
        {VK_ESCAPE, "Escape", "Escape"}, {VK_LEFT, "ArrowLeft", "ArrowLeft"}, {VK_RIGHT, "ArrowRight", "ArrowRight"},
        {VK_UP, "ArrowUp", "ArrowUp"}, {VK_DOWN, "ArrowDown", "ArrowDown"}, {VK_HOME, "Home", "Home"}, {VK_END, "End", "End"},
        {VK_PRIOR, "PageUp", "PageUp"}, {VK_NEXT, "PageDown", "PageDown"}, {VK_INSERT, "Insert", "Insert"},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) if (map[i].vk == vk) { *key = map[i].key; *code = map[i].code; return 1; }
    return 0;
}
static void wb_native_key(WinBrowser *b, const char *type, WPARAM vk) {
    char p[300], letter[2] = {0}, code_buf[16];
    const char *key = "", *code = "";
    if (!wb_key_name(vk, &key, &code)) {
        if (vk >= 'A' && vk <= 'Z') { letter[0] = (char)(vk + 32); key = letter; snprintf(code_buf, sizeof(code_buf), "Key%c", (char)vk); code = code_buf; }
        else if (vk >= '0' && vk <= '9') { letter[0] = (char)vk; key = letter; snprintf(code_buf, sizeof(code_buf), "Digit%c", (char)vk); code = code_buf; }
    }
    snprintf(p, sizeof(p), "{\"type\":\"%s\",\"key\":\"%s\",\"code\":\"%s\",\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d,\"modifiers\":%d}",
             type, key, code, (int)vk, (int)vk, wb_cdp_mods());
    wb_command(b, "Input.dispatchKeyEvent", p, NULL, NULL);
}
static void wb_insert_text(WinBrowser *b, const char *utf8) {
    SBuf p = {0};
    sb_printf(&p, "{\"text\":");
    sb_json(&p, utf8);
    sb_put(&p, "}", 1);
    wb_command(b, "Input.insertText", p.p, NULL, NULL);
    free(p.p);
}
static char *wb_clipboard(void) {
    char *out = NULL;
    if (!OpenClipboard(g_hwnd)) return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w = h ? (const wchar_t *)GlobalLock(h) : NULL;
    if (w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
        if (n > 0 && (out = (char *)malloc((size_t)n))) WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
        GlobalUnlock(h);
    }
    CloseClipboard();
    return out;
}

/* ── the browser's share of input ── */
static int wb_edit_insert(WinBrowser *b, const char *utf8) {
    if (!b || !b->edit_kind) return 0;
    if (b->edit_all) { b->edit_len = b->edit_caret = 0; b->edit_all = 0; }
    for (const char *p = utf8; *p;) {
        uint32_t cp = wb_utf8(&p);
        if (cp == '\r') continue;
        if (cp == '\n' && b->edit_kind != 3) cp = ' ';
        if (cp < 32 && cp != '\n') continue;
        if (b->edit_len >= 4095) { MessageBeep(MB_OK); break; }
        memmove(b->edit + b->edit_caret + 1, b->edit + b->edit_caret, (size_t)(b->edit_len - b->edit_caret) * sizeof(uint32_t));
        b->edit[b->edit_caret++] = cp;
        b->edit_len++;
    }
    plat_invalidate();
    return 1;
}
static void wb_menu_choose(WinBrowser *b, int at) {
    if (!b->menu || at < 0 || at >= b->menu_count) return;
    WbItem item = b->menu[at];
    char url[4096];
    int kind = b->menu_kind, node = b->menu_node, rev = b->menu_revision;
    snprintf(url, sizeof(url), "%s", b->menu_url);
    free(b->menu); b->menu = NULL; b->menu_kind = b->menu_count = 0;
    if (kind == 2) { wb_activate_value(b, node, b->epoch, item.value); return; }
    Tab *tb = active_tab();
    switch (item.op) {
    case 1: set_clipboard_utf8(url, (int)strlen(url)); break;
    case 2: { char go[4096]; if (wb_url(url, go, sizeof(go))) wb_new_tab(go, tb ? tb->parent_id : 0, 0, NULL); break; }
    case 3:
        if (tb && tb->browser_native) wb_eval(b, "globalThis.getSelection()?.toString() || ''", EV_COPY, 0, 0);
        else sel_copy();
        break;
    case 4: if (rev == b->generation) wb_inspect_request(b, node); break;
    case 5: mac_browser_command(1); break;
    case 6: mac_browser_command(3); break;
    }
    plat_invalidate();
}
static void wb_context_at(WinBrowser *b, int mx, int my, int node, int link) {
    Tab *tb = active_tab();
    char js[96];
    wb_context_items(b, 0);
    if (!b->menu) return;
    b->menu_node = node; b->menu_revision = b->generation;
    b->menu_w = chrome_min(tb->t.cols, 28);
    b->menu_x = chrome_max(0, chrome_min(tb->t.cols - b->menu_w, mx / CW));
    b->menu_y = chrome_max(0, chrome_min(tb->t.rows - 8, my / CH));
    b->menu_url[0] = 0;
    snprintf(js, sizeof(js), "globalThis.hardcoreBrowser?.info(%d,%d)", link ? link : node, b->generation);
    if (node || link) wb_eval(b, js, EV_INFO_MENU, node, b->generation);
    plat_invalidate();
}
static void mac_browser_context(int mx, int my) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b) return;
    mac_browser_prepare(tb);
    int x = mx / CW, y = my / CH;
    if (x < 0 || x >= tb->t.cols || y < 0 || y >= mac_browser_page_rows(tb)) return;
    int node = b->node_grid && !tb->browser_native && x < b->node_cols && y < b->node_rows ? b->node_grid[y * b->node_cols + x] : 0;
    wb_context_at(b, mx, my, node, tb->browser_native ? 0 : tb->t.row[0][y][x].link);
}
static void mac_browser_context_native(WinBrowser *b, JDoc *d, int message) {
    POINT pt;
    int node = jint(d, jget(d, message, "node")), link = jint(d, jget(d, message, "link"));
    if (!active_tab()->browser_native || jint(d, jget(d, message, "generation")) != b->generation || node < 0 || node > 4000 || link < 0 || link > 4000) return;
    GetCursorPos(&pt);
    ScreenToClient(g_hwnd, &pt);
    wb_context_at(b, pt.x, pt.y, node, link);
}
static int mac_browser_ui_contains(int mx, int my) {
    WinBrowser *b = browser_for(active_tab());
    (void)mx;
    return b && (b->menu_kind || b->edit_kind > 1 || (b->inspect_open && my >= mac_browser_page_rows(active_tab()) * CH));
}
/* A left press: the menus, editors and nav bar first. 1 when taken. */
static int mac_browser_ui_mouse(int mx, int my) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b) return 0;
    int col = mx / CW, row = my / CH;
    if (b->menu_kind) {
        int cap = chrome_max(1, chrome_min(10, tb->t.rows - b->menu_y - 2)), offset = wb_menu_offset(b, cap);
        if (col >= b->menu_x && col < b->menu_x + b->menu_w && row > b->menu_y && row <= b->menu_y + cap) wb_menu_choose(b, offset + row - b->menu_y - 1);
        else { free(b->menu); b->menu = NULL; b->menu_kind = b->menu_count = 0; }
        plat_invalidate();
        return 1;
    }
    if (b->inspect_open && row >= mac_browser_page_rows(tb)) {
        int top = mac_browser_page_rows(tb);
        if (row == top) {
            if (col >= tb->t.cols - 3) { b->inspect_open = 0; b->dirty = 1; wb_viewport(b); }
            else if (col < 10) { if (b->inspect_revision == b->generation && b->insp_n[0]) wb_inspect_request(b, b->inspect_parent); }
            else { b->inspect_tab = col < 18 ? 0 : col < 27 ? 1 : 2; b->inspect_scroll = 0; }
        } else {
            int id;
            wb_inspect_line(b, row - top - 1 + b->inspect_scroll, &id);
            if (id && b->inspect_revision == b->generation) wb_inspect_request(b, id);
        }
        plat_invalidate();
        return 1;
    }
    if (g_browser_nav_hover) {
        BrowserNav n = browser_nav();
        if (row == n.controls_row) {
            if (col >= n.back && col < n.back + 3) { mac_browser_command(1); return 1; }
            if (col >= n.forward && col < n.forward + 3) { mac_browser_command(2); return 1; }
            if (col >= n.close && col < n.close + 3) { close_tab(g_active); return 1; }
            if (col >= n.add && col < n.add + 3) { mac_browser_open_welcome(); return 1; }
            if (col >= n.mode && col < n.mode + 8) { mac_browser_command(0); return 1; }
        }
        if (row == n.url_row && col >= n.url - 1 && col < n.go) {
            if (b->edit_kind == 1) { b->edit_caret = chrome_min(b->edit_len, b->edit_view + chrome_max(0, col - n.url)); b->edit_all = 0; plat_invalidate(); }
            else wb_begin_edit(b, 1, tb->browser_url);
            return 1;
        }
        if (row == n.url_row && col >= n.go && col < n.go + 4) { if (b->edit_kind == 1) wb_commit(b); else wb_load(b, tb->browser_url); return 1; }
        if (row < n.rows) return 1;
    }
    if (b->edit_kind && b->edit_kind != 1) {
        if (col >= b->edit_x && col < b->edit_x + b->edit_w && row >= b->edit_y && row < b->edit_y + b->edit_h) {
            if (row == b->edit_y && col >= b->edit_x + b->edit_w - 5) wb_commit(b);
            else if (row > b->edit_y && row < b->edit_y + b->edit_h - 1) {
                b->edit_caret = chrome_min(b->edit_len, chrome_max(0, b->edit_caret - (b->edit_w - 4)) + chrome_max(0, col - b->edit_x - 1));
                b->edit_all = 0;
                plat_invalidate();
            }
            return 1;
        }
        wb_end_edit(b);
    }
    if (b->edit_kind == 1 && !mac_browser_nav_contains(mx, my)) { wb_end_edit(b); mac_browser_nav_hover_at(mx, my); }
    return 0;
}
static int mac_browser_ui_scroll(int mx, int my, int lines) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    (void)mx;
    if (!b) return 0;
    if (b->inspect_open && my >= mac_browser_page_rows(tb) * CH) {
        b->inspect_scroll = chrome_max(0, chrome_min(chrome_max(0, wb_inspect_count(b) - (tb->t.rows - mac_browser_page_rows(tb) - 1)), b->inspect_scroll + lines));
        plat_invalidate();
        return 1;
    }
    if (!b->menu_kind) return 0;
    b->menu_selected = chrome_max(0, chrome_min(b->menu_count - 1, b->menu_selected + lines));
    plat_invalidate();
    return 1;
}
/* The pointer's shape over the browser's own parts; 1 when it set one. */
static int mac_browser_ui_move(int mx, int my) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b) return 0;
    int col = mx / CW, row = my / CH;
    if (b->menu_kind) {
        int cap = chrome_max(1, chrome_min(10, tb->t.rows - b->menu_y - 2)), offset = wb_menu_offset(b, cap), at = offset + row - b->menu_y - 1;
        if (col > b->menu_x && col < b->menu_x + b->menu_w - 1 && row > b->menu_y && row <= b->menu_y + cap && at >= 0 && at < b->menu_count && at != b->menu_selected) {
            b->menu_selected = at;
            plat_invalidate();
        }
        win_cursor(IDC_ARROW);
        return 1;
    }
    if (g_browser_nav_hover && mac_browser_nav_contains(mx, my)) {
        BrowserNav n = browser_nav();
        if (row == n.url_row && col >= n.url && col < n.go - 1) win_cursor(IDC_IBEAM);
        else if ((row == n.controls_row && ((col >= n.back && col < n.forward + 3) || col >= n.close)) || (row == n.url_row && col >= n.go && col < n.go + 4)) win_cursor(IDC_HAND);
        else win_cursor(IDC_ARROW);
        return 1;
    }
    if (b->edit_kind > 1 && col >= b->edit_x && col < b->edit_x + b->edit_w && row >= b->edit_y && row < b->edit_y + b->edit_h) { win_cursor(IDC_IBEAM); return 1; }
    if (b->inspect_open && row >= mac_browser_page_rows(tb)) { win_cursor(IDC_ARROW); return 1; }
    if (!g_grid_on && tb->browser_native && !over_chrome(mx, my)) { win_cursor(IDC_ARROW); return 1; }
    if (!tb->browser_native && mac_browser_hover_link()) { win_cursor(IDC_HAND); return 1; }
    if (!tb->browser_native) { win_cursor(IDC_ARROW); return 1; }
    return 0;
}
/* A key for the active browser tab; 1 when it took it. */
static int mac_browser_key(WPARAM vk, int ctrl, int shift, int alt) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b || g_cutie_focus_id) return 0;
    if (ctrl && !alt && (vk == 'L')) { wb_begin_edit(b, 1, tb->browser_url); g_browser_nav_hover = 1; return 1; }
    if (alt && !ctrl && vk == 'D') { wb_begin_edit(b, 1, tb->browser_url); g_browser_nav_hover = 1; return 1; }
    if (vk == VK_F5 || (ctrl && !alt && vk == 'R')) { mac_browser_command(3); return 1; }
    if (alt && !ctrl && (vk == VK_LEFT || vk == VK_RIGHT)) { mac_browser_command(vk == VK_LEFT ? 1 : 2); return 1; }
    if (b->menu_kind) {
        if (vk == VK_ESCAPE) { free(b->menu); b->menu = NULL; b->menu_kind = b->menu_count = 0; }
        else if (vk == VK_UP) b->menu_selected = chrome_max(0, b->menu_selected - 1);
        else if (vk == VK_DOWN) b->menu_selected = chrome_min(b->menu_count - 1, b->menu_selected + 1);
        else if (vk == VK_RETURN) wb_menu_choose(b, b->menu_selected);
        plat_invalidate();
        return 1;
    }
    if (b->edit_kind) {
        if (ctrl) {
            if (vk == 'A') b->edit_all = 1;
            else if (vk == 'V') { char *s = wb_clipboard(); if (s) { wb_edit_insert(b, s); free(s); } }
            else if (vk == 'C' && b->edit_all && !b->edit_secret) { char *s = wb_edit_utf8(b); if (s) { set_clipboard_utf8(s, (int)strlen(s)); free(s); } }
            else if (vk == VK_RETURN) wb_commit(b);
            else return 1;
            plat_invalidate();
            return 1;
        }
        if (vk == VK_ESCAPE) wb_end_edit(b);
        else if (vk == VK_RETURN) { if (b->edit_kind == 3 && !shift) wb_edit_insert(b, "\n"); else wb_commit(b); }
        else if (vk == VK_LEFT) { b->edit_caret = b->edit_all ? 0 : chrome_max(0, b->edit_caret - 1); b->edit_all = 0; }
        else if (vk == VK_RIGHT) { b->edit_caret = b->edit_all ? b->edit_len : chrome_min(b->edit_len, b->edit_caret + 1); b->edit_all = 0; }
        else if (vk == VK_HOME) { b->edit_caret = 0; b->edit_all = 0; }
        else if (vk == VK_END) { b->edit_caret = b->edit_len; b->edit_all = 0; }
        else if (vk == VK_BACK || vk == VK_DELETE) {
            if (b->edit_all) { b->edit_len = b->edit_caret = 0; b->edit_all = 0; }
            else if (vk == VK_BACK && b->edit_caret > 0) {
                memmove(b->edit + b->edit_caret - 1, b->edit + b->edit_caret, (size_t)(b->edit_len - b->edit_caret) * sizeof(uint32_t));
                b->edit_caret--; b->edit_len--;
            } else if (vk == VK_DELETE && b->edit_caret < b->edit_len) {
                memmove(b->edit + b->edit_caret, b->edit + b->edit_caret + 1, (size_t)(b->edit_len - b->edit_caret - 1) * sizeof(uint32_t));
                b->edit_len--;
            }
        } else return 1;           /* typing arrives as WM_CHAR */
        plat_invalidate();
        return 1;
    }
    if (g_grid_on) return 0;
    if (tb->browser_native) {
        if (ctrl && !alt && vk == 'C') { wb_eval(b, "(()=>{const e=document.activeElement;return e && /^(INPUT|TEXTAREA)$/.test(e.tagName) && e.type!=='password' ? e.value.slice(e.selectionStart||0,e.selectionEnd||0) : globalThis.getSelection()?.toString() || ''})()", EV_COPY, 0, 0); return 1; }
        if (ctrl && !alt && vk == 'V') { char *s = wb_clipboard(); if (s) { wb_insert_text(b, s); free(s); } return 1; }
        if (ctrl && !alt && vk == 'A') { wb_eval(b, "(()=>{const e=document.activeElement;if(e && /^(INPUT|TEXTAREA)$/.test(e.tagName)){e.select();return;}const s=getSelection();s.removeAllRanges();const r=document.createRange();r.selectNodeContents(document.body);s.addRange(r);})()", EV_NONE, 0, 0); return 1; }
        wb_native_key(b, "rawKeyDown", vk);
        if (vk == VK_RETURN) wb_command(b, "Input.dispatchKeyEvent", "{\"type\":\"char\",\"text\":\"\\r\",\"unmodifiedText\":\"\\r\",\"windowsVirtualKeyCode\":13}", NULL, NULL);
        return 1;
    }
    /* Mono: the keyboard scrolls and walks the links */
    switch (vk) {
    case VK_UP: mac_browser_scroll(-1); return 1;
    case VK_DOWN: mac_browser_scroll(1); return 1;
    case VK_PRIOR: mac_browser_scroll(1 - tb->t.rows); return 1;
    case VK_NEXT: case VK_SPACE: mac_browser_scroll(tb->t.rows - 1); return 1;
    case VK_HOME: mac_browser_scroll(-20000); return 1;
    case VK_END: mac_browser_scroll(20000); return 1;
    case VK_TAB: if (ctrl) return 0; mac_browser_focus(shift ? -1 : 1); return 1;
    case VK_RETURN: if (b->focused_action) mac_browser_action(tb, b->focused_action); return 1;
    case VK_BACK: if (!ctrl && !alt) { mac_browser_command(1); return 1; } return 0;
    }
    return !ctrl && !alt;            /* nothing else goes to a shell: there isn't one */
}
static int mac_browser_key_up(WPARAM vk) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b || g_cutie_focus_id || g_grid_on || !tb->browser_native || b->edit_kind || b->menu_kind) return 0;
    wb_native_key(b, "keyUp", vk);
    return 1;
}
/* Typed text for the active browser tab; 1 when it took it. */
static int mac_browser_char(uint32_t cp) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    char utf[8];
    if (!b || g_cutie_focus_id) return 0;
    if (cp < 32 || cp == 127) return 1;
    utf[utf8_encode(cp, utf)] = 0;
    if (b->edit_kind) { wb_edit_insert(b, utf); return 1; }
    if (b->menu_kind || g_grid_on) return 1;
    if (tb->browser_native) wb_insert_text(b, utf);
    return 1;
}
/* The window is going: our pages close, and if no page is left in Chrome
   (another window's), Chrome closes too. Replies are awaited briefly here. */
static int g_quit_waiting, g_quit_pages;
static void wb_quit_closed(JDoc *d, int result, const char *error, void *ctx) { (void)d; (void)result; (void)error; (void)ctx; g_quit_waiting--; }
static void wb_quit_targets(JDoc *d, int result, const char *error, void *ctx) {
    (void)error; (void)ctx;
    int infos = jget(d, result, "targetInfos");
    g_quit_pages = result < 0 ? 1 : 0;
    for (int t = jtype(d, infos) == JN_ARR ? d->n[infos].first : -1; t >= 0; t = d->n[t].next)
        if (!strcmp(jstr(d, jget(d, t, "type")), "page") && strcmp(jstr(d, jget(d, t, "url")), "about:blank")) g_quit_pages++;
    g_quit_waiting--;
}
static void wb_quit_pump(void) {
    DWORD t0 = GetTickCount();
    MSG m;
    while (g_quit_waiting > 0 && g_cdp.state == CDP_READY && GetTickCount() - t0 < 1500) {
        if (PeekMessageW(&m, g_hwnd, WM_APP_CDP, WM_APP_CDP, PM_REMOVE)) cdp_message(m.wParam, (char *)m.lParam);
        else MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_POSTMESSAGE);
    }
}
static void browser_quit(void) {
    if (g_cdp.state != CDP_READY) return;
    g_quit_waiting = 0;
    for (int i = 0; i < g_ntabs; i++) {
        WinBrowser *b = browser_for(g_tabs[i]);
        if (b && b->target[0] && b->owns_target) {
            SBuf p = {0};
            sb_printf(&p, "{\"targetId\":");
            sb_json(&p, b->target);
            sb_put(&p, "}", 1);
            if (cdp_send(NULL, "Target.closeTarget", p.p, wb_quit_closed, NULL)) g_quit_waiting++;
            free(p.p);
            b->owns_target = 0;
        }
    }
    wb_quit_pump();
    g_quit_waiting = 1;
    if (!cdp_send(NULL, "Target.getTargets", NULL, wb_quit_targets, NULL)) g_quit_waiting = 0;
    wb_quit_pump();
    if (!g_quit_waiting && !g_quit_pages) cdp_send(NULL, "Browser.close", NULL, NULL, NULL);
    cdp_close_socket();
    g_cdp.state = CDP_IDLE;
}
/* The mouse on a Native page: everything not over our own chrome goes to Chrome.
   kind: 0 press, 1 release, 2 move; button 0 left 1 middle 2 right, -1 none. 1 when it went. */
static int mac_browser_native_mouse(int kind, int button, int mx, int my, int clicks) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b || g_grid_on || !tb->browser_native || chrome_preview_active() || !b->ready) return 0;
    if (kind != 1 && !g_browser_native_drag &&
        (over_chrome(mx, my) || chrome_any_tv_contains(mx, my) || mac_browser_nav_contains(mx, my) || mac_browser_ui_contains(mx, my))) return 0;
    wb_native_mouse(b, kind == 0 ? "mousePressed" : kind == 1 ? "mouseReleased" : "mouseMoved", button, mx, my, clicks, 0);
    return 1;
}
static int mac_browser_native_wheel(int mx, int my, int notches) {
    Tab *tb = active_tab();
    WinBrowser *b = browser_for(tb);
    if (!b || g_grid_on || !tb->browser_native || chrome_preview_active() || !b->ready || over_chrome(mx, my)) return 0;
    wb_native_mouse(b, "mouseWheel", -1, mx, my, 0, notches);
    return 1;
}
static int mac_browser_active(void) { return !g_cutie_focus_id && browser_for(active_tab()) != NULL; }
/* A page from the session, or handed over by a reload: its Chrome page is made
   (or taken over) when the tab is first shown. Not activated, not inserted. */
static Tab *browser_tab_restore(const char *address, int native) {
    char url[4096];
    Tab *tb;
    if (!wb_url(address, url, sizeof(url)) || !(tb = (Tab *)calloc(1, sizeof(Tab)))) return NULL;
    if (!term_init(&tb->t, g_cols, g_rows)) { free(tb); return NULL; }
    tb->id = g_next_id++;
    InitializeCriticalSection(&tb->lock);
    tb->kind = TK_BROWSER;
    tb->browser_native = native > 0;
    tb->t.m_cursor_vis = 0;
    tb->born = GetTickCount();
    wb_set_metadata(tb, NULL, NULL, url);
    return tb;
}
