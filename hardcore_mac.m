/*
 * hardcore_mac.m — the macOS platform layer, #included at the end of
 * hardcore.c (one translation unit, built with clang -x objective-c).
 *
 * The shape dumbterm's Mac side has: a Cocoa window, forkpty for the shell,
 * the pasteboard for copy and paste. What hardcore.c does on Win32 with
 * ConPTY, D3D11, WM_* messages and the clipboard, this does with:
 *
 *   - forkpty: a real local pty; your login shell (zsh, bash or fish) with a
 *     few lines of hooks that report the folder (OSC 9;9) and mark prompts
 *     (OSC 133;A), the same two things rc.bash does under WSL. zsh and bash
 *     also keep a pre-warmed spare shell for the next new tab.
 *   - dispatch sources: output and process exit on the main queue; writes go
 *     straight into the (non-blocking) pty, and only what doesn't fit waits on
 *     a serial queue, so a big paste never blocks the window.
 *   - Metal on a CAMetalLayer (hardcore.metal, compiled at build time): the
 *     same vertex array and glyph atlas as the Direct3D path, one draw per
 *     frame. Frames are built on the main thread and drawn by a render
 *     thread, which is the one that waits for a drawable.
 *   - The standard traffic lights over the tab column instead of hardcore's
 *     own glass caption buttons.
 *
 * Mac keys follow Mac habits: Cmd+T / Cmd+W / Cmd+C / Cmd+V, Cmd+1…9,
 * Cmd+Shift+[ ], Cmd+J, Cmd+↑/↓. Left Option is Meta (ESC prefix, what Claude
 * Code and readline expect); right Option still types é, ü, @ and friends.
 *
 * Measured with bench/macbench (see bench/RESULTS-mac.md). HARDCORE_TRACE=1
 * logs startup phases, HARDCORE_LAT=1 each keypress's path to the display.
 */

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include "hardcore_camera_capture.m"
static HCCameraCapture *g_camera_capture;
static NSString *g_record_status;
static void mac_camera_toggle(void);
static int mac_camera_mouse(NSEvent *,int,int);
static int mac_camera_cursor(int,int);
static NSDictionary *mac_camera_metadata(void);
static NSDictionary *mac_camera_status(void);
static void mac_music_toggle(void);
static int mac_music_mouse(NSEvent *,int,int);
static int mac_music_cursor(int,int);
static void mac_music_cancel_press(void);
static void mac_music_play_pause(void);
static void mac_music_next(int);
static void mac_music_stop(void);
static NSDictionary *mac_music_panel_status(void);
static int mac_record_active(void);
static void mac_record_start(void);
static void mac_record_pause(void);
static void mac_record_stop(void);
static void mac_record_discard(void);
static void mac_record_library(void);
static void mac_record_file_viewer(NSString *);
static int mac_record_exports_active(void);
static void mac_record_upload(int,int,const unsigned char *);
static void mac_record_event(NSEvent *,int,int);
static void mac_record_hide_pointer(void);
#include <util.h>
#include <os/lock.h>
#include <fcntl.h>
#include <libproc.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <poll.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <math.h>

@class HCView;
static char g_mac_exe_path[MAX_PATH];
#ifndef HARDCORE_BUILD
#define HARDCORE_BUILD "unknown"
#endif
static NSWindow *g_win;
static HCView *g_view;
static int g_gfx_ready;          /* Metal device and shaders are up */
static int g_quitting;           /* skip the "programs are running" question: last tab closed */
static int g_paint_pending;      /* a frame is wanted (see plat_invalidate) */
static double uptime_ms(void);
static NSTimer *g_timers[8];
static int g_mac_title_hover;
static int g_mac_title_drag;
static int g_browser_native_drag;
static id g_mac_title_monitor;
static void mac_browser_address(void);
static void mac_browser_open_welcome(void);
static void mac_browser_command(int command);
static void mac_browser_action(Tab *tb, int action);
static void mac_browser_focus(int direction);
static void mac_browser_enter(void);
static void mac_browser_edit(SEL action, id sender);
static int mac_browser_title_height(void);
static int mac_browser_nav_editing(void);
static int mac_browser_page_rows(Tab *tb);
static void mac_browser_draw_nav(void);
static int mac_browser_ui_mouse(int mx,int my);
static int mac_browser_ui_contains(int mx,int my);
static int mac_browser_ui_scroll(int mx,int my,int lines);
static int mac_browser_ui_key(NSEvent *e);
static int mac_browser_ui_move(int mx,int my);
static int mac_browser_insert(NSString *text);
static void mac_browser_context(int mx,int my);
static int mac_browser_editing(void);
static int mac_browser_copy_edit(void);

static char g_mac_bin_dir[4096];
static const char *g_shell;      /* your login shell */

#include "hardcore_mac_dock.m"
#define HARDCORE_CAF_CHANGED() plat_invalidate()
#include "hardcore_mac_caf.m"

/* A Dock tile summarizes metadata, never terminal output or command arguments.
   Both the tab model and AppKit live on the main thread. Rendering is bounded
   independently of terminal frames, including during output floods/resizing. */
static int g_mac_dock_summary = 1, g_mac_dock_ready;
static HCDockSummaryView *g_mac_dock_view;
static double g_mac_dock_last_check = -1000;

static NSString *mac_dock_text(const char *text, NSString *fallback) {
    return hc_dock_label(text ? [NSString stringWithUTF8String:text] : nil, fallback);
}

/* Reuse the terminal's native bitmap lookup, including its Unicode fallback.
   The Dock scales these pixels to its tile; it never substitutes a system font.
   This callback is only used while drawing, not when polling unchanged tabs. */
static int mac_dock_glyph(NSString *fontKey, uint32_t cp, unsigned char *pixels, int stride) {
    const Font *font = font_by_key([fontKey length] ? [fontKey characterAtIndex:0] : 0);
    if (!font) font = FONTS;
    if (cp_width(cp) <= 0) return 0;
    if (cp == ' ') return font->w; /* blank cells aren't stored in fonts.bin */
    unsigned char bitmap[SLOT * SLOT] = {0};
    int width = glyph_bits(font, cp, bitmap);
    for (int y = 0; y < font->h; y++)
        memcpy(pixels + y * stride, bitmap + y * SLOT, (size_t)MIN(width, stride));
    return width;
}

static NSDictionary *mac_dock_appearance(void) {
    /* g_shown/g_look also follow hover previews, just like the window. g_font
       is the selected font; g_cf is temporary state used by chrome drawing. */
    return @{@"fontKey":[NSString stringWithFormat:@"%c", g_font->key],
             @"fontWidth":@(g_font->w), @"fontHeight":@(g_font->h),
             @"themeKey":[NSString stringWithFormat:@"%c", g_shown->key],
             @"bg":@(g_look.bg), @"fg":@(g_look.fg), @"accent":@(g_look.accent),
             @"bar":@(g_look.bar), @"hover":@(g_look.hover), @"dim":@(g_look.dim),
             @"red":@(g_shown->pal[9]), @"yellow":@(g_shown->pal[11]),
             @"green":@(g_shown->pal[10]), @"blue":@(g_shown->pal[12])};
}

static NSDictionary *mac_dock_summary(void) {
    Tab *tb = active_tab();
    NSString *folder = @"no tabs", *program = @"hardcore", *status = @"no tabs";
    if (tb && tb->kind == TK_BROWSER) {
        NSString *host = [[NSURL URLWithString:[NSString stringWithUTF8String:tb->browser_url] ?: @""] host];
        folder = mac_dock_text(tb->browser_site, host.length ? host : @"Browser");
        NSString *title = [[[NSString alloc] initWithBytes:tb->t.title
            length:wcslen(tb->t.title) * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding] autorelease];
        program = hc_dock_label(title, @"Web page");
        status = tb->busy ? @"loading" : tb->browser_native ? @"native web" : @"web";
    } else if (tb) {
        const char *dir = tb->t.cwd[0] ? tb->t.cwd : tb->start_dir;
        NSString *path = dir[0] ? [NSString stringWithUTF8String:dir] : nil;
        folder = mac_dock_text([[path lastPathComponent] UTF8String], @"~");
        program = mac_dock_text(tb->program, mac_dock_text(g_shell, @"shell").lastPathComponent);
        status = tb->program[0] ? @"running" : @"ready";
        if (tb->t.ps_state == PS_IDLE || tb->t.ps_state == PS_DONE) status = @"ready";
        if (tb->busy) status = @"busy";
        if (tb->attention || tb->t.ps_state == PS_BLOCKED) status = @"needs you";
        if (tb->t.ps_state == PS_ERROR) status = @"error";
        if (!strncmp(tb->run, "terminal:", 9)) status = @"bridge";
        if (tb->help) { program = @"help"; status = @"help"; }
        if (tb->exited) { program = @"exited"; status = @"exited"; }
    }
    return @{@"folder":folder, @"program":program, @"status":status, @"tabs":@(g_ntabs),
             @"appearance":mac_dock_appearance()};
}

static void mac_dock_update(int force) {
    if (!g_mac_dock_ready || !NSApp) return; /* early shell/title setup precedes AppKit */
    double now = uptime_ms();
    if (!force && now - g_mac_dock_last_check < 250) return;
    g_mac_dock_last_check = now;
    NSDockTile *tile = [NSApp dockTile];
    if (!g_mac_dock_summary) {
        if (g_mac_dock_view) {
            if ([tile contentView] == g_mac_dock_view) {
                [tile setContentView:nil]; /* AppKit resumes the installed application icon */
                [tile display];
            }
            [g_mac_dock_view release]; g_mac_dock_view = nil;
        }
        return;
    }
    BOOL installed = NO;
    hc_dock_read_glyph = mac_dock_glyph;
    if (!g_mac_dock_view) {
        g_mac_dock_view = [[HCDockSummaryView alloc] initWithFrame:NSMakeRect(0, 0, 128, 128)];
        [g_mac_dock_view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    }
    if ([tile contentView] != g_mac_dock_view) {
        [tile setContentView:g_mac_dock_view]; installed = YES;
    }
    if ([g_mac_dock_view setSummary:mac_dock_summary()] || installed) [tile display];
}

/* ── platform layer ─────────────────────────────────────────────────── */

static void plat_invalidate(void);

static void plat_client_size(int *w, int *h) {
    NSSize px;
    *w = *h = 0;
    if (!g_view) return;
    px = [(NSView *)g_view convertSizeToBacking:[(NSView *)g_view bounds].size];
    *w = (int)px.width;
    *h = (int)px.height;
}

static void plat_flash(void) {
    if (![NSApp isActive]) [NSApp requestUserAttention:NSInformationalRequest];   /* one Dock bounce */
}

static void make_spare(void);

static void timer_fired(int id) {
    if (id == TIMER_THROB) { update_busy(1); return; }
    plat_kill_timer(id);
    if (id == TIMER_SPARE) make_spare();
    else if (id == TIMER_SETTLE) settle_all();
    plat_invalidate();
}

static void plat_timer(int id, int ms) {
    plat_kill_timer(id);
    g_timers[id] = [[NSTimer scheduledTimerWithTimeInterval:ms / 1000.0 repeats:(id == TIMER_THROB)
                                                       block:^(NSTimer *tm) { (void)tm; timer_fired(id); }] retain];
    [[NSRunLoop currentRunLoop] addTimer:g_timers[id] forMode:NSEventTrackingRunLoopMode];   /* keep ticking during a live resize */
}

static void plat_kill_timer(int id) {
    if (!g_timers[id]) return;
    [g_timers[id] invalidate];
    [g_timers[id] release];
    g_timers[id] = nil;
}

static void plat_set_title(const wchar_t *title) {
    static wchar_t shown[300];
    NSString *s;
    if (!g_win) return;
    /* every output from the active tab asks; a title is only news when it changed */
    if (!wcsncmp(shown, title, 299) && wcslen(title) < 299) return;
    wcsncpy(shown, title, 299);
    shown[299] = 0;
    s = [[NSString alloc] initWithBytes:title length:wcslen(title) * sizeof(wchar_t) encoding:NSUTF32LittleEndianStringEncoding];
    [g_win setTitle:s ? s : @"hardcore"];
    [s release];
    mac_dock_update(0);
}

static void plat_close_window(void) {
    g_quitting = 1;
    [NSApp terminate:nil];
}

/* ── renderer: Metal on a CAMetalLayer ─────────────────────────────── */

/* The same vertex array and 16×16-slot atlas as the Direct3D path, drawn
   with one draw call per frame. Why Metal and not the OpenGL the first port
   used: an NSOpenGLView frame waits for AppKit's display cycle and goes
   through an extra surface copy, and GL is deprecated. Measured side by side
   (bench/RESULTS-mac.md): keypress → screen 22.6 ms p50 against 27.2. A
   CAMetalLayer frame is presented the moment it's encoded: two drawables,
   no vsync wait on our side, and the window server takes the newest one. */

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

static id<MTLDevice> g_mtl;
static id<MTLCommandQueue> g_queue;
static id<MTLRenderPipelineState> g_pipe;
static id<MTLTexture> g_atlas;
static CAMetalLayer *g_layer;
static int g_lat;                /* HARDCORE_LAT=1: per key, to stderr, when the echo arrived, the frame was
                                    committed, and it reached the display (Metal's presentedTime) */
static double g_lat_key, g_lat_out, g_lat_down, g_lat_written;
static void trace(const char *what);
static int g_tracing, g_frames, g_trace_commit;
static double g_trace_tab_t;     /* when the last Cmd+T came in */
static double uptime_ms(void);
static double g_t_exec;

/* The shader is hardcore.metal. make compiles it ahead of time when Xcode's
   metal compiler is there (build/hardcore.metallib), which saves ~175 ms of
   compiling on a cold start; its source is embedded too, and compiled at
   startup when there's no precompiled library. */
__asm__(".const_data\n.balign 16\n.global _metal_src\n_metal_src:\n.incbin \"hardcore.metal\"\n.byte 0\n"
#ifdef HAVE_METALLIB
        ".balign 16\n.global _metal_lib\n_metal_lib:\n.incbin \"build/hardcore.metallib\"\n.global _metal_lib_end\n_metal_lib_end:\n"
#endif
        ".text\n");
extern const char metal_src[];
#ifdef HAVE_METALLIB
extern const unsigned char metal_lib[], metal_lib_end[];
#endif

/* Device, shaders, atlas: no window needed, so this runs on a worker while
   the main thread builds the window and the first shell starts. */
static int mtl_init(void) {
    NSError *err = nil;
    id<MTLLibrary> lib;
    MTLRenderPipelineDescriptor *pd;
    MTLTextureDescriptor *td;
    g_mtl = MTLCreateSystemDefaultDevice();
    if (!g_mtl) return 0;
    g_queue = [g_mtl newCommandQueue];
#ifdef HAVE_METALLIB
    {
        dispatch_data_t d = dispatch_data_create(metal_lib, (size_t)(metal_lib_end - metal_lib), NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        lib = [g_mtl newLibraryWithData:d error:&err];
        dispatch_release(d);
    }
    if (!lib)
#endif
    lib = [g_mtl newLibraryWithSource:[NSString stringWithUTF8String:metal_src] options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "hardcore: shader: %s\n", [[err description] UTF8String]);
        return 0;
    }
    pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [[lib newFunctionWithName:@"vs"] autorelease];
    pd.fragmentFunction = [[lib newFunctionWithName:@"fs"] autorelease];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    pd.colorAttachments[0].blendingEnabled = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    g_pipe = [g_mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    [pd release];
    [lib release];
    if (!g_pipe) return 0;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:ATLAS height:ATLAS mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead;
    g_atlas = [g_mtl newTextureWithDescriptor:td];
    return g_atlas != nil;
}

/* Frames are built on the main thread (the vertex array: ~0.15 ms for a
   full screen) and drawn by a render thread. Waiting for a free drawable
   (up to a refresh, when one frame is already on its way to the display)
   happens there, so input and shell output never wait on the display. The
   newest frame wins: one the render thread hasn't started yet is replaced. */

typedef struct Frame {
    Inst *v;                     /* the frame's quads */
    int n, cap, w, h;
    int transparent;
    int snap;                    /* test channel: write this frame to snap_path */
    int handoff, painted;        /* receipt belongs only to this exact frame */
    uint64_t serial;             /* main-thread submission order */
    char snap_path[MAX_PATH];
    double lat_key, lat_out, lat_down, lat_written;   /* HARDCORE_LAT, for the key this frame answers */
    int trace_tab;               /* HARDCORE_TRACE: the new tab's first frame */
} Frame;

static void mac_record_frame(Frame *f);

static Frame *g_build;           /* being built by render() */
static Frame *g_pending;         /* built, waiting for the render thread */
static pthread_mutex_t g_fmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_draw_mx = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_frame_serial, g_last_draw_serial;

static void frame_free(Frame *f) {
    if (!f) return;
    free(f->v);
    free(f);
}

static void r_flush(void) {      /* the whole frame goes in one draw, so this only collects */
    Frame *f = g_build;
    if (!f || !g_ninst) { g_ninst = 0; return; }
    if (f->n + g_ninst > f->cap) {
        f->cap = (f->n + g_ninst) * 2;
        f->v = (Inst *)realloc(f->v, sizeof(Inst) * (size_t)f->cap);
    }
    memcpy(f->v + f->n, g_inst, sizeof(Inst) * (size_t)g_ninst);
    f->n += g_ninst;
    g_ninst = 0;
}

/* Glyph slots are written once, before the first frame that uses them is
   handed over, so the shared texture needs no fence. (After a full-atlas
   reset, the frame being built at that moment can show stale glyphs once.) */
static void r_upload(int x, int y, const unsigned char *bmp) {
    mac_record_upload(x,y,bmp);
    [g_atlas replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, SLOT, SLOT) mipmapLevel:0 withBytes:bmp bytesPerRow:SLOT];
}

static void r_begin(int w, int h) {
    g_ninst = 0;
    frame_free(g_build);
    g_build = (Frame *)calloc(1, sizeof(Frame));
    g_build->w = w;
    g_build->h = h;
    Tab *tb = active_tab();
    g_build->transparent = tb && tb->kind == TK_BROWSER && tb->browser_native && !chrome_preview_active();
}

static void write_bmp(const char *path, const void *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    uint32_t hdr[13];
    if (!f) return;
    memset(hdr, 0, sizeof(hdr));
    fwrite("BM", 1, 2, f);
    hdr[0] = 54 + (uint32_t)(w * h * 4); hdr[2] = 54;
    hdr[3] = 40; hdr[4] = (uint32_t)w; hdr[5] = (uint32_t)-h;   /* top-down */
    hdr[6] = 1 | (32 << 16);
    fwrite(hdr, 4, 13, f);
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

/* Encodes and presents on the render thread, including during live resize.
   Only explicit test snapshots draw synchronously on the main thread. */
static int g_handoff_paint, g_handoff_painted;

static void draw_frame_contents(Frame *f) {
    id<CAMetalDrawable> d = [g_layer nextDrawable];
    id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> enc;
    id<MTLBuffer> snap = nil;
    MTLRenderPassDescriptor *rp;
    int w, h;
    if (!d) return;
    w = (int)d.texture.width;
    h = (int)d.texture.height;
    cb = [g_queue commandBuffer];
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = d.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = f->transparent ? MTLClearColorMake(0,0,0,0) : MTLClearColorMake(((DEF_BG >> 16) & 255) / 255.0, ((DEF_BG >> 8) & 255) / 255.0, (DEF_BG & 255) / 255.0, 1);
    enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_pipe];
    [enc setFragmentTexture:g_atlas atIndex:0];
    if (f->n) {
        size_t bytes = sizeof(Inst) * (size_t)f->n;
        float inv[2] = {2.0f / (float)f->w, 2.0f / (float)f->h};
        if (bytes <= 4096) [enc setVertexBytes:f->v length:bytes atIndex:0];
        else {
            id<MTLBuffer> vb = [g_mtl newBufferWithBytes:f->v length:bytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            [enc setVertexBuffer:vb offset:0 atIndex:0];
            [vb release];        /* the command buffer keeps it until it's done */
        }
        [enc setVertexBytes:inv length:sizeof(inv) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:(NSUInteger)f->n];
    }
    [enc endEncoding];
    if (f->snap && w == f->w && h == f->h) {   /* test channel: copy this frame out before it's shown */
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        snap = [g_mtl newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
        [bl copyFromTexture:d.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1) toBuffer:snap destinationOffset:0
          destinationBytesPerRow:(NSUInteger)w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
        [bl endEncoding];
    }
    if (f->lat_key && f->lat_out) {
        double key = f->lat_key, out = f->lat_out, down = f->lat_down, written = f->lat_written;
        double commit = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
        [d addPresentedHandler:^(id<MTLDrawable> dd) {
            fprintf(stderr, "lat %.2f %.2f %.2f %.2f %.2f\n", out - key, commit - key,
                    dd.presentedTime > 0 ? dd.presentedTime * 1000.0 - key : -1.0, down - key, written - key);
        }];
    }
    if (f->trace_tab || (g_tracing && uptime_ms() - g_trace_tab_t < 100)) {
        double enc = uptime_ms();
        [d addPresentedHandler:^(id<MTLDrawable> dd) {
            fprintf(stderr, "%8.2f ms  frame after Cmd+T: encoded at %.2f, on screen at %.2f\n", enc - g_t_exec, enc - g_t_exec,
                    dd.presentedTime > 0 ? dd.presentedTime * 1000.0 - g_t_exec : -1.0);
        }];
    }
    if (g_tracing && g_frames < 3) {   /* when the first frames really reached the screen */
        int k = ++g_frames;
        [d addPresentedHandler:^(id<MTLDrawable> dd) {
            if (dd.presentedTime > 0) fprintf(stderr, "%8.2f ms  frame %d on screen\n", dd.presentedTime * 1000.0 - g_t_exec, k);
        }];
    }
    [cb presentDrawable:d];
    [cb commit];
    if (f->handoff) {
        [cb waitUntilCompleted];
        f->painted = cb.status == MTLCommandBufferStatusCompleted && w == f->w && h == f->h;
    }
    if (snap) {
        [cb waitUntilCompleted];
        write_bmp(f->snap_path, [snap contents], w, h);
        [snap release];
    }
}

/* Synchronous receipts and worker frames share one presentation order. A
   worker may already hold an old frame when the main thread paints a receipt;
   it must never present that stale frame afterward or certify the receipt. */
static void draw_frame(Frame *f) {
    pthread_mutex_lock(&g_draw_mx);
    if (f->serial > g_last_draw_serial) {
        g_last_draw_serial = f->serial;
        draw_frame_contents(f);
    }
    pthread_mutex_unlock(&g_draw_mx);
}

static void r_present(void) {
    Frame *f;
    r_flush();
    if (!(f = g_build)) return;
    g_build = NULL;
    f->serial = ++g_frame_serial;
    mac_record_frame(f);
    f->handoff = g_handoff_paint;
    if (g_lat && g_lat_key && g_lat_out) {
        f->lat_key = g_lat_key;
        f->lat_out = g_lat_out;
        f->lat_down = g_lat_down;
        f->lat_written = g_lat_written;
        g_lat_key = g_lat_out = 0;
    }
    if (g_trace_commit && g_ntabs && active_tab()->t.nmarks) {
        g_trace_commit = 0;
        trace("new tab's first frame with its prompt, handed to the render thread");
        f->trace_tab = 1;
    }
    if (f->snap || f->handoff) {   /* snapshot or the first frame before acknowledging a handoff */
        @autoreleasepool { draw_frame(f); }
        if (f->handoff) g_handoff_painted = f->painted;
        frame_free(f);
        return;
    }
    pthread_mutex_lock(&g_fmx);
    frame_free(g_pending);       /* not started yet: the newer frame replaces it */
    g_pending = f;
    pthread_cond_signal(&g_fcv);
    pthread_mutex_unlock(&g_fmx);
}

static void *render_thread(void *unused) {
    (void)unused;
    for (;;) {
        Frame *f;
        pthread_mutex_lock(&g_fmx);
        while (!g_pending) pthread_cond_wait(&g_fcv, &g_fmx);
        f = g_pending;
        g_pending = NULL;
        pthread_mutex_unlock(&g_fmx);
        @autoreleasepool { draw_frame(f); }   /* nextDrawable may wait here: that's the point */
        frame_free(f);
    }
    return NULL;
}

static void render_thread_start(void) {
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    pthread_create(&t, &a, render_thread, NULL);
    pthread_attr_destroy(&a);
}

/* test channel: the next frame goes to a file, drawn synchronously */
static void snap_backbuffer(void) {
    if (!g_build) return;
    g_build->snap = 1;
    snprintf(g_build->snap_path, sizeof(g_build->snap_path), "%s", g_snap_path);
}

/* ── shell integration ─────────────────────────────────────────────── */

/* bash: login-shell startup files, then report $PWD and mark each prompt.
   Written on every launch to the config folder; your dotfiles never change. */
/* Login profiles can reorder PATH. Reassert this installation's CLI at the
   prompt, after the user's startup files, not only before exec'ing the shell. */
#define MAC_CLI_PATH "if [ -n \"$HARDCORE_BIN_DIR\" ] && [ \"${PATH%%:*}\" != \"$HARDCORE_BIN_DIR\" ]; then export PATH=\"$HARDCORE_BIN_DIR:$PATH\"; fi; "
static const char RC_BASH_MAC[] =
    "# written by hardcore on every launch; edits here are overwritten\n"
    "[ -r /etc/profile ] && . /etc/profile\n"
    "if [ -r ~/.bash_profile ]; then . ~/.bash_profile\n"
    "elif [ -r ~/.bash_login ]; then . ~/.bash_login\n"
    "elif [ -r ~/.profile ]; then . ~/.profile\n"
    "elif [ -r ~/.bashrc ]; then . ~/.bashrc\n"
    "fi\n"
    "# a pre-warmed spare tab: profile already loaded, wait (silently) to be told where to go\n"
    "if [ -n \"$HARDCORE_SPARE\" ]; then\n"
    "  unset HARDCORE_SPARE\n"
    "  IFS= read -rs __hardcore_dir && cd -- \"$__hardcore_dir\" 2>/dev/null\n"
    "  unset __hardcore_dir\n"
    "fi\n"
    "__hardcore_cwd() { " MAC_CLI_PATH "printf '\\033]9;9;%s\\033\\\\\\033]133;A\\033\\\\' \"$PWD\"; }\n"
    "# report the folder on every cd too, so \"cd x && claude\" labels the tab x right away\n"
    "if ! declare -F cd >/dev/null; then\n"
    "  cd() { builtin cd \"$@\" && printf '\\033]9;9;%s\\033\\\\' \"$PWD\"; }\n"
    "fi\n"
    "case \";${PROMPT_COMMAND};\" in *\";__hardcore_cwd;\"*) ;; *)\n"
    "  PROMPT_COMMAND=\"__hardcore_cwd${PROMPT_COMMAND:+;$PROMPT_COMMAND}\" ;;\n"
    "esac\n";

/* zsh: ZDOTDIR points here first. This .zshenv puts your ZDOTDIR back, so
   zsh goes on to read your own .zshenv, .zprofile, .zshrc and .zlogin, and
   adds two hooks for interactive shells. */
static const char ZSHENV[] =
    "# written by hardcore on every launch; edits here are overwritten\n"
    "if [[ -n ${HARDCORE_ZDOTDIR+x} ]]; then ZDOTDIR=$HARDCORE_ZDOTDIR; else unset ZDOTDIR; fi\n"
    "unset HARDCORE_ZDOTDIR\n"
    "[[ -r ${ZDOTDIR:-$HOME}/.zshenv ]] && builtin source ${ZDOTDIR:-$HOME}/.zshenv\n"
    "if [[ -o interactive ]]; then\n"
    "  __hardcore_cwd() { " MAC_CLI_PATH "builtin printf '\\e]9;9;%s\\e\\\\\\e]133;A\\e\\\\' \"$PWD\" }\n"
    "  __hardcore_dir() { builtin printf '\\e]9;9;%s\\e\\\\' \"$PWD\" }\n"
    "  typeset -ag precmd_functions chpwd_functions\n"
    "  precmd_functions+=(__hardcore_cwd)\n"
    "  chpwd_functions+=(__hardcore_dir)\n"
    "  # a pre-warmed spare tab: your startup files have all run by the first prompt;\n"
    "  # wait there (silently) to be told where to go, once\n"
    "  if [[ -n $HARDCORE_SPARE ]]; then\n"
    "    unset HARDCORE_SPARE\n"
    "    __hardcore_spare() {\n"
    "      precmd_functions=(${precmd_functions:#__hardcore_spare})\n"
    "      local d; IFS= builtin read -rs d && builtin cd -- \"$d\" 2>/dev/null\n"
    "    }\n"
    "    precmd_functions=(__hardcore_spare $precmd_functions)\n"
    "  fi\n"
    "fi\n";

/* fish: one --init-command */
static const char FISH_INIT[] =
    "function __hardcore_cwd --on-event fish_prompt; if test -n \"$HARDCORE_BIN_DIR\"; and test \"$PATH[1]\" != \"$HARDCORE_BIN_DIR\"; set -gx PATH $HARDCORE_BIN_DIR $PATH; end; printf '\\e]9;9;%s\\e\\\\\\e]133;A\\e\\\\' $PWD; end; "
    "function __hardcore_dir --on-variable PWD; printf '\\e]9;9;%s\\e\\\\' $PWD; end";

static void write_file(const char *dir, const char *name, const char *text) {
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if ((f = fopen(path, "wb"))) {
        fputs(text, f);
        fclose(f);
    }
}

static const char *shell_base(void) {
    const char *b = strrchr(g_shell, '/');
    return b ? b + 1 : g_shell;
}

static void write_rc(void) {
    char zdir[MAX_PATH];
    struct passwd *pw = getpwuid(getuid());
    const char *sh = getenv("HARDCORE_SHELL");   /* tests */
    if (!sh || !sh[0]) sh = pw && pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : getenv("SHELL");
    g_shell = strdup(sh && sh[0] ? sh : "/bin/zsh");
    write_file(g_cfg_dir, "rc.bash", RC_BASH_MAC);
    snprintf(zdir, sizeof(zdir), "%s/zsh", g_cfg_dir);
    mkdir(zdir, 0755);
    write_file(zdir, ".zshenv", ZSHENV);
}

/* ── tabs: forkpty + dispatch sources ──────────────────────────────── */

static void on_output(void);
static void on_tab_exited(int id, int code);

static int g_output_pending;

static void schedule_output(void) {   /* (from the reader queues too) */
    if (__atomic_exchange_n(&g_output_pending, 1, __ATOMIC_ACQ_REL)) return;
    dispatch_async(dispatch_get_main_queue(), ^{ __atomic_store_n(&g_output_pending, 0, __ATOMIC_RELEASE); on_output(); });
}

/* A keystroke goes straight into the pty (it's non-blocking). Whatever the
   pty can't take right now (a big paste into a busy program) goes to the tab's
   serial queue, which waits for room; later writes queue behind it so the
   order holds. The window never blocks on a child that isn't reading. */
static int mac_input_held(Tab *tb) {
    return tb && tb->claude_guard && !tb->claude_released && !tb->exited &&
           (DWORD)(GetTickCount() - tb->claude_guard_at) < 120000;
}

static void tab_write(Tab *tb, const void *s, int n) {
    char *copy;
    int fd = tb->fd, off = 0;
    if (!tb->wq || fd < 0 || n <= 0) return;   /* no shell (a --replay terminal, or it's gone) */
    if (tb->t.nmarks && (memchr(s, '\r', (size_t)n) || memchr(s, '\n', (size_t)n))) tb->t.had_activity = 1;
    tb->write_seq++;
    if (!__atomic_load_n(tb->wpending, __ATOMIC_ACQUIRE)) {
        while (off < n) {
            ssize_t w = write(fd, (const char *)s + off, (size_t)(n - off));
            if (w > 0) off += (int)w;
            else if (w < 0 && errno == EINTR) continue;
            else break;          /* EAGAIN: full; anything else: gone */
        }
        if (g_lat && !g_lat_written) g_lat_written = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
        if (off == n || (errno != EAGAIN && errno != EINTR)) return;
    }
    n -= off;
    copy = (char *)malloc((size_t)n);
    memcpy(copy, (const char *)s + off, (size_t)n);
    {
        int *pending = tb->wpending;    /* not the Tab's own memory: a closed tab is freed while this may still run */
        __atomic_add_fetch(pending, 1, __ATOMIC_ACQ_REL);
        dispatch_async((dispatch_queue_t)tb->wq, ^{
            int o = 0;
            while (o < n) {
                ssize_t w = write(fd, copy + o, (size_t)(n - o));
                if (w > 0) o += (int)w;
                else if (w < 0 && (errno == EINTR)) continue;
                else if (w < 0 && errno == EAGAIN) {
                    struct pollfd p = {fd, POLLOUT, 0};
                    if (poll(&p, 1, -1) < 0 || (p.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
                } else break;
            }
            free(copy);
            __atomic_sub_fetch(pending, 1, __ATOMIC_ACQ_REL);
        });
    }
}

static void tab_set_size(Tab *tb, int cols, int rows) {
    if (tb->kind == TK_BROWSER) { mac_browser_resize(tb); return; }
    struct winsize ws;
    if (!tb->wq || tb->fd < 0) return;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ws.ws_xpixel = (unsigned short)(cols * CW);
    ws.ws_ypixel = (unsigned short)(rows * CH);
    ioctl(tb->fd, TIOCSWINSZ, &ws);
}

/* Output is read on a queue of the tab's own, so the 1 KB reads a macOS pty
   hands out overlap parsing and painting on the main queue instead of taking
   turns with them (a cat of 19 MB of CJK spent half its time in reads). The
   reader appends to a PtyIn; the main queue takes it in tab_drain. The source
   is suspended while any of these hold: the buffer is full (the main queue
   is behind: don't take the whole flood into memory), the main queue is
   reading synchronously (read_some: the rest of that turn sees no reads, as
   when everything happened on the main queue), or the pty has ended. */
enum { PIN_FULL = 1, PIN_HELD = 2, PIN_EOF = 4 };
#define PIN_MAX (8 << 20)

typedef struct {
    os_unfair_lock lock;
    char *buf;
    int n, cap, stop, refs;      /* stop: PIN_* bits; the source is suspended while any is set */
    int fd;
    dispatch_source_t src;
    dispatch_queue_t q;
} PtyIn;

static void pin_release(PtyIn *p) {
    if (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(p->buf);
    dispatch_release(p->q);
    free(p);
}

/* Under the lock: sets or clears a reason to stop, suspending or resuming the source on the first / last. */
static void pin_stop(PtyIn *p, int bit, int on) {
    int was = p->stop;
    p->stop = on ? was | bit : was & ~bit;
    if (!was && p->stop) dispatch_suspend(p->src);
    else if (was && !p->stop) dispatch_resume(p->src);
}

#define PTY_CHUNK 1024
static double g_more_at;         /* when a read last came back full (written by the reader queues) */

/* On the reader queue: everything there is, up to 64 reads, into the buffer. */
static void pin_read(PtyIn *p) {
    char buf[65536];
    int k, got = 0;
    for (k = 0; k < 64; k++) {
        ssize_t n;
        os_unfair_lock_lock(&p->lock);
        if (p->n >= PIN_MAX) { pin_stop(p, PIN_FULL, 1); os_unfair_lock_unlock(&p->lock); break; }
        os_unfair_lock_unlock(&p->lock);
        n = read(p->fd, buf, sizeof(buf));
        if (n > 0) {
            os_unfair_lock_lock(&p->lock);
            if (p->n + (int)n > p->cap) {
                int cap = p->cap ? p->cap : 65536;
                char *more;
                while (cap < p->n + (int)n) cap *= 2;
                if ((more = (char *)realloc(p->buf, (size_t)cap))) { p->buf = more; p->cap = cap; }
            }
            if (p->n + (int)n <= p->cap) { memcpy(p->buf + p->n, buf, (size_t)n); p->n += (int)n; }
            os_unfair_lock_unlock(&p->lock);
            got = 1;
            if (n >= PTY_CHUNK) { double now = uptime_ms(); __atomic_store(&g_more_at, &now, __ATOMIC_RELAXED); }
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EINTR)) {   /* EIO: every copy of the slave side is closed */
            os_unfair_lock_lock(&p->lock);
            pin_stop(p, PIN_EOF, 1);
            os_unfair_lock_unlock(&p->lock);
            got = 1;
        }
        break;
    }
    if (got) schedule_output();
}

/* Main queue: moves what was read into tb->in. Returns 1 at the pty's end. */
static int pin_take(Tab *tb) {
    PtyIn *p = (PtyIn *)tb->pin;
    int eof;
    if (!p) return 0;
    os_unfair_lock_lock(&p->lock);
    if (p->n) {
        if (!tb->in_n) {         /* the usual case: just take the buffer */
            free(tb->in);
            tb->in = p->buf; tb->in_n = p->n; tb->in_cap = p->cap;
            p->buf = NULL; p->n = p->cap = 0;
        } else {
            if (tb->in_n + p->n > tb->in_cap) {
                int cap = tb->in_cap;
                while (cap < tb->in_n + p->n) cap *= 2;
                tb->in = (char *)realloc(tb->in, (size_t)cap);
                tb->in_cap = cap;
            }
            memcpy(tb->in + tb->in_n, p->buf, (size_t)p->n);
            tb->in_n += p->n;
            p->n = 0;
        }
    }
    if (p->stop & PIN_FULL) pin_stop(p, PIN_FULL, 0);
    eof = (p->stop & PIN_EOF) != 0;
    os_unfair_lock_unlock(&p->lock);
    return eof;
}

/* Stops reading; the fd closes once the writes already queued have gone out. */
static void stop_reading(Tab *tb) {
    int fd = tb->fd, *pending = tb->wpending;
    dispatch_queue_t wq = (dispatch_queue_t)tb->wq;
    if (fd < 0) return;
    if (tb->rsrc) {
        dispatch_source_t r = (dispatch_source_t)tb->rsrc;
        PtyIn *p = (PtyIn *)tb->pin;
        if (p) pin_take(tb);     /* whatever was read before stopping still gets parsed */
        /* the close (and the counter) go last on the write queue, behind any queued writes */
        dispatch_source_set_cancel_handler(r, ^{
            dispatch_async(wq, ^{ close(fd); free(pending); });
            if (p) pin_release(p);
        });
        dispatch_source_cancel(r);   /* (cancelled first: resuming can't let another read in) */
        if (p) {
            os_unfair_lock_lock(&p->lock);
            if (p->stop) { p->stop = 0; dispatch_resume(r); }   /* a suspended source never runs its cancel handler */
            os_unfair_lock_unlock(&p->lock);
            tb->pin = NULL;
        }
        dispatch_release(r);
        tb->rsrc = NULL;
    } else {
        dispatch_async(wq, ^{ close(fd); free(pending); });
    }
    tb->fd = -1;
}

/* A macOS pty hands out at most 1 KB per read, so anything bigger than a
   keystroke's echo (Claude Code's slash menu: 3.2 KB in four writes over
   2.3 ms, recorded) arrives in pieces. Everything that's there is read at
   once; and a read that came back full means more of the same redraw is on
   its way, so the paint waits a moment for it (see plat_invalidate) instead
   of showing half a menu for a frame.

   read_some is the main queue reading right now, for the moments that need
   everything the pty has said up to here (a handoff, a reload, the first
   prompt). The reader queue stays out of the way until this turn of the main
   queue is over, so nothing read after it can slip past whatever this turn
   does with the pty. */
static void read_some(Tab *tb) {
    PtyIn *p = (PtyIn *)tb->pin;
    if (tb->fd < 0) return;
    if (!p) return;
    __atomic_add_fetch(&p->refs, 1, __ATOMIC_ACQ_REL);
    dispatch_sync(p->q, ^{
        os_unfair_lock_lock(&p->lock);
        pin_stop(p, PIN_HELD, 1);
        os_unfair_lock_unlock(&p->lock);
        pin_read(p);
    });
    dispatch_async(dispatch_get_main_queue(), ^{   /* after this turn */
        os_unfair_lock_lock(&p->lock);
        if (p->stop & PIN_HELD) pin_stop(p, PIN_HELD, 0);
        os_unfair_lock_unlock(&p->lock);
        pin_release(p);
    });
    if (pin_take(tb)) stop_reading(tb);
}

static int tab_drain(Tab *tb) {
    if (pin_take(tb)) stop_reading(tb);
    char *data = tb->in;
    int n = tb->in_n;
    tb->in = NULL;
    tb->in_n = tb->in_cap = 0;
    if (n) {vt_feed(&tb->t, (unsigned char *)data, n);tb->output_generation++;}
    free(data);
    return n;
}

/* the environment every tab's shell starts with */
static void child_env(void) {
    static const char *drop[] = {"TERM_SESSION_ID", "ITERM_SESSION_ID", "ITERM_PROFILE", "LC_TERMINAL", "LC_TERMINAL_VERSION",
                                 "WT_SESSION", "VTE_VERSION", "TMUX", "TMUX_PANE", "STY",
                                 /* A launcher/agent's output policy is not this new terminal's
                                    capability. In particular NO_COLOR=1 hides TUI colors even
                                    with COLORTERM=truecolor. Shell startup files run afterward
                                    and may still set an intentional per-shell color preference. */
                                 "NO_COLOR", "FORCE_COLOR", "CLICOLOR", "CLICOLOR_FORCE",
                                 /* started from inside Claude Code: its session markers would make every
                                    claude in our tabs a "child session" (no transcripts, wrong session id) */
                                 "CLAUDECODE", "CLAUDE_PID", "CLAUDE_EFFORT", "AI_AGENT", "CLAUDE_CODE_ENTRYPOINT",
                                 "CLAUDE_CODE_SSE_PORT", "CLAUDE_CODE_CHILD_SESSION", "CLAUDE_CODE_EXECPATH",
                                 "CLAUDE_CODE_SESSION_ID", "CLAUDE_CODE_SESSION_ATTENDED", "CLAUDE_CODE_MESSAGING_SOCKET",
                                 "CLAUDE_CODE_MESSAGING_TOKEN", "OLDPWD",
                                 /* our own test knobs */
                                 "HARDCORE_CTL", "HARDCORE_NO_ACTIVATE", "HARDCORE_SHELL", "HARDCORE_MAC_ROLLBACK", "HARDCORE_TEST_CONFIG",
                                 "HARDCORE_HERE_SESSION", "HARDCORE_HERE_PID", "HARDCORE_HERE_TAB_PID", "HC_HERE_MEDIA_FD",
                                 "HARDCORE_BRIDGE_READY_FD", "HARDCORE_CAF_CHILD"};
    size_t i;
    for (i = 0; i < sizeof(drop) / sizeof(drop[0]); i++) unsetenv(drop[i]);
    const char *path = getenv("PATH");
    char cli_path[16384];
    snprintf(cli_path, sizeof(cli_path), "%s:%s", g_mac_bin_dir, path ? path : "/usr/bin:/bin");
    setenv("PATH", cli_path, 1);
    setenv("HARDCORE_BIN_DIR", g_mac_bin_dir, 1);
    /* 24-bit color (chalk drops to 256 colors without it), OSC 8 links */
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("TERM_PROGRAM", "hardcore", 1);
    { char pid[32]; snprintf(pid, sizeof(pid), "%d", getppid()); setenv("HARDCORE_WINDOW_PID", pid, 1); }
    setenv("TERM_PROGRAM_VERSION", HARDCORE_VERSION, 1);
    setenv("FORCE_HYPERLINK", "1", 1);
    setenv("BASH_SILENCE_DEPRECATION_WARNING", "1", 1);   /* macOS bash 3.2's "now zsh" nag */
    /* started from Finder there's no locale, and zsh and bash then mangle UTF-8 */
    if (!getenv("LANG") && !getenv("LC_ALL") && !getenv("LC_CTYPE")) setenv("LANG", "en_US.UTF-8", 1);
}

static void expand_home(const char *in, char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (in[0] == '~' && (in[1] == '/' || !in[1]) && home) snprintf(out, cap, "%s%s", home, in + 1);
    else snprintf(out, cap, "%s", in);
}

static int spare_ok(void) {
    const char *b = shell_base();
    return !strcmp(b, "zsh") || !strcmp(b, "bash");
}

/* in the child, after forkpty: start the shell in dir (a spare waits to be told) */
static void exec_shell(const char *dir, int spare, int tab_id) {
    char cwd[MAX_PATH], zdir[MAX_PATH], argv0[256], rc[MAX_PATH];
    const char *base = shell_base();
    expand_home(dir, cwd, sizeof(cwd));
    if (chdir(cwd) != 0 && (!getenv("HOME") || chdir(getenv("HOME")) != 0)) chdir("/");
    child_env();
    char window_id[32], tab_key[32];
    snprintf(window_id,sizeof(window_id),"%d",getppid());
    snprintf(tab_key,sizeof(tab_key),"%d",tab_id);
    setenv("HARDCORE_WINDOW_PID",window_id,1); setenv("HARDCORE_TAB_ID",tab_key,1);
    if (spare) setenv("HARDCORE_SPARE", "1", 1);
    signal(SIGPIPE, SIG_DFL);
    snprintf(argv0, sizeof(argv0), "-%s", base);   /* a login shell, like Terminal starts */
    if (!strcmp(base, "zsh")) {
        snprintf(zdir, sizeof(zdir), "%s/zsh", g_cfg_dir);
        if (getenv("ZDOTDIR")) setenv("HARDCORE_ZDOTDIR", getenv("ZDOTDIR"), 1);
        setenv("ZDOTDIR", zdir, 1);
        execl(g_shell, argv0, (char *)NULL);
    } else if (!strcmp(base, "bash")) {
        /* not a login shell: --rcfile only counts for a non-login one, and rc.bash reads the login files itself */
        snprintf(rc, sizeof(rc), "%s/rc.bash", g_cfg_dir);
        execl(g_shell, "bash", "--rcfile", rc, "-i", (char *)NULL);
    } else if (!strcmp(base, "fish")) {
        execl(g_shell, argv0, "--init-command", FISH_INIT, (char *)NULL);
    } else {
        execl(g_shell, argv0, (char *)NULL);
    }
    fprintf(stderr, "hardcore: can't run %s: %s\r\n", g_shell, strerror(errno));
    _exit(127);
}

static void tab_start_io(Tab *tb) {
    int fd = tb->fd, id = tb->id;
    pid_t pid = tb->pid;
    dispatch_source_t r, p;
    fcntl(fd, F_SETFD, FD_CLOEXEC);    /* later tabs' shells mustn't hold this one open */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    tb->fd = fd;
    tb->pid = pid;
    tb->born = GetTickCount();
    tb->wpending = (int *)calloc(1, sizeof(int));
    tb->wq = dispatch_queue_create("hardcore.tab.write",
                                   dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    {
        PtyIn *in = (PtyIn *)calloc(1, sizeof(PtyIn));
        in->lock = OS_UNFAIR_LOCK_INIT;
        in->fd = fd;
        in->refs = 1;            /* the source's: its cancel handler lets go */
        in->q = dispatch_queue_create("hardcore.tab.read",
                                      dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
        r = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, in->q);
        in->src = r;
        dispatch_source_set_event_handler(r, ^{ pin_read(in); });
        tb->pin = in;
        dispatch_resume(r);
        tb->rsrc = r;
        (void)id;
    }
    /* exit: catches the shell even while a background job keeps the pty open;
       holds only the pid and id, so it reaps tabs that were already closed too */
    p = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, (uintptr_t)pid, DISPATCH_PROC_EXIT, dispatch_get_main_queue());
    dispatch_source_set_event_handler(p, ^{
        int st = 0, code;
        pid_t waited = waitpid(pid, &st, WNOHANG);
        if (waited == 0) return;
        /* A transferred shell is no longer our child. The process source still
           reports its exit; waitpid then returns ECHILD. */
        code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        Tab *t = tab_by_id(id);
        if (!t && g_spare && g_spare->id == id) t = g_spare;
        if (t) t->psrc = NULL;
        dispatch_source_cancel(p);
        dispatch_release(p);
        on_tab_exited(id, code);
    });
    tb->psrc = p;
    dispatch_resume(p);
}
static int tab_spawn(Tab *tb) {
    struct winsize ws;
    pid_t pid;
    int fd;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)tb->t.cols;
    ws.ws_row = (unsigned short)tb->t.rows;
    ws.ws_xpixel = (unsigned short)(tb->t.cols * CW);
    ws.ws_ypixel = (unsigned short)(tb->t.rows * CH);
    pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "\x1b[31mhardcore: forkpty failed: %s\x1b[0m\r\n", strerror(errno));
        term_message(&tb->t, msg);
        return 0;
    }
    if (pid == 0) {
        if (!strncmp(tb->run, "claude-wait:", 12)) {
            child_env();
            execl("/usr/bin/env", "env", "python3", "-B", tb->hist, "wait", "--request", tb->run + 12, (char *)NULL);
            _exit(127);
        }
        if (!strncmp(tb->run, "terminal:", 9)) {
            child_env();
            execl(g_mac_exe_path, "hardcore", "--terminal-bridge", tb->run + 9, tb->hist, (char *)NULL);
            _exit(127);
        }
        if (!strcmp(tb->run,"files:")) {
            child_env();
            execl("/usr/bin/env","env","python3","-I","-B",tb->hist,tb->start_dir,(char *)NULL);
            _exit(127);
        }
        if (tb->run[0]) {   /* tmux slurp: argv only, never shell-evaluate a session name */
            child_env();
            signal(SIGPIPE, SIG_DFL);
            if (tb->hist[0]) execl("/usr/bin/env", "env", "tmux", "-S", tb->hist, "attach-session", "-t", tb->run, (char *)NULL);
            else execl("/usr/bin/env", "env", "tmux", "attach-session", "-t", tb->run, (char *)NULL);
            _exit(127);
        }
        exec_shell(tb->start_dir, tb->spare, tb->id);
    }
    tb->fd = fd;
    tb->pid = pid;
    tab_start_io(tb);
    return 1;
}

static Tab *tab_create(const char *dir, int spare) {
    Tab *tb = (Tab *)calloc(1, sizeof(Tab));
    if (!tb || !term_init(&tb->t, g_cols, g_rows)) {
        free(tb);
        return NULL;
    }
    tb->id = g_next_id++;
    tb->fd = -1;
    tb->spare = spare;
    snprintf(tb->start_dir, sizeof(tb->start_dir), "%s", dir && *dir ? dir : g_start_dir);
    if (tb->start_dir[0] == '/') set_cwd(&tb->t, tb->start_dir);
    tab_spawn(tb);
    return tb;
}

static void tab_destroy_async(Tab *tb) {
    mac_browser_destroy(tb);
    if (tb->pid > 0 && !tb->exited) {
        kill(-tb->pid, SIGHUP);  /* its process group; closing the pty hangs up the foreground job too */
        kill(tb->pid, SIGHUP);
    }
    stop_reading(tb);
    if (tb->wq) dispatch_release((dispatch_queue_t)tb->wq);
    free(tb->in);
    term_free(&tb->t);
    free(tb);
}

/* The program in the foreground of a tab's pty, if it isn't the shell itself
   (claude, vim, a build...). Returns 0 when it's just the shell at a prompt. */
static int tab_busy_program(Tab *tb, char *name, int cap) {
    pid_t fg;
    if (tb->fd < 0 || tb->exited) return 0;
    fg = tcgetpgrp(tb->fd);
    if (fg <= 0 || fg == tb->pid) return 0;
    if (proc_name(fg, name, (uint32_t)cap) <= 0) snprintf(name, cap, "a program");
    return 1;
}

/* Read argv without consulting titles. JS launchers often keep "node" as
   their process name; their script gives us "codex", "claude", or "grok". */
static NSString *mac_terminal_known_program(NSString *path);
static void mac_foreground_name(Tab *tb, char *out, int cap) {
    out[0] = 0;
    if (tb->fd < 0 || tb->exited) return;
    pid_t fg = tcgetpgrp(tb->fd);
    if (fg <= 0 || fg == tb->pid) return;
    if (proc_name(fg, out, (uint32_t)cap) <= 0) return;
    /* Native Claude's process name is its release number (e.g. 2.1.301).
       Use the same kernel executable-path classifier as source sessions. */
    char executable[PROC_PIDPATHINFO_MAXSIZE] = {0};
    if (proc_pidpath(fg, executable, sizeof(executable)) > 0) {
        NSString *known = mac_terminal_known_program([NSString stringWithUTF8String:executable] ?: @"");
        if (known) { snprintf(out, cap, "%s", known.UTF8String); return; }
    }
    if (strcmp(out,"node") && strcmp(out,"bun") && strcmp(out,"deno")) return;
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, fg};
    size_t n = 1024 * 1024;
    char *data = malloc(n);
    if (!data) return;
    if (!sysctl(mib,3,data,&n,NULL,0) && n > sizeof(int)) {
        int argc; memcpy(&argc,data,sizeof(argc));
        char *p = data+sizeof(int), *end = data+n;
        while (p < end && *p) p++; /* executable path */
        while (p < end && !*p) p++;
        for (int i=0;i<argc && p<end;i++) {
            char *next = memchr(p,0,(size_t)(end-p));
            if (!next) break;
            if (i && *p != '-') {
                const char *base = strrchr(p,'/'); base = base ? base+1 : p;
                if (!strncmp(base,"codex",5)) snprintf(out,cap,"codex");
                else if (!strncmp(base,"claude",6) || strstr(p,"claude-code")) snprintf(out,cap,"claude");
                else if (!strncmp(base,"grok",4) || strstr(p,"grok-cli")) snprintf(out,cap,"grok");
                break;
            }
            p = next+1;
        }
    }
    free(data);
}
static NSDictionary *mac_terminal_metadata(NSString *tty);
static NSDictionary *mac_tmux_metadata(NSString *session, NSString *socket);

/* External views own a local client PTY, but their labels belong to the source
   session. Do not let the helper's HOME/cwd or process name replace that source. */
static int mac_apply_session_metadata(Tab *tb, NSDictionary *metadata) {
    if (!metadata) return 0;
    int changed = 0;
    NSString *cwd = metadata[@"cwd"], *program = metadata[@"program"];
    if ([cwd isKindOfClass:[NSString class]] && [cwd hasPrefix:@"/"] &&
        [cwd lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < sizeof(tb->t.cwd) &&
        strcmp(tb->t.cwd, cwd.UTF8String)) {
        set_cwd(&tb->t, cwd.UTF8String);
        changed = 1;
    }
    if ([program isKindOfClass:[NSString class]]) {
        char name[sizeof(tb->program)];
        const char *utf8 = program.UTF8String ?: "";
        size_t length = MIN(strlen(utf8), sizeof(name) - 1);
        while (length && ((unsigned char)utf8[length] & 0xc0) == 0x80) length--;
        memcpy(name, utf8, length); name[length] = 0;
        if (name[0]) tb->t.had_activity = 1;
        if (strcmp(name, tb->program)) {
            snprintf(tb->program, sizeof(tb->program), "%s", name);
            changed = 1;
        }
    }
    return changed;
}

static void mac_refresh_programs(void) {
    int changed = 0;
    for (int i=0;i<g_ntabs;i++) {
        Tab *tb = g_tabs[i];
        if (tb->kind == TK_BROWSER) continue;
        if (tb->run[0] && !tb->exited) {
            if (!strncmp(tb->run, "terminal:", 9))
                changed |= mac_apply_session_metadata(tb, mac_terminal_metadata(@(tb->run + 9)));
            else if (strncmp(tb->run, "claude-wait:", 12))
                changed |= mac_apply_session_metadata(tb, mac_tmux_metadata(@(tb->run), tb->hist[0] ? @(tb->hist) : nil));
            else if (strcmp(tb->program, "claude")) {
                snprintf(tb->program, sizeof(tb->program), "claude");
                changed = 1;
            }
            continue;
        }
        char name[64]; mac_foreground_name(tb,name,sizeof(name));
        if (name[0]) tb->t.had_activity = 1;
        if (strcmp(name,tb->program)) {
            snprintf(tb->program,sizeof(tb->program),"%s",name);
            changed = 1;
        }
    }
    if (changed) { update_title(); plat_invalidate(); }
}

/* the folder a shell is in, from the kernel: for shells without our hooks */
static void tab_proc_cwd(Tab *tb, char *out, int cap) {
    struct proc_vnodepathinfo vi;
    out[0] = 0;
    if (tb->pid > 0 && proc_pidinfo(tb->pid, PROC_PIDVNODEPATHINFO, 0, &vi, sizeof(vi)) == (int)sizeof(vi))
        snprintf(out, cap, "%s", vi.pvi_cdir.vip_path);
}

/* ── output, exit ──────────────────────────────────────────────────── */

static void paint_view_now(void);

static void on_output(void) {
    /* Hidden tabs are parsed but only cause a frame when something the tab
       column shows changes (see WM_APP_OUTPUT in hardcore.c): 63 busy tabs
       were ~1800 identical frames a second on Windows. */
    int i, changed = 0;
    for (i = 0; i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tab_drain(tb) <= 0) continue;
        if(tb==display_tab() || cutie_slot_for(tb->id)>=0)changed=1;
        if (i == g_active) {
            changed = 1;
            if (g_lat && g_lat_key && !g_lat_out) g_lat_out = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
        } else if (!tb->activity) {
            tb->activity = 1;
            changed = 1;
        }
        if (tb->t.bell) {
            tb->t.bell = 0;
            if (!g_focused || i != g_active) {
                tb->attention = 1;
                changed = 1;
                if (!g_focused) plat_flash();
            }
        }
        if (tb->t.side) {
            tb->t.side = 0;
            changed = 1;
        }
    }
    update_busy(0);
    for (i = 0; g_tracing && i < g_ntabs; i++) {
        Tab *tb = g_tabs[i];
        if (tb->adopted && tb->t.nmarks && tb->t.cx && !tb->traced) {
            char m[80];
            tb->traced = 1;
            snprintf(m, sizeof(m), "spare adopted -> prompt printed: %u ms", GetTickCount() - tb->adopted);
            trace(m);
        }
    }
    if (g_tracing && changed) {
        static int out, prompt;
        if (!out++) trace("first shell output");
        if (!prompt && g_ntabs && g_tabs[0]->t.nmarks) { prompt = 1; trace("first prompt (OSC 133;A)"); }
    }
    if (changed) {
        update_title();
        plat_invalidate();
    }
}

static void replace_tab(int i) {
    Tab *old = g_tabs[i], *tb = tab_create(old->t.cwd[0] ? old->t.cwd : old->start_dir, 0);
    if (!tb) return;
    g_tabs[i] = tb;
    tab_destroy_async(old);
    activate(i);
}

/* A shell that died within 3 s with an error is a setup problem, not you
   typing exit: keep the tab and say so, instead of the window vanishing. */
static void on_tab_exited(int id, int code) {
    Tab *tb = tab_by_id(id);
    int i;
    char buf[300];
    if (!tb && g_spare && g_spare->id == id) {   /* the spare died waiting: drop it, the next tab makes another */
        tb = g_spare;
        g_spare = NULL;
        tb->exited = 1;
        tab_destroy_async(tb);
        return;
    }
    if (!tb) return;             /* already closed: this was just the reaping */
    if (tb->fd >= 0) {           /* whatever it said last */
        fcntl(tb->fd, F_SETFL, O_NONBLOCK);
        while (tb->fd >= 0) {
            int before = tb->in_n;
            read_some(tb);
            if (tb->in_n == before) break;
        }
        stop_reading(tb);
    }
    tab_drain(tb);
    tb->exited = 1;
    int claude_receiver = !strncmp(tb->run, "claude-wait:", 12);
    if (code != 0 && (claude_receiver || GetTickCount() - tb->born < 3000) && !tb->help) {
        tb->help = HELP_FAILED;
        tb->t.m_cursor_vis = 0;
        if (claude_receiver)
            snprintf(buf, sizeof(buf), "\r\n\x1b[2mClaude attachment ended (code %d).\x1b[0m\r\n"
                     "The conversation may still be available in claude agents.\r\n\r\n"
                     "  \x1b[38;2;255;166;13mEnter\x1b[0m  open a shell\r\n", code);
        else
            snprintf(buf, sizeof(buf), "\r\n\x1b[2m%s exited right away (code %d).\x1b[0m\r\n\r\n"
                                       "  \x1b[38;2;255;166;13mEnter\x1b[0m  try again\r\n", g_shell, code);
        term_message(&tb->t, buf);
        plat_invalidate();
        return;
    }
    for (i = 0; i < g_ntabs; i++)
        if (g_tabs[i] == tb) close_tab(i);
}

/* ── config ────────────────────────────────────────────────────────── */

static void config_load(void) {
    char path[MAX_PATH], line[1200];
    const char *home = getenv("HOME");
    FILE *f;
    if (getenv("HARDCORE_TEST_CONFIG")) snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s", getenv("HARDCORE_TEST_CONFIG"));
    else snprintf(g_cfg_dir, sizeof(g_cfg_dir), "%s/Library/Application Support/hardcore", home ? home : "/tmp");
    mkdir(g_cfg_dir, 0755);
    snprintf(path, sizeof(path), "%s/config.ini", g_cfg_dir);
    if (!(f = fopen(path, "rb"))) return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        size_t n;
        if (!v || line[0] == '#') continue;
        *v++ = 0;
        n = strlen(v);
        while (n && (v[n - 1] == '\n' || v[n - 1] == '\r' || v[n - 1] == ' ')) v[--n] = 0;
        if (!strcmp(line, "dir") && *v) expand_home(v, g_start_dir, sizeof(g_start_dir));
        else if (!strcmp(line, "last")) snprintf(g_last_dir, sizeof(g_last_dir), "%s", v);
        else if (!strcmp(line, "dock_summary")) g_mac_dock_summary = strcmp(v, "0") != 0;
        else config_look(line, v);
    }
    fclose(f);
}

static void config_save(void) {
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof(path), "%s/config.ini", g_cfg_dir);
    if (!(f = fopen(path, "wb"))) return;
    fprintf(f, "# hardcore settings\n");
    fprintf(f, "# starting folder for new windows; empty = your home folder\n");
    fprintf(f, "dir=%s\n", g_cfg_dir_line);
    fprintf(f, "# the folder the last window closed in; new windows open there (empty: use dir=)\n");
    fprintf(f, "last=%s\n", g_last_dir);
    fprintf(f, "# macOS Dock icon: 1=active folder/program summary, 0=standard icon\ndock_summary=%d\n", g_mac_dock_summary);
    config_look_save(f);
    fclose(f);
}

/* ── settings: every window is its own process, so a pick goes to config.ini
   and a note to the others, which read it back ───────────────────────── */

#define SETTINGS_NOTE @"com.williamsharkey.hardcore.settings"

static void plat_settings_tell(void) {
    if (getenv("HARDCORE_TEST_CONFIG")) return;
    [[NSDistributedNotificationCenter defaultCenter] postNotificationName:SETTINGS_NOTE object:nil userInfo:nil deliverImmediately:YES];
}

/* the window's background shows while resizing, and the title bar's buttons follow light or dark */
static void plat_settings_shown(void) {
    mac_dock_update(1);
    if (!g_win) return;
    [g_win setBackgroundColor:[NSColor colorWithSRGBRed:((DEF_BG >> 16) & 255) / 255.0 green:((DEF_BG >> 8) & 255) / 255.0
                                                   blue:(DEF_BG & 255) / 255.0 alpha:1]];
    [g_win setAppearance:[NSAppearance appearanceNamed:g_shown->dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua]];
    [g_win invalidateCursorRectsForView:[g_win contentView]];
    plat_invalidate();
}

/* Native buttons float above a full-size content view. Keep them hidden until
   the whole title band is hovered; their quiet counterparts are Metal circles
   below the terminal. No title geometry consumes terminal grid rows. */
static const NSWindowButton mac_title_buttons[] = {
    NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton
};

static int mac_title_native_height(void) {
    NSRect layout;
    if (!g_win || !g_view || ([g_win styleMask] & NSWindowStyleMaskFullScreen)) return 0;
    layout = [(NSView *)g_view convertRect:[g_win contentLayoutRect] fromView:nil];
    /* AppKit may not have laid out the titlebar yet during initial creation. */
    CGFloat height = NSMinY(layout);
    if (height < 1) height = 22;
    return (int)(height * [g_win backingScaleFactor] + .5);
}

static int mac_title_height(void) { return MAX(mac_title_native_height(),mac_browser_title_height()); }

static int mac_title_contains(int x, int y) {
    int w, h;
    plat_client_size(&w, &h);
    return x >= 0 && x < w && y >= 0 && y < mac_title_height();
}

static void mac_title_sync_buttons(void) {
    /* Full screen retains AppKit's own auto-hiding titlebar behavior. */
    BOOL visible = g_mac_title_hover || ([g_win styleMask] & NSWindowStyleMaskFullScreen);
    for (int i = 0; i < 3; i++)
        [[g_win standardWindowButton:mac_title_buttons[i]] setHidden:!visible];
}

static void mac_title_hover_at(int x, int y) {
    int chrome = over_chrome(x, y);
    int hover = !g_chrome_drag && g_tv_drag_slot<0 && !chrome && !g_sel_drag && !g_sb_drag && !g_browser_native_drag && mac_title_contains(x, y);
    /* An explicitly focused address editor remains visible until committed or
       dismissed, even if the pointer happens to cross the menu. */
    if ((g_mac_title_drag || mac_browser_nav_editing()) && !g_chrome_drag && g_tv_drag_slot<0) hover = 1;
    if (hover == g_mac_title_hover) return;
    g_mac_title_hover = hover;
    mac_title_sync_buttons();
    plat_invalidate();
}

static NSRect mac_title_button_rect(int i) {
    NSButton *button = [g_win standardWindowButton:mac_title_buttons[i]];
    if (!button || !g_view) return NSZeroRect;
    NSRect r = [button convertRect:[button bounds] toView:nil];
    CGFloat scale = [g_win backingScaleFactor];
    /* Standard button views sit in AppKit's unflipped titlebar hierarchy.
       Convert window coordinates explicitly, as all renderer coordinates start
       at the drawable's top left (including a full-size content view). */
    r.origin.y = [(NSView *)g_view bounds].size.height - NSMaxY(r);
    return NSMakeRect(r.origin.x*scale, r.origin.y*scale,
                      r.size.width*scale, r.size.height*scale);
}

static int plat_lights_bottom(void) { return mac_title_height(); }

typedef struct { float cx, cy, radius; int left, top, right, bottom; } MacTitleCircle;
static MacTitleCircle g_mac_title_circles[3];
static int g_mac_title_circle_count, g_mac_title_circle_right, g_mac_title_circle_bottom;

static uint32_t plat_title_mix(uint32_t fg, uint32_t bg, float alpha) {
    int a = (int)(alpha * 255 + .5f);
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8)
        out |= (uint32_t)((((fg >> shift) & 255)*a + ((bg >> shift) & 255)*(255-a) + 127) / 255) << shift;
    return out;
}

static uint32_t plat_title_background_color(uint32_t bg) {
    int r = (bg >> 16) & 255, g = (bg >> 8) & 255, b = bg & 255;
    /* A neutral 1/8 shift toward white on dark backgrounds, black on light
       ones. Choosing the direction gives at least 16 levels on every gray,
       including middle gray: exclusion or blending toward gray has a null
       point there. Colored backgrounds keep their hue with less saturation. */
    int light = 77*r + 150*g + 29*b < 128*256;
    r += light ? (255-r+4)/8 : -(r+4)/8;
    g += light ? (255-g+4)/8 : -(g+4)/8;
    b += light ? (255-b+4)/8 : -(b+4)/8;
    return (uint32_t)(r << 16 | g << 8 | b);
}

/* Called immediately after a background fill, before any foreground strokes.
   Geometry is cached once per frame; most cells reject at the first check.
   Resolve coverage against the background explicitly so a later cell/menu
   fill replaces, rather than repeatedly blends, an existing circle edge. */
static void plat_render_title_background(float x0, float y0, float x1, float y1, uint32_t bg, uint32_t base) {
    int translation = g_chrome_x;
    float alpha = g_draw_alpha;
    x0 += translation; x1 += translation;
    if (!g_mac_title_circle_count || x0 >= g_mac_title_circle_right || y0 >= g_mac_title_circle_bottom || x1 <= 0 || y1 <= 0) return;
    bg = plat_title_mix(bg, base, alpha);
    uint32_t color = plat_title_background_color(bg);
    g_chrome_x = 0;
    g_draw_alpha = 1;
    for (int i = 0; i < g_mac_title_circle_count; i++) {
        const MacTitleCircle *c = &g_mac_title_circles[i];
        int left = (int)fmaxf(x0, c->left), right = (int)fminf(x1, c->right);
        int top = (int)fmaxf(y0, c->top), bottom = (int)fminf(y1, c->bottom);
        for (int y = top; y < bottom; y++) {
            for (int x = left; x < right; x++) {
                float dx = x + .5f - c->cx, dy = y + .5f - c->cy;
                float coverage = c->radius + .5f - sqrtf(dx*dx + dy*dy);
                if (coverage <= 0) continue;
                if (coverage > 1) coverage = 1;
                fill_rect(x, y, x+1, y+1, plat_title_mix(color, bg, coverage), 1);
            }
        }
    }
    g_chrome_x = translation;
    g_draw_alpha = alpha;
}

static void plat_render_title_underlay(void) {
    g_mac_title_circle_count = g_mac_title_circle_right = g_mac_title_circle_bottom = 0;
    if (!g_win || !g_view || g_mac_title_hover || !mac_title_height()) return;
    for (int i = 0; i < 3; i++) {
        NSRect r = mac_title_button_rect(i);
        float radius = (NSWidth(r) / 2) - [g_win backingScaleFactor];
        if (radius <= 0) continue;
        MacTitleCircle *c = &g_mac_title_circles[g_mac_title_circle_count++];
        c->cx = NSMidX(r); c->cy = NSMidY(r); c->radius = radius;
        c->left = (int)floorf(c->cx-radius); c->right = (int)ceilf(c->cx+radius);
        c->top = (int)floorf(c->cy-radius); c->bottom = (int)ceilf(c->cy+radius);
        if (c->right > g_mac_title_circle_right) g_mac_title_circle_right = c->right;
        if (c->bottom > g_mac_title_circle_bottom) g_mac_title_circle_bottom = c->bottom;
    }
    plat_render_title_background(0, 0, g_clip_w, g_clip_h, DEF_BG, DEF_BG);
}

static void plat_render_title_overlay(void) {
    if (!g_mac_title_hover || g_sel_drag || g_sb_drag || g_chrome_drag || g_tv_drag_slot>=0) return;
    int w, h, clip = g_clip_w;
    plat_client_size(&w, &h);
    g_clip_w = w; /* The native title band covers even a partial rightmost cell. */
    fill_rect(0, 0, w, mac_title_height(), BAR_BG, .91f);
    g_clip_w = clip;
    mac_browser_draw_nav();
}

/* ── clipboard, drops, links ───────────────────────────────────────── */

static void set_clipboard_utf8(const char *s, int n) {
    NSString *str = [[NSString alloc] initWithBytes:s length:(NSUInteger)n encoding:NSUTF8StringEncoding];
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    if (!str) return;
    [pb clearContents];
    [pb setString:str forType:NSPasteboardTypeString];
    [str release];
}

/* a path as you'd type it, shell-quoted only when it needs to be */
static void quoted_path(const char *path, char *q, size_t cap) {
    static const char safe[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-+:@%,=";
    size_t k, j = 0;
    int needq = !path[0];
    for (k = 0; path[k]; k++)
        if (!strchr(safe, path[k])) needq = 1;
    if (needq) q[j++] = '\'';
    for (k = 0; path[k] && j + 6 < cap; k++) {
        if (path[k] == '\'') { memcpy(q + j, "'\\''", 4); j += 4; }
        else q[j++] = path[k];
    }
    if (needq) q[j++] = '\'';
    q[j] = 0;
}

/* files (dropped, or copied in Finder) type their paths, space separated */
static int paste_file_urls(NSPasteboard *pb) {
    NSArray *urls = [pb readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
    NSMutableString *out;
    if (![urls count]) return 0;
    out = [NSMutableString string];
    for (NSURL *u in urls) {
        char q[MAX_PATH * 2 + 16];
        quoted_path([[u path] fileSystemRepresentation], q, sizeof(q));
        [out appendFormat:@"%s ", q];
    }
    paste_utf8([out UTF8String]);
    return 1;
}

/* An image on the pasteboard (a screenshot, "Copy Image") is saved as a PNG
   under the config folder's pasted/ and its path is typed instead. Claude
   Code's own Ctrl+V image paste still goes straight through. */
static int paste_image(NSPasteboard *pb) {
    NSImage *img = [[[NSImage alloc] initWithPasteboard:pb] autorelease];
    NSBitmapImageRep *rep;
    NSData *png;
    char dir[MAX_PATH], path[MAX_PATH], q[MAX_PATH * 2 + 16];
    struct timespec ts;
    struct tm tm;
    if (!img || ![pb availableTypeFromArray:@[NSPasteboardTypePNG, NSPasteboardTypeTIFF]]) return 0;
    rep = [NSBitmapImageRep imageRepWithData:[img TIFFRepresentation]];
    png = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    if (!png) return 0;
    snprintf(dir, sizeof(dir), "%s/pasted", g_cfg_dir);
    mkdir(dir, 0755);
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    snprintf(path, sizeof(path), "%s/%04d%02d%02d-%02d%02d%02d-%03d.png", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ts.tv_nsec / 1000000));
    if (![png writeToFile:[NSString stringWithUTF8String:path] atomically:YES]) return 0;
    quoted_path(path, q, sizeof(q));
    strcat(q, " ");
    paste_utf8(q);
    return 1;
}

/* Text pastes as text; files as their paths; an image as a saved PNG's path. */
static void paste(void) {
    if (mac_input_held(input_tab())) { NSBeep(); return; }
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    NSString *s;
    if (paste_file_urls(pb)) return;
    if ((s = [pb stringForType:NSPasteboardTypeString])) { paste_utf8([s UTF8String]); return; }
    paste_image(pb);
}

static void open_url(const char *url) {
    NSString *s = [NSString stringWithUTF8String:url];
    NSURL *u = s ? [NSURL URLWithString:s] : nil;
    if (!u && s && [s hasPrefix:@"file://"])    /* unescaped spaces and such */
        u = [NSURL fileURLWithPath:[[s substringFromIndex:7] stringByRemovingPercentEncoding] ?: [s substringFromIndex:7]];
    if (u) [[NSWorkspace sharedWorkspace] openURL:u];
}

/* ── tabs the Mac way ──────────────────────────────────────────────── */

/* Closing a tab (or the window) that's running something asks first; a tab
   sitting at a prompt just closes. Returns 1 to go ahead. */
static int confirm_close(int tab_index) {
    char name[256] = "", list[600] = "";
    int i, n = 0, ok;
    NSAlert *a;
    for (i = 0; i < g_ntabs; i++) {
        if (tab_index >= 0 && i != tab_index) continue;
        if (tab_busy_program(g_tabs[i], name, sizeof(name))) {
            if (n++ < 3) snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", n > 1 ? ", " : "", name);
        }
    }
    if (!n) return 1;
    a = [[NSAlert alloc] init];
    [a setMessageText:tab_index >= 0 ? @"Close this tab?" : @"Quit hardcore?"];
    [a setInformativeText:[NSString stringWithFormat:@"%s %s still running.", list, n == 1 ? "is" : "are"]];
    [a addButtonWithTitle:tab_index >= 0 ? @"Close" : @"Quit"];
    [a addButtonWithTitle:@"Cancel"];
    ok = [a runModal] == NSAlertFirstButtonReturn;
    [a release];
    return ok;
}

static void user_close_tab(int i) {
    if (i >= 0 && i < g_ntabs && confirm_close(i)) close_tab(i);
}

/* New tabs open where the current one is: the folder its shell last reported,
   or, for a shell without our hooks, where the kernel says it is. */
static void new_tab_mac(void) {
    Tab *cur = active_tab();
    if (cur && !cur->t.cwd[0]) {
        char dir[MAX_PATH];
        tab_proc_cwd(cur, dir, sizeof(dir));
        if (dir[0]) set_cwd(&cur->t, dir);
    }
    new_tab_here();
}

static void mac_files_at(const char *requested) {
    if(g_ntabs>=MAX_TABS)return;
    Tab *cur=active_tab();
    const char *dir=requested ?: (cur && cur->t.cwd[0]?cur->t.cwd:cur?cur->start_dir:g_start_dir);
    NSString *script=[[[NSString stringWithUTF8String:g_mac_bin_dir] stringByDeletingLastPathComponent]
        stringByAppendingPathComponent:@"Resources/files.py"];
    if(![[NSFileManager defaultManager] isReadableFileAtPath:script]) {
        script=[[[NSString stringWithUTF8String:g_mac_bin_dir] stringByDeletingLastPathComponent]
            stringByAppendingPathComponent:@"tools/files.py"];
    }
    if(![[NSFileManager defaultManager] isReadableFileAtPath:script]){NSBeep();return;}
    Tab *tb=calloc(1,sizeof(Tab));
    if(!tb || !term_init(&tb->t,g_cols,g_rows)){free(tb);return;}
    tb->id=g_next_id++;tb->fd=-1;
    snprintf(tb->start_dir,sizeof(tb->start_dir),"%s",dir);
    snprintf(tb->run,sizeof(tb->run),"files:");
    snprintf(tb->hist,sizeof(tb->hist),"%s",[script fileSystemRepresentation]);
    set_cwd(&tb->t,dir);
    tab_spawn(tb);g_tabs[g_ntabs++]=tb;activate(g_ntabs-1);
}
static void mac_files_new(void) {mac_files_at(NULL);}
static void mac_record_file_viewer(NSString *path) {
    BOOL directory=NO;
    if(![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] || !directory)
        path=path.stringByDeletingLastPathComponent;
    if(path.length)mac_files_at(path.fileSystemRepresentation);
}
static int mac_header_action(int hit) {
    if(hit==HIT_PLUS)new_tab_mac();
    else if(hit==HIT_WEB)mac_browser_open_welcome();
    else if(hit==HIT_FILES)mac_files_new();
    else if(hit==HIT_CAMERA)mac_camera_toggle();
    else if(hit==HIT_MUSIC)mac_music_toggle();
    else if(hit==HIT_MUSIC_PLAY)mac_music_play_pause();
    else if(hit==HIT_MUSIC_PREV)mac_music_next(-1);
    else if(hit==HIT_MUSIC_NEXT)mac_music_next(1);
    else if(hit==HIT_MUSIC_STOP)mac_music_stop();
    else if(hit==HIT_CAF) {
        int on=mac_caf_toggle();
        if(on>0)hc_tv_open(&g_tvs[HC_TV_CAF].state,GetTickCount());
        else hc_tv_close(&g_tvs[HC_TV_CAF].state);
        plat_invalidate();
    } else return 0;
    return 1;
}

/* ── the view: drawing, keys, mouse, drops ─────────────────────────── */

@interface HCView : NSView <NSTextInputClient> {
@public
    NSMutableAttributedString *marked;
    float scroll_acc;
    int handled;                 /* keyDown sent something itself; skip insertText */
}
@end

static int g_mac_test_monitor_consumed;
static int g_cutie_mouse_consumed, g_cutie_mouse_id, g_cutie_mouse_col, g_cutie_mouse_row, g_cutie_mouse_mods, g_cutie_aux_buttons;
static void mac_cutie_release(void) {
    Tab *tb=tab_by_id(g_cutie_mouse_id);
    if(tb && !mac_input_held(tb) && tb->t.m_mouse) {
        char buf[64];int n;
        if(tb->t.m_mouse_sgr)n=snprintf(buf,sizeof(buf),"\033[<%d;%d;%dm",g_cutie_mouse_mods,g_cutie_mouse_col+1,g_cutie_mouse_row+1);
        else {n=6;memcpy(buf,"\033[M",3);buf[3]=35;buf[4]=(char)(33+g_cutie_mouse_col);buf[5]=(char)(33+g_cutie_mouse_row);}
        tab_write(tb,buf,n);
    }
    g_cutie_mouse_consumed=g_cutie_mouse_id=0;
}
static void mac_cutie_blur(void) {
    if(!g_cutie_focus_id)return;
    Tab *tb=tab_by_id(g_cutie_focus_id);g_cutie_focus_id=0;
    mac_cutie_release();
    Tab *selected=active_tab();
    if(tb && tb!=selected) {
        if(tb->t.m_focus && !mac_input_held(tb))tab_write(tb,"\033[O",3);
        if(g_focused && selected && selected->t.m_focus && !mac_input_held(selected))tab_write(selected,"\033[I",3);
    }
    if(g_view){[g_view unmarkText];mac_browser_sync();}
    plat_invalidate();
}
/* Pointer capture belongs to the TV chrome, never its terminal. Manual
   geometry is grid-aligned and only affects the independent projection. */
static HcTvRect g_tv_drag_origin;
static int g_tv_drag_x, g_tv_drag_y;
static NSCursor *mac_tv_resize_cursor(void) {
    static NSCursor *cursor;
    if(!cursor) {
        NSImage *image=[[NSImage alloc] initWithSize:NSMakeSize(20,20)];
        [image lockFocus];
        NSBezierPath *path=[NSBezierPath bezierPath];
        [path moveToPoint:NSMakePoint(4,16)];[path lineToPoint:NSMakePoint(16,4)];
        [path moveToPoint:NSMakePoint(4,10)];[path lineToPoint:NSMakePoint(4,16)];[path lineToPoint:NSMakePoint(10,16)];
        [path moveToPoint:NSMakePoint(10,4)];[path lineToPoint:NSMakePoint(16,4)];[path lineToPoint:NSMakePoint(16,10)];
        [path setLineJoinStyle:NSRoundLineJoinStyle];[path setLineCapStyle:NSRoundLineCapStyle];
        [[NSColor blackColor] setStroke];[path setLineWidth:4];[path stroke];
        [[NSColor whiteColor] setStroke];[path setLineWidth:1.5];[path stroke];
        [image unlockFocus];cursor=[[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(10,10)];[image release];
    }
    return cursor;
}
static void mac_tv_drag_end(void) {
    if(g_tv_drag_slot<0)return;
    g_tv_drag_slot=-1;
    /* Keep the matching mouse-up consumed even when focus loss ends capture. */
    g_cutie_mouse_consumed=1;g_cutie_mouse_id=0;
    [[NSCursor arrowCursor] set];plat_invalidate();
}
static int mac_tv_drag_begin(int mx,int my) {
    int slot=-1,hit=chrome_tv_hit(mx,my,&slot);
    if(slot<0 || slot>=HC_TV_COUNT || (hit!=HIT_TV_GRIP && hit!=HIT_TV_RESIZE))return 0;
    mac_cutie_blur();chrome_preview_cancel();
    g_tv_drag_slot=slot;g_tv_drag_mode=hit==HIT_TV_RESIZE;
    g_tv_drag_origin=g_tvs[slot].state.rect;g_tv_drag_x=mx;g_tv_drag_y=my;
    g_cutie_mouse_consumed=1;g_cutie_mouse_id=0;
    g_mouse_x=mx;g_mouse_y=my;g_sel_drag=g_sb_drag=0;
    hc_tv_motion_reset(&g_tvs[slot].motion);
    chrome_tv_interaction(slot);
    [(g_tv_drag_mode?mac_tv_resize_cursor():[NSCursor closedHandCursor]) set];plat_invalidate();return 1;
}
static void mac_tv_drag_move(int mx,int my) {
    if(g_tv_drag_slot<0 || g_tv_drag_slot>=HC_TV_COUNT)return;
    ChromeTv *tv=g_tvs+g_tv_drag_slot;Tab *tb=active_tab();
    if(!tv->state.open || !tb){mac_tv_drag_end();return;}
    int cols=tb->t.cols,rows=tb->kind==TK_BROWSER?mac_browser_page_rows(tb):tb->t.rows;
    HcTvRect r=g_tv_drag_origin;
    int dx=(int)lround((double)(mx-g_tv_drag_x)/CW),dy=(int)lround((double)(my-g_tv_drag_y)/CH);
    if(g_tv_drag_mode) {
        r.w=chrome_min(chrome_max(HC_TV_MIN_WIDTH,r.w+dx),chrome_min(HC_TV_MAX_WIDTH,cols-r.x));
        r.h=chrome_min(chrome_max(HC_TV_MIN_HEIGHT,r.h+dy),chrome_min(HC_TV_MAX_HEIGHT,rows-r.y));
    } else {
        r.x=chrome_min(chrome_max(0,r.x+dx),chrome_max(0,cols-r.w));
        r.y=chrome_min(chrome_max(0,r.y+dy),chrome_max(0,rows-r.h));
    }
    g_mouse_x=mx;g_mouse_y=my;
    HcTvRect primary={sidebar_x()/CW,g_chrome.top,SB_COLS,g_chrome.bottom-g_chrome.top};
    int fits=hc_tv_rect_fits(r,cols,rows) && !hc_tv_rect_overlap(r,primary);
    for(int i=0;i<HC_TV_COUNT && fits;i++)
        if(i!=g_tv_drag_slot && g_tvs[i].state.placed && hc_tv_rect_overlap(r,g_tvs[i].state.rect))fits=0;
    if(fits && !hc_tv_rect_equal(r,tv->state.rect)) {
        tv->manual=1;tv->requested=r;tv->state.rect=r;
        tv->placement=(HcTvPlacement){.rect=r,.valid=1};
        tv->frame.count=0;
        if(g_tv_drag_slot>=HC_TV_CUTIE_FIRST)g_cuties[g_tv_drag_slot].valid=0;
        hc_tv_motion_reset(&tv->motion);chrome_tv_interaction(g_tv_drag_slot);plat_invalidate();
    }
    [(g_tv_drag_mode?mac_tv_resize_cursor():[NSCursor closedHandCursor]) set];
}
static int mac_cutie_click(int mx,int my,int mods) {
    int slot=cutie_at(mx,my);if(slot<0)return 0;
    g_cutie_mouse_consumed=1;g_cutie_mouse_id=0;
    ChromeTv *tv=g_tvs+slot;Tab *tb=tab_by_id(tv->tab_id);
    if(!tb)return 1;
    int idx=-1,hit=chrome_tv_hit(mx,my,&idx);
    if(hit==HIT_NONE)return 1;
    if(hit==HIT_TV_CLOSE){mac_cutie_blur();chrome_tv_close_slot(slot);g_cutie_mouse_consumed=1;return 1;}
    HcTvRect r=tv->state.rect;
    if(my/CH==r.y) {
        g_cutie_mouse_consumed=1;
        if(hit==HIT_TV_EXPAND)for(int i=0;i<g_ntabs;i++)if(g_tabs[i]==tb){activate(i);break;}
        g_cutie_mouse_consumed=1;return 1;
    }
    int col=0,row=0,node=0,mapped=cutie_source_at(slot,mx,my,&col,&row,&node);
    if(tb->kind==TK_BROWSER){mac_cutie_blur();g_cutie_mouse_consumed=1;if(mapped)mac_browser_cutie_action(tb,node);return 1;}
    if(mac_input_held(tb)){NSBeep();return 1;}
    if(g_cutie_focus_id!=tb->id) {
        mac_cutie_blur();g_cutie_focus_id=tb->id;
        [g_win makeFirstResponder:g_view];[g_view unmarkText];g_sel_drag=g_sel_on=0;
        Tab *selected=active_tab();
        if(tb!=selected) {
            if(selected && selected->t.m_focus && !mac_input_held(selected))tab_write(selected,"\033[O",3);
            if(tb->t.m_focus)tab_write(tb,"\033[I",3);
        }
    }
    g_cutie_mouse_consumed=1;
    if(mapped && tb->t.m_mouse && !(mods&M_SHIFT) && (tb->t.m_mouse_sgr || (col<223 && row<223))) {
        int flags=(mods&M_SHIFT?4:0)|(mods&M_ALT?8:0)|(mods&M_CTRL?16:0);char buf[64];int n;
        if(tb->t.m_mouse_sgr)n=snprintf(buf,sizeof(buf),"\033[<%d;%d;%dM",flags,col+1,row+1);
        else {n=6;memcpy(buf,"\033[M",3);buf[3]=(char)(32+flags);buf[4]=(char)(33+col);buf[5]=(char)(33+row);}
        tab_write(tb,buf,n);g_cutie_mouse_id=tb->id;g_cutie_mouse_col=col;g_cutie_mouse_row=row;g_cutie_mouse_mods=flags;
    }
    tv->scroll=0;plat_invalidate();return 1;
}

/* Redraws coalesce into one frame, drawn as soon as the current event or
   output has been handled: a keystroke's echo is on its way to the display
   within ~0.3 ms. But never more often than the display refreshes: under a
   flood, painting after every chunk of output cut cat throughput to a third
   (sampled: 75% of the main thread drawing frames nobody could see), so a
   frame that comes too soon after the last one waits out the period.
   (Painting from a before-sleep run loop observer instead looked equivalent
   but measured worse: half the frames reached the screen late.) */
static double g_refresh_ms = 1000.0 / 60;
static double g_painted_at;

#define SETTLE_MS 2.0           /* wait this long after a full read for the rest of a redraw */
#define SETTLE_MAX 8.0          /* ...but never hold a frame longer than this for it */
static double g_paint_wanted;    /* when the pending paint was first asked for */

static void paint_scheduled(void) {
    double now = uptime_ms(), settle = g_more_at + SETTLE_MS - now;
    if (!g_paint_pending) return;
    if (settle > 0.1 && now - g_paint_wanted < SETTLE_MAX) {   /* still arriving */
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(settle * 1e6)), dispatch_get_main_queue(), ^{ paint_scheduled(); });
        return;
    }
    g_paint_pending = 0;
    g_painted_at = uptime_ms();
    paint_view_now();
}

static void plat_invalidate(void) {
    double wait;
    if (g_paint_pending) return;
    g_paint_pending = 1;
    g_paint_wanted = uptime_ms();
    wait = g_painted_at + g_refresh_ms - g_paint_wanted;
    if (g_more_at + SETTLE_MS - g_paint_wanted > wait) wait = g_more_at + SETTLE_MS - g_paint_wanted;
    if (wait <= 0.5) dispatch_async(dispatch_get_main_queue(), ^{ paint_scheduled(); });
    else dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * 1e6)), dispatch_get_main_queue(), ^{ paint_scheduled(); });
}

static void paint_view_now(void) {
    double t0;
    if (!g_layer || !g_layer.device) return;   /* the worker hasn't finished setting Metal up yet */
    t0 = g_lat ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6 : 0;
    paint_now();
    if (g_lat) fprintf(stderr, "paint %.3f\n", clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6 - t0);
}

static void layer_fit(void) {
    NSSize px = [(NSView *)g_view convertSizeToBacking:[(NSView *)g_view bounds].size];
    g_layer.contentsScale = [g_win backingScaleFactor];
    g_layer.drawableSize = CGSizeMake(px.width > 1 ? px.width : 1, px.height > 1 ? px.height : 1);
}

/* point in the view → pixels from the top left, like the Win32 client area */
static void view_px(NSEvent *e, int *x, int *y) {
    NSPoint p = [(NSView *)g_view convertPoint:[e locationInWindow] fromView:nil];
    CGFloat s = [g_win backingScaleFactor];
    *x = (int)(p.x * s);
    *y = (int)(p.y * s);
}

/* Cursor rectangles used to register an I-beam across the title band, racing
   mouseMoved's arrow. All cursor-update, motion and modifier paths now agree
   on the same geometry. Native WebKit retains ownership of its page body. */
static void mac_pointer_cursor_at(int x,int y,NSEventModifierFlags flags) {
    if(mac_music_cursor(x,y))return;
    if(mac_camera_cursor(x,y))return;
    if(g_tv_drag_slot>=0){[(g_tv_drag_mode?mac_tv_resize_cursor():[NSCursor closedHandCursor]) set];return;}
    if(g_chrome_drag){[[NSCursor closedHandCursor] set];return;}
    int w,h;plat_client_size(&w,&h);
    if(x<0 || y<0 || x>=w || y>=h)return;
    if(over_chrome(x,y) || chrome_any_tv_contains(x,y)) {
        int idx=-1,hit=hit_tabbar(x,y,&idx);
        [(hit==HIT_TV_RESIZE?mac_tv_resize_cursor():
          (hit==HIT_TV_GRIP || hit==HIT_GRIP)?[NSCursor openHandCursor]:
          tabbar_action(x,y)?[NSCursor pointingHandCursor]:[NSCursor arrowCursor]) set];
        return;
    }
    if(mac_browser_ui_move(x,y))return;
    if(mac_title_contains(x,y) && !g_sel_drag && !g_sb_drag) {[[NSCursor arrowCursor] set];return;}
    Tab *tb=active_tab();
    if(tb && tb->kind==TK_BROWSER && tb->browser_native && !chrome_preview_active())return;
    int col,row;char url[2048];mouse_cell(x,y,&col,&row);
    if(tb && tb->kind==TK_BROWSER && col>=0 && col<tb->t.cols && row>=0 && row<tb->t.rows && tb->t.row[0][row][col].link) {
        [[NSCursor pointingHandCursor] set];return;
    }
    if((flags&NSEventModifierFlagCommand) && link_at(col,row,url,sizeof(url))) [[NSCursor pointingHandCursor] set];
    else [[NSCursor IBeamCursor] set];
}

/* A view tracking area alone misses motion delivered to the native buttons.
   A local monitor observes the same window's events before AppKit routes them,
   including traffic-light views, without capturing events from other apps. */
static void mac_preview_event(NSEvent *e) {
    NSEventType type=[e type];
    if(type==NSEventTypeKeyDown || type==NSEventTypeScrollWheel || type==NSEventTypeLeftMouseDown)
        chrome_preview_cancel();
    if([e window]!=g_win && (type==NSEventTypeMouseMoved || type==NSEventTypeLeftMouseDown)) {
        mac_cutie_blur();chrome_preview_cancel();g_mouse_x=g_mouse_y=-1;settings_point();plat_invalidate();
    }
}

static NSEvent *mac_title_event(NSEvent *e) {
            mac_preview_event(e);
            if ([e window] == g_win) {
                int x, y;
                view_px(e, &x, &y);
                mac_record_event(e,x,y);
                if(mac_music_mouse(e,x,y))return nil;
                if(mac_camera_mouse(e,x,y))return nil;
                Tab *tb=active_tab();
                if([e type]==NSEventTypeLeftMouseDown && cutie_at(x,y)<0)mac_cutie_blur();
                if ([e type]==NSEventTypeScrollWheel) return g_tv_drag_slot>=0?nil:e;
                if ([e type]==NSEventTypeKeyDown) {
                    if(g_tv_drag_slot>=0){if([e keyCode]==53)mac_tv_drag_end();return nil;}
                    if (!g_cutie_focus_id && tb && tb->kind==TK_BROWSER && tb->browser_native) {
                        NSString *key=[e charactersIgnoringModifiers];
                        if (([e modifierFlags]&NSEventModifierFlagCommand) && key.length==1) {
                            unichar ch=[key characterAtIndex:0];
                            if (ch>='1' && ch<='9') { activate(ch=='9' ? g_ntabs-1 : ch-'1'); return nil; }
                        }
                        if ([e keyCode]==48 && ([e modifierFlags]&NSEventModifierFlagControl)) {
                            activate((g_active+(([e modifierFlags]&NSEventModifierFlagShift) ? g_ntabs-1 : 1))%g_ntabs); return nil;
                        }
                    }
                    return e;
                }
                /* The grid handle owns its drag even where native title controls
                   or a WKWebView would otherwise receive the pointer event. */
                if([e type]==NSEventTypeLeftMouseDown && mac_tv_drag_begin(x,y))return nil;
                if([e type]==NSEventTypeLeftMouseDragged && g_tv_drag_slot>=0){mac_tv_drag_move(x,y);return nil;}
                if([e type]==NSEventTypeLeftMouseUp && g_tv_drag_slot>=0){mac_tv_drag_move(x,y);mac_tv_drag_end();mac_cutie_release();return nil;}
                /* A captured press owns its later drag/up even after Escape
                   or focus loss, including events destined for a WKWebView. */
                if([e type]==NSEventTypeLeftMouseDragged && g_cutie_mouse_consumed)return nil;
                if([e type]==NSEventTypeLeftMouseUp && g_cutie_mouse_consumed){mac_cutie_release();return nil;}
                if([e type]==NSEventTypeRightMouseDown && g_tv_drag_slot>=0){g_cutie_aux_buttons|=2;return nil;}
                if([e type]==NSEventTypeOtherMouseDown && g_tv_drag_slot>=0){g_cutie_aux_buttons|=1;return nil;}
                if([e type]==NSEventTypeRightMouseDragged && (g_cutie_aux_buttons&2))return nil;
                if([e type]==NSEventTypeOtherMouseDragged && (g_cutie_aux_buttons&1))return nil;
                if([e type]==NSEventTypeRightMouseUp && (g_cutie_aux_buttons&2)){g_cutie_aux_buttons&=~2;return nil;}
                if([e type]==NSEventTypeOtherMouseUp && (g_cutie_aux_buttons&1)){g_cutie_aux_buttons&=~1;return nil;}
                if ([e type] == NSEventTypeLeftMouseDown && chrome_drag_begin(x, y)) {
                    mac_title_hover_at(x, y); [[NSCursor closedHandCursor] set]; return nil;
                }
                if ([e type] == NSEventTypeLeftMouseDragged && g_chrome_drag) {
                    chrome_drag_move(x, y); [[NSCursor closedHandCursor] set]; return nil;
                }
                if ([e type] == NSEventTypeLeftMouseUp && g_chrome_drag) {
                    chrome_drag_move(x, y); chrome_drag_end(); mac_title_hover_at(x, y);
                    [[NSCursor openHandCursor] set]; return nil;
                }
                if ([e type]==NSEventTypeLeftMouseDown && tb && tb->kind==TK_BROWSER && tb->browser_native)
                    g_browser_native_drag=!over_chrome(x,y) && !mac_title_contains(x,y);
                if ([e type]==NSEventTypeLeftMouseUp) g_browser_native_drag=0;
                if([e type]==NSEventTypeLeftMouseDown && mac_browser_nav_editing() && !mac_title_contains(x,y))mac_browser_ui_mouse(x,y);
                mac_title_hover_at(x, y);
                if([e type]==NSEventTypeMouseMoved || [e type]==NSEventTypeMouseEntered)
                    mac_pointer_cursor_at(x,y,[e modifierFlags]);
                if (tb && tb->kind==TK_BROWSER && tb->browser_native && !g_sel_drag) {
                    int before=chrome_hovered() || chrome_any_tv_hovered();
                    g_mouse_x=x; g_mouse_y=y;
                    NSSize bounds=[g_view bounds].size;CGFloat scale=[g_win backingScaleFactor];
                    if([e type]==NSEventTypeMouseExited && (x<0 || y<0 || x>=bounds.width*scale || y>=bounds.height*scale)) {
                        mac_cutie_blur();chrome_preview_cancel();g_mouse_x=g_mouse_y=-1;
                    } else if([e type]==NSEventTypeMouseMoved || [e type]==NSEventTypeMouseEntered)
                        chrome_preview_pointer(x,y);
                    int after=chrome_hovered() || chrome_any_tv_hovered();
                    if(before || after){settings_point();plat_invalidate();}
                }
            } else if ([e type] == NSEventTypeMouseMoved || [e type] == NSEventTypeLeftMouseDown) {
                mac_title_hover_at(-1, -1);
            }
            return e;
}
static void mac_title_track_events(void) {
    if (g_mac_title_monitor) return;
    g_mac_title_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:
        NSEventMaskMouseMoved | NSEventMaskMouseEntered | NSEventMaskMouseExited |
        NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp | NSEventMaskKeyDown | NSEventMaskScrollWheel |
        NSEventMaskRightMouseDown | NSEventMaskRightMouseDragged | NSEventMaskRightMouseUp |
        NSEventMaskOtherMouseDown | NSEventMaskOtherMouseDragged | NSEventMaskOtherMouseUp
        handler:^NSEvent *(NSEvent *e) { return mac_title_event(e); }];
    mac_title_sync_buttons();
}

static int ev_mods(NSEvent *e) {
    NSEventModifierFlags f = [e modifierFlags];
    /* Option drags select, the Mac habit; Shift does too, the hardcore one */
    return ((f & (NSEventModifierFlagShift | NSEventModifierFlagOption)) ? M_SHIFT : 0) |
           ((f & NSEventModifierFlagControl) ? M_CTRL : 0);
}

static void send_str(const char *s) {
    g_sel_on = 0;
    send_to_active(s, (int)strlen(s));
}

/* Is the current input source an ordinary keyboard layout (an ID like
   com.apple.keylayout.US), not an input method like Pinyin or Kotoeri?
   Cached; macOS says when it changes. */
static int g_plain_layout = -1;

static int plain_layout(void) {
    if (g_plain_layout < 0) {
        NSString *id = [[(NSView *)g_view inputContext] selectedKeyboardInputSource];
        g_plain_layout = id && [id rangeOfString:@".keylayout."].location != NSNotFound;
    }
    return g_plain_layout;
}

@implementation HCView

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque { Tab *tb=active_tab(); return chrome_preview_active() || !(tb && tb->kind==TK_BROWSER && tb->browser_native); }
- (NSView *)hitTest:(NSPoint)point {
    Tab *tb=active_tab();
    if (tb && tb->kind==TK_BROWSER && tb->browser_native) {
        NSPoint p=[self convertPoint:point fromView:self.superview];
        CGFloat scale=[g_win backingScaleFactor];
        int x=(int)(p.x*scale), y=(int)(p.y*scale);
        if (!over_chrome(x,y) && !mac_title_contains(x,y) && !mac_browser_ui_contains(x,y)) return nil;
    }
    return [super hitTest:point];
}
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (BOOL)mouseDownCanMoveWindow { return NO; }

- (BOOL)wantsUpdateLayer { return YES; }

- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.framebufferOnly = NO;      /* the test channel's snapshot reads it back */
    l.maximumDrawableCount = 2;  /* one on screen, one on its way: a third only adds a frame of queue */
    l.displaySyncEnabled = NO;   /* present now: display sync measured ~4 ms slower at p50 */
    l.presentsWithTransaction = NO; /* keep drawable/GPU waits off AppKit's resize event loop */
    l.opaque = YES;
    l.colorspace = cs;           /* the palette is sRGB, as on Windows */
    l.contentsGravity = kCAGravityTopLeft;   /* mid-resize, never stretch the old frame */
    CGColorSpaceRelease(cs);
    g_layer = l;
    return l;
}

/* AppKit wants the layer drawn: shown, uncovered, moved to another screen */
- (void)updateLayer { paint_view_now(); }

- (void)setFrameSize:(NSSize)size {
    mac_tv_drag_end();mac_cutie_blur();
    double t0 = g_lat ? uptime_ms() : 0, t1;
    chrome_preview_cancel();
    [super setFrameSize:size];
    if (!g_layer) return;
    layer_fit();
    resize_all();
    t1 = g_lat ? uptime_ms() : 0;
    /* Build the latest frame now, but let the render thread acquire/present
       its drawable. A transaction-synchronous draw here can stall AppKit for
       nextDrawable's one-second timeout during a drag. Pending frames coalesce. */
    paint_view_now();
    /* Main-thread time: reflow plus building/enqueuing the frame, never GPU waits. */
    if (g_lat) fprintf(stderr, "resize %dx%d %.3f %.3f\n", g_cols, g_rows, uptime_ms() - t0, t1 - t0);
}

- (void)viewDidEndLiveResize {
    [super viewDidEndLiveResize];
    paint_view_now();   /* enqueue the final size even if the last drag frame was replaced */
}

- (void)viewDidChangeBackingProperties {
    int s = (int)([g_win backingScaleFactor] + 0.5);
    [super viewDidChangeBackingProperties];
    if ((s < 1 ? 1 : s) != g_scale) glyph_cache_reset();   /* thin box lines are drawn for the scale */
    g_scale = s < 1 ? 1 : s;
    if (g_layer) layer_fit();
    resize_all();
    plat_invalidate();
}

- (void)resetCursorRects {
    /* Dynamic cursorUpdate tracking owns the cursor, including the title band.
       Do not reintroduce a full-view I-beam rectangle here. */
}

- (void)cursorUpdate:(NSEvent *)e {
    int x,y;view_px(e,&x,&y);
    mac_title_hover_at(x,y);
    mac_pointer_cursor_at(x,y,[e modifierFlags]);
}

- (void)updateTrackingAreas {
    for (NSTrackingArea *a in [self trackingAreas]) [self removeTrackingArea:a];
    [self addTrackingArea:[[[NSTrackingArea alloc] initWithRect:[self bounds]
                                                         options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                                 NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                           owner:self userInfo:nil] autorelease]];
    /* Cursor tracking cannot use NSTrackingActiveAlways. Keep background
       hover/preview tracking separate from AppKit's key-window cursor path. */
    [self addTrackingArea:[[[NSTrackingArea alloc] initWithRect:[self bounds]
        options:NSTrackingCursorUpdate | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
        owner:self userInfo:nil] autorelease]];
    [super updateTrackingAreas];
}

/* ── keys ── */

/* xterm's modifier parameter: 1 + shift(1) + meta(2) + ctrl(4), 0 = none */
static int key_mods(NSEventModifierFlags f, int meta) {
    int m = ((f & NSEventModifierFlagShift) ? 1 : 0) | (meta ? 2 : 0) | ((f & NSEventModifierFlagControl) ? 4 : 0);
    return m ? m + 1 : 0;
}

/* Keys hardcore turns into escape sequences itself. Returns 1 if sent. */
- (int)specialKey:(NSEvent *)e {
    NSEventModifierFlags f = [e modifierFlags];
    unsigned short kc = [e keyCode];
    Tab *tb = input_tab();
    int shift = (f & NSEventModifierFlagShift) != 0, ctrl = (f & NSEventModifierFlagControl) != 0;
    int cmd = (f & NSEventModifierFlagCommand) != 0;
    int meta = (f & 0x20) != 0;  /* NX_DEVICELALTKEYMASK: the left Option key */
    int m = key_mods(f, meta);
    char buf[32];
    const char *seq = NULL;
    int n = 0;
    if (!tb) return 0;
    switch (kc) {
    case 126: case 125: case 124: case 123: case 115: case 119: {   /* arrows, Home, End */
        char c = kc == 126 ? 'A' : kc == 125 ? 'B' : kc == 124 ? 'C' : kc == 123 ? 'D' : kc == 115 ? 'H' : 'F';
        if (cmd && (kc == 123 || kc == 124)) { seq = kc == 123 ? "\x01" : "\x05"; break; }   /* Cmd+←/→: line start / end */
        if (meta && !shift && !ctrl && (kc == 123 || kc == 124)) { seq = kc == 123 ? "\x1b" "b" : "\x1b" "f"; break; }   /* word */
        if (shift && !ctrl && !meta && (kc == 115 || kc == 119)) {
            int slot=cutie_slot_for(g_cutie_focus_id);
            if(slot>=0)chrome_tv_scroll(slot,kc==115?INT_MAX:-INT_MAX);
            else scroll_view(kc == 115 ? tb->t.hcount : -tb->t.hcount);
            return 1;
        }
        if (m) n = snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, c);
        else n = snprintf(buf, sizeof(buf), tb->t.m_appcursor ? "\x1bO%c" : "\x1b[%c", c);
        break;
    }
    case 116: case 121:          /* PgUp / PgDn; with Shift, scrollback */
        if (shift && !ctrl && !meta) {
            int slot=cutie_slot_for(g_cutie_focus_id);
            if(slot>=0)chrome_tv_scroll(slot,(kc==116?1:-1)*chrome_max(1,g_tvs[slot].state.rect.h-3));
            else scroll_view(kc == 116 ? tb->t.rows - 1 : -(tb->t.rows - 1));
            return 1;
        }
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", kc == 116 ? 5 : 6, m) : snprintf(buf, sizeof(buf), "\x1b[%d~", kc == 116 ? 5 : 6);
        break;
    case 117: case 114:          /* forward delete, Help/Insert */
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", kc == 117 ? 3 : 2, m) : snprintf(buf, sizeof(buf), "\x1b[%d~", kc == 117 ? 3 : 2);
        break;
    case 122: case 120: case 99: case 118: {   /* F1-F4 */
        char c = kc == 122 ? 'P' : kc == 120 ? 'Q' : kc == 99 ? 'R' : 'S';
        n = m ? snprintf(buf, sizeof(buf), "\x1b[1;%d%c", m, c) : snprintf(buf, sizeof(buf), "\x1bO%c", c);
        break;
    }
    case 96: case 97: case 98: case 100: case 101: case 109: case 103: case 111: {   /* F5-F12 */
        static const unsigned short kcs[] = {96, 97, 98, 100, 101, 109, 103, 111};
        static const int codes[] = {15, 17, 18, 19, 20, 21, 23, 24};
        int i;
        for (i = 0; i < 8 && kcs[i] != kc; i++) {}
        n = m ? snprintf(buf, sizeof(buf), "\x1b[%d;%d~", codes[i], m) : snprintf(buf, sizeof(buf), "\x1b[%d~", codes[i]);
        break;
    }
    case 36: case 76:            /* Return, keypad Enter: Shift/Option+Enter = ESC CR, Claude Code's newline */
        seq = (shift || (f & NSEventModifierFlagOption)) ? "\x1b\r" : "\r";
        break;
    case 51:                     /* Backspace */
        seq = cmd ? "\x15" : ctrl ? "\x17" : meta ? "\x1b\x7f" : "\x7f";
        break;
    case 48:                     /* Tab; Ctrl+Tab switches tabs */
        if (ctrl) { activate((g_active + (shift ? g_ntabs - 1 : 1)) % g_ntabs); return 1; }
        seq = shift ? "\x1b[Z" : "\t";
        break;
    case 53:
        seq = "\x1b";
        break;
    }
    if (cmd && !seq) return 0;
    if (seq) n = (int)strlen(seq), memcpy(buf, seq, (size_t)n);
    if (!n) return 0;
    g_sel_on = 0;
    send_to_active(buf, n);
    return 1;
}

/* Ctrl+key and Meta (left Option)+key, from the unmodified character */
- (int)controlKey:(NSEvent *)e {
    NSEventModifierFlags f = [e modifierFlags];
    NSString *chars = [e charactersIgnoringModifiers];
    int ctrl = (f & NSEventModifierFlagControl) != 0, meta = (f & 0x20) != 0;
    char buf[16];
    int n = 0;
    unichar c;
    if ((!ctrl && !meta) || [chars length] != 1) return 0;
    c = [chars characterAtIndex:0];
    if (meta) buf[n++] = 0x1B;
    if (ctrl) {
        if (c >= 'a' && c <= 'z') buf[n++] = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') buf[n++] = (char)(c - 'A' + 1);
        else if (c == ' ' || c == '@' || c == '2') buf[n++] = 0;
        else if (c == '[' || c == '3') buf[n++] = 0x1B;
        else if (c == '\\' || c == '4') buf[n++] = 0x1C;
        else if (c == ']' || c == '5') buf[n++] = 0x1D;
        else if (c == '^' || c == '6') buf[n++] = 0x1E;
        else if (c == '_' || c == '-' || c == '/' || c == '7') buf[n++] = 0x1F;
        else if (c == '?' || c == '8') buf[n++] = 0x7F;
        else return 0;
    } else {
        NSString *s = [chars precomposedStringWithCanonicalMapping];
        const char *u = [s UTF8String];
        size_t k = strlen(u);
        if (k + 1 >= sizeof(buf)) return 0;
        memcpy(buf + n, u, k);
        n += (int)k;
    }
    g_sel_on = 0;
    send_to_active(buf, n);
    return 1;
}

- (void)keyDown:(NSEvent *)e {
    if(g_tv_drag_slot>=0){if([e keyCode]==53)mac_tv_drag_end();return;}
    chrome_preview_cancel();
    g_chrome_input_at = GetTickCount();
    NSEventModifierFlags f = [e modifierFlags];
    if(g_cutie_focus_id && [e keyCode]==53 && !(f&NSEventModifierFlagOption)){mac_cutie_blur();return;}
    Tab *tb = input_tab();
    if (!tb) return;
    if(!g_cutie_focus_id && mac_browser_ui_key(e))return;
    if(!g_cutie_focus_id && mac_browser_editing() && !(f&NSEventModifierFlagCommand)){[self interpretKeyEvents:@[e]];return;}
    if (g_lat && !g_lat_key) {
        g_lat_key = [e timestamp] * 1000.0;
        g_lat_down = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6;
        g_lat_written = 0;
    }
    [NSCursor setHiddenUntilMouseMoves:YES];
    if (tb->help) {
        int at;
        if ([e keyCode] == 36 || [e keyCode] == 76)
            for (at = 0; at < g_ntabs; at++) if (g_tabs[at] == tb) { replace_tab(at); break; }
        return;
    }
    if (f & NSEventModifierFlagCommand) {
        NSString *c = [e charactersIgnoringModifiers];
        unichar ch = [c length] ? [c characterAtIndex:0] : 0;
        if (ch >= '1' && ch <= '9') {
            if (g_tracing) { g_trace_tab_t = uptime_ms(); trace("Cmd+digit"); }
            activate(ch == '9' ? g_ntabs - 1 : ch - '1');
            return;
        }
        [self specialKey:e];     /* Cmd+←/→/Backspace; other Cmd keys belong to the menus or nobody */
        return;
    }
    if (tb->kind==TK_BROWSER) {
        switch ([e keyCode]) {
        case 126: mac_browser_scroll(-1); break;
        case 125: mac_browser_scroll(1); break;
        case 116: mac_browser_scroll(1-tb->t.rows); break;
        case 121: case 49: mac_browser_scroll(tb->t.rows-1); break;
        case 115: mac_browser_scroll(-20000); break;
        case 119: mac_browser_scroll(20000); break;
        case 48:
            if ([e modifierFlags]&NSEventModifierFlagControl) [self specialKey:e];
            else mac_browser_focus(([e modifierFlags]&NSEventModifierFlagShift) ? -1 : 1);
            break;
        case 36: case 76: mac_browser_enter(); break;
        default: break;
        }
        return;
    }
    if (mac_input_held(tb)) { NSBeep(); return; }
    if ([self specialKey:e] || [self controlKey:e]) return;
    /* Plain typing on a plain keyboard layout goes straight out: the text-input
       system (interpretKeyEvents) costs a few ms at p90, measured. Dead keys
       (no characters yet), input methods and anything mid-composition still
       take the long way. */
    if (!marked && plain_layout() && [[e characters] length] == 1 && [[e characters] characterAtIndex:0] >= 0x20 &&
        [[e characters] characterAtIndex:0] != 0x7F) {
        send_str([[e characters] UTF8String]);
        return;
    }
    handled = 0;
    [self interpretKeyEvents:@[e]];   /* text, dead keys, input methods → insertText: */
}

/* Ctrl+Tab and Ctrl+Shift+Tab never reach keyDown by default (the window
   uses them to move between controls) */
- (BOOL)performKeyEquivalent:(NSEvent *)e {
    /* Cmd+Q straight to quitting: through the menu it waits ~13 ms for the menu bar's flash */
    if ([e type] == NSEventTypeKeyDown && ([e modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask) == NSEventModifierFlagCommand &&
        [[e charactersIgnoringModifiers] isEqualToString:@"q"]) {
        [NSApp terminate:nil];
        return YES;
    }
    if ([e type] == NSEventTypeKeyDown && [e keyCode] == 48 && ([e modifierFlags] & NSEventModifierFlagControl) &&
        [[self window] firstResponder] == self) {
        [self specialKey:e];
        return YES;
    }
    return [super performKeyEquivalent:e];
}

- (void)flagsChanged:(NSEvent *)e {
    /* Cmd held over a link: the pointing hand */
    NSPoint p = [self convertPoint:[g_win mouseLocationOutsideOfEventStream] fromView:nil];
    CGFloat s = [g_win backingScaleFactor];
    int x=(int)(p.x*s),y=(int)(p.y*s);
    /* The isolated test channel supplies a coordinate-bearing pointer event. */
    if([e type]!=NSEventTypeFlagsChanged)view_px(e,&x,&y);
    mac_title_hover_at(x,y);
    mac_pointer_cursor_at(x,y,[e modifierFlags]);
}

/* NSTextInputClient: what text the keys (and input methods) produced */
- (void)insertText:(id)s replacementRange:(NSRange)r {
    NSString *str = [s isKindOfClass:[NSAttributedString class]] ? [s string] : s;
    (void)r;
    [marked release];
    marked = nil;
    if ([str length] && (g_cutie_focus_id || !mac_browser_insert(str))) send_str([str UTF8String]);
}
- (void)doCommandBySelector:(SEL)sel { (void)sel; }
- (void)setMarkedText:(id)s selectedRange:(NSRange)a replacementRange:(NSRange)b {
    (void)a; (void)b;
    [marked release];
    marked = [s isKindOfClass:[NSAttributedString class]] ? [[NSMutableAttributedString alloc] initWithAttributedString:s]
                                                           : [[NSMutableAttributedString alloc] initWithString:s];
}
- (void)unmarkText { [marked release]; marked = nil; }
- (NSRange)selectedRange { return NSMakeRange(NSNotFound, 0); }
- (NSRange)markedRange { return marked ? NSMakeRange(0, [marked length]) : NSMakeRange(NSNotFound, 0); }
- (BOOL)hasMarkedText { return marked && [marked length]; }
- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r actualRange:(NSRangePointer)a { (void)r; (void)a; return nil; }
- (NSArray *)validAttributesForMarkedText { return @[]; }
- (NSUInteger)characterIndexForPoint:(NSPoint)p { (void)p; return NSNotFound; }
- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRangePointer)a {
    /* where the input method's candidate window goes: at the cursor */
    Tab *tb = input_tab();
    CGFloat s = [g_win backingScaleFactor];
    NSRect cell = NSMakeRect(0, 0, CW / s, CH / s);
    (void)r; (void)a;
    if (tb) {
        int col, row;
        display_cursor_position(&tb->t, &col, &row);
        int slot=cutie_slot_for(g_cutie_focus_id);
        if(slot>=0 && g_cuties[slot].valid && g_cuties[slot].view.cursor>=0) {
            HcCutieProjection *p=&g_cuties[slot].view;HcTvRect box=g_tvs[slot].state.rect;
            col=box.x+1+p->cursor%p->cols;row=box.y+1+p->cursor/p->cols;
        }
        cell.origin.x = (term_ox() + col * CW) / s;
        cell.origin.y = (term_oy() + (row + 1) * CH) / s;
    }
    return [g_win convertRectToScreen:[self convertRect:cell toView:nil]];
}

/* ── mouse ── */

- (void)mouseDown:(NSEvent *)e {
    chrome_preview_cancel();
    int mx, my, idx = -1, col, row, hit, w, h, mods = ev_mods(e);
    Tab *tb = active_tab();
    view_px(e, &mx, &my);
    if(mac_tv_drag_begin(mx,my))return;
    if(mac_cutie_click(mx,my,mods))return;
    mac_cutie_blur();
    if (chrome_drag_begin(mx, my)) { mac_title_hover_at(mx, my); [[NSCursor closedHandCursor] set]; return; }
    mac_title_hover_at(mx, my);
    if((!over_chrome(mx,my) || mac_browser_nav_editing()) && mac_browser_ui_mouse(mx,my))return;
    if (g_mac_title_hover && mac_title_contains(mx,my)) {
        if ([e clickCount] == 2) [g_win performZoom:nil];
        else {
            g_mac_title_drag = 1;
            [g_win performWindowDragWithEvent:e];
            g_mac_title_drag = 0;
            NSPoint p = [self convertPoint:[g_win mouseLocationOutsideOfEventStream] fromView:nil];
            CGFloat scale = [g_win backingScaleFactor];
            mac_title_hover_at((int)(p.x*scale), (int)(p.y*scale));
        }
        return;
    }
    hit = hit_tabbar(mx, my, &idx);
    if(hit!=HIT_NONE){g_cutie_mouse_consumed=1;g_cutie_mouse_id=0;}
    if (hit == HIT_CUTIE) { g_cutie_mouse_consumed=1;g_cutie_mouse_id=0;if(idx>=0 && idx<g_ntabs){int slot=cutie_slot_for(g_tabs[idx]->id);if(slot>=0){mac_cutie_blur();chrome_tv_close_slot(slot);}else if(chrome_tv_open_tab(g_tabs[idx]->id)<0)NSBeep();} return; }
    if (hit == HIT_TAB) { activate(idx); return; }
    if (hit == HIT_CLOSE) { user_close_tab(idx); return; }
    if (mac_header_action(hit)) return;
    if (chrome_setting_hit(hit)) { settings_click(mx, my); return; }
    if (hit == HIT_BAR || hit == HIT_CHROME_INERT) return;
    plat_client_size(&w, &h);
    if (tb && mx >= term_ox() + tb->t.cols * CW - SCROLLBAR_W && mx < term_ox() + tb->t.cols * CW && tb->t.hcount && !tb->t.alt) {
        g_sb_drag = 1;
        scrollbar_set(my);
        return;
    }
    if (!tb) return;
    mouse_cell(mx, my, &col, &row);
    if (tb->kind==TK_BROWSER && !(mods&M_SHIFT) && col>=0 && col<tb->t.cols && row>=0 && row<tb->t.rows) {
        int action=tb->t.row[0][row][col].link;
        if (action) { mac_browser_action(tb,action); return; }
    }
    if ([e modifierFlags] & NSEventModifierFlagCommand) {   /* Cmd+click opens a link, even inside mouse-aware apps */
        char url[2048];
        if (link_at(col, row, url, sizeof(url))) { open_url(url); return; }
    }
    if (mouse_report(0, 1, 0, mx, my, mods)) return;
    if ([e clickCount] == 2) {
        select_word(col, row);
        plat_invalidate();
        return;
    }
    g_sel_on = 0;
    g_sel_drag = 1;
    g_sel_a_row = g_sel_b_row = term_base(&tb->t) + tb->t.hcount - tb->t.view + row;
    g_sel_a_col = g_sel_b_col = col;
    plat_invalidate();
}

- (void)mouseDragged:(NSEvent *)e {
    if(g_tv_drag_slot>=0){int x,y;view_px(e,&x,&y);mac_tv_drag_move(x,y);return;}
    if(g_cutie_mouse_consumed)return;
    int mx, my, col, row;
    Tab *tb = active_tab();
    view_px(e, &mx, &my);
    g_mouse_x = mx;
    g_mouse_y = my;
    mac_title_hover_at(mx, my);
    if (g_chrome_drag) { chrome_drag_move(mx, my); [[NSCursor closedHandCursor] set]; return; }
    if (g_sb_drag) { scrollbar_set(my); return; }
    if (g_sel_drag && tb) {
        mouse_cell(mx, my, &col, &row);
        if (my < term_oy()) scroll_view(1);
        else if (my >= term_oy() + tb->t.rows * CH) scroll_view(-1);
        g_sel_b_row = term_base(&tb->t) + tb->t.hcount - tb->t.view + row;
        g_sel_b_col = col;
        g_sel_on = 1;
        plat_invalidate();
        return;
    }
    if (!over_chrome(mx, my)) mouse_report(0, 1, 1, mx, my, ev_mods(e));
}

- (void)mouseUp:(NSEvent *)e {
    if(g_tv_drag_slot>=0){int x,y;view_px(e,&x,&y);mac_tv_drag_move(x,y);mac_tv_drag_end();mac_cutie_release();return;}
    if(g_cutie_mouse_consumed){mac_cutie_release();return;}
    int mx, my;
    if (g_chrome_drag) { view_px(e, &mx, &my); chrome_drag_move(mx, my); chrome_drag_end(); [[NSCursor openHandCursor] set]; return; }
    if (g_sb_drag || g_sel_drag) {
        g_sb_drag = g_sel_drag = 0;
        return;
    }
    view_px(e, &mx, &my);
    mouse_report(0, 0, 0, mx, my, ev_mods(e));
}

- (void)rightMouseDown:(NSEvent *)e {
    if(g_tv_drag_slot>=0){g_cutie_aux_buttons|=2;return;}
    int mx, my;
    NSMenu *m;
    view_px(e, &mx, &my);
    if (cutie_at(mx,my)>=0 || over_chrome(mx, my)) {g_cutie_aux_buttons|=2;return;}
    mac_cutie_blur();
    if(active_tab() && active_tab()->kind==TK_BROWSER){mac_browser_context(mx,my);return;}
    if (mouse_report(2, 1, 0, mx, my, ev_mods(e))) return;
    m = [[[NSMenu alloc] init] autorelease];
    [m setAutoenablesItems:NO];
    [[m addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@""] setEnabled:g_sel_on];
    [m addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@""];
    [m addItem:[NSMenuItem separatorItem]];
    [m addItemWithTitle:@"New Tab" action:@selector(newTab:) keyEquivalent:@""];
    [NSMenu popUpContextMenu:m withEvent:e forView:self];
}

- (void)rightMouseUp:(NSEvent *)e {
    if(g_cutie_aux_buttons&2){g_cutie_aux_buttons&=~2;return;}
    int mx, my;
    view_px(e, &mx, &my);
    mouse_report(2, 0, 0, mx, my, ev_mods(e));
}

- (void)rightMouseDragged:(NSEvent *)e {
    if(g_cutie_aux_buttons&2)return;
    int mx, my;
    view_px(e, &mx, &my);
    if (!over_chrome(mx, my)) mouse_report(2, 1, 1, mx, my, ev_mods(e));
}

- (void)otherMouseDown:(NSEvent *)e {
    if(g_tv_drag_slot>=0){g_cutie_aux_buttons|=1;return;}
    int mx, my, idx = -1, hit;
    view_px(e, &mx, &my);
    hit = hit_tabbar(mx, my, &idx);
    if (hit == HIT_TAB || hit == HIT_CLOSE || hit==HIT_CUTIE) { g_cutie_aux_buttons|=1;user_close_tab(idx); return; }   /* middle-click closes a tab */
    if(cutie_at(mx,my)>=0 || over_chrome(mx,my)){g_cutie_aux_buttons|=1;return;}
    mac_cutie_blur();
    mouse_report(1, 1, 0, mx, my, ev_mods(e));
}

- (void)otherMouseUp:(NSEvent *)e {
    if(g_cutie_aux_buttons&1){g_cutie_aux_buttons&=~1;return;}
    int mx, my;
    view_px(e, &mx, &my);
    mouse_report(1, 0, 0, mx, my, ev_mods(e));
}

- (void)mouseMoved:(NSEvent *)e {
    int mx, my, was_bar = over_chrome(g_mouse_x,g_mouse_y) || chrome_any_tv_contains(g_mouse_x,g_mouse_y), on_bar;
    Tab *tb = active_tab();
    view_px(e, &mx, &my);
    mac_title_hover_at(mx, my);
    on_bar = over_chrome(mx, my) || chrome_any_tv_contains(mx,my);
    g_mouse_x = mx;
    g_mouse_y = my;
    chrome_preview_pointer(mx,my);
    if (was_bar || on_bar) plat_invalidate();    if (was_bar || on_bar) settings_point();
    mac_pointer_cursor_at(mx,my,[e modifierFlags]);
    if(tb && tb->kind==TK_BROWSER)plat_invalidate();
    if (!on_bar && !mac_title_contains(mx,my) && tb && tb->t.m_mouse == 1003) mouse_report(3, 1, 1, mx, my, ev_mods(e));
}

- (void)mouseEntered:(NSEvent *)e { [self mouseMoved:e]; }

- (void)mouseExited:(NSEvent *)e {
    mac_record_hide_pointer();
    mac_cutie_blur();
    chrome_preview_cancel();
    int x, y;
    view_px(e, &x, &y);
    mac_title_hover_at(x, y);
    g_mouse_x = g_mouse_y = -1;
    settings_point();
    plat_invalidate();
}

- (void)scrollAtX:(int)mx y:(int)my delta:(CGFloat)dy precise:(BOOL)precise mods:(int)mods {
    if(g_tv_drag_slot>=0)return;
    chrome_preview_cancel();
    Tab *tb = active_tab();
    int lines, i;
    if (!tb) return;
    int cutie=cutie_at(mx,my);
    if(cutie>=0) {
        scroll_acc += precise ? (float)(dy*[g_win backingScaleFactor]/CH) : (float)(dy*3);
        lines=(int)scroll_acc;scroll_acc-=lines;
        Tab *source=tab_by_id(g_tvs[cutie].tab_id);
        if(lines)chrome_tv_scroll(cutie,source && source->kind==TK_BROWSER?-lines:lines);
        return;
    }
    if (over_chrome(mx, my)) { scroll_acc = 0; return; }
    /* trackpads report points: a line per cell height; wheels report notches: three lines each */
    scroll_acc += precise ? (float)(dy * [g_win backingScaleFactor] / CH) : (float)(dy * 3);
    lines = (int)scroll_acc;
    if (!lines) return;
    if(mac_browser_ui_scroll(mx,my,-lines)){scroll_acc-=lines;return;}
    scroll_acc -= lines;
    if (tb->t.m_mouse && !(mods & M_SHIFT)) {
        for (i = 0; i < abs(lines); i++) mouse_report(lines > 0 ? 64 : 65, 1, 0, mx, my, mods);
    } else if (tb->t.alt) {
        if (mac_input_held(tb)) return;
        /* alternate scroll: less, man, git log scroll with the wheel */
        const char *seq = lines > 0 ? (tb->t.m_appcursor ? "\x1bOA" : "\x1b[A") : (tb->t.m_appcursor ? "\x1bOB" : "\x1b[B");
        for (i = 0; i < abs(lines); i++) tab_write(tb, seq, 3);
    } else {
        scroll_view(lines);
    }
}
- (void)scrollWheel:(NSEvent *)e {
    int mx,my;view_px(e,&mx,&my);
    [self scrollAtX:mx y:my delta:[e scrollingDeltaY] precise:[e hasPreciseScrollingDeltas] mods:ev_mods(e)];
}

/* ── drops: files type their paths ── */

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info { (void)info; return NSDragOperationCopy; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
    [NSApp activateIgnoringOtherApps:YES];
    if (paste_file_urls([info draggingPasteboard])) return YES;
    {
        NSString *s = [[info draggingPasteboard] stringForType:NSPasteboardTypeString];
        if (s) paste_utf8([s UTF8String]);
        return s != nil;
    }
}

/* ── menu actions ── */

- (void)copy:(id)sender { if(mac_browser_copy_edit())return; if (active_tab() && active_tab()->kind==TK_BROWSER && active_tab()->browser_native) { mac_browser_edit(@selector(copy:),sender); return; } (void)sender; sel_copy(); g_sel_on = 0; plat_invalidate(); }
- (void)paste:(id)sender { if(g_cutie_focus_id){paste();return;} NSString *text=[[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];if(text && mac_browser_insert(text))return; if (active_tab() && active_tab()->kind==TK_BROWSER && active_tab()->browser_native) { mac_browser_edit(@selector(paste:),sender); return; } (void)sender; paste(); }
- (void)newTab:(id)sender {
    (void)sender;
    if (g_tracing) {
        char m[96];
        snprintf(m, sizeof(m), "Cmd+T: event %.2f ms ago", uptime_ms() - [[NSApp currentEvent] timestamp] * 1000.0);
        trace(m);
        g_trace_commit = 1;
        g_trace_tab_t = uptime_ms();
    }
    new_tab_mac();
    trace("new tab made");
}
- (void)newBrowser:(id)sender { (void)sender; mac_browser_open_welcome(); }
- (void)browserAddress:(id)sender { (void)sender; mac_browser_address(); }
- (void)browserMode:(id)sender { (void)sender; mac_browser_command(0); }
- (void)browserBack:(id)sender { (void)sender; mac_browser_command(1); }
- (void)browserForward:(id)sender { (void)sender; mac_browser_command(2); }
- (void)browserReload:(id)sender { (void)sender; mac_browser_command(3); }
- (void)closeTab:(id)sender { (void)sender; user_close_tab(g_active); }
- (void)nextTab:(id)sender { (void)sender; if (g_ntabs) activate((g_active + 1) % g_ntabs); }
- (void)prevTab:(id)sender { (void)sender; if (g_ntabs) activate((g_active + g_ntabs - 1) % g_ntabs); }
- (void)jumpAttention:(id)sender { (void)sender; jump_to_attention(); }
- (void)prevMark:(id)sender { (void)sender; jump_mark(-1); }
- (void)nextMark:(id)sender { (void)sender; jump_mark(1); }
- (void)toggleDockSummary:(id)sender {
    (void)sender;
    g_mac_dock_summary = !g_mac_dock_summary;
    mac_dock_update(1);
    config_save();
    plat_settings_tell();
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if ([item action] == @selector(copy:)) return g_sel_on || mac_browser_editing() || (active_tab() && active_tab()->kind==TK_BROWSER && active_tab()->browser_native);
    if ([item action] == @selector(browserMode:) || [item action] == @selector(browserBack:) ||
        [item action] == @selector(browserForward:) || [item action] == @selector(browserReload:))
        return active_tab() && active_tab()->kind==TK_BROWSER;
    if ([item action] == @selector(toggleDockSummary:))
        [item setState:g_mac_dock_summary ? NSControlStateValueOn : NSControlStateValueOff];
    return YES;
}

@end

#include "hardcore_browser_mac.m"
#include "hardcore_recording.m"
static void record_jobs_changed(void) {update_busy(0);plat_invalidate();}
#include "hardcore_recording_jobs.m"
#include "hardcore_recording_library.m"
#include "hardcore_camera_panel.m"
static void mac_music_changed(void) {update_busy(0);plat_invalidate();}
#include "hardcore_mac_music.m"
#include "hardcore_music_panel.m"

/* ── window and app ────────────────────────────────────────────────── */

static void focus_changed(int focused) {
    Tab *tb = input_tab();
    int cutie_reports_out=g_cutie_focus_id && tb!=active_tab();
    g_focused = focused;
    if(!focused){mac_music_cancel_press();mac_camera_cancel_press();mac_tv_drag_end();mac_cutie_blur();chrome_preview_cancel();g_mouse_x=g_mouse_y=-1;settings_point();}
    if (g_focused && tb) tb->attention = 0;
    if (tb && tb->t.m_focus && (g_focused || !cutie_reports_out)) tab_write(tb, g_focused ? "\x1b[I" : "\x1b[O", 3);
    mac_dock_update(0);
    plat_invalidate();
}

@interface HCDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation HCDelegate
- (void)applicationWillFinishLaunching:(NSNotification *)n { (void)n; trace("applicationWillFinishLaunching"); }
- (void)applicationDidFinishLaunching:(NSNotification *)n { (void)n; trace("applicationDidFinishLaunching"); }
- (void)windowWillMiniaturize:(NSNotification *)n {(void)n;mac_music_cancel_press();mac_camera_cancel_press();mac_record_hide_pointer();mac_tv_drag_end();mac_cutie_blur();chrome_preview_cancel();g_mouse_x=g_mouse_y=-1;settings_point();}
- (void)windowDidBecomeKey:(NSNotification *)n { (void)n; trace("window key"); focus_changed(1); }
- (void)windowDidResignKey:(NSNotification *)n { (void)n;mac_record_hide_pointer(); if (g_chrome_drag) chrome_drag_end(); focus_changed(0); }
- (void)windowDidEnterFullScreen:(NSNotification *)n {
    (void)n; g_mac_title_hover = 0; mac_title_sync_buttons(); plat_invalidate();
}
- (void)windowDidExitFullScreen:(NSNotification *)n {
    (void)n; g_mac_title_hover = 0; mac_title_sync_buttons(); plat_invalidate();
}
- (BOOL)windowShouldClose:(NSWindow *)w {
    (void)w;
    if (!confirm_close(-1)) return NO;
    g_quitting = 1;
    [NSApp terminate:nil];
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    int i;
    (void)app;
    if (g_tracing) {
        char m[96];
        snprintf(m, sizeof(m), "applicationShouldTerminate: event %.2f ms ago", uptime_ms() - [[NSApp currentEvent] timestamp] * 1000.0);
        trace(m);
    }
    if (mac_record_active()) {
        NSAlert *alert=[[[NSAlert alloc] init] autorelease];
        alert.messageText=@"Save or discard the camera recording before quitting.";
        [alert runModal];return NSTerminateCancel;
    }
    if(mac_record_exports_active()) {
        NSAlert *alert=[[[NSAlert alloc] init] autorelease];
        alert.messageText=@"Exports are still running.";
        alert.informativeText=@"You can hide the camera or Clips TV while they finish. Reopen Clips to see progress.";
        [alert runModal];return NSTerminateCancel;
    }
    if (!g_quitting && !confirm_close(-1)) return NSTerminateCancel;
    trace("confirmed");
    mac_music_shutdown();
    /* Gone now: AppKit's own teardown took ~60 ms (measured), for nothing a
       terminal needs, and the window server drops a dead process's windows
       faster than an orderOut round trip. The window's frame is saved
       whenever it changes. */
    remember_dir(active_tab());  /* the next window opens where this one closed */
    config_save();
    /* Reload keeps Chrome targets alive; an actual quit closes this window's
       pages and waits briefly for their asynchronous close acknowledgments. */
    hc_chrome_close_pages_main();
    for (i = 0; i < g_ntabs; i++)   /* the ptys close as we exit, which hangs them up too */
        if (g_tabs[i]->pid > 0 && !g_tabs[i]->exited) kill(g_tabs[i]->pid, SIGHUP);
    if (g_spare && g_spare->pid > 0) kill(g_spare->pid, SIGHUP);
    trace("exiting");
    _exit(0);
}
- (void)applicationWillTerminate:(NSNotification *)n {
    int i;
    (void)n;
    config_save();
    for (i = 0; i < g_ntabs; i++)   /* hang up the shells: closing the ptys does it too, this is just sure */
        if (g_tabs[i]->pid > 0 && !g_tabs[i]->exited) kill(g_tabs[i]->pid, SIGHUP);
    if (g_spare && g_spare->pid > 0) kill(g_spare->pid, SIGHUP);
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)v {
    (void)app; (void)v;
    [g_win makeKeyAndOrderFront:nil];
    return YES;
}
/* a folder dropped on the Dock icon, or `open -a hardcore DIR`: a tab there */
- (void)application:(NSApplication *)app openURLs:(NSArray<NSURL *> *)urls {
    (void)app;
    for (NSURL *u in urls) {
        BOOL dir = NO;
        NSString *p = [u path];
        if (![[NSFileManager defaultManager] fileExistsAtPath:p isDirectory:&dir]) continue;
        if (!dir) p = [p stringByDeletingLastPathComponent];
        if (g_ntabs && active_tab()) set_cwd(&active_tab()->t, [p fileSystemRepresentation]);
        else snprintf(g_start_dir, sizeof(g_start_dir), "%s", [p fileSystemRepresentation]);
        if (g_ntabs) new_tab_here();
    }
}
@end

static NSMenuItem *menu_item(NSMenu *m, NSString *title, SEL action, NSString *key, NSEventModifierFlags mods) {
    NSMenuItem *it = [m addItemWithTitle:title action:action keyEquivalent:key];
    [it setKeyEquivalentModifierMask:mods];
    if ([g_view respondsToSelector:action]) [it setTarget:g_view];   /* ours: the view, key window or not */
    return it;
}

static void build_menus(void) {
    NSMenu *bar = [[NSMenu alloc] init], *app = [[NSMenu alloc] initWithTitle:@"hardcore"];
    NSMenu *shell = [[NSMenu alloc] initWithTitle:@"Shell"], *edit = [[NSMenu alloc] initWithTitle:@"Edit"];
    NSMenu *win = [[NSMenu alloc] initWithTitle:@"Window"];
    NSMenu *browser = [[NSMenu alloc] initWithTitle:@"Browser"];
    NSEventModifierFlags C = NSEventModifierFlagCommand, CS = C | NSEventModifierFlagShift;
    NSString *up = [NSString stringWithFormat:@"%C", (unichar)NSUpArrowFunctionKey];
    NSString *down = [NSString stringWithFormat:@"%C", (unichar)NSDownArrowFunctionKey];
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
    menu_item(app, @"About hardcore", @selector(orderFrontStandardAboutPanel:), @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    menu_item(app, @"Hide hardcore", @selector(hide:), @"h", C);
    menu_item(app, @"Hide Others", @selector(hideOtherApplications:), @"h", C | NSEventModifierFlagOption);
    menu_item(app, @"Show All", @selector(unhideAllApplications:), @"", 0);
    [app addItem:[NSMenuItem separatorItem]];
    menu_item(app, @"Quit hardcore", @selector(terminate:), @"q", C);
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:shell];
    menu_item(shell, @"New Tab", @selector(newTab:), @"t", C);
    menu_item(shell, @"Close Tab", @selector(closeTab:), @"w", C);
    [shell addItem:[NSMenuItem separatorItem]];
    menu_item(shell, @"Next Tab", @selector(nextTab:), @"]", CS);
    menu_item(shell, @"Previous Tab", @selector(prevTab:), @"[", CS);
    menu_item(shell, @"Jump to the Tab That Needs You", @selector(jumpAttention:), @"j", C);
    [shell addItem:[NSMenuItem separatorItem]];
    menu_item(shell, @"Previous Prompt", @selector(prevMark:), up, C);
    menu_item(shell, @"Next Prompt", @selector(nextMark:), down, C);
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:edit];
    menu_item(edit, @"Copy", @selector(copy:), @"c", C);
    menu_item(edit, @"Paste", @selector(paste:), @"v", C);
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:win];
    menu_item(win, @"Minimize", @selector(performMiniaturize:), @"m", C);
    menu_item(win, @"Zoom", @selector(performZoom:), @"", 0);
    menu_item(win, @"Enter Full Screen", @selector(toggleFullScreen:), @"f", C | NSEventModifierFlagControl);
    [win addItem:[NSMenuItem separatorItem]];
    menu_item(win, @"Show Active Tab in Dock Icon", @selector(toggleDockSummary:), @"", 0);
    [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:browser];
    menu_item(browser,@"New Browser Tab",@selector(newBrowser:),@"b",CS);
    menu_item(browser,@"Open Address…",@selector(browserAddress:),@"l",C);
    menu_item(browser,@"Switch Hardcore / Native",@selector(browserMode:),@"m",CS);
    [browser addItem:[NSMenuItem separatorItem]];
    menu_item(browser,@"Back",@selector(browserBack:),@"[",C);
    menu_item(browser,@"Forward",@selector(browserForward:),@"]",C);
    menu_item(browser,@"Reload Page",@selector(browserReload:),@"r",C);
    [NSApp setMainMenu:bar];
    [NSApp setWindowsMenu:win];
}

/* ── test channel ───────────────────────────────────────────────────
   HARDCORE_CTL=/path/fifo: lines of "type TEXT" (\n is Enter, \e is ESC),
   "cmd newtab|nexttab|closetab|jump|tab N|hover X,Y|click X,Y|resize W,H|sel Y0 X0 Y1 X1" (click: the
   tab column's settings only), "cmd pointer down|drag|up X,Y" (content-origin
   selection drags, in backing pixels), "snap OUT.bmp", and
   "winid OUT" (the window number, for screencapture -l, which also gets the
   traffic lights), and "key KEYCODE FLAGS CHARS [UNMODIFIED]": a synthetic
   key event through the same keyDown: path as the keyboard (FLAGS in hex,
   NSEventModifierFlags plus the device bits, e.g. 0x80120 for left Option;
   CHARS may use \xHH and \uHHHH, "-" is none).
   It drives a window that isn't focused, so tests never take your keyboard
   (the Mac side of bench/termbench.exe's send / cmd / snap). */

static NSString *ctl_unescape(const char *s) {
    NSMutableString *out = [NSMutableString string];
    while (*s) {
        unsigned v;
        if (s[0] == '\\' && s[1] == 'x' && sscanf(s + 2, "%2x", &v) == 1) { [out appendFormat:@"%C", (unichar)v]; s += 4; }
        else if (s[0] == '\\' && s[1] == 'u' && sscanf(s + 2, "%4x", &v) == 1) { [out appendFormat:@"%C", (unichar)v]; s += 6; }
        else {
            NSString *one = [[[NSString alloc] initWithBytes:s length:1 + ((*s & 0xE0) == 0xC0) + 2 * ((*s & 0xF0) == 0xE0) + 3 * ((*s & 0xF8) == 0xF0)
                                                     encoding:NSUTF8StringEncoding] autorelease];
            [out appendString:one ? one : @"?"];
            s += [one lengthOfBytesUsingEncoding:NSUTF8StringEncoding] ? [one lengthOfBytesUsingEncoding:NSUTF8StringEncoding] : 1;
        }
    }
    return out;
}

/* Native renderer inspection, available only through the opt-in local test FIFO. */
static void ctl_chrome(const char *path) {
    Tab *tb = display_tab();
    if (!tb) return;
    Term *t = &tb->t;
    chrome_prepare_display(t);
    NSMutableArray *source = [NSMutableArray array], *display = [NSMutableArray array];
    NSMutableArray *markers = [NSMutableArray array], *tab_rows = [NSMutableArray array];
    for (int y = 0; y < t->rows; y++) {
        const Cell *cells;
        int n, wrapped;
        term_line(t, t->hcount - t->view + y, &cells, &n, &wrapped);
        NSMutableString *a = [NSMutableString string], *b = [NSMutableString string];
        for (int x = 0; x < t->cols; x++) {
            uint32_t cp = x < n && cells[x].ch ? cells[x].ch : ' ';
            char utf[8]; int count = utf8_encode(cp, utf);
            NSString *part = [[[NSString alloc] initWithBytes:utf length:count encoding:NSUTF8StringEncoding] autorelease];
            [a appendString:part ?: @"?"];
            cp = g_display.cells ? g_display.cells[y*t->cols+x].ch : cp;
            count = utf8_encode(cp ? cp : ' ', utf);
            part = [[[NSString alloc] initWithBytes:utf length:count encoding:NSUTF8StringEncoding] autorelease];
            [b appendString:part ?: @"?"];
        }
        [source addObject:a]; [display addObject:b];
        [markers addObject:@(g_display.cells ? g_display.markers[y] : -1)];
    }
    for (int i = 0; i < g_ntabs; i++) [tab_rows addObject:@(g_row_y[i])];
    NSMutableDictionary *state = [@{
        @"mode":@(g_tab_underlay), @"cols":@(t->cols), @"rows":@(t->rows),
        @"dock":@{@"x":@(sidebar_x()/CW), @"y":@(chrome_top_row()), @"width":@(SB_COLS),
                  @"height":@(chrome_bottom_row()-chrome_top_row())},
        @"cell":@{@"width":@(CW), @"height":@(CH)}, @"scale":@(g_scale), @"hovered":@(chrome_hovered()),
        @"settings_open":@(g_settings_open), @"settings_tab":@(g_settings_tab),
        @"reflow_enabled":@(g_chrome_reflow), @"reflow_markers":@(g_chrome_debug), @"reflow_changed":@(g_display.changed),
        @"source":source, @"display":display, @"markers":markers, @"tab_rows":tab_rows
    } mutableCopy];
    NSMutableArray *button_rects = [NSMutableArray array];
    for (int i = 0; i < 3; i++) {
        NSRect r = mac_title_button_rect(i);
        [button_rects addObject:@{@"x":@(NSMinX(r)), @"y":@(NSMinY(r)),
                                 @"width":@(NSWidth(r)), @"height":@(NSHeight(r))}];
    }
    state[@"titlebar"] = @{@"height":@(mac_title_height()), @"hovered":@(g_mac_title_hover),
        @"buttons_visible":@(![[g_win standardWindowButton:NSWindowCloseButton] isHidden]),
        @"button_rects":button_rects, @"selection_drag":@(g_sel_drag), @"selection_on":@(g_sel_on)};
    NSCursor *pointer=[NSCursor currentCursor];
    state[@"pointer_cursor"]=pointer==[NSCursor arrowCursor]?@"arrow":
        pointer==[NSCursor IBeamCursor]?@"ibeam":pointer==[NSCursor openHandCursor]?@"open-hand":
        pointer==[NSCursor closedHandCursor]?@"closed-hand":pointer==[NSCursor pointingHandCursor]?@"pointing-hand":
        pointer==mac_tv_resize_cursor()?@"resize":@"other";
    NSMutableArray *header_controls=[NSMutableArray array],*header_hits=[NSMutableArray array];
    for(int i=0;i<CHROME_HEADER_CONTROL_COUNT;i++) {
        const ChromeHeaderControl *c=CHROME_HEADER_CONTROLS+i;
        if(c->row>=g_chrome.grip_rows)continue;
        const wchar_t *text=c->hit==HIT_CAF && !mac_caf_active()?L"caf":c->label;
        NSString *label=[[[NSString alloc] initWithBytes:text length:wcslen(text)*sizeof(*text)
            encoding:NSUTF32LittleEndianStringEncoding] autorelease];
        [header_controls addObject:@{@"name":@(c->name),@"label":label?:@"",@"row":@(c->row),
            @"col":@(c->col),@"width":@(c->width),@"hit":@(c->hit)}];
    }
    if(mac_music_menu_visible() && g_chrome.grip_rows>2)for(int i=0;i<CHROME_MUSIC_CONTROL_COUNT;i++) {
        const ChromeHeaderControl *c=CHROME_MUSIC_CONTROLS+i;
        [header_controls addObject:@{@"name":[@"music_" stringByAppendingString:@(c->name)],@"row":@(c->row),
            @"col":@(c->col),@"width":@(c->width),@"hit":@(c->hit)}];
    }
    for(int row=0;row<g_chrome.grip_rows;row++) {
        NSMutableArray *hits=[NSMutableArray array];
        for(int col=0;col<SB_COLS;col++) {
            int index=-1;
            [hits addObject:@(hit_tabbar(sidebar_x()+col*CW+CW/2,(g_chrome.top+row)*CH+CH/2,&index))];
        }
        [header_hits addObject:hits];
    }
    state[@"header_controls"]=header_controls;state[@"header_hits"]=header_hits;
    int cursor_col, cursor_row;
    display_cursor_position(t, &cursor_col, &cursor_row);
    state[@"cursor"] = @{@"source":@[@(t->cx), @(t->cy)],
        @"display":@[@(cursor_col), @(cursor_row)], @"visible":@(t->m_cursor_vis)};
    state[@"fontkey"] = [NSString stringWithFormat:@"%c", g_font->key];
    state[@"schemekey"] = [NSString stringWithFormat:@"%c", g_scheme->key];
    state[@"shown_schemekey"] = [NSString stringWithFormat:@"%c", g_shown->key];
    state[@"motion"] = @{@"phase":@(g_chrome_motion.phase), @"alpha":@(g_chrome_motion_alpha),
        @"side":@(g_chrome_motion.side), @"row":@(g_chrome_motion.row),
        @"target_side":@(g_chrome_motion.target_side), @"target_row":@(g_chrome_motion.target_row),
        @"searches":@(g_chrome_motion.searches), @"commits":@(g_chrome_motion.commits)};
    NSMutableArray *menu_rows = [NSMutableArray array];
    for (int y = g_chrome.top; y < g_chrome.bottom; y++) {
        int left = 0, right = 0;
        chrome_intrusion_span(y, &left, &right);
        NSMutableArray *mapping = [NSMutableArray array];
        for (int x = 0; x < SB_COLS; x++) [mapping addObject:@(chrome_menu_source_col(y, x))];
        [menu_rows addObject:@{@"row":@(y), @"left":@(left), @"right":@(right), @"map":mapping}];
    }
    state[@"menu_rows"] = menu_rows;
    NSMutableArray *tvs=[NSMutableArray array];
    for(int i=0;i<HC_TV_COUNT;i++) {
        ChromeTv *tv=g_tvs+i;HcTvRect r=tv->state.rect;
        NSMutableArray *maprows=[NSMutableArray array];
        for(int y=0;y<tv->frame.count;y++) {
            NSMutableArray *mapping=[NSMutableArray array];
            ChromePackRow *packed=tv->frame.rows+y;
            for(int x=0;x<tv->frame.cols;x++)[mapping addObject:@(tv->frame.raised?x:packed->actionable[x]?packed->src[x]:-1)];
            [maprows addObject:@{@"row":@(tv->frame.top+y),@"left":@(r.x+packed->blocked_left),
                @"right":@(r.x+packed->blocked_right),@"map":mapping}];
        }
        CutieCache *cache=g_cuties+i;HcCutieProjection *project=&cache->view;
        NSMutableArray *source=[NSMutableArray array],*content=[NSMutableArray array];
        if(i>=HC_TV_CUTIE_FIRST && cache->valid)for(int n=0;n<project->cols*project->rows;n++) {
            [source addObject:@(project->source[n])];Cell *c=project->cells+n;
            [content addObject:@{@"ch":@(c->ch),@"fg":@(c->fg),@"bg":@(c->bg),@"attr":@(c->attr),@"wide":@(c->wflag),@"link":@(c->link)}];
        }
        [tvs addObject:@{@"name":i>=HC_TV_CUTIE_FIRST?@"cutie":i?@"caf":@"settings",
            @"highlighted":@(chrome_cutie_highlighted(i)),@"manual":@(tv->manual),@"source_tab_id":@(tv->tab_id),@"focused":@(tv->tab_id>0 && g_cutie_focus_id==tv->tab_id),
            @"body":@{@"x":@(r.x+1),@"y":@(r.y+1),@"width":@(r.w-2),@"height":@(r.h-2)},
            @"source_cols":@(cache->source_cols),@"source_first_line":@(cache->first_line),@"source_hcount":@(cache->hcount),
            @"source_map":source,@"content":content,@"cursor":@(project->cursor),@"scroll":@(tv->scroll),@"menu_rows":maprows,@"open":@(tv->state.open),@"placed":@(tv->state.placed),
            @"rect":@{@"x":@(r.x),@"y":@(r.y),@"width":@(r.w),@"height":@(r.h)},
            @"alpha":@(tv->state.alpha*tv->motion.alpha),@"countdown":@(hc_tv_seconds_left(&tv->state,GetTickCount())),
            @"motion":@{@"phase":@(tv->motion.phase),@"alpha":@(tv->motion.alpha),
                @"target":@{@"x":@(tv->motion.target.x),@"y":@(tv->motion.target.y),@"width":@(tv->motion.target.w),@"height":@(tv->motion.target.h)}},
            @"placement_candidates":@(tv->placement.candidates)}];
    }
    state[@"monitor_consumed"]=@(g_mac_test_monitor_consumed);
    state[@"tv_drag_slot"]=@(g_tv_drag_slot);state[@"tv_drag_mode"]=@(g_tv_drag_mode);
    state[@"input_tab_id"]=@(input_tab()?input_tab()->id:0);
    state[@"tvs"]=tvs;state[@"caf_active"]=@(mac_caf_active());
    state[@"camera"]=mac_camera_status();state[@"exports"]=mac_record_jobs();
    state[@"music"]=mac_music_panel_status();
    Tab *selected=active_tab(),*shown=display_tab();
    state[@"preview"]=@{@"active_id":@(selected?selected->id:0),@"display_id":@(shown?shown->id:0),
        @"preview_id":@(g_tab_preview.target_id)};
    ChromeLayout settings=g_tvs[HC_TV_SETTINGS].layout;
    state[@"pages"] = @{
        @"tabs":@{@"offset":@(g_tab_offset), @"capacity":@(g_chrome.tab_capacity), @"total":@(g_ntabs), @"row":@(g_chrome.tab_row)},
        @"settings":@{@"offset":@(g_setting_offset[g_settings_tab]), @"capacity":@(settings.list_capacity), @"total":@(chrome_item_total()), @"row":@(settings.list_row)},
        @"settings_row":@(g_chrome.top), @"settings_x":@(g_tvs[HC_TV_SETTINGS].state.rect.x), @"section_row":@(settings.section_row)
    };
    NSMutableArray *font_buttons = [NSMutableArray array];
    if (g_settings_open && g_settings_tab == 0) {
        for (int n = 0; n < settings.list_count; n++) {
            const FontMenuRow *row = &g_font_menu_rows[g_setting_offset[0] + n];
            for (int i = 0; i < row->count; i++) {
                const FontMenuSize *size = &row->sizes[i];
                const Font *font = &FONTS[size->font];
                [font_buttons addObject:@{
                    @"fontkey":[NSString stringWithFormat:@"%c", font->key],
                    @"name":[NSString stringWithUTF8String:font->name],
                    @"width":@(cell_w(font)), @"height":@(cell_h(font)),
                    @"row":@(settings.list_row + n), @"left":@(size->left), @"right":@(size->right)
                }];
            }
        }
    }
    state[@"font_buttons"] = font_buttons;
    NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    [data writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
    [state release];
}

static void ctl_line(char *s) {
    Tab *tb = active_tab();
    if (!strncmp(s, "type ", 5) && tb) {
        char *o = s, *p;
        for (p = s + 5; *p; p++) {
            if (*p == '\\' && p[1] == 'n') { *o++ = '\r'; p++; }
            else if (*p == '\\' && p[1] == 'e') { *o++ = 0x1B; p++; }
            else if (*p == '\\' && p[1] == '\\') { *o++ = '\\'; p++; }
            else *o++ = *p;
        }
        if (tb->help) { int at; for (at = 0; at < g_ntabs; at++) if (g_tabs[at] == tb) { replace_tab(at); break; } }
        else if (!mac_input_held(tb)) tab_write(tb, s, (int)(o - s));
    } else if (!strncmp(s, "cmd ", 4)) {
        const char *c = s + 4;
        if(!strncmp(c,"record-fixture ",15))mac_record_fixture([NSString stringWithUTF8String:c+15]);
        else if(!strcmp(c,"clips"))mac_record_library();
        else if(!strcmp(c,"music"))mac_music_toggle();
        else if(!strncmp(c,"focus ",6))focus_changed(atoi(c+6));
        else if(!strcmp(c,"preview-cancel"))chrome_preview_cancel();
        else if(!strncmp(c,"preview-event ",14)) {
            const char *event=c+14;
            if(!strcmp(event,"exit")) {
                NSEvent *e=[NSEvent enterExitEventWithType:NSEventTypeMouseExited location:NSMakePoint(-1,-1)
                    modifierFlags:0 timestamp:0 windowNumber:[g_win windowNumber] context:nil eventNumber:0 trackingNumber:0 userData:NULL];
                [g_view mouseExited:e];
            }
            else if(!strcmp(event,"resign"))[(HCDelegate *)[g_win delegate] windowDidResignKey:
                [NSNotification notificationWithName:NSWindowDidResignKeyNotification object:g_win]];
            else if(!strcmp(event,"minimize"))[(HCDelegate *)[g_win delegate] windowWillMiniaturize:
                [NSNotification notificationWithName:NSWindowWillMiniaturizeNotification object:g_win]];
            else if(!strcmp(event,"monitor-key")) {
                NSEvent *e=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0
                    windowNumber:[g_win windowNumber] context:nil characters:@"x" charactersIgnoringModifiers:@"x" isARepeat:NO keyCode:0];
                mac_preview_event(e);
            }
        }
        else if (!strcmp(c, "newtab")) { g_trace_tab_t = uptime_ms(); trace("ctl newtab"); new_tab_mac(); }
        else if (!strcmp(c, "nexttab") && g_ntabs) activate((g_active + 1) % g_ntabs);
        else if (!strcmp(c, "closetab")) close_tab(g_active);
        else if (!strcmp(c, "jump")) jump_to_attention();
        else if (!strncmp(c, "tab ", 4)) activate(atoi(c + 4));
        else if (!strncmp(c, "chrome ", 7)) ctl_chrome(c+7);
        else if (!strncmp(c, "feed ", 5) && tb) {
            NSData *bytes = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:c+5]];
            if (bytes && [bytes length] <= 1024*1024) {
                g_sel_on = g_sel_drag = 0;
                vt_feed(&tb->t, [bytes bytes], (int)[bytes length]);
            }
        } else if (!strncmp(c, "map ", 4) && tb) {
            int x, y; char path[4096];
            if (sscanf(c+4, "%d,%d %4095[^\n]", &x, &y, path) == 3) {
                chrome_prepare_display(&tb->t);
                int col, row;
                mouse_cell(term_ox() + x*CW + CW/2, term_oy() + y*CH + CH/2, &col, &row);
                NSData *data = [NSJSONSerialization dataWithJSONObject:@{@"col":@(col), @"row":@(row)} options:0 error:nil];
                [data writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
            }
        } else if (!strncmp(c, "state ", 6)) {
            NSMutableArray *tabs = [NSMutableArray array];
            for (int k = 0; k < g_ntabs; k++) {
                Tab *t = g_tabs[k];
                [tabs addObject:@{@"pid":@(t->pid), @"fd":@(t->fd), @"view":@(t->t.view), @"hist":@(t->t.hcount),
                                  @"alt":@(t->t.alt), @"cols":@(t->t.cols), @"rows":@(t->t.rows),
                                  @"cx":@(t->t.cx), @"cy":@(t->t.cy), @"parser":@(t->t.st),
                                  @"browser_scroll":@(browser_for(t)?browser_for(t)->scroll:0),
                                  @"id":@(t->id), @"activity":@(t->activity), @"attention":@(t->attention),
                                  @"program":[NSString stringWithUTF8String:t->program] ?: @""}];
            }
            NSData *data = [NSJSONSerialization dataWithJSONObject:@{@"pid":@(getpid()), @"active":@(g_active),
                @"frame":NSStringFromRect([g_win frame]), @"tabs":tabs,
                @"first_responder":[NSString stringWithFormat:@"%p:%@",[g_win firstResponder],NSStringFromClass([[g_win firstResponder] class])],
                @"key_window":@([g_win isKeyWindow])} options:0 error:nil];
            [data writeToFile:[NSString stringWithUTF8String:c+6] atomically:YES];
        } else if (!strncmp(c, "view ", 5) && tb) {
            tb->t.view = atoi(c+5); if (tb->t.view > tb->t.hcount) tb->t.view = tb->t.hcount;
        }
        else if (!strncmp(c, "sel ", 4) && tb) {   /* sel Y0 X0 Y1 X1: select, in screen rows */
            int y0, x0, y1, x1;
            if (sscanf(c + 4, "%d %d %d %d", &y0, &x0, &y1, &x1) == 4) {
                g_sel_a_row = term_abs_line(&tb->t, y0); g_sel_a_col = x0;
                g_sel_b_row = term_abs_line(&tb->t, y1); g_sel_b_col = x1;
                g_sel_on = 1;
            }
        } else if (!strncmp(c, "resize ", 7)) {   /* content size in points, as a drag would leave it */
            NSRect fr = [g_win frameRectForContentRect:NSMakeRect(0, 0, atoi(c + 7), strchr(c + 7, ',') ? atoi(strchr(c + 7, ',') + 1) : 400)];
            fr.origin = [g_win frame].origin;
            [g_win setFrame:fr display:YES];
        } else if (!strncmp(c, "hover ", 6)) {
            g_mouse_x = atoi(c + 6);
            g_mouse_y = strchr(c + 6, ',') ? atoi(strchr(c + 6, ',') + 1) : -1;
            mac_title_hover_at(g_mouse_x, g_mouse_y);
            chrome_preview_pointer(g_mouse_x,g_mouse_y);
            settings_point();
        } else if(!strncmp(c,"wheel ",6)) {
            int x,y,delta;
            if(sscanf(c+6,"%d,%d,%d",&x,&y,&delta)==3)
                [g_view scrollAtX:x y:y delta:delta precise:NO mods:0];
            return;
        } else if (!strncmp(c, "pointer ", 8)) {
            char action[16]; int x, y;
            if (sscanf(c+8, "%15s %d,%d", action, &x, &y) == 3) {
                NSEventType kind = !strcmp(action,"right-down")?NSEventTypeRightMouseDown:
                                   !strcmp(action,"right-up")?NSEventTypeRightMouseUp:
                                   !strcmp(action,"middle-down")?NSEventTypeOtherMouseDown:
                                   !strcmp(action,"middle-up")?NSEventTypeOtherMouseUp:
                                   (!strcmp(action,"down") || !strcmp(action,"monitor-down")) ? NSEventTypeLeftMouseDown :
                                   (!strcmp(action, "drag") || !strcmp(action,"monitor-drag")) ? NSEventTypeLeftMouseDragged :
                                   (!strcmp(action,"move") || !strcmp(action,"monitor") || !strcmp(action,"cursor") || !strcmp(action,"flags")) ? NSEventTypeMouseMoved : NSEventTypeLeftMouseUp;
                CGFloat scale = [g_win backingScaleFactor];
                NSPoint p = [(NSView *)g_view convertPoint:NSMakePoint(x/scale, y/scale) toView:nil];
                NSEvent *e = [NSEvent mouseEventWithType:kind location:p modifierFlags:0 timestamp:0
                    windowNumber:[g_win windowNumber] context:nil eventNumber:0 clickCount:1 pressure:1];
                if(!strcmp(action,"monitor-down") || !strcmp(action,"monitor-up") || !strcmp(action,"monitor-drag")) {
                    g_mac_test_monitor_consumed=mac_title_event(e)==nil;return;
                }
                /* Match AppKit hit routing when a browser surface, rather
                   than the Metal terminal view, owns this point. */
                if(tb && tb->kind==TK_BROWSER && tb->browser_native &&
                   !over_chrome(x,y) && !mac_title_contains(x,y) && !mac_browser_ui_contains(x,y) &&
                   strcmp(action,"monitor") && strcmp(action,"cursor") && strcmp(action,"flags")) {
                    NSView *surface=[browser_for(tb) documentView];
                    if(surface && mac_title_event(e)) {
                        if(kind==NSEventTypeMouseMoved)[surface mouseMoved:e];
                        else if(kind==NSEventTypeLeftMouseDown)[surface mouseDown:e];
                        else if(kind==NSEventTypeLeftMouseUp)[surface mouseUp:e];
                        else if(kind==NSEventTypeLeftMouseDragged)[surface mouseDragged:e];
                        else if(kind==NSEventTypeRightMouseDown)[surface rightMouseDown:e];
                        else if(kind==NSEventTypeRightMouseUp)[surface rightMouseUp:e];
                        else if(kind==NSEventTypeOtherMouseDown)[surface otherMouseDown:e];
                        else if(kind==NSEventTypeOtherMouseUp)[surface otherMouseUp:e];
                        return;
                    }
                }
                if(kind==NSEventTypeMouseMoved) {
                    if(!strcmp(action,"monitor"))mac_title_event(e);
                    else if(!strcmp(action,"cursor"))[g_view cursorUpdate:e];
                    else if(!strcmp(action,"flags"))[g_view flagsChanged:e];
                    else[g_view mouseMoved:e];
                    return; /* Exercise real invalidation; do not mask a missed hover-exit redraw. */
                }
                /* Allow grid controls at the top edge, but never enter AppKit's
                   modal window-drag loop from a synthetic titlebar down. */
                int idx = -1, hit = hit_tabbar(x, y, &idx);
                if(kind==NSEventTypeRightMouseDown && (over_chrome(x,y) || g_tv_drag_slot>=0))[g_view rightMouseDown:e];
                else if(kind==NSEventTypeRightMouseUp)[g_view rightMouseUp:e];
                else if(kind==NSEventTypeOtherMouseDown)[g_view otherMouseDown:e];
                else if(kind==NSEventTypeOtherMouseUp)[g_view otherMouseUp:e];
                else if (kind == NSEventTypeLeftMouseDown && hit != HIT_BAR &&
                    (!mac_title_contains(x, y) || hit != HIT_NONE)) [g_view mouseDown:e];
                else if (kind == NSEventTypeLeftMouseDragged) [g_view mouseDragged:e];
                else if (kind == NSEventTypeLeftMouseUp) [g_view mouseUp:e];
            }
        } else if (!strncmp(c, "click ", 6)) {
            int x=atoi(c+6),y=strchr(c+6,',')?atoi(strchr(c+6,',')+1):-1,idx=-1;
            int hit=hit_tabbar(x,y,&idx);
            if(!mac_header_action(hit))settings_click(x,y);
        }
        plat_invalidate();
    } else if (!strncmp(s, "key ", 4)) {
        unsigned kc = 0, flags = 0;
        char chars[64] = "", plain[64] = "";
        NSEvent *e;
        if (sscanf(s + 4, "%u %x %63s %63s", &kc, &flags, chars, plain) < 2) return;
        if (!strcmp(chars, "-")) chars[0] = 0;
        e = [NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:flags timestamp:0
                         windowNumber:[g_win windowNumber] context:nil characters:ctl_unescape(chars)
          charactersIgnoringModifiers:ctl_unescape(plain[0] ? plain : chars) isARepeat:NO keyCode:(unsigned short)kc];
        if (!(flags & NSEventModifierFlagCommand) || ![[NSApp mainMenu] performKeyEquivalent:e]) {
            NSResponder *responder=[g_win firstResponder];
            if(tb && tb->kind==TK_BROWSER && tb->browser_native && responder!=(NSResponder *)g_view &&
               [responder respondsToSelector:@selector(keyDown:)]) [responder keyDown:e];
            else [g_view keyDown:e];
        }
    } else if (!strncmp(s, "winid ", 6)) {
        FILE *f = fopen(s + 6, "wb");
        if (f) { fprintf(f, "%ld\n", (long)[g_win windowNumber]); fclose(f); }
    } else if (!strncmp(s, "snap ", 5)) {
        snprintf(g_snap_path, sizeof(g_snap_path), "%s", s + 5);
        paint_view_now();
    }
}

static void ctl_start(const char *path) {
    static char line[8192];
    static int n;
    int fd;
    dispatch_source_t r;
    unlink(path);
    if (mkfifo(path, 0600) != 0 || (fd = open(path, O_RDWR | O_NONBLOCK)) < 0) return;   /* RDWR: never sees EOF */
    r = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(r, ^{
        char buf[4096];
        ssize_t k = read(fd, buf, sizeof(buf)), i;
        for (i = 0; i < k; i++) {
            if (buf[i] == '\n') { line[n] = 0; ctl_line(line); n = 0; }
            else if (n < (int)sizeof(line) - 1) line[n++] = buf[i];
        }
    });
    dispatch_resume(r);
}

/* ── tests ─────────────────────────────────────────────────────────── */

static double bench_now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

/* Headless check of the whole child path: forkpty → your shell → our hooks → parser. */
static int selftest(const char *out_path) {
    Tab *tb;
    FILE *f;
    int y, phase = 0;
    DWORD start = GetTickCount(), phase_t = 0;
    char line[4096], tmp[MAX_PATH], tmp_real[MAX_PATH];
    g_cols = 100;
    g_rows = 30;
    tb = tab_create(NULL, 0);
    if (!tb) return 2;
    g_tabs[g_ntabs++] = tb;
    snprintf(tmp, sizeof(tmp), "%s", "/tmp");
    if (!realpath(tmp, tmp_real)) snprintf(tmp_real, sizeof(tmp_real), "%s", tmp);
    while (GetTickCount() - start < 20000) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        tab_drain(tb);
        if (phase == 0 && strstr(tb->t.cwd, "/") && GetTickCount() - start > 1500) {
            const char *cmd = "cd /tmp && printf '\\033[1;32mSELFTEST\\033[0m %s\\n' \"$PWD\" && tput cols\r";
            tab_write(tb, cmd, (int)strlen(cmd));
            phase = 1;
            phase_t = GetTickCount();
        } else if (phase == 1 && (!strcmp(tb->t.cwd, "/tmp") || !strcmp(tb->t.cwd, tmp_real)) && GetTickCount() - phase_t > 500) {
            phase = 2;
            break;
        }
    }
    f = fopen(out_path, "wb");
    if (!f) return 3;
    fprintf(f, "shell=%s\n", g_shell);
    fprintf(f, "phase=%d cwd=%s cols=%d rows=%d hist=%d\n", phase, tb->t.cwd, tb->t.cols, tb->t.rows, tb->t.hcount);
    {
        char title[1024];
        size_t i, k = 0;
        for (i = 0; tb->t.title[i] && k < sizeof(title) - 4; i++) k += utf8_encode((uint32_t)tb->t.title[i], title + k);
        title[k] = 0;
        fprintf(f, "title=%s\n----\n", title);
    }
    for (y = 0; y < tb->t.rows; y++) {
        int x, n = 0;
        for (x = 0; x < tb->t.cols; x++) {
            Cell *c = &tb->t.row[tb->t.alt][y][x];
            if (c->wflag == W_CONT) continue;
            n += utf8_encode(c->ch ? c->ch : ' ', line + n);
        }
        while (n && line[n - 1] == ' ') n--;
        line[n] = 0;
        fprintf(f, "%s\n", line);
    }
    fclose(f);
    g_ntabs = 0;
    tab_destroy_async(tb);
    return phase == 2 ? 0 : 1;
}

static int mac_update_unit(FILE *f);

/* --unit OUT: table tests for the pure helpers behind drops, links and the UTF-8 decoder */
static int unit_tests(const char *out_path) {
    static const struct { const char *in, *want; } paths[] = {
        {"/Users/you/notes.txt", "/Users/you/notes.txt"},
        {"/Users/you/My Stuff/a b.png", "'/Users/you/My Stuff/a b.png'"},
        {"/tmp/it's/here", "'/tmp/it'\\''s/here'"},
        {"/Volumes/USB/x(1).c", "'/Volumes/USB/x(1).c'"},
    };
    static const struct { const char *word; int at; const char *want; } urls[] = {
        {"https://example.com/a?b=c", 3, "https://example.com/a?b=c"},
        {"(see:https://x.io/y).", 10, "https://x.io/y"},
        {"(see:https://x.io/y).", 1, NULL},
        {"file:///Users/you", 0, "file:///Users/you"},
        {"http://", 2, NULL},
        {"nourl.here", 2, NULL},
    };
    static const struct { const char *in; const wchar_t *want; } wide[] = {
        {"abc", L"abc"}, {"caf\xc3\xa9", L"caf\x00e9"}, {"\xe2\x9c\xb3 x", L"\x2733 x"}, {"\xf0\x9f\x98\x80", L"\x1F600"},
        {"bad\xff!", L"bad\xFFFD!"},
    };
    FILE *f = fopen(out_path, "wb");
    int i, fails = 0;
    if (!f) return 3;
    for (i = 0; i < (int)(sizeof(paths) / sizeof(paths[0])); i++) {
        char got[1024];
        quoted_path(paths[i].in, got, sizeof(got));
        fprintf(f, "%s drop  %s\n", strcmp(got, paths[i].want) ? "FAIL" : "ok  ", got);
        fails += strcmp(got, paths[i].want) != 0;
    }
    for (i = 0; i < (int)(sizeof(urls) / sizeof(urls[0])); i++) {
        char got[256] = "";
        int hit = url_in_word(urls[i].word, urls[i].at, got, sizeof(got));
        int ok = urls[i].want ? (hit && !strcmp(got, urls[i].want)) : !hit;
        fprintf(f, "%s url   %s @%d -> %s\n", ok ? "ok  " : "FAIL", urls[i].word, urls[i].at, hit ? got : "(none)");
        fails += !ok;
    }
    for (i = 0; i < (int)(sizeof(wide) / sizeof(wide[0])); i++) {
        wchar_t got[64];
        int ok;
        utf8_to_wide(wide[i].in, got, 64);
        ok = !wcscmp(got, wide[i].want);
        fprintf(f, "%s utf8  #%d\n", ok ? "ok  " : "FAIL", i);
        fails += !ok;
    }
    fails += unit_scrollback(f);
    fails += unit_issues(f);
    fails += unit_underlay(f);
    fails += unit_chrome_display(f);
    fails += unit_chrome(f);
    fails += unit_tab_labels(f);
    fails += browser_unit(f);
    fails += mac_update_unit(f);
    {
        int e, ok = reflow_check(5000, &e) && !e, rt = reflow_roundtrip();
        fprintf(f, "%s reflow tail-only + settle == full reflow\n", ok ? "ok  " : "FAIL");
        fprintf(f, "%s reflow 60 resizes and back == never resized\n", rt ? "ok  " : "FAIL");
        fails += !ok + !rt;
    }
    fprintf(f, "%d failed\n", fails);
    fclose(f);
    return fails ? 1 : 0;
}

static int replay(const char *out_path, int cols, int rows, char **files, int nfiles) {
    FILE *out = fopen(out_path, "wb");
    int i;
    if (!out) return 3;
    for (i = 0; i < nfiles; i++) {
        FILE *f = fopen(files[i], "rb");
        fprintf(out, "=== %s\n", files[i]);
        replay_one(out, f, cols, rows);
        if (f) fclose(f);
    }
    fclose(out);
    return 0;
}


/* ── startup ───────────────────────────────────────────────────────── */

/* The first shell has usually printed its prompt (~20 ms after the fork) long
   before AppKit lets the main queue run (~165 ms after exec, measured). Take
   it straight from the pty, waiting up to wait_ms for the prompt, so the
   window's first frame already shows it. */
static void first_prompt_now(int wait_ms) {
    Tab *tb = g_ntabs ? g_tabs[0] : NULL;
    double end = uptime_ms() + wait_ms;
    while (tb && tb->fd >= 0) {
        struct pollfd p = {tb->fd, POLLIN, 0};
        int left = (int)(end - uptime_ms());
        read_some(tb);           /* (first: it holds the reader queue for this turn, so the poll sees everything) */
        tab_drain(tb);
        if (tb->fd < 0 || (tb->t.nmarks && tb->t.cx != 0)) break;
        if (left <= 0 || poll(&p, 1, left) <= 0) break;
    }
    if (tb) update_title();
}

/* HARDCORE_TRACE=1: startup phases, in ms since the process was exec'd, to
   stderr (the Mac side of %TEMP%\hardcore-trace.txt) */

static double uptime_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }

static void trace(const char *what) {
    if (g_tracing) fprintf(stderr, "%8.2f ms  %s\n", uptime_ms() - g_t_exec, what);
}

static void trace_init(void) {
    struct kinfo_proc kp;
    size_t n = sizeof(kp);
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct timeval now;
    g_tracing = getenv("HARDCORE_TRACE") != NULL;
    g_t_exec = uptime_ms();
    if (!g_tracing || sysctl(mib, 4, &kp, &n, NULL, 0) != 0) return;
    gettimeofday(&now, NULL);    /* p_starttime is wall clock: carry the gap over to uptime */
    g_t_exec -= (now.tv_sec - kp.kp_proc.p_starttime.tv_sec) * 1000.0 + (now.tv_usec - kp.kp_proc.p_starttime.tv_usec) / 1000.0;
}

static int mac_update_cmd(int argc, char **argv);
static int mac_reload_windows(void);
static int mac_reload(int ack, char *why, size_t cap);
static int mac_browser_cli(int argc, char **argv);
#include "hardcore_mac_transfer.m"
#include "hardcore_mac_update.h"
#include "hardcore_browser_cli.h"
#include "hardcore_mac_launch.h"

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "chrome")) return hc_chrome_setup_main(argc, argv);
    /* The ANSI backend does not initialize Cocoa, Metal, a Dock app, or the
       GUI's dispatch queues. Its session owner forks before any of those. */
    if (argc > 1 && !strcmp(argv[1], "here")) return here_main(argc - 2, argv + 2);
    if (argc > 1 && (!strcmp(argv[1], "exit") || !strcmp(argv[1], "detach"))) return here_lifecycle_main(argc - 2, argv + 2, !strcmp(argv[1], "exit"));
    if (argc > 1 && !strcmp(argv[1], "--here-files")) return hc_here_files_with_media(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--here-web")) return hc_here_web_main(argc - 1, argv + 1);
    if (argc > 1 && !strcmp(argv[1], "--here-media")) return hc_here_media_main(argc - 1, argv + 1);
    int i;
    char selftest_out[MAX_PATH] = "";
    HCDelegate *dlg;
    NSRect frame;
    dispatch_group_t gfx = dispatch_group_create();
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    mac_executable_init();
    if (argc == 2 && !strcmp(argv[1], "--mac-update-protocol")) { if (!mtl_init()) return 1; puts("hardcore-mac-reload-1"); return 0; }
    if (argc == 4 && !strcmp(argv[1], "--mac-adopt")) {
        g_mac_state_fd = atoi(argv[2]); g_mac_ack_fd = atoi(argv[3]);
    }
    trace_init();
    trace("main");
    g_lat = getenv("HARDCORE_LAT") != NULL;
    g_reflow_all = getenv("HARDCORE_REFLOW_ALL") != NULL;   /* bench: every resize step rewraps all history, as before */
    signal(SIGPIPE, SIG_IGN);    /* a write to a pty whose shell just died returns EIO instead */
    int cli_status = mac_cli(argc, argv);
    if (cli_status >= 0) return cli_status;
    int launch_status = mac_gui_launch(argc, argv);
    if (launch_status >= 0) return launch_status;
    palette_init();
    drip_init();
    config_load();
    snprintf(g_cfg_dir_line, sizeof(g_cfg_dir_line), "%s", g_start_dir);
    {   /* reopen where the last window closed, if that folder is still there (a folder argument wins) */
        struct stat st;
        if (g_last_dir[0] && stat(g_last_dir, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(g_start_dir, sizeof(g_start_dir), "%s", g_last_dir);
    }
    for (i = g_mac_state_fd >= 0 ? argc : 1; i < argc && strcmp(argv[1], "--adopt-fd") && strcmp(argv[1], "--slurp-window") && strcmp(argv[1], "--claude-window"); i++) {
        if ((!strcmp(argv[i], "--cd") || !strcmp(argv[i], "-d")) && i + 1 < argc) {
            snprintf(g_start_dir, sizeof(g_start_dir), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--unit") && i + 1 < argc) {
            return unit_tests(argv[i + 1]);
        } else if (!strcmp(argv[i], "--replay") && i + 3 < argc) {
            /* --replay OUT COLS ROWS FILE... */
            return replay(argv[i + 1], atoi(argv[i + 2]), atoi(argv[i + 3]), argv + i + 4, argc - i - 4);
        } else if (!strcmp(argv[i], "--bench") && i + 4 < argc) {
            /* --bench OUT COLS ROWS FILE: the parser alone, on a captured stream */
            return parse_bench(argv[i + 1], atoi(argv[i + 2]), atoi(argv[i + 3]), argv[i + 4]);
        } else if (!strcmp(argv[i], "--bench-resize") && i + 1 < argc) {
            return bench_resize(argv[i + 1]);
        } else if (!strcmp(argv[i], "--selftest") && i + 1 < argc) {
            snprintf(selftest_out, sizeof(selftest_out), "%s", argv[++i]);
        } else if (!strncmp(argv[i], "-psn_", 5)) {
            /* Finder's process serial number, on older macOS */
        } else if (argv[i][0] != '-') {
            char real[MAX_PATH];
            snprintf(g_start_dir, sizeof(g_start_dir), "%s", realpath(argv[i], real) ? real : argv[i]);
        }
    }
    if (!g_start_dir[0]) snprintf(g_start_dir, sizeof(g_start_dir), "%s", getenv("HOME") ? getenv("HOME") : "/");
    write_rc();
    if (selftest_out[0]) return selftest(selftest_out);
    int importing;
    @autoreleasepool { importing = mac_prepare(argc, argv); }
    if (importing < 0) return 1;
    /* The first shell starts before AppKit does (that's ~60 ms of loading and
       launching): its grid comes from where the window was last time, and the
       display's scale, both known without AppKit. If the window comes up a
       different size, resize_all sends the shell a SIGWINCH. */
    if (!importing) {
        NSString *saved = [[NSUserDefaults standardUserDefaults] stringForKey:@"NSWindow Frame hardcore"];
        CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID());
        int w = 0, h = 0;
        if (mode) {
            size_t pw = CGDisplayModeGetPixelWidth(mode), lw = CGDisplayModeGetWidth(mode);
            g_scale = lw ? (int)((double)pw / lw + 0.5) : 1;
            if (g_scale < 1) g_scale = 1;
            CGDisplayModeRelease(mode);
        }
        if (saved) {
            NSArray *v = [saved componentsSeparatedByString:@" "];
            if ([v count] >= 4) { w = [v[2] intValue] * g_scale; h = [v[3] intValue] * g_scale; }
        }
        if (w < 100 || h < 100) { w = 120 * CW; h = 36 * CH; }
        compute_grid(w, h, &g_cols, &g_rows);
        if (g_mac_state_fd >= 0) {
            if (!mac_adopt_read()) { mac_adopt_failure("could not restore terminal state"); return 1; }
        } else new_tab_here();
        trace("first tab spawned");
    }
    /* the Metal device and shaders on a worker, while the window and the first shell get going */
    dispatch_group_async(gfx, dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ g_gfx_ready = mtl_init(); });
    trace("config, rc");

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    dlg = [[HCDelegate alloc] init];
    [NSApp setDelegate:dlg];
    trace("NSApp");

    g_scale = (int)([[NSScreen mainScreen] backingScaleFactor] + 0.5);
    if (g_scale < 1) g_scale = 1;
    frame = NSMakeRect(0, 0, (120 * CW) / (CGFloat)g_scale,
                       (36 * CH) / (CGFloat)g_scale);
    g_win = [[NSWindow alloc] initWithContentRect:frame
                                        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                                  NSWindowStyleMaskResizable | NSWindowStyleMaskFullSizeContentView
                                          backing:NSBackingStoreBuffered defer:NO];
    [g_win center];
    [g_win setFrameAutosaveName:@"hardcore"];
    if (g_mac_adopt) [g_win setFrame:NSRectFromString(g_mac_adopt[@"frame"]) display:NO];
    else if (g_mac_placement) [g_win setFrame:NSRectFromString(g_mac_placement[@"frame"]) display:NO];
    g_scale = (int)([g_win backingScaleFactor] + 0.5);
    if (g_scale < 1) g_scale = 1;
    /* Full-size terminal canvas; native controls are revealed by top-band hover. */
    [g_win setTitlebarAppearsTransparent:YES];
    [g_win setTitleVisibility:NSWindowTitleHidden];
    [g_win setBackgroundColor:[NSColor colorWithSRGBRed:((DEF_BG >> 16) & 255) / 255.0 green:((DEF_BG >> 8) & 255) / 255.0
                                                   blue:(DEF_BG & 255) / 255.0 alpha:1]];
    [g_win setAppearance:[NSAppearance appearanceNamed:g_shown->dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua]];
    [g_win setContentMinSize:NSMakeSize(300, 160)];
    [g_win setAnimationBehavior:NSWindowAnimationBehaviorNone];
    [g_win setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
    [g_win setDelegate:dlg];
    [g_win setAcceptsMouseMovedEvents:YES];
    [g_win setTitle:@"hardcore"];

    g_view = [[HCView alloc] initWithFrame:[[g_win contentView] frame]];
    [g_view setWantsLayer:YES];
    [g_view setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawDuringViewResize];
    [g_view registerForDraggedTypes:@[NSPasteboardTypeFileURL, NSPasteboardTypeString]];
    [g_win setContentView:g_view];
    [g_win makeFirstResponder:g_view];
    mac_title_track_events();
    build_menus();
    {
        if (@available(macOS 12.0, *))   /* 120 Hz ProMotion screens get 8 ms frames */
            if ([[g_win screen] maximumFramesPerSecond] > 0) g_refresh_ms = 1000.0 / [[g_win screen] maximumFramesPerSecond];
    }
    [[NSNotificationCenter defaultCenter] addObserverForName:NSTextInputContextKeyboardSelectionDidChangeNotification
                                                      object:nil queue:nil
                                                  usingBlock:^(NSNotification *n) { (void)n; g_plain_layout = -1; }];
    [[NSDistributedNotificationCenter defaultCenter] addObserverForName:SETTINGS_NOTE object:nil queue:nil
                                                             usingBlock:^(NSNotification *n) { (void)n; config_load(); settings_apply(); }];
    if (getenv("HARDCORE_CTL")) ctl_start(getenv("HARDCORE_CTL"));
    trace("window built");
    dispatch_group_wait(gfx, DISPATCH_TIME_FOREVER);
    trace("Metal ready");
    if (!g_gfx_ready) {
        if (g_mac_adopt) { mac_adopt_failure("Metal initialization failed"); return 1; }
        NSAlert *a = [[NSAlert alloc] init];
        [a setMessageText:@"hardcore couldn't start Metal."];
        [a runModal];
        return 1;
    }
    g_layer.device = g_mtl;
    if (g_mac_imports.count) g_handoff_paint = 1;
    render_thread_start();
    layer_fit();
    resize_all();
    mac_browser_sync();
    if (!importing && !g_mac_adopt) first_prompt_now(40);
    trace("first prompt read");
    [g_win makeKeyAndOrderFront:nil];
    /* on screen now, not after AppKit finishes launching (~30 ms more) */
    [CATransaction flush];
    paint_view_now();
    trace("window shown");
    if (!mac_commit_imports()) return 1;
    mac_server();
    if (g_mac_ready_fd >= 0) {
        mac_send(g_mac_ready_fd, mac_slurp_result());
        close(g_mac_ready_fd); g_mac_ready_fd = -1;
    }
    if (!getenv("HARDCORE_NO_ACTIVATE") && (!g_mac_adopt || [g_mac_adopt[@"focused"] boolValue])) [NSApp activateIgnoringOtherApps:YES];   /* tests: open behind */
    mac_adopt_ready();
    if(g_mac_adopt)mac_music_view_restore(g_mac_adopt[@"music"]);
    mac_refresh_programs();
    g_mac_dock_ready = 1;
    mac_dock_update(1);
    NSTimer *program_timer = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t) {
        (void)t; mac_refresh_programs(); mac_dock_update(0);
    }];
    [[NSRunLoop currentRunLoop] addTimer:program_timer forMode:NSEventTrackingRunLoopMode];
    [NSApp run];
    [pool release];
    return 0;
}
