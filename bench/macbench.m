/*
 * macbench.m — termbench for macOS: measures a terminal window from the
 * outside, the way a person experiences it. Keypresses go in through the
 * window server (CGEventPost at the HID tap, like a real keyboard), and pixels
 * come out through ScreenCaptureKit, each frame stamped with the time the
 * window server displayed it (SCStreamFrameInfoDisplayTime). "Key → pixels"
 * is therefore timed to the frame that actually showed the change, not to
 * when we looked.
 *
 *   macbench latency --app A [--title T] [--n 300] [KEYS]
 *       KEYS: a comma list pressed in turn instead (cmd+1,cmd+2 times tab switches).
 *       Needs bench/echoprobe.py running in the terminal (it hides the
 *       cursor and toggles a block on every key). Times key → first frame
 *       that differs.
 *   macbench newtab  --app A [--title T] [--n 10] --left PT --top PT --row PT
 *       Cmd+T → time until the old tab's prompt (on row 12) is gone (tab
 *       shown) and until a prompt is drawn on row 0 (tab usable). Closes the
 *       new tab with Cmd+W each time.
 *   macbench launch  --app A [--n 5] --left PT --top PT --row PT COMMAND...
 *       Runs COMMAND (e.g. open -n hardcore.app), then times until the app's
 *       window is listed, until its pixels are on screen, and until a prompt is
 *       drawn on its first text row. Watches the whole display, so the first
 *       frame is caught. Quits the app with Cmd+Q between runs.
 *   macbench quit    --app A [--title T] [--keys cmd+q] [--n 5] COMMAND...
 *       Starts COMMAND, waits for its window, then presses the keys and times
 *       until the window is gone and until the process has exited.
 *   macbench type    --app A [--title T] TEXT      (\n = Enter)
 *   macbench key     --app A [--title T] cmd+t     (cmd ctrl alt shift + a key)
 *   macbench find    --app A [--title T]
 *
 * PT values are points from the window's top left (the content, including
 * a transparent title bar if the app draws under it). --row is the height of
 * one text row.
 *
 * Every keystroke is preceded by a check that the target window is the
 * frontmost one; if it isn't, macbench stops rather than type somewhere else.
 *
 * Build: clang -fobjc-arc -O2 -o bench/macbench bench/macbench.m \
 *          -framework Cocoa -framework ScreenCaptureKit -framework CoreMedia -framework CoreVideo
 */
#import <Cocoa/Cocoa.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#include <mach/mach_time.h>
#include <os/lock.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

static const char *g_app, *g_title;
static pid_t g_pid;
static CGWindowID g_wid;
static CGRect g_bounds;          /* points, global display coordinates */
static mach_timebase_info_data_t g_tb;

static double ms_of(uint64_t mach) { return (double)mach * g_tb.numer / g_tb.denom / 1e6; }
static double now_ms(void) { return ms_of(mach_absolute_time()); }

static void die(const char *msg) {
    fprintf(stderr, "macbench: %s\n", msg);
    exit(3);
}

/* ── windows ─────────────────────────────────────────────────────────── */

/* the app's largest on-screen normal window, optionally by title substring */
static int find_window(void) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    double best = 0;
    int found = 0;
    for (NSDictionary *w in (__bridge NSArray *)list) {
        NSString *owner = w[(id)kCGWindowOwnerName], *title = w[(id)kCGWindowName];
        CGRect r;
        if ([w[(id)kCGWindowLayer] intValue] != 0 || ![owner isEqualToString:@(g_app)]) continue;
        if (g_title && (!title || [title rangeOfString:@(g_title)].location == NSNotFound)) continue;
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r);
        if (r.size.width * r.size.height <= best) continue;
        best = r.size.width * r.size.height;
        g_wid = [w[(id)kCGWindowNumber] unsignedIntValue];
        g_pid = [w[(id)kCGWindowOwnerPID] intValue];
        g_bounds = r;
        found = 1;
    }
    CFRelease(list);
    return found;
}

static int window_listed(CGWindowID wid) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, wid);
    int n = list ? (int)CFArrayGetCount(list) : 0;
    int on = 0;
    if (n) on = [((__bridge NSArray *)list)[0][(id)kCGWindowIsOnscreen] boolValue];
    if (list) CFRelease(list);
    return on;
}

static char g_front_owner[128];

/* the frontmost real window: layer 0, visible, bigger than a tooltip */
static CGWindowID frontmost_window(void) {
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    CGWindowID front = 0;
    for (NSDictionary *w in (__bridge NSArray *)list) {
        CGRect r;
        if ([w[(id)kCGWindowLayer] intValue] != 0 || [w[(id)kCGWindowAlpha] doubleValue] < 0.1) continue;
        CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)w[(id)kCGWindowBounds], &r);
        if (r.size.width < 120 || r.size.height < 60) continue;
        front = [w[(id)kCGWindowNumber] unsignedIntValue];
        snprintf(g_front_owner, sizeof(g_front_owner), "%s", [w[(id)kCGWindowOwnerName] UTF8String] ?: "?");
        break;
    }
    CFRelease(list);
    return front;
}

/* Since macOS 14 a background tool can't just activate another app; System
   Events can (it needs the Accessibility permission macbench needs anyway).
   With --title, the window whose title matches is raised first. */
static void focus(void) {
    NSString *src;
    if (frontmost_window() == g_wid) return;
    NSDictionary *err = nil;
    src = [NSString stringWithFormat:@"tell application \"System Events\"\ntell (first process whose unix id is %d)\n"
                                      "set frontmost to true\n%@end tell\nend tell\n", g_pid,
                                      g_title ? [NSString stringWithFormat:@"perform action \"AXRaise\" of (first window whose name contains \"%s\")\n", g_title] : @""];
    [[[NSAppleScript alloc] initWithSource:src] executeAndReturnError:&err];
    if (err) fprintf(stderr, "macbench: focus: %s\n", [[err description] UTF8String]);
    for (int i = 0; i < 100 && frontmost_window() != g_wid; i++) usleep(10000);
}

static void must_be_front(void) {
    if (frontmost_window() != g_wid) {
        focus();
        for (int i = 0; i < 50 && frontmost_window() != g_wid; i++) usleep(10000);
        if (frontmost_window() != g_wid) {
            fprintf(stderr, "macbench: %s window %u is in front of target %u\n", g_front_owner, frontmost_window(), g_wid);
            die("target window isn't in front; stopping before typing anywhere else");
        }
    }
}

/* ── keys ───────────────────────────────────────────────────────────── */

static int keycode_of(const char *k) {
    static const struct { const char *name; int code; } names[] = {
        {"a", 0}, {"s", 1}, {"d", 2}, {"f", 3}, {"h", 4}, {"g", 5}, {"z", 6}, {"x", 7}, {"c", 8}, {"v", 9}, {"b", 11},
        {"q", 12}, {"w", 13}, {"e", 14}, {"r", 15}, {"y", 16}, {"t", 17}, {"1", 18}, {"2", 19}, {"3", 20}, {"4", 21},
        {"6", 22}, {"5", 23}, {"9", 25}, {"7", 26}, {"8", 28}, {"0", 29}, {"]", 30}, {"o", 31}, {"u", 32}, {"[", 33},
        {"i", 34}, {"p", 35}, {"enter", 36}, {"l", 37}, {"j", 38}, {"k", 40}, {"n", 45}, {"m", 46}, {"tab", 48},
        {"space", 49}, {"backspace", 51}, {"esc", 53}, {"left", 123}, {"right", 124}, {"down", 125}, {"up", 126}};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcasecmp(names[i].name, k)) return names[i].code;
    return -1;
}

static void post_key(int code, CGEventFlags flags) {
    CGEventRef d = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)code, true), u = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)code, false);
    CGEventSetFlags(d, flags);
    CGEventSetFlags(u, flags);
    CGEventPost(kCGHIDEventTap, d);
    CGEventPost(kCGHIDEventTap, u);
    CFRelease(d);
    CFRelease(u);
}

/* "cmd+shift+]" */
static void press(const char *spec) {
    char buf[64], *tok, *save;
    CGEventFlags f = 0;
    int code = -1;
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        if (!strcmp(tok, "cmd")) f |= kCGEventFlagMaskCommand;
        else if (!strcmp(tok, "ctrl")) f |= kCGEventFlagMaskControl;
        else if (!strcmp(tok, "alt")) f |= kCGEventFlagMaskAlternate;
        else if (!strcmp(tok, "shift")) f |= kCGEventFlagMaskShift;
        else code = keycode_of(tok);
    }
    if (code < 0) die("unknown key");
    must_be_front();
    /* real modifier keys around it: at the HID tap the window server tracks
       modifier state from key events, and a flag alone doesn't make a shortcut */
    {
        static const struct { CGEventFlags flag; int code; } mods[] = {
            {kCGEventFlagMaskCommand, 55}, {kCGEventFlagMaskShift, 56}, {kCGEventFlagMaskAlternate, 58}, {kCGEventFlagMaskControl, 59}};
        CGEventFlags held = 0;
        int i;
        for (i = 0; i < 4; i++)
            if (f & mods[i].flag) {
                CGEventRef e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)mods[i].code, true);
                held |= mods[i].flag;
                CGEventSetFlags(e, held);
                CGEventPost(kCGHIDEventTap, e);
                CFRelease(e);
            }
        post_key(code, f);
        for (i = 3; i >= 0; i--)
            if (f & mods[i].flag) {
                CGEventRef e = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)mods[i].code, false);
                held &= ~mods[i].flag;
                CGEventSetFlags(e, held);
                CGEventPost(kCGHIDEventTap, e);
                CFRelease(e);
            }
    }
}

/* text as unicode key events; \n is Enter */
static void type_text(const char *s) {
    NSString *str = @(s);
    must_be_front();
    for (NSUInteger i = 0; i < str.length; i++) {
        unichar c = [str characterAtIndex:i];
        if (c == '\\' && i + 1 < str.length && [str characterAtIndex:i + 1] == 'n') { post_key(36, 0); i++; usleep(3000); continue; }
        CGEventRef d = CGEventCreateKeyboardEvent(NULL, 0, true), u = CGEventCreateKeyboardEvent(NULL, 0, false);
        CGEventKeyboardSetUnicodeString(d, 1, &c);
        CGEventKeyboardSetUnicodeString(u, 1, &c);
        CGEventSetFlags(d, 0);
        CGEventSetFlags(u, 0);
        CGEventPost(kCGHIDEventTap, d);
        CGEventPost(kCGHIDEventTap, u);
        CFRelease(d);
        CFRelease(u);
        usleep(2000);
    }
}

/* ── frames ─────────────────────────────────────────────────────────── */

/* The capture delivers BGRA frames at 1 pixel per point; a detector runs on
   each one on the capture queue, and the measuring thread waits on it. */
typedef enum { D_NONE, D_DIFF, D_GONE, D_INK, D_SHOWN, D_NEWTAB } Detect;

static os_unfair_lock g_lk = OS_UNFAIR_LOCK_INIT;
static Detect g_detect;
static uint8_t *g_base;          /* luma of the last settled frame, D_DIFF */
static int g_fw, g_fh;
static double g_hit_ms;          /* display time of the frame that satisfied the detector */
static dispatch_semaphore_t g_sem;
static CGRect g_band;            /* points within the captured area: D_GONE / D_INK look here */
static uint32_t g_bg;            /* background, 0xRRGGBB, for ink tests */
static CGPoint g_pts[3];         /* D_SHOWN: these points must all be the background */
static double g_last_frame_ms;
static CGRect g_band2;           /* D_NEWTAB: the prompt band (g_band: the old content that must go) */
static double g_gone_ms;         /* D_NEWTAB: display time of the first frame without the old content */

static int luma(const uint8_t *p) { return (p[0] * 29 + p[1] * 150 + p[2] * 77) >> 8; }
static int is_ink(const uint8_t *p, uint32_t bg) {
    return abs(p[2] - (int)(bg >> 16)) + abs(p[1] - (int)((bg >> 8) & 255)) + abs(p[0] - (int)(bg & 255)) > 90;
}

static int band_ink(const uint8_t *px, size_t stride, int w, int h) {
    int x0 = (int)g_band.origin.x, y0 = (int)g_band.origin.y, x1 = x0 + (int)g_band.size.width, y1 = y0 + (int)g_band.size.height, n = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            n += is_ink(px + (size_t)y * stride + x * 4, g_bg);
    return n;
}

@interface Sink : NSObject <SCStreamOutput>
@end

@implementation Sink
- (void)stream:(SCStream *)s didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type {
    CFArrayRef att;
    NSDictionary *info;
    CVPixelBufferRef pb;
    const uint8_t *px;
    size_t stride;
    int w, h, hit = 0;
    double shown;
    (void)s;
    if (type != SCStreamOutputTypeScreen) return;
    att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    if (!att || !CFArrayGetCount(att)) return;
    info = (__bridge NSDictionary *)CFArrayGetValueAtIndex(att, 0);
    if ([info[SCStreamFrameInfoStatus] intValue] != SCFrameStatusComplete) return;
    shown = ms_of([info[SCStreamFrameInfoDisplayTime] unsignedLongLongValue]);
    pb = CMSampleBufferGetImageBuffer(sb);
    if (!pb) return;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    px = CVPixelBufferGetBaseAddress(pb);
    stride = CVPixelBufferGetBytesPerRow(pb);
    w = (int)CVPixelBufferGetWidth(pb);
    h = (int)CVPixelBufferGetHeight(pb);
    os_unfair_lock_lock(&g_lk);
    g_last_frame_ms = shown;
    if (g_detect == D_DIFF || g_detect == D_NONE) {
        int changed = 0;
        if (!g_base || g_fw != w || g_fh != h) {
            free(g_base);
            g_base = calloc((size_t)w * h, 1);
            g_fw = w;
            g_fh = h;
            changed = -1;
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int l = luma(px + (size_t)y * stride + x * 4);
                uint8_t *b = &g_base[(size_t)y * w + x];
                if (abs(l - *b) > 40) changed++;
                *b = (uint8_t)l;
            }
        hit = g_detect == D_DIFF && changed > 40;
    } else if (g_detect == D_GONE) {
        hit = band_ink(px, stride, w, h) < 4;
    } else if (g_detect == D_INK) {
        hit = band_ink(px, stride, w, h) >= 12;
    } else if (g_detect == D_NEWTAB) {
        /* both checks on every frame: tab and prompt often arrive in the same one */
        if (g_gone_ms < 0 && band_ink(px, stride, w, h) < 4) g_gone_ms = shown;
        if (g_gone_ms >= 0) {
            CGRect old = g_band;
            g_band = g_band2;
            hit = band_ink(px, stride, w, h) >= 12;
            g_band = old;
        }
    } else if (g_detect == D_SHOWN) {
        hit = 1;
        for (int i = 0; i < 3; i++) {
            int x = (int)g_pts[i].x, y = (int)g_pts[i].y;
            if (x < 0 || y < 0 || x >= w || y >= h || is_ink(px + (size_t)y * stride + x * 4, g_bg)) hit = 0;
        }
    }
    if (hit) {
        g_hit_ms = shown;
        g_detect = D_NONE;
        dispatch_semaphore_signal(g_sem);
    }
    os_unfair_lock_unlock(&g_lk);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
}
@end

static SCStream *g_stream;
static Sink *g_sink;
static CGRect g_cap;             /* what the stream captures, in global points */

/* Starts capturing what's on the display: the whole display, or just the
   target window's rectangle of it. Not a window capture: the window server
   sometimes composites a CAMetalLayer straight to the display, past the
   window's own image, and a window capture then sees no new frames while the
   screen does (hardcore's Metal frames, measured both ways). */
static void capture_start(int whole_display) {
    __block SCShareableContent *content = nil;
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    SCContentFilter *filter = nil;
    SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *c, NSError *e) {
        content = c;
        if (e) fprintf(stderr, "macbench: %s\n", e.localizedDescription.UTF8String);
        dispatch_semaphore_signal(s);
    }];
    dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
    if (!content) die("no shareable content (Screen Recording permission?)");
    {
        SCDisplay *d = content.displays.firstObject;
        filter = [[SCContentFilter alloc] initWithDisplay:d excludingWindows:@[]];
        g_cap = d.frame;
        if (!whole_display) {    /* just the window's part of the screen (display-local points) */
            cfg.sourceRect = CGRectMake(g_bounds.origin.x - d.frame.origin.x, g_bounds.origin.y - d.frame.origin.y,
                                        g_bounds.size.width, g_bounds.size.height);
            g_cap = g_bounds;
        }
    }
    cfg.width = (size_t)g_cap.size.width;   /* one pixel per point: plenty to see text, cheap to scan */
    cfg.height = (size_t)g_cap.size.height;
    cfg.minimumFrameInterval = CMTimeMake(1, 240);
    cfg.queueDepth = 8;
    cfg.pixelFormat = kCVPixelFormatType_32BGRA;
    cfg.showsCursor = NO;
    g_sink = [[Sink alloc] init];
    g_stream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:nil];
    [g_stream addStreamOutput:g_sink type:SCStreamOutputTypeScreen
           sampleHandlerQueue:dispatch_queue_create("frames", DISPATCH_QUEUE_SERIAL) error:nil];
    [g_stream startCaptureWithCompletionHandler:^(NSError *e) {
        if (e) fprintf(stderr, "macbench: capture: %s\n", e.localizedDescription.UTF8String);
        dispatch_semaphore_signal(s);
    }];
    dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
    usleep(300000);              /* let the first frames settle the baseline */
}

static void capture_stop(void) {
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    [g_stream stopCaptureWithCompletionHandler:^(NSError *e) { (void)e; dispatch_semaphore_signal(s); }];
    dispatch_semaphore_wait(s, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
    g_stream = nil;
}

static void arm(Detect d) {
    os_unfair_lock_lock(&g_lk);
    while (dispatch_semaphore_wait(g_sem, DISPATCH_TIME_NOW) == 0) {}
    g_detect = d;
    os_unfair_lock_unlock(&g_lk);
}

/* waits for the armed detector; returns the hit frame's display time, or -1 */
static double wait_hit(int timeout_ms) {
    if (dispatch_semaphore_wait(g_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * NSEC_PER_MSEC))) {
        arm(D_NONE);
        return -1;
    }
    return g_hit_ms;
}

/* a band in window points → in captured-frame pixels */
static CGRect band_in_capture(double left, double top, double width, double height) {
    return CGRectMake(g_bounds.origin.x - g_cap.origin.x + left, g_bounds.origin.y - g_cap.origin.y + top, width, height);
}

/* ── statistics ─────────────────────────────────────────────────────── */

static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void report(const char *what, double *v, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) if (v[i] >= 0) v[k++] = v[i];
    if (!k) { printf("%-28s no samples\n", what); return; }
    qsort(v, k, sizeof(double), cmpd);
    printf("%-28s p50 %7.1f  p90 %7.1f  p99 %7.1f  min %6.1f  max %7.1f ms  (n=%d)\n", what, v[k / 2], v[k * 9 / 10],
           v[k * 99 / 100 < k ? k * 99 / 100 : k - 1], v[0], v[k - 1], k);
}

/* ── commands ───────────────────────────────────────────────────────── */

/* keys = NULL: x, for echoprobe; else a comma list pressed in turn (cmd+1,cmd+2: tab switches) */
static void cmd_latency(int n, const char *keys) {
    double *v = calloc(n, sizeof(double));
    char list[16][32];
    int nk = 0;
    if (keys) {
        char buf[512], *tok, *save;
        snprintf(buf, sizeof(buf), "%s", keys);
        for (tok = strtok_r(buf, ",", &save); tok && nk < 16; tok = strtok_r(NULL, ",", &save)) snprintf(list[nk++], 32, "%s", tok);
    }
    /* MACBENCH_NOCAP=1: only press the keys; the terminal's own log has the
       times (hardcore's HARDCORE_LAT). Capturing the screen changes how the
       window server composites, so this is the check on what capture costs. */
    int nocap = getenv("MACBENCH_NOCAP") != NULL;
    if (!nocap) capture_start(0);
    for (int i = 0; i < n; i++) {
        double t0;
        must_be_front();
        if (nocap) {
            if (nk) press(list[i % nk]);
            else post_key(7, 0);
            usleep(60000 + arc4random_uniform(40000));
            continue;
        }
        arm(D_DIFF);
        t0 = now_ms();
        if (nk) press(list[i % nk]);
        else post_key(7, 0);     /* x: echoprobe toggles its block */
        v[i] = wait_hit(1000);
        if (v[i] >= 0) v[i] -= t0;
        usleep(30000 + arc4random_uniform(40000));   /* off any vsync rhythm */
    }
    if (nocap) return;
    capture_stop();
    report("key -> frame", v, n);
}

static void cmd_newtab(int n, double left, double top, double row) {
    double *shown = calloc(n, sizeof(double)), *prompt = calloc(n, sizeof(double));
    capture_start(0);
    for (int i = 0; i < n; i++) {
        double t0;
        must_be_front();
        os_unfair_lock_lock(&g_lk);
        g_band = band_in_capture(left, top + 12 * row, 300, row);   /* the old tab's prompt, pushed to row 12 */
        g_band2 = band_in_capture(left, top, 300, row);             /* the new tab's prompt, on row 0 */
        g_gone_ms = -1;
        os_unfair_lock_unlock(&g_lk);
        arm(D_NEWTAB);
        t0 = now_ms();
        press("cmd+t");
        prompt[i] = wait_hit(5000);
        os_unfair_lock_lock(&g_lk);
        shown[i] = g_gone_ms >= 0 ? g_gone_ms - t0 : -1;
        os_unfair_lock_unlock(&g_lk);
        if (prompt[i] >= 0) prompt[i] -= t0;
        if (getenv("MACBENCH_V")) fprintf(stderr, "  #%d shown %.1f prompt %.1f\n", i, shown[i], prompt[i]);
        usleep(400000);
        press("cmd+w");
        usleep(1300000);         /* past the terminal's own spare-shell delay, if it keeps one */
    }
    capture_stop();
    report("new tab -> tab shown", shown, n);
    report("new tab -> prompt drawn", prompt, n);
}

static pid_t spawn_cmd(char **argv) {
    pid_t p;
    if (posix_spawnp(&p, argv[0], NULL, NULL, argv, environ)) die("can't run the command");
    return p;
}

static void wait_gone(void) {
    for (int i = 0; i < 500 && find_window(); i++) usleep(10000);
}

/* Cold launch: listed → pixels on screen → prompt. Quits with Cmd+Q after. */
static void cmd_launch(int n, double left, double top, double row, char **argv) {
    double *listed = calloc(n, sizeof(double)), *drawn = calloc(n, sizeof(double)), *prompt = calloc(n, sizeof(double));
    for (int i = 0; i < n; i++) {
        double t0;
        pid_t p;
        if (find_window()) die("quit the app before measuring a cold launch");
        g_wid = 0;
        g_bounds = CGRectZero;
        capture_start(1);
        t0 = now_ms();
        p = spawn_cmd(argv);
        listed[i] = -1;
        while (now_ms() - t0 < 5000) {
            if (find_window()) { listed[i] = now_ms() - t0; break; }
            usleep(500);
        }
        if (listed[i] < 0) die("no window appeared");
        os_unfair_lock_lock(&g_lk);
        for (int k = 0; k < 3; k++)   /* three points in the empty part of the text area */
            g_pts[k] = CGPointMake(g_bounds.origin.x - g_cap.origin.x + left + 200 + 150 * k,
                                   g_bounds.origin.y - g_cap.origin.y + top + 8 * row + 40 * k);
        g_band = band_in_capture(left, top, 300, row);
        os_unfair_lock_unlock(&g_lk);
        arm(D_SHOWN);
        drawn[i] = wait_hit(5000);
        if (drawn[i] >= 0) drawn[i] -= t0;
        arm(D_INK);
        prompt[i] = wait_hit(5000);
        if (prompt[i] >= 0) prompt[i] -= t0;
        capture_stop();
        usleep(500000);
        press("cmd+q");
        wait_gone();
        waitpid(p, NULL, WNOHANG);
        usleep(800000);
    }
    report("launch -> window listed", listed, n);
    report("launch -> window drawn", drawn, n);
    report("launch -> prompt drawn", prompt, n);
}

/* Close: KEYS → window gone → process exited. COMMAND starts a fresh one each run. */
static void cmd_quit(int n, const char *keys, char **argv) {
    double *gone = calloc(n, sizeof(double)), *exited = calloc(n, sizeof(double));
    for (int i = 0; i < n; i++) {
        double t0;
        pid_t p = argv ? spawn_cmd(argv) : 0;
        for (int k = 0; k < 500 && !find_window(); k++) usleep(10000);
        if (!g_wid) die("no window");
        usleep(1500000);         /* shell up, prompt drawn */
        {
            CGWindowID w = g_wid;
            pid_t app = g_pid;
            t0 = now_ms();
            press(keys);
            gone[i] = exited[i] = -1;
            while (now_ms() - t0 < 5000 && (gone[i] < 0 || exited[i] < 0)) {
                if (gone[i] < 0 && !window_listed(w)) gone[i] = now_ms() - t0;
                /* our own child lingers as a zombie until reaped, and kill(pid, 0) still finds it */
                if (exited[i] < 0 && (app == p ? waitpid(p, NULL, WNOHANG) == p : kill(app, 0) != 0)) exited[i] = now_ms() - t0;
                usleep(250);
            }
        }
        if (p) waitpid(p, NULL, WNOHANG);
        g_wid = 0;
        usleep(500000);
    }
    report("close -> window gone", gone, n);
    report("close -> process exited", exited, n);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const char *cmd = argc > 1 ? argv[1] : "";
        const char *keys = "cmd+q";
        int n = 0, i;
        double left = 0, top = 0, row = 16;
        char **rest = NULL;
        mach_timebase_info(&g_tb);
        g_sem = dispatch_semaphore_create(0);
        [NSApplication sharedApplication];
        for (i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--app") && i + 1 < argc) g_app = argv[++i];
            else if (!strcmp(argv[i], "--title") && i + 1 < argc) g_title = argv[++i];
            else if (!strcmp(argv[i], "--n") && i + 1 < argc) n = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--left") && i + 1 < argc) left = atof(argv[++i]);
            else if (!strcmp(argv[i], "--top") && i + 1 < argc) top = atof(argv[++i]);
            else if (!strcmp(argv[i], "--row") && i + 1 < argc) row = atof(argv[++i]);
            else if (!strcmp(argv[i], "--bg") && i + 1 < argc) g_bg = (uint32_t)strtoul(argv[++i], NULL, 16);
            else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
            else { rest = argv + i; break; }
        }
        if (!g_app) die("--app NAME is required");
        if (strcmp(cmd, "launch") && strcmp(cmd, "quit") && !find_window()) die("window not found");
        if (!strcmp(cmd, "find")) {
            printf("window %u pid %d at %.0f,%.0f %.0fx%.0f\n", g_wid, g_pid, g_bounds.origin.x, g_bounds.origin.y,
                   g_bounds.size.width, g_bounds.size.height);
        } else if (!strcmp(cmd, "type") && rest) {
            focus();
            type_text(rest[0]);
            usleep(50000);
        } else if (!strcmp(cmd, "key") && rest) {
            focus();
            press(rest[0]);
            usleep(50000);       /* posted events are dropped if we exit before the window server takes them */
        } else if (!strcmp(cmd, "latency")) {
            focus();
            cmd_latency(n ? n : 300, rest ? rest[0] : NULL);
        } else if (!strcmp(cmd, "newtab")) {
            focus();
            cmd_newtab(n ? n : 10, left, top, row);
        } else if (!strcmp(cmd, "launch") && rest) {
            cmd_launch(n ? n : 5, left, top, row, rest);
        } else if (!strcmp(cmd, "quit")) {
            cmd_quit(n ? n : 5, keys, rest);
        } else {
            die("usage: see the comment at the top of bench/macbench.m");
        }
    }
    return 0;
}
