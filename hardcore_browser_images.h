#import <ImageIO/ImageIO.h>
#include "hardcore_browser_pixels.h"
static void browser_image_ready(int tabID);
@interface HCBrowserImage : NSObject <NSURLSessionDataDelegate> {
@public
    NSString *source;
    NSMutableData *received;
    NSData *rgba;
    NSURLSession *session;
    int owner,sw,sh,source_width,source_height,cols,rows,qw,qh;
    unsigned char *indexed;
    uint32_t palette[18],background;
    int failed;
}
- (id)initWithSource:(NSString *)url owner:(int)identity;
- (void)decode:(NSData *)data;
- (void)cancel;
@end
@implementation HCBrowserImage
- (id)initWithSource:(NSString *)url owner:(int)identity {
    if(!(self=[super init]))return nil;
    source=[url copy];owner=identity;received=[[NSMutableData alloc] init];
    if([url hasPrefix:@"data:image/"] && url.length<3*1024*1024) {
        NSRange comma=[url rangeOfString:@","];
        if(comma.location!=NSNotFound && [[url substringToIndex:comma.location] hasSuffix:@";base64"]) {
            NSData *bytes=[[[NSData alloc] initWithBase64EncodedString:[url substringFromIndex:comma.location+1] options:0] autorelease];
            if(bytes.length && bytes.length<=2*1024*1024)[self decode:bytes];
        }
        if(!rgba)failed=1;
    } else {
        NSURL *u=[NSURL URLWithString:url];
        if(![@[@"http",@"https"] containsObject:u.scheme.lowercaseString]) {failed=1;return self;}
        NSURLSessionConfiguration *config=[NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest=10;config.timeoutIntervalForResource=15;
        config.HTTPMaximumConnectionsPerHost=2;
        session=[[NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:[NSOperationQueue mainQueue]] retain];
        [[session dataTaskWithURL:u] resume];
    }
    return self;
}
- (void)decode:(NSData *)data {
    if(!data.length)return;
    CGImageSourceRef src=CGImageSourceCreateWithData((CFDataRef)data,NULL);if(!src)return;
    NSDictionary *properties=[(NSDictionary *)CGImageSourceCopyPropertiesAtIndex(src,0,NULL) autorelease];
    long long original_width=[properties[(id)kCGImagePropertyPixelWidth] longLongValue];
    long long original_height=[properties[(id)kCGImagePropertyPixelHeight] longLongValue];
    int orientation=[properties[(id)kCGImagePropertyOrientation] intValue];
    if(orientation>=5 && orientation<=8) {
        long long swap=original_width;original_width=original_height;original_height=swap;
    }
    CGImageRef image=CGImageSourceCreateThumbnailAtIndex(src,0,(CFDictionaryRef)@{
        (id)kCGImageSourceCreateThumbnailFromImageAlways:@YES,(id)kCGImageSourceThumbnailMaxPixelSize:@(BROWSER_IMAGE_DECODE_LIMIT),
        (id)kCGImageSourceCreateThumbnailWithTransform:@YES,(id)kCGImageSourceShouldCacheImmediately:@YES});
    CFRelease(src);if(!image)return;
    sw=(int)CGImageGetWidth(image);sh=(int)CGImageGetHeight(image);
    if(sw<1 || sh<1 || sw>BROWSER_IMAGE_DECODE_LIMIT || sh>BROWSER_IMAGE_DECODE_LIMIT){CGImageRelease(image);return;}
    if(original_width>0 && original_width<=INT_MAX && original_height>0 && original_height<=INT_MAX) {
        source_width=(int)original_width;source_height=(int)original_height;
    } else {
        if(source_width<1)source_width=sw;if(source_height<1)source_height=sh;
    }
    NSMutableData *pixels=[NSMutableData dataWithLength:(NSUInteger)sw*sh*4];
    CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context=CGBitmapContextCreate(pixels.mutableBytes,sw,sh,8,sw*4,color,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(color);
    if(context) {
        CGContextSetInterpolationQuality(context,kCGInterpolationNone);
        /* ImageIO already applies EXIF orientation. Bitmap memory is read
           top-to-bottom by the grid; a view-coordinate flip inverts it. */
        CGContextDrawImage(context,CGRectMake(0,0,sw,sh),image);
        [rgba release];rgba=[pixels copy];CGContextRelease(context);
    }
    CGImageRelease(image);
}
- (void)cancel { [session invalidateAndCancel];[session release];session=nil; }
- (void)dealloc { [self cancel];[source release];[received release];[rgba release];free(indexed);[super dealloc]; }
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    (void)s;(void)task;
    BOOL valid=response.expectedContentLength<=2*1024*1024;
    if([response isKindOfClass:NSHTTPURLResponse.class])valid=valid && [(NSHTTPURLResponse *)response statusCode]>=200 && [(NSHTTPURLResponse *)response statusCode]<300;
    completion(valid ? NSURLSessionResponseAllow : NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)s;
    if(received.length+data.length>2*1024*1024){[task cancel];return;}
    [received appendData:data];
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)s;(void)task;
    if(!error)[self decode:received];
    failed=!rgba;[received setLength:0];[self cancel];browser_image_ready(owner);
}
@end
static void browser_image_palette(HCBrowserImage *image,int w,int h) {
    uint32_t colors[18];for(int i=0;i<16;i++)colors[i]=g_shown->pal[i];colors[16]=DEF_BG;colors[17]=DEF_FG;
    if(image->qw==w && image->qh==h && image->background==DEF_BG && !memcmp(colors,image->palette,sizeof(colors)))return;
    free(image->indexed);image->indexed=NULL;
    if(!image->rgba)return;
    image->indexed=malloc((size_t)w*h);if(!image->indexed)return;
    memcpy(image->palette,colors,sizeof(colors));image->background=DEF_BG;image->qw=w;image->qh=h;
    browser_image_quantize(image->rgba.bytes,image->sw,image->sh,image->indexed,w,h,colors,18,DEF_BG);
}
