/* AppKit-only Dock summary drawing. Included by hardcore_mac.m; the caller
 * owns Dock registration and update scheduling. No terminal output is read. */
#import <Cocoa/Cocoa.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* The platform supplies the exact same pixels used by the terminal, including
 * Unicode fallback. Buffers are caller-zeroed, top-down, 40 pixels square. */
static int (*hc_dock_read_glyph)(NSString *fontKey, uint32_t cp, unsigned char *pixels, int stride);

static NSColor *hc_dock_rgb(unsigned rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 255) / 255.0
                             green:((rgb >> 8) & 255) / 255.0
                              blue:(rgb & 255) / 255.0 alpha:1];
}

/* Model strings are plain labels. Bound work and make controls/line breaks
 * harmless without cutting a surrogate pair or a combining character. */
static NSString *hc_dock_label(id value, NSString *fallback) {
    if (![value isKindOfClass:[NSString class]]) return fallback;
    NSMutableString *label = [NSMutableString string];
    NSCharacterSet *space = [NSCharacterSet whitespaceAndNewlineCharacterSet];
    NSUInteger length = [value length], offset = 0, count = 0;
    BOOL pendingSpace = NO;
    while (offset < length && count < 160) {
        NSRange range = [value rangeOfComposedCharacterSequenceAtIndex:offset];
        NSString *part = [value substringWithRange:range];
        offset = NSMaxRange(range);
        if ([part rangeOfCharacterFromSet:space].location != NSNotFound) {
            pendingSpace = [label length] > 0;
            continue;
        }
        BOOL control = NO;
        for (NSUInteger i = 0; i < [part length]; i++) {
            unichar c = [part characterAtIndex:i];
            /* Reject explicit directional overrides without treating ZWJ or
             * ZWNJ, which legitimate emoji/scripts need, as controls. */
            if (c < 0x20 || (c >= 0x7f && c <= 0x9f) ||
                c == 0x200e || c == 0x200f ||
                (c >= 0x202a && c <= 0x202e) ||
                (c >= 0x2066 && c <= 0x2069)) { control = YES; break; }
        }
        if (control) continue;
        if (pendingSpace) [label appendString:@" "];
        [label appendString:part];
        pendingSpace = NO;
        count++;
    }
    if (offset < length) [label appendString:@"…"];
    return [label length] ? [[label copy] autorelease] : fallback;
}

static NSDictionary *hc_dock_canonical_appearance(id value) {
    NSDictionary *source = [value isKindOfClass:[NSDictionary class]] ? value : nil;
    NSMutableDictionary *appearance = [NSMutableDictionary dictionaryWithDictionary:@{
        @"fontKey":@"H", @"themeKey":@"g", @"fontWidth":@12, @"fontHeight":@26,
        @"bg":@0x282828, @"fg":@0xebdbb2, @"accent":@0xebdbb2,
        @"bar":@0x343330, @"hover":@0x47453e, @"dim":@0x9d937b,
        @"red":@0xfb4934, @"yellow":@0xfabd2f, @"green":@0xb8bb26, @"blue":@0x83a598}];
    for (NSString *key in @[@"fontKey", @"themeKey"])
        appearance[key] = hc_dock_label(source[key], appearance[key]);
    for (NSString *key in @[@"fontWidth", @"fontHeight"]) {
        id number = source[key];
        if ([number isKindOfClass:[NSNumber class]])
            appearance[key] = @(MAX(1, MIN(40, [number integerValue])));
    }
    for (NSString *key in @[@"bg", @"fg", @"accent", @"bar", @"hover", @"dim",
                           @"red", @"yellow", @"green", @"blue"]) {
        id number = source[key];
        if ([number isKindOfClass:[NSNumber class]]) appearance[key] = @([number unsignedIntValue] & 0xffffff);
    }
    return [[appearance copy] autorelease];
}

static NSDictionary *hc_dock_canonical_summary(NSDictionary *summary) {
    if (![summary isKindOfClass:[NSDictionary class]]) summary = nil;
    id count = [summary objectForKey:@"tabs"];
    NSInteger tabs = [count isKindOfClass:[NSNumber class]] ? [count integerValue] : 0;
    tabs = MAX(0, MIN(9999, tabs));
    return @{@"folder": hc_dock_label([summary objectForKey:@"folder"], @"Hardcore"),
             @"program": hc_dock_label([summary objectForKey:@"program"], @"terminal"),
             @"status": hc_dock_label([summary objectForKey:@"status"], @""),
             @"tabs": @(tabs),
             @"appearance": hc_dock_canonical_appearance(summary[@"appearance"])};
}

/* Standalone/offscreen callers may omit the application font adapter. Use a
 * deterministic missing-glyph outline, never a substituted system typeface. */
static int hc_dock_glyph(NSDictionary *appearance, uint32_t cp, unsigned char *pixels) {
    memset(pixels, 0, 40 * 40);
    if (hc_dock_read_glyph)
        return MAX(0, MIN(40, hc_dock_read_glyph(appearance[@"fontKey"], cp, pixels, 40)));
    int width = [appearance[@"fontWidth"] intValue], height = [appearance[@"fontHeight"] intValue];
    if (cp != ' ') {
        for (int y = 2; y < height - 2; y++)
            for (int x = 1; x < width - 1; x++)
                if (x == 1 || x == width - 2 || y == 2 || y == height - 3) pixels[y * 40 + x] = 255;
    }
    return width;
}

static uint32_t hc_dock_next_scalar(NSString *text, NSUInteger *offset) {
    unichar lead = [text characterAtIndex:(*offset)++];
    if (CFStringIsSurrogateHighCharacter(lead) && *offset < [text length]) {
        unichar trail = [text characterAtIndex:*offset];
        if (CFStringIsSurrogateLowCharacter(trail)) {
            (*offset)++;
            return CFStringGetLongCharacterForSurrogatePair(lead, trail);
        }
    }
    return CFStringIsSurrogateHighCharacter(lead) || CFStringIsSurrogateLowCharacter(lead) ? 0xfffd : lead;
}

static CGFloat hc_dock_label_width(NSString *text, NSDictionary *appearance) {
    NSString *composed = [text precomposedStringWithCanonicalMapping];
    unsigned char pixels[40 * 40];
    CGFloat width = 0;
    NSUInteger offset = 0;
    while (offset < [composed length]) width += hc_dock_glyph(appearance, hc_dock_next_scalar(composed, &offset), pixels);
    return width;
}

/* The source prefix always ends at a composed-character boundary. Measuring
 * uses the selected font's real advances, including its double-width glyphs. */
static NSString *hc_dock_fit_label(NSString *text, NSDictionary *appearance, CGFloat width) {
    CGFloat ellipsis = hc_dock_label_width(@"…", appearance), used = 0;
    NSUInteger offset = 0, prefix = 0;
    while (offset < [text length]) {
        NSRange cluster = [text rangeOfComposedCharacterSequenceAtIndex:offset];
        used += hc_dock_label_width([text substringWithRange:cluster], appearance);
        if (used > width) {
            if (ellipsis > width) return @"";
            return [[text substringToIndex:prefix] stringByAppendingString:@"…"];
        }
        offset = NSMaxRange(cluster);
        if (used + ellipsis <= width) prefix = offset;
    }
    return text;
}

/* Native font pixels are copied into a small RGBA bitmap. Integral device
 * scaling stays pixel-exact; smaller/fractional text uses coverage sampling
 * so a one-pixel stem does not vanish between samples. The main program line
 * uses an integral enlargement whenever it fits. */
static void hc_dock_draw_label(NSString *text, NSRect rect, NSDictionary *appearance,
                               unsigned rgb, BOOL nativeSize, BOOL centered) {
    int height = [appearance[@"fontHeight"] intValue];
    CGFloat scale = rect.size.height / height;
    if (nativeSize && scale >= 1) scale = floor(scale);
    if (scale <= 0) return;
    CGFloat fullWidth = hc_dock_label_width(text, appearance);
    if (nativeSize && fullWidth * scale > rect.size.width && fullWidth > 0) {
        CGFloat fitScale = rect.size.width / fullWidth;
        if (fitScale >= 1) scale = floor(fitScale);
        else if (fitScale >= .7) scale = fitScale;
    }
    NSString *fitted = fullWidth * scale <= rect.size.width + .001
        ? text : hc_dock_fit_label(text, appearance, rect.size.width / scale);
    NSString *composed = [fitted precomposedStringWithCanonicalMapping];
    int width = (int)hc_dock_label_width(composed, appearance);
    if (width <= 0) return;
    NSBitmapImageRep *bitmap = [[[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
        pixelsWide:width pixelsHigh:height bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
        isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:width * 4 bitsPerPixel:32] autorelease];
    if (!bitmap) return;
    unsigned char *data = [bitmap bitmapData];
    memset(data, 0, (size_t)width * height * 4);
    unsigned char pixels[40 * 40];
    NSUInteger offset = 0;
    int advance = 0;
    while (offset < [composed length]) {
        int glyphWidth = hc_dock_glyph(appearance, hc_dock_next_scalar(composed, &offset), pixels);
        for (int y = 0; y < height; y++) for (int x = 0; x < glyphWidth && advance + x < width; x++) {
            if (!pixels[y * 40 + x]) continue;
            unsigned char *pixel = data + ((size_t)y * width + advance + x) * 4;
            pixel[0] = (rgb >> 16) & 255; pixel[1] = (rgb >> 8) & 255; pixel[2] = rgb & 255; pixel[3] = 255;
        }
        advance += glyphWidth;
    }
    CGFloat x = rect.origin.x + (centered ? (rect.size.width - width * scale) / 2 : 0);
    CGFloat y = rect.origin.y + (rect.size.height - height * scale) / 2;
    CGAffineTransform ctm = CGContextGetCTM([[NSGraphicsContext currentContext] CGContext]);
    CGFloat deviceScale = scale * hypot(ctm.a, ctm.b);
    NSImageInterpolation interpolation = deviceScale >= 1 && fabs(deviceScale - round(deviceScale)) < .001
        ? NSImageInterpolationNone : NSImageInterpolationHigh;
    [[NSGraphicsContext currentContext] setImageInterpolation:interpolation];
    [bitmap drawInRect:NSMakeRect(x, y, width * scale, height * scale)
             fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1
       respectFlipped:NO hints:@{NSImageHintInterpolation:@(interpolation)}];
}

/* Also usable by an offscreen NSBitmapImageRep harness. Draws into the current
 * AppKit context; bounds may be any size and use a normal, unflipped origin. */
static void hc_dock_draw_summary(NSRect bounds, NSDictionary *summary) {
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return;
    NSDictionary *appearance = hc_dock_canonical_appearance(summary[@"appearance"]);
    unsigned bg = [appearance[@"bg"] unsignedIntValue], fg = [appearance[@"fg"] unsignedIntValue];
    unsigned accent = [appearance[@"accent"] unsignedIntValue], dim = [appearance[@"dim"] unsignedIntValue];
    CGFloat side = MIN(bounds.size.width, bounds.size.height);
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform *transform = [NSAffineTransform transform];
    [transform translateXBy:NSMinX(bounds) + (bounds.size.width - side) * .5
                       yBy:NSMinY(bounds) + (bounds.size.height - side) * .5];
    [transform scaleBy:side / 128.0];
    [transform concat];

    NSBezierPath *shell = [NSBezierPath bezierPathWithRoundedRect:NSMakeRect(5, 7, 118, 114) xRadius:21 yRadius:21];
    [hc_dock_rgb(bg) setFill]; [shell fill];
    [hc_dock_rgb([appearance[@"hover"] unsignedIntValue]) setStroke];
    [shell setLineWidth:1.5]; [shell stroke];
    [hc_dock_rgb([appearance[@"bar"] unsignedIntValue]) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(6, 96, 116, 24) xRadius:20 yRadius:20] fill];
    NSRectFill(NSMakeRect(6, 96, 116, 9));
    NSArray *lights = @[@"red", @"yellow", @"green"];
    for (int i = 0; i < 3; i++) {
        [hc_dock_rgb([appearance[lights[i]] unsignedIntValue]) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(18 + 13 * i, 104, 7, 7)] fill];
    }
    hc_dock_draw_label(@">_", NSMakeRect(91, 99, 26, 18), appearance, accent, NO, NO);

    NSString *folder = summary[@"folder"] ?: @"Hardcore";
    NSString *program = summary[@"program"] ?: @"terminal";
    NSString *status = summary[@"status"] ?: @"";
    NSInteger tabs = [summary[@"tabs"] integerValue];
    hc_dock_draw_label(folder, NSMakeRect(17, 75, 95, 18), appearance, fg, NO, NO);
    hc_dock_draw_label(program, NSMakeRect(16, 40, 98, 34), appearance, accent, YES, NO);
    if ([status length]) {
        unsigned statusColor = [appearance[@"green"] unsignedIntValue];
        if ([status isEqualToString:@"busy"] || [status isEqualToString:@"running"]) statusColor = accent;
        else if ([status isEqualToString:@"needs you"]) statusColor = [appearance[@"blue"] unsignedIntValue];
        else if ([status isEqualToString:@"error"]) statusColor = [appearance[@"red"] unsignedIntValue];
        [hc_dock_rgb(statusColor) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(18, 24, 4, 4)] fill];
        hc_dock_draw_label(status, NSMakeRect(27, 20, tabs > 1 ? 49 : 82, 12), appearance, dim, NO, NO);
    }
    if (tabs > 1) {
        NSString *count = tabs > 99 ? @"99+" : [NSString stringWithFormat:@"%ld", (long)tabs];
        [hc_dock_rgb([appearance[@"hover"] unsignedIntValue]) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(84, 19, 28, 15) xRadius:5 yRadius:5] fill];
        hc_dock_draw_label(count, NSMakeRect(85, 20, 26, 12), appearance, fg, NO, YES);
    }
    [NSGraphicsContext restoreGraphicsState];
}

@interface HCDockSummaryView : NSView {
    NSDictionary *_summary;
}
- (BOOL)setSummary:(NSDictionary *)summary;
- (NSDictionary *)summary;
@end

@implementation HCDockSummaryView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        [self setAccessibilityElement:YES];
        [self setAccessibilityRole:NSAccessibilityImageRole];
        [self setSummary:nil];
    }
    return self;
}
- (BOOL)isOpaque { return NO; }
- (NSDictionary *)summary { return _summary; }
- (BOOL)setSummary:(NSDictionary *)summary {
    NSDictionary *next = hc_dock_canonical_summary(summary);
    if ([_summary isEqualToDictionary:next]) return NO;
    [_summary release];
    _summary = [next copy];
    NSMutableArray *parts = [NSMutableArray arrayWithObjects:@"Hardcore", [_summary objectForKey:@"folder"],
                             [_summary objectForKey:@"program"], nil];
    NSString *status = [_summary objectForKey:@"status"];
    if ([status length]) [parts addObject:status];
    NSInteger tabs = [[_summary objectForKey:@"tabs"] integerValue];
    if (tabs > 0) [parts addObject:[NSString stringWithFormat:@"%ld %@", (long)tabs, tabs == 1 ? @"tab" : @"tabs"]];
    [self setAccessibilityLabel:[parts componentsJoinedByString:@", "]];
    [self setNeedsDisplay:YES];
    return YES;
}
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    hc_dock_draw_summary([self bounds], _summary);
}
- (void)dealloc {
    [_summary release];
    [super dealloc];
}
@end
