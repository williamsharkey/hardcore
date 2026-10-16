/* Portable Hardcore replay -> H.264/AAC MP4. No capture APIs or external codecs.
 * Build: clang -O2 -fobjc-arc -Wno-deprecated-declarations tools/recording_export.m
 *   -framework Foundation -framework AppKit -framework AVFoundation
 *   -framework CoreMedia -framework CoreVideo -framework CoreGraphics -framework ImageIO
 * Replay v1 uses milliseconds, top-left source pixels and original 20-byte LE quads.
 */
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static NSString *partialPath;
static void fail(NSString *message) {
    @throw [NSException exceptionWithName:@"HardcoreExportError" reason:message userInfo:nil];
}
static double number(id value, NSString *name) {
    if (![value isKindOfClass:NSNumber.class] || !isfinite([value doubleValue]))
        fail([NSString stringWithFormat:@"%@ must be a finite number", name]);
    return [value doubleValue];
}
static double optionalNumber(id value, double fallback, NSString *name) {
    return value && value != NSNull.null ? number(value, name) : fallback;
}
static NSDictionary *dictionary(id value, NSString *name) {
    if (![value isKindOfClass:NSDictionary.class]) fail([name stringByAppendingString:@" must be an object"]);
    return value;
}
static NSArray *array(id value, NSString *name) {
    if (![value isKindOfClass:NSArray.class]) fail([name stringByAppendingString:@" must be an array"]);
    return value;
}
static NSString *string(id value, NSString *name) {
    if (![value isKindOfClass:NSString.class]) fail([name stringByAppendingString:@" must be text"]);
    return value;
}
static NSData *base64(id value, NSString *name) {
    NSData *data = [[NSData alloc] initWithBase64EncodedString:string(value, name) options:0];
    if (!data) fail([name stringByAppendingString:@" is not valid base64"]);
    return data;
}
static NSString *bundleFile(NSString *bundle, NSString *relative) {
    string(relative, @"media filename");
    NSString *root = bundle.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    NSString *path = [root stringByAppendingPathComponent:relative].stringByStandardizingPath.stringByResolvingSymlinksInPath;
    if (relative.isAbsolutePath || ![path hasPrefix:[root stringByAppendingString:@"/"]])
        fail(@"A bundle media filename must stay inside the recording bundle");
    return path;
}
static id readJSON(NSString *path, BOOL required) {
    NSError *error;
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:path error:&error];
    if (!attributes) { if (required) fail(error.localizedDescription); return nil; }
    if ([attributes[NSFileSize] unsignedLongLongValue] > 512ull*1024*1024) fail(@"Replay JSON exceeds the 512 MiB export limit");
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:&error];
    if (!data) fail(error.localizedDescription);
    id result = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (!result) fail(error.localizedDescription);
    return result;
}
static CGRect rectValue(id value, NSString *name) {
    NSArray *a = array(value, name);
    if (a.count != 4) fail([name stringByAppendingString:@" requires x,y,width,height"]);
    CGRect rect = CGRectMake(number(a[0],name), number(a[1],name), number(a[2],name), number(a[3],name));
    if (rect.size.width <= 0 || rect.size.height <= 0) fail([name stringByAppendingString:@" must have positive dimensions"]);
    return rect;
}
static uint16_t little16(const uint8_t *p) { return p[0] | (uint16_t)p[1]<<8; }
static void drawImage(CGContextRef ctx, CGImageRef image, CGRect rect) {
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, rect.origin.x, rect.origin.y + rect.size.height);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextDrawImage(ctx, CGRectMake(0, 0, rect.size.width, rect.size.height), image);
    CGContextRestoreGState(ctx);
}
static CGColorSpaceRef colorSpace(void) { return CGColorSpaceCreateWithName(kCGColorSpaceSRGB); }

@interface HCReplay : NSObject {
    NSMutableData *_atlas;
    NSMutableData *_pixels;
    CGImageRef _image;
    NSUInteger _nextFrame;
    NSUInteger _nextPointer;
    NSDictionary *_pointer;
    NSMutableArray *_clicks;
    NSMutableArray *_scrolls;
    double _lastMotion;
    double _motionRunStart;
}
@property NSDictionary *replay;
@property NSDictionary *frame;
@property NSArray *frames;
@property NSArray *pointers;
@property NSArray *captions;
@property NSDictionary *camera;
@property double duration;
@property int width;
@property int height;
- (instancetype)initWithBundle:(NSString *)bundle;
- (void)advance:(double)time;
- (CGImageRef)image;
- (void)drawPointer:(CGContextRef)ctx time:(double)time;
@end
@implementation HCReplay
- (instancetype)initWithBundle:(NSString *)bundle {
    if (!(self = [super init])) return nil;
    _replay = dictionary(readJSON([bundle stringByAppendingPathComponent:@"replay.json"], YES), @"replay");
    if (number(_replay[@"version"], @"version") != 1) fail(@"Unsupported replay version");
    _duration = number(_replay[@"durationMs"], @"durationMs");
    if (_duration <= 0 || _duration > 7*24*3600*1000.0) fail(@"Replay duration must be between 0 and 7 days");
    _frames = array(_replay[@"frames"], @"frames");
    if (!_frames.count) fail(@"Replay has no frames");
    _pointers = _replay[@"pointer"] ? array(_replay[@"pointer"], @"pointer") : @[];
    _captions = _replay[@"captions"] ? array(_replay[@"captions"], @"captions") : @[];
    NSDictionary *manifest = readJSON([bundle stringByAppendingPathComponent:@"manifest.json"], NO);
    id camera = _replay[@"camera"] ?: manifest[@"camera"];
    if (camera && camera != NSNull.null) _camera = dictionary(camera, @"camera");
    for (NSArray *events in @[_frames, _pointers]) {
        double previous = -1;
        for (id item in events) {
            NSDictionary *event = dictionary(item, @"event");
            double t = number(event[@"t"], @"event.t");
            if (t < 0 || t < previous || t > _duration + 1) fail(@"Replay event times must increase within the recording duration");
            previous = t;
        }
    }
    _atlas = [NSMutableData dataWithLength:4096*4096];
    _clicks = [NSMutableArray array]; _scrolls = [NSMutableArray array];
    _lastMotion = -100000;
    [self advance:0];
    if (!_frame) [self render:_frames[0]];
    return self;
}
- (void)dealloc { if (_image) CGImageRelease(_image); }
- (CGImageRef)image { return _image; }
- (void)render:(NSDictionary *)frame {
    _frame = frame;
    double fw = number(frame[@"w"], @"frame.w"), fh = number(frame[@"h"], @"frame.h");
    if (fw < 1 || fh < 1 || fw > 8192 || fh > 8192 || floor(fw) != fw || floor(fh) != fh)
        fail(@"Replay frame dimensions must be integers between 1 and 8192");
    _width = (int)fw; _height = (int)fh;
    uint8_t *atlas = _atlas.mutableBytes;
    for (id entry in frame[@"atlas"] ? array(frame[@"atlas"], @"frame.atlas") : @[]) {
        NSDictionary *patch = dictionary(entry, @"atlas patch");
        double values[] = {number(patch[@"x"],@"atlas.x"),number(patch[@"y"],@"atlas.y"),number(patch[@"w"],@"atlas.w"),number(patch[@"h"],@"atlas.h")};
        for (int i=0;i<4;i++) if (floor(values[i]) != values[i] || values[i] < 0 || values[i] > 4096) fail(@"Invalid atlas patch bounds");
        int x=values[0], y=values[1], w=values[2], h=values[3];
        NSData *data = base64(patch[@"data"], @"atlas.data");
        if (x+w>4096 || y+h>4096 || data.length != (NSUInteger)w*h) fail(@"Atlas patch data does not match its bounds");
        for (int row=0;row<h;row++) memcpy(atlas+(y+row)*4096+x, (const uint8_t *)data.bytes+row*w, w);
    }
    NSData *quads = base64(frame[@"quads"], @"frame.quads");
    if (quads.length % 20 || quads.length > 20*1000000ull) fail(@"Replay quads must contain at most one million 20-byte instances");
    _pixels = [NSMutableData dataWithLength:(NSUInteger)_width*_height*4];
    uint8_t *pixels = _pixels.mutableBytes;
    uint32_t bg = (uint32_t)optionalNumber(frame[@"bg"],0,@"frame.bg");
    for (NSUInteger p=0;p<_pixels.length;p+=4) { pixels[p]=bg&255; pixels[p+1]=(bg>>8)&255; pixels[p+2]=(bg>>16)&255; pixels[p+3]=255; }
    const uint8_t *q = quads.bytes;
    for (NSUInteger i=0;i<quads.length;i+=20) {
        const uint8_t *v=q+i;
        int x0=(int16_t)little16(v),y0=(int16_t)little16(v+2),x1=(int16_t)little16(v+4),y1=(int16_t)little16(v+6);
        int u0=little16(v+8),v0=little16(v+10),u1=little16(v+12),v1=little16(v+14);
        if (x1<=x0 || y1<=y0) continue;
        int loX=MAX(0,x0),loY=MAX(0,y0),hiX=MIN(_width,x1),hiY=MIN(_height,y1);
        for (int y=loY;y<hiY;y++) {
            int av=(int)floor(v0+((double)y+.5-y0)*(v1-v0)/(y1-y0));
            uint8_t *dst=pixels+((NSUInteger)y*_width+loX)*4;
            for (int x=loX;x<hiX;x++,dst+=4) {
                unsigned coverage=255;
                if (u0!=65535) {
                    int au=(int)floor(u0+((double)x+.5-x0)*(u1-u0)/(x1-x0));
                    coverage=atlas[MAX(0,MIN(4095,av))*4096+MAX(0,MIN(4095,au))];
                }
                /* Preserve both 8-bit alpha factors until the final blend,
                   matching the renderer's normalized-float multiplication. */
                unsigned a=v[19]*coverage;
                if (!a) continue;
                unsigned inv=65025-a;
                dst[0]=(v[18]*a+dst[0]*inv+32512)/65025;
                dst[1]=(v[17]*a+dst[1]*inv+32512)/65025;
                dst[2]=(v[16]*a+dst[2]*inv+32512)/65025;
            }
        }
    }
    if (_image) CGImageRelease(_image);
    CGColorSpaceRef cs=colorSpace();
    CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)_pixels);
    _image=CGImageCreate(_width,_height,8,32,_width*4,cs,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst,provider,NULL,false,kCGRenderingIntentDefault);
    CGDataProviderRelease(provider); CGColorSpaceRelease(cs);
    if (!_image) fail(@"Could not render replay frame");
}
- (void)advance:(double)time {
    while (_nextFrame < _frames.count && [_frames[_nextFrame][@"t"] doubleValue] <= time) [self render:_frames[_nextFrame++]];
    while (_nextPointer < _pointers.count && [_pointers[_nextPointer][@"t"] doubleValue] <= time) {
        NSDictionary *p = _pointers[_nextPointer++];
        number(p[@"x"], @"pointer.x"); number(p[@"y"], @"pointer.y");
        if ([p[@"type"] isEqual:@"move"] || !_pointer || ![p[@"x"] isEqual:_pointer[@"x"]] || ![p[@"y"] isEqual:_pointer[@"y"]]) {
            double at=[p[@"t"] doubleValue];
            if (at-_lastMotion>240) _motionRunStart=at;
            _lastMotion=at;
        }
        if ([p[@"type"] isEqual:@"down"]) [_clicks addObject:p];
        if ([p[@"type"] isEqual:@"scroll"]) {
            optionalNumber(p[@"dx"],0,@"pointer.dx"); optionalNumber(p[@"dy"],0,@"pointer.dy");
            [_scrolls addObject:p];
        }
        _pointer=p;
    }
    while (_clicks.count && time-[_clicks[0][@"t"] doubleValue]>450) [_clicks removeObjectAtIndex:0];
    while (_scrolls.count && time-[_scrolls[0][@"t"] doubleValue]>450) [_scrolls removeObjectAtIndex:0];
}
- (void)drawPointer:(CGContextRef)ctx time:(double)time {
    for (NSDictionary *click in _clicks) {
        double age=(time-[click[@"t"] doubleValue])/450, radius=8+20*age;
        CGContextSetRGBStrokeColor(ctx,1,1,1,1-age);
        CGContextSetLineWidth(ctx,2);
        CGContextStrokeEllipseInRect(ctx,CGRectMake([click[@"x"] doubleValue]-radius,[click[@"y"] doubleValue]-radius,radius*2,radius*2));
    }
    NSDictionary *scroll=_scrolls.lastObject;
    if (scroll && (!scroll[@"visible"] || [scroll[@"visible"] boolValue])) {
        double dx=[scroll[@"dx"] doubleValue],dy=[scroll[@"dy"] doubleValue];
        if (dx || dy) {
            double ux=fabs(dx)>fabs(dy)?(dx>0?1:-1):0,uy=ux?0:(dy>0?1:-1);
            double cx=[scroll[@"x"] doubleValue]+20,cy=[scroll[@"y"] doubleValue]+4;
            double tipx=cx+ux*6,tipy=cy+uy*6;
            CGContextSetRGBStrokeColor(ctx,1,202/255.0,106/255.0,1-(time-[scroll[@"t"] doubleValue])/450);
            CGContextSetLineWidth(ctx,2); CGContextSetLineCap(ctx,kCGLineCapRound);
            CGContextMoveToPoint(ctx,cx-ux*6,cy-uy*6); CGContextAddLineToPoint(ctx,tipx,tipy);
            CGContextMoveToPoint(ctx,tipx-ux*4-uy*4,tipy-uy*4+ux*4); CGContextAddLineToPoint(ctx,tipx,tipy);
            CGContextAddLineToPoint(ctx,tipx-ux*4+uy*4,tipy-uy*4-ux*4); CGContextStrokePath(ctx);
        }
    }
    if (!_pointer || (_pointer[@"visible"] && ![_pointer[@"visible"] boolValue])) return;
    double size=14+9*fmin(1,fmax(0,(time-_motionRunStart)/80))*fmax(0,1-(time-_lastMotion)/240);
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx,[_pointer[@"x"] doubleValue],[_pointer[@"y"] doubleValue]);
    CGContextScaleCTM(ctx,size/14,size/14);
    CGContextMoveToPoint(ctx,0,0); CGContextAddLineToPoint(ctx,0,14); CGContextAddLineToPoint(ctx,3.8,10.3);
    CGContextAddLineToPoint(ctx,6.5,16.5); CGContextAddLineToPoint(ctx,9,15.4); CGContextAddLineToPoint(ctx,6.2,9.3);
    CGContextAddLineToPoint(ctx,11.5,9.2); CGContextClosePath(ctx);
    CGContextSetRGBFillColor(ctx,1,1,1,1); CGContextSetRGBStrokeColor(ctx,22/255.0,22/255.0,22/255.0,1); CGContextSetLineWidth(ctx,1.2); CGContextSetLineJoin(ctx,kCGLineJoinRound);
    CGContextDrawPath(ctx,kCGPathFillStroke); CGContextRestoreGState(ctx);
}
@end

@interface HCCamera : NSObject {
    CMSampleBufferRef _next;
    CGImageRef _image;
}
@property AVURLAsset *asset;
@property AVAssetReader *reader;
@property AVAssetReaderTrackOutput *output;
@property double offset;
@property double duration;
- (instancetype)initWithPath:(NSString *)path offset:(double)offset;
- (CGImageRef)imageAt:(double)time;
@end
@implementation HCCamera
- (instancetype)initWithPath:(NSString *)path offset:(double)offset {
    if (!(self=[super init])) return nil;
    _asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil]; _offset=offset;
    AVAssetTrack *track=[_asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    if (!track) fail([@"No video track in " stringByAppendingString:path]);
    _duration=CMTimeGetSeconds(_asset.duration)*1000;
    NSError *error;
    _reader=[[AVAssetReader alloc] initWithAsset:_asset error:&error];
    if (!_reader) fail(error.localizedDescription);
    _output=[AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)}];
    _output.alwaysCopiesSampleData=NO;
    [_reader addOutput:_output];
    if (![_reader startReading]) fail(_reader.error.localizedDescription);
    _next=[_output copyNextSampleBuffer];
    return self;
}
- (void)dealloc { if (_next) CFRelease(_next); if (_image) CGImageRelease(_image); }
- (CGImageRef)imageAt:(double)time {
    double local=time-_offset;
    if (local<0 || local>=_duration) return NULL;
    while (_next && CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(_next))*1000 <= local+.01) {
        CVPixelBufferRef buffer=CMSampleBufferGetImageBuffer(_next);
        CVPixelBufferLockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly);
        size_t w=CVPixelBufferGetWidth(buffer),h=CVPixelBufferGetHeight(buffer),stride=CVPixelBufferGetBytesPerRow(buffer);
        NSData *pixels=[NSData dataWithBytes:CVPixelBufferGetBaseAddress(buffer) length:stride*h];
        CVPixelBufferUnlockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly);
        CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels); CGColorSpaceRef cs=colorSpace();
        if (_image) CGImageRelease(_image);
        _image=CGImageCreate(w,h,8,32,stride,cs,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst,provider,NULL,false,kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs); CGDataProviderRelease(provider);
        CFRelease(_next); _next=[_output copyNextSampleBuffer];
    }
    if (_reader.status==AVAssetReaderStatusFailed) fail(_reader.error.localizedDescription);
    return _image;
}
@end

typedef struct { double start,end,snapshot; int width,height,fps; BOOL pointer,captions,hasCrop; CGRect crop; } Options;
static NSArray *readSRT(NSString *path) {
    NSError *error;
    NSString *text=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
    if (!text) fail(error.localizedDescription);
    text=[[text stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"] stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
    NSRegularExpression *rx=[NSRegularExpression regularExpressionWithPattern:@"(?m)^(?:[0-9]+\\n)?([0-9]{2,}):([0-9]{2}):([0-9]{2})[,.]([0-9]{3})[ \\t]+-->[ \\t]+([0-9]{2,}):([0-9]{2}):([0-9]{2})[,.]([0-9]{3})[^\\n]*\\n([\\s\\S]*?)(?=\\n\\n|\\z)" options:0 error:&error];
    NSMutableArray *cues=[NSMutableArray array];
    for (NSTextCheckingResult *m in [rx matchesInString:text options:0 range:NSMakeRange(0,text.length)]) {
        double a[8]; for (int i=0;i<8;i++) a[i]=[[text substringWithRange:[m rangeAtIndex:i+1]] doubleValue];
        double start=((a[0]*60+a[1])*60+a[2])*1000+a[3],end=((a[4]*60+a[5])*60+a[6])*1000+a[7];
        if (end>start) [cues addObject:@{@"start":@(start),@"end":@(end),@"text":[text substringWithRange:[m rangeAtIndex:9]]}];
    }
    if (!cues.count && [text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length) fail(@"Captions file has no valid SRT cues");
    return cues;
}
static void drawCaption(CGContextRef ctx, NSArray *captions, double time, int width, int height) {
    NSMutableArray *active=[NSMutableArray array];
    for (id value in captions) {
        NSDictionary *cue=dictionary(value,@"caption");
        double start=number(cue[@"start"],@"caption.start"),end=number(cue[@"end"],@"caption.end");
        if (time>=start && time<end) [active addObject:string(cue[@"text"],@"caption.text")];
    }
    if (!active.count) return;
    NSString *text=[active componentsJoinedByString:@"\n"];
    NSMutableParagraphStyle *style=[[NSMutableParagraphStyle alloc] init]; style.alignment=NSTextAlignmentCenter;
    NSDictionary *attrs=@{NSFontAttributeName:[NSFont systemFontOfSize:fmax(16,height*.032) weight:NSFontWeightMedium],NSForegroundColorAttributeName:NSColor.whiteColor,NSParagraphStyleAttributeName:style};
    NSAttributedString *label=[[NSAttributedString alloc] initWithString:text attributes:attrs];
    CGFloat maxWidth=fmax(1,width*.9-24);
    NSRect bounds=[label boundingRectWithSize:CGSizeMake(maxWidth,height*.65) options:NSStringDrawingUsesLineFragmentOrigin|NSStringDrawingUsesFontLeading];
    CGFloat w=ceil(fmin(maxWidth,bounds.size.width)),h=ceil(fmin(height*.65,bounds.size.height));
    CGRect box=CGRectMake((width-w)/2-12,height-fmax(16,height*.04)-h-16,w+24,h+16);
    CGPathRef rounded=CGPathCreateWithRoundedRect(box,5,5,NULL);
    CGContextSetRGBFillColor(ctx,0,0,0,.76); CGContextAddPath(ctx,rounded); CGContextFillPath(ctx); CGPathRelease(rounded);
    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithCGContext:ctx flipped:YES]];
    [label drawWithRect:NSMakeRect(box.origin.x+12,box.origin.y+8,w,h) options:NSStringDrawingUsesLineFragmentOrigin|NSStringDrawingUsesFontLeading];
    [NSGraphicsContext restoreGraphicsState];
}
static void render(CGContextRef ctx, HCReplay *replay, HCCamera *camera, Options o, double time) {
    [replay advance:time];
    CGRect crop=o.hasCrop?o.crop:CGRectMake(0,0,replay.width,replay.height);
    double scale=fmin(o.width/crop.size.width,o.height/crop.size.height);
    double dx=(o.width-crop.size.width*scale)/2,dy=(o.height-crop.size.height*scale)/2;
    CGContextSetRGBFillColor(ctx,0,0,0,1); CGContextFillRect(ctx,CGRectMake(0,0,o.width,o.height));
    CGContextSaveGState(ctx);
    CGContextClipToRect(ctx,CGRectMake(dx,dy,crop.size.width*scale,crop.size.height*scale));
    CGContextTranslateCTM(ctx,dx-crop.origin.x*scale,dy-crop.origin.y*scale); CGContextScaleCTM(ctx,scale,scale);
    CGContextSetInterpolationQuality(ctx,kCGInterpolationNone);
    drawImage(ctx,replay.image,CGRectMake(0,0,replay.width,replay.height));
    NSDictionary *placement=replay.frame[@"camera"];
    if (camera && (!placement[@"visible"] || [placement[@"visible"] boolValue])) {
        CGImageRef image=[camera imageAt:time];
        if (image) {
            CGFloat w=replay.width*.24,h=w*9/16;
            CGRect rect=placement[@"rect"]?rectValue(placement[@"rect"],@"camera.rect"):CGRectMake(replay.width-w-24,replay.height-h-24,w,h);
            CGContextSaveGState(ctx);
            CGContextClipToRect(ctx,rect);
            CGContextSetRGBFillColor(ctx,0,0,0,1); CGContextFillRect(ctx,rect);
            BOOL mirrored=placement[@"mirrored"]?[placement[@"mirrored"] boolValue]:[replay.camera[@"mirrored"] boolValue];
            if (mirrored) { CGContextTranslateCTM(ctx,CGRectGetMidX(rect)*2,0); CGContextScaleCTM(ctx,-1,1); }
            double fit=fmin(rect.size.width/CGImageGetWidth(image),rect.size.height/CGImageGetHeight(image));
            CGRect fill=CGRectMake(CGRectGetMidX(rect)-CGImageGetWidth(image)*fit/2,CGRectGetMidY(rect)-CGImageGetHeight(image)*fit/2,CGImageGetWidth(image)*fit,CGImageGetHeight(image)*fit);
            CGContextSetInterpolationQuality(ctx,kCGInterpolationMedium); drawImage(ctx,image,fill); CGContextRestoreGState(ctx);
        }
    }
    if (o.pointer) [replay drawPointer:ctx time:time];
    CGContextRestoreGState(ctx);
    if (o.captions) drawCaption(ctx,replay.captions,time,o.width,o.height);
}
static CGContextRef pixelContext(CVPixelBufferRef buffer) {
    CGColorSpaceRef cs=colorSpace();
    CGContextRef ctx=CGBitmapContextCreate(CVPixelBufferGetBaseAddress(buffer),CVPixelBufferGetWidth(buffer),CVPixelBufferGetHeight(buffer),8,CVPixelBufferGetBytesPerRow(buffer),cs,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(cs);
    if (!ctx) fail(@"Could not create export frame context");
    CGContextTranslateCTM(ctx,0,CVPixelBufferGetHeight(buffer)); CGContextScaleCTM(ctx,1,-1);
    return ctx;
}
static AVAssetReader *audioReader(NSString *bundle, HCReplay *replay, Options o, AVAssetReaderAudioMixOutput **output) {
    NSDictionary *media=replay.replay[@"audio"] ?: replay.camera;
    if (!media || !media[@"file"]) return nil;
    NSString *path=bundleFile(bundle,media[@"file"]);
    AVURLAsset *asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:path] options:nil];
    AVAssetTrack *track=[asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
    if (!track) return nil;
    double offset=optionalNumber(media[@"offsetMs"],0,@"audio.offsetMs")/1000;
    double sourceStart=fmax(0,o.start-offset),sourceEnd=fmin(CMTimeGetSeconds(asset.duration),o.end-offset);
    if (sourceEnd<=sourceStart) return nil;
    AVMutableComposition *composition=[AVMutableComposition composition];
    AVMutableCompositionTrack *dest=[composition addMutableTrackWithMediaType:AVMediaTypeAudio preferredTrackID:kCMPersistentTrackID_Invalid];
    NSError *error;
    CMTimeRange range=CMTimeRangeMake(CMTimeMakeWithSeconds(sourceStart,48000),CMTimeMakeWithSeconds(sourceEnd-sourceStart,48000));
    if (![dest insertTimeRange:range ofTrack:track atTime:CMTimeMakeWithSeconds(fmax(0,offset-o.start),48000) error:&error]) fail(error.localizedDescription);
    AVAssetReader *reader=[[AVAssetReader alloc] initWithAsset:composition error:&error];
    if (!reader) fail(error.localizedDescription);
    *output=[AVAssetReaderAudioMixOutput assetReaderAudioMixOutputWithAudioTracks:@[dest] audioSettings:@{AVFormatIDKey:@(kAudioFormatLinearPCM),AVSampleRateKey:@48000,AVNumberOfChannelsKey:@2,AVLinearPCMBitDepthKey:@16,AVLinearPCMIsFloatKey:@NO,AVLinearPCMIsBigEndianKey:@NO,AVLinearPCMIsNonInterleaved:@NO}];
    (*output).alwaysCopiesSampleData=NO;
    [reader addOutput:*output];
    if (![reader startReading]) fail(reader.error.localizedDescription);
    return reader;
}
static void exportMovie(NSString *bundle,NSString *destination,HCReplay *replay,HCCamera *camera,Options o) {
    NSError *error;
    AVAssetWriter *writer=[[AVAssetWriter alloc] initWithURL:[NSURL fileURLWithPath:partialPath] fileType:AVFileTypeMPEG4 error:&error];
    if (!writer) fail(error.localizedDescription);
    writer.shouldOptimizeForNetworkUse=YES;
    NSDictionary *compression=@{AVVideoAverageBitRateKey:@((int)fmin(16000000,fmax(1500000,o.width*o.height*o.fps*.13))),AVVideoProfileLevelKey:AVVideoProfileLevelH264HighAutoLevel,AVVideoMaxKeyFrameIntervalKey:@(o.fps*2),AVVideoAllowFrameReorderingKey:@NO};
    AVAssetWriterInput *video=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@(o.width),AVVideoHeightKey:@(o.height),AVVideoCompressionPropertiesKey:compression}];
    video.expectsMediaDataInRealTime=NO;
    NSDictionary *pixelAttributes=@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),(id)kCVPixelBufferWidthKey:@(o.width),(id)kCVPixelBufferHeightKey:@(o.height),(id)kCVPixelBufferCGImageCompatibilityKey:@YES,(id)kCVPixelBufferCGBitmapContextCompatibilityKey:@YES};
    AVAssetWriterInputPixelBufferAdaptor *adaptor=[AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:video sourcePixelBufferAttributes:pixelAttributes];
    [writer addInput:video];
    AVAssetReaderAudioMixOutput *audioOutput;
    AVAssetReader *reader=audioReader(bundle,replay,o,&audioOutput);
    AVAssetWriterInput *audio;
    if (reader) {
        audio=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{AVFormatIDKey:@(kAudioFormatMPEG4AAC),AVSampleRateKey:@48000,AVNumberOfChannelsKey:@2,AVEncoderBitRateKey:@128000}];
        audio.expectsMediaDataInRealTime=NO; [writer addInput:audio];
    }
    if (![writer startWriting]) fail(writer.error.localizedDescription);
    [writer startSessionAtSourceTime:kCMTimeZero];
    NSUInteger count=(NSUInteger)ceil((o.end-o.start)*o.fps),index=0;
    BOOL videoDone=NO,audioDone=!audio;
    double lastProgress=NSDate.timeIntervalSinceReferenceDate;
    while (!videoDone || !audioDone) {
        @autoreleasepool {
            BOOL progress=NO;
            if (!videoDone && video.readyForMoreMediaData) {
                if (index==count) { [video markAsFinished]; videoDone=YES; }
                else {
                    CVPixelBufferRef buffer;
                    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,adaptor.pixelBufferPool,&buffer)!=kCVReturnSuccess) fail(@"Could not allocate export frame");
                    CVPixelBufferLockBaseAddress(buffer,0);
                    CGContextRef ctx=pixelContext(buffer);
                    render(ctx,replay,camera,o,(o.start+(double)index/o.fps)*1000);
                    CGContextRelease(ctx); CVPixelBufferUnlockBaseAddress(buffer,0);
                    BOOL appended=[adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(index,o.fps)];
                    CVPixelBufferRelease(buffer);
                    if (!appended) fail(writer.error.localizedDescription ?: @"Could not encode video frame");
                    index++;
                    if (index==1 || index%(o.fps*5)==0) fprintf(stderr,"hardcore export: %lu/%lu frames\n",(unsigned long)index,(unsigned long)count);
                }
                progress=YES;
            }
            if (!audioDone && audio.readyForMoreMediaData) {
                CMSampleBufferRef sample=[audioOutput copyNextSampleBuffer];
                if (!sample) {
                    if (reader.status==AVAssetReaderStatusFailed) fail(reader.error.localizedDescription);
                    [audio markAsFinished]; audioDone=YES;
                } else {
                    BOOL appended=[audio appendSampleBuffer:sample]; CFRelease(sample);
                    if (!appended) fail(writer.error.localizedDescription ?: @"Could not encode audio sample");
                }
                progress=YES;
            }
            if (writer.status==AVAssetWriterStatusFailed) fail(writer.error.localizedDescription);
            if (progress) lastProgress=NSDate.timeIntervalSinceReferenceDate;
            else { if (NSDate.timeIntervalSinceReferenceDate-lastProgress>60) fail(@"Video encoder stopped making progress"); usleep(1000); }
        }
    }
    [writer endSessionAtSourceTime:CMTimeMakeWithSeconds(o.end-o.start,60000)];
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    if (dispatch_semaphore_wait(done,dispatch_time(DISPATCH_TIME_NOW,60*NSEC_PER_SEC))) fail(@"Timed out finalizing MP4");
    if (writer.status!=AVAssetWriterStatusCompleted) fail(writer.error.localizedDescription ?: @"MP4 export failed");
    if (![NSFileManager.defaultManager moveItemAtPath:partialPath toPath:destination error:&error]) fail(error.localizedDescription);
    partialPath=nil;
}
static void snapshot(NSString *path,HCReplay *replay,HCCamera *camera,Options o) {
    CVPixelBufferRef buffer;
    if (CVPixelBufferCreate(kCFAllocatorDefault,o.width,o.height,kCVPixelFormatType_32BGRA,(__bridge CFDictionaryRef)@{(id)kCVPixelBufferCGImageCompatibilityKey:@YES,(id)kCVPixelBufferCGBitmapContextCompatibilityKey:@YES},&buffer)!=kCVReturnSuccess) fail(@"Could not allocate snapshot");
    CVPixelBufferLockBaseAddress(buffer,0); CGContextRef ctx=pixelContext(buffer);
    render(ctx,replay,camera,o,o.snapshot*1000);
    CGImageRef image=CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef dest=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],CFSTR("public.png"),1,NULL);
    if (!dest) fail(@"Could not create PNG output");
    CGImageDestinationAddImage(dest,image,NULL);
    BOOL ok=CGImageDestinationFinalize(dest);
    CFRelease(dest); CGImageRelease(image); CGContextRelease(ctx); CVPixelBufferUnlockBaseAddress(buffer,0); CVPixelBufferRelease(buffer);
    if (!ok) fail(@"Could not write PNG output");
}
static double argumentNumber(const char *arg) {
    char *end; double value=strtod(arg,&end);
    if (!*arg || *end || !isfinite(value)) fail(@"Expected a finite numeric command-line value");
    return value;
}
static void usage(void) {
    puts("Usage: recording-export BUNDLE OUTPUT.mp4 [options]\n"
         "  --start SECONDS       Trim start (default 0)\n"
         "  --end SECONDS         Trim end (default recording end)\n"
         "  --crop X,Y,W,H        Source-pixel crop, top-left origin\n"
         "  --width PIXELS        Output width (even; aspect retained)\n"
         "  --height PIXELS       Output height (even; aspect retained)\n"
         "  --fps FPS             1 to 60 (default 30)\n"
         "  --no-pointer          Hide pointer and click cues\n"
         "  --no-captions         Hide captions\n"
         "  --captions FILE.srt   Use SRT captions on original timeline\n"
         "  --snapshot SECONDS    Write a PNG preview at this source time\n"
         "Default output fits within 1920x1920. Uses H.264 video and AAC audio.\n"
         "Output files must not already exist. No camera or microphone is opened.");
}
int main(int argc,const char **argv) {
    @autoreleasepool { @try {
        if (argc==2 && !strcmp(argv[1],"--help")) { usage(); return 0; }
        if (argc<3) { usage(); return 2; }
        Options o={.end=-1,.snapshot=-1,.fps=30,.pointer=YES,.captions=YES};
        NSString *bundle=[[NSString stringWithUTF8String:argv[1]] stringByStandardizingPath];
        if (!bundle.isAbsolutePath) bundle=[NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:bundle];
        NSString *destination=[[NSString stringWithUTF8String:argv[2]] stringByStandardizingPath];
        if (!destination.isAbsolutePath) destination=[NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:destination];
        NSString *captions;
        for (int i=3;i<argc;i++) {
            const char *key=argv[i];
            if (!strcmp(key,"--no-pointer")) { o.pointer=NO; continue; }
            if (!strcmp(key,"--no-captions")) { o.captions=NO; continue; }
            if (++i>=argc) fail(@"Missing option value");
            if (!strcmp(key,"--captions")) { captions=[NSString stringWithUTF8String:argv[i]]; continue; }
            if (!strcmp(key,"--crop")) {
                NSArray *parts=[[NSString stringWithUTF8String:argv[i]] componentsSeparatedByString:@","];
                NSMutableArray *coords=[NSMutableArray array];
                for (NSString *part in parts) [coords addObject:@(argumentNumber(part.UTF8String))];
                o.crop=rectValue(coords,@"crop"); o.hasCrop=YES; continue;
            }
            double n=argumentNumber(argv[i]);
            if (!strcmp(key,"--start") || !strcmp(key,"--end") || !strcmp(key,"--snapshot")) {
                if (n<0) fail(@"Timeline values must be nonnegative");
                if (!strcmp(key,"--start")) o.start=n;
                else if (!strcmp(key,"--end")) o.end=n;
                else o.snapshot=n;
            }
            else if (!strcmp(key,"--width") || !strcmp(key,"--height") || !strcmp(key,"--fps")) {
                if (n!=floor(n) || n<1 || n>8192) fail(@"Dimensions and fps must be positive integers");
                if (!strcmp(key,"--width")) o.width=n;
                else if (!strcmp(key,"--height")) o.height=n;
                else o.fps=n;
            } else fail([@"Unknown option: " stringByAppendingString:[NSString stringWithUTF8String:key]]);
        }
        if ([NSFileManager.defaultManager fileExistsAtPath:destination]) fail(@"Output already exists; choose a new filename");
        HCReplay *replay=[[HCReplay alloc] initWithBundle:bundle];
        if (captions) replay.captions=readSRT(captions);
        if (o.end<0) o.end=replay.duration/1000;
        if (o.start<0 || o.end<=o.start || o.end>replay.duration/1000+.000001 || o.fps>60) fail(@"Invalid trim range or frame rate");
        if (o.snapshot>=0 && o.snapshot>replay.duration/1000) fail(@"Snapshot time exceeds recording duration");
        CGRect crop=o.hasCrop?o.crop:CGRectMake(0,0,replay.width,replay.height);
        if (o.hasCrop && (crop.origin.x<0 || crop.origin.y<0 || CGRectGetMaxX(crop)>replay.width || CGRectGetMaxY(crop)>replay.height)) fail(@"Crop must fit within the first replay frame");
        if (!o.width && !o.height) { double scale=fmin(1,1920/fmax(crop.size.width,crop.size.height)); o.width=MAX(2,((int)(crop.size.width*scale)/2)*2); o.height=MAX(2,((int)(crop.size.height*scale)/2)*2); }
        else if (!o.width) o.width=MAX(2,((int)round(o.height*crop.size.width/crop.size.height)/2)*2);
        else if (!o.height) o.height=MAX(2,((int)round(o.width*crop.size.height/crop.size.width)/2)*2);
        if (o.width<2 || o.height<2 || o.width>4096 || o.height>4096 || o.width%2 || o.height%2) fail(@"Output dimensions must be even and between 2 and 4096 pixels");
        HCCamera *camera;
        if (replay.camera[@"file"]) camera=[[HCCamera alloc] initWithPath:bundleFile(bundle,replay.camera[@"file"]) offset:optionalNumber(replay.camera[@"offsetMs"],0,@"camera.offsetMs")];
        partialPath=[destination stringByAppendingFormat:@".%@.partial",NSUUID.UUID.UUIDString];
        if (o.snapshot>=0) {
            snapshot(partialPath,replay,camera,o);
            NSError *error;
            if (![NSFileManager.defaultManager moveItemAtPath:partialPath toPath:destination error:&error]) fail(error.localizedDescription);
            partialPath=nil;
        } else exportMovie(bundle,destination,replay,camera,o);
        printf("%s\n",destination.UTF8String); return 0;
    } @catch (NSException *exception) {
        if (partialPath) [NSFileManager.defaultManager removeItemAtPath:partialPath error:nil];
        fprintf(stderr,"hardcore export: %s\n",exception.reason.UTF8String); return 1;
    } }
}
