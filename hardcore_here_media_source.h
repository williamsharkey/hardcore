/* Bounded media sources for the native `here` renderer. No screen capture,
 * audio input, recording, interpreter, or GUI window is involved. A source is
 * opened only by the explicit media helper, never by the persistent PTY owner.
 * RGBA frames are top-to-bottom, straight alpha; the caller owns its copy. */
#ifndef HARDCORE_HERE_MEDIA_SOURCE_H
#define HARDCORE_HERE_MEDIA_SOURCE_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define HC_HERE_MEDIA_WIDTH 640
#define HC_HERE_MEDIA_HEIGHT 480
#define HC_HERE_MEDIA_FILE_MAX (64u * 1024u * 1024u)

typedef struct HcHereMediaSource {
    pthread_mutex_t lock;
    unsigned char *pixels;
    int width, height, failed, camera, pending;
    uint64_t revision;
    double opened, last_frame;
    char message[256];
    void *controller;
} HcHereMediaSource;

static double hc_here_media_now(void) {
    struct timespec value; clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
}
static void hc_here_media_error(char *error,size_t cap,const char *text) {
    if(error && cap) {
        snprintf(error,cap,"%s",text?text:"");
        for(char *p=error;*p;p++)if((unsigned char)*p<32 || (unsigned char)*p==127)*p=' ';
    }
}
static HcHereMediaSource *hc_here_media_alloc(char *error,size_t cap) {
    HcHereMediaSource *source=calloc(1,sizeof(*source));
    if(!source){hc_here_media_error(error,cap,"Not enough memory for media preview.");return NULL;}
    if(pthread_mutex_init(&source->lock,NULL)) {
        free(source);hc_here_media_error(error,cap,"Could not initialize media preview.");return NULL;
    }
    source->opened=hc_here_media_now();return source;
}
static void hc_here_media_set_message(HcHereMediaSource *source,int failed,const char *message) {
    pthread_mutex_lock(&source->lock);source->failed=failed;
    hc_here_media_error(source->message,sizeof(source->message),message);
    pthread_mutex_unlock(&source->lock);
}
/* Ownership is transferred even on failure. There is never a frame queue. */
static int hc_here_media_publish(HcHereMediaSource *source,unsigned char *pixels,int width,int height) {
    if(!source || !pixels || width<1 || height<1 || width>HC_HERE_MEDIA_WIDTH || height>HC_HERE_MEDIA_HEIGHT) {
        free(pixels);return 0;
    }
    pthread_mutex_lock(&source->lock);
    if(source->failed){pthread_mutex_unlock(&source->lock);free(pixels);return 0;}
    free(source->pixels);source->pixels=pixels;source->width=width;source->height=height;
    source->revision++;if(!source->revision)source->revision=1;
    source->failed=source->pending=0;source->message[0]=0;source->last_frame=hc_here_media_now();
    pthread_mutex_unlock(&source->lock);return 1;
}
/* 1=new frame, 0=unchanged/waiting, -1=error. revision must start at zero.
 * On 0, error is an optional nonfatal status message (permission/startup).
 * A failed source can still be closed normally. The returned buffer is reusable
 * across frames and sources; free it after the final call. */
static int hc_here_media_copy_frame(HcHereMediaSource *source,unsigned char **rgba,size_t *capacity,
                                    int *width,int *height,uint64_t *revision,char *error,size_t cap) {
    if(!source || !rgba || !capacity || !width || !height || !revision) {
        hc_here_media_error(error,cap,"Invalid media source.");return -1;
    }
    pthread_mutex_lock(&source->lock);
    double now=hc_here_media_now();
    if(source->camera && !source->failed && !source->pending &&
       now-(source->last_frame?source->last_frame:source->opened)>15.0) {
        source->failed=1;hc_here_media_error(source->message,sizeof(source->message),
            source->last_frame?"Camera stopped delivering video. Close and reopen the preview.":
            "Camera did not deliver video. Check camera availability, then reopen the preview.");
    }
    /* A permission sheet remains user-controlled, but it cannot leave a view
       spinning forever. Closing the source cancels eventual startup safely. */
    if(source->camera && source->pending && now-source->opened>120.0) {
        source->failed=1;hc_here_media_error(source->message,sizeof(source->message),
            "Camera permission timed out. Allow access in System Settings, then reopen the preview.");
    }
    hc_here_media_error(error,cap,source->message);
    if(source->failed){pthread_mutex_unlock(&source->lock);return -1;}
    if(!source->pixels || *revision==source->revision){pthread_mutex_unlock(&source->lock);return 0;}
    size_t needed=(size_t)source->width*(size_t)source->height*4;
    if(*capacity<needed || !*rgba) {
        unsigned char *next=realloc(*rgba,needed);
        if(!next){pthread_mutex_unlock(&source->lock);hc_here_media_error(error,cap,"Not enough memory for video frame.");return -1;}
        *rgba=next;*capacity=needed;
    }
    memcpy(*rgba,source->pixels,needed);*width=source->width;*height=source->height;*revision=source->revision;
    pthread_mutex_unlock(&source->lock);return 1;
}

#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <CoreGraphics/CoreGraphics.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreMedia/CoreMedia.h>

@interface HCHereMediaCamera : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
    HcHereMediaSource *_source;
    dispatch_queue_t _queue;
    AVCaptureSession *_session;
    AVCaptureVideoDataOutput *_output;
    BOOL _closed;
    double _lastSample;
}
- (instancetype)initWithSource:(HcHereMediaSource *)source;
- (void)prepare;
- (void)shutdown;
- (void)configure;
- (void)consumeSample:(CMSampleBufferRef)sample;
- (AVAuthorizationStatus)authorizationStatus;
- (void)requestAccess:(void (^)(BOOL))completion;
@end

@implementation HCHereMediaCamera
- (instancetype)initWithSource:(HcHereMediaSource *)source {
    if((self=[super init])) {
        _source=source;_queue=dispatch_queue_create("app.hardcore.here.camera",DISPATCH_QUEUE_SERIAL);
    }
    return self;
}
- (void)dealloc {
    [_output release];[_session release];
    if(_queue)dispatch_release(_queue);
    [super dealloc];
}
- (AVAuthorizationStatus)authorizationStatus {
    return [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
}
- (void)requestAccess:(void (^)(BOOL))completion {
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:completion];
}
- (void)prepare {
    AVAuthorizationStatus status=[self authorizationStatus];
    if(status==AVAuthorizationStatusNotDetermined) {
        pthread_mutex_lock(&_source->lock);_source->pending=1;pthread_mutex_unlock(&_source->lock);
        hc_here_media_set_message(_source,0,"Waiting for camera permission...");
        [self requestAccess:^(BOOL allowed) {
            dispatch_async(self->_queue,^{
                if(self->_closed)return;
                if(!allowed)hc_here_media_set_message(self->_source,1,
                    "Camera access denied. Allow Hardcore or your terminal in System Settings > Privacy & Security > Camera.");
                else [self configure];
            });
        }];
    } else if(status!=AVAuthorizationStatusAuthorized) {
        hc_here_media_set_message(_source,1,
            "Camera access denied. Allow Hardcore or your terminal in System Settings > Privacy & Security > Camera.");
    } else dispatch_async(_queue,^{[self configure];});
}
- (void)captureFailed:(NSNotification *)notification {
    NSError *error=[notification.userInfo objectForKey:AVCaptureSessionErrorKey];
    NSString *message=error.localizedDescription?:@"Camera interrupted. Close and reopen the preview.";
    dispatch_async(_queue,^{if(!self->_closed)hc_here_media_set_message(self->_source,1,message.UTF8String);});
}
- (void)configure {
    @autoreleasepool {
        if(_closed)return;
        pthread_mutex_lock(&_source->lock);
        BOOL cancelled=_source->failed;_source->pending=0;_source->opened=hc_here_media_now();
        pthread_mutex_unlock(&_source->lock);
        if(cancelled)return;
        hc_here_media_set_message(_source,0,"Opening camera...");
        AVCaptureDevice *camera=nil,*builtIn=nil;
        for(AVCaptureDevice *device in [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]) {
            if(device.position==AVCaptureDevicePositionFront){camera=device;break;}
            if([device.deviceType isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera])builtIn=device;
        }
        if(!camera)camera=builtIn?:[AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if(!camera){hc_here_media_set_message(_source,1,"No camera is available. Connect a camera and reopen the preview.");return;}
        NSError *error=nil;
        AVCaptureDeviceInput *input=[AVCaptureDeviceInput deviceInputWithDevice:camera error:&error];
        if(!input){hc_here_media_set_message(_source,1,error.localizedDescription.UTF8String?:"Could not open the camera.");return;}
        _session=[[AVCaptureSession alloc]init];[_session beginConfiguration];
        if([_session canSetSessionPreset:AVCaptureSessionPreset640x480])_session.sessionPreset=AVCaptureSessionPreset640x480;
        else if([_session canSetSessionPreset:AVCaptureSessionPresetLow])_session.sessionPreset=AVCaptureSessionPresetLow;
        if(![_session canAddInput:input]) {
            [_session commitConfiguration];hc_here_media_set_message(_source,1,"Camera input is unavailable.");return;
        }
        [_session addInput:input];
        _output=[[AVCaptureVideoDataOutput alloc]init];_output.alwaysDiscardsLateVideoFrames=YES;
        _output.videoSettings=@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)};
        [_output setSampleBufferDelegate:self queue:_queue];
        if(![_session canAddOutput:_output]) {
            [_session commitConfiguration];hc_here_media_set_message(_source,1,"Camera cannot provide a video preview.");return;
        }
        [_session addOutput:_output];
        AVCaptureConnection *connection=[_output connectionWithMediaType:AVMediaTypeVideo];
        if(connection.isVideoMirroringSupported) {
            connection.automaticallyAdjustsVideoMirroring=NO;
            connection.videoMirrored=camera.position==AVCaptureDevicePositionFront || camera==builtIn;
        }
        [_session commitConfiguration];
        if([camera lockForConfiguration:NULL]) {
            for(AVFrameRateRange *range in camera.activeFormat.videoSupportedFrameRateRanges) {
                if(range.minFrameRate<=12 && range.maxFrameRate>=12) {
                    camera.activeVideoMinFrameDuration=CMTimeMake(1,12);
                    camera.activeVideoMaxFrameDuration=CMTimeMake(1,12);break;
                }
            }
            [camera unlockForConfiguration];
        }
        NSNotificationCenter *center=[NSNotificationCenter defaultCenter];
        [center addObserver:self selector:@selector(captureFailed:) name:AVCaptureSessionRuntimeErrorNotification object:_session];
        [center addObserver:self selector:@selector(captureFailed:) name:AVCaptureSessionWasInterruptedNotification object:_session];
        [_session startRunning];
        if(!_session.running)hc_here_media_set_message(_source,1,"Camera did not start. It may be in use by another application.");
    }
}
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection *)connection {
    (void)output;(void)connection;[self consumeSample:sample];
}
- (void)consumeSample:(CMSampleBufferRef)sample {
    @autoreleasepool {
        if(_closed || !_source)return;
        double now=hc_here_media_now();if(now-_lastSample<1.0/12.0)return;_lastSample=now;
        CVPixelBufferRef buffer=CMSampleBufferGetImageBuffer(sample);
        if(!buffer || CVPixelBufferGetPixelFormatType(buffer)!=kCVPixelFormatType_32BGRA)return;
        size_t sw=CVPixelBufferGetWidth(buffer),sh=CVPixelBufferGetHeight(buffer);
        if(!sw || !sh || sw>16384 || sh>16384)return;
        double scale=1.0;
        if(sw>HC_HERE_MEDIA_WIDTH)scale=(double)HC_HERE_MEDIA_WIDTH/sw;
        if(sh*scale>HC_HERE_MEDIA_HEIGHT)scale=(double)HC_HERE_MEDIA_HEIGHT/sh;
        int width=(int)(sw*scale),height=(int)(sh*scale);if(width<1)width=1;if(height<1)height=1;
        unsigned char *pixels=malloc((size_t)width*height*4);
        if(!pixels){hc_here_media_set_message(_source,1,"Not enough memory for camera preview.");return;}
        if(CVPixelBufferLockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly)!=kCVReturnSuccess){free(pixels);return;}
        const unsigned char *base=CVPixelBufferGetBaseAddress(buffer);size_t stride=CVPixelBufferGetBytesPerRow(buffer);
        if(!base || stride<sw*4){CVPixelBufferUnlockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly);free(pixels);return;}
        for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
            const unsigned char *p=base+(size_t)y*sh/height*stride+(size_t)x*sw/width*4;
            unsigned char *q=pixels+((size_t)y*width+x)*4;
            q[0]=p[2];q[1]=p[1];q[2]=p[0];q[3]=255;
        }
        CVPixelBufferUnlockBaseAddress(buffer,kCVPixelBufferLock_ReadOnly);
        hc_here_media_publish(_source,pixels,width,height);
    }
}
- (void)shutdown {
    dispatch_sync(_queue,^{
        self->_closed=YES;
        [[NSNotificationCenter defaultCenter] removeObserver:self];
        [self->_output setSampleBufferDelegate:nil queue:NULL];
        [self->_session stopRunning];self->_source=NULL;
    });
}
@end

static HcHereMediaSource *hc_here_media_open_image(const char *path,char *error,size_t cap) {
    hc_here_media_error(error,cap,"");
    @autoreleasepool {
        int fd=path?open(path,O_RDONLY|O_NONBLOCK|O_CLOEXEC):-1;
        struct stat st;
        if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode)) {
            if(fd>=0)close(fd);
            hc_here_media_error(error,cap,"Choose a regular image file.");return NULL;
        }
        if(st.st_size<=0 || (uint64_t)st.st_size>HC_HERE_MEDIA_FILE_MAX) {
            close(fd);hc_here_media_error(error,cap,"Image file must be between 1 byte and 64 MiB.");return NULL;
        }
        size_t length=(size_t)st.st_size,offset=0;
        unsigned char *bytes=malloc(length);
        if(!bytes){close(fd);hc_here_media_error(error,cap,"Not enough memory to read image.");return NULL;}
        while(offset<length) {
            ssize_t got=read(fd,bytes+offset,length-offset);
            if(got<0 && errno==EINTR)continue;
            if(got<=0)break;
            offset+=(size_t)got;
        }
        close(fd);
        if(offset!=length){free(bytes);hc_here_media_error(error,cap,"Could not read the complete image file.");return NULL;}
        NSData *data=[NSData dataWithBytesNoCopy:bytes length:length freeWhenDone:YES];
        if(!data){free(bytes);hc_here_media_error(error,cap,"Could not read image data.");return NULL;}
        CGImageSourceRef decoder=CGImageSourceCreateWithData((CFDataRef)data,(CFDictionaryRef)@{(id)kCGImageSourceShouldCache:@NO});
        if(!decoder){hc_here_media_error(error,cap,"Unsupported or damaged image file.");return NULL;}
        NSDictionary *properties=[(NSDictionary *)CGImageSourceCopyPropertiesAtIndex(decoder,0,NULL) autorelease];
        double ow=[properties[(id)kCGImagePropertyPixelWidth] doubleValue],oh=[properties[(id)kCGImagePropertyPixelHeight] doubleValue];
        int orientation=[properties[(id)kCGImagePropertyOrientation] intValue];
        if(orientation>=5 && orientation<=8){double swap=ow;ow=oh;oh=swap;}
        if(ow<1 || oh<1 || ow>100000 || oh>100000 || ow*oh>100000000) {
            CFRelease(decoder);hc_here_media_error(error,cap,"Image dimensions are invalid or exceed the 100 megapixel preview limit.");return NULL;
        }
        double scale=1;if(ow>HC_HERE_MEDIA_WIDTH)scale=HC_HERE_MEDIA_WIDTH/ow;
        if(oh*scale>HC_HERE_MEDIA_HEIGHT)scale=HC_HERE_MEDIA_HEIGHT/oh;
        int max_side=(int)((ow>oh?ow:oh)*scale);if(max_side<1)max_side=1;
        CGImageRef image=CGImageSourceCreateThumbnailAtIndex(decoder,0,(CFDictionaryRef)@{
            (id)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
            (id)kCGImageSourceCreateThumbnailWithTransform:@YES,
            (id)kCGImageSourceThumbnailMaxPixelSize:@(max_side),
            (id)kCGImageSourceShouldCacheImmediately:@YES});
        CFRelease(decoder);
        if(!image){hc_here_media_error(error,cap,"Image decoder could not produce a preview.");return NULL;}
        int width=(int)CGImageGetWidth(image),height=(int)CGImageGetHeight(image);
        if(width<1 || height<1 || width>HC_HERE_MEDIA_WIDTH || height>HC_HERE_MEDIA_HEIGHT) {
            CGImageRelease(image);hc_here_media_error(error,cap,"Image decoder exceeded preview bounds.");return NULL;
        }
        unsigned char *pixels=calloc((size_t)width*height,4);
        CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context=pixels&&color?CGBitmapContextCreate(pixels,width,height,8,width*4,color,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big):NULL;
        if(color)CGColorSpaceRelease(color);
        if(!context){free(pixels);CGImageRelease(image);hc_here_media_error(error,cap,"Not enough memory to decode image.");return NULL;}
        /* ImageIO already applies EXIF; bitmap memory is read top-to-bottom.
           A view-coordinate CTM flip here would turn images upside down. */
        CGContextDrawImage(context,CGRectMake(0,0,width,height),image);CGContextRelease(context);CGImageRelease(image);
        for(size_t i=0;i<(size_t)width*height;i++) {
            unsigned char *p=pixels+i*4;unsigned a=p[3];
            if(a && a<255)for(int c=0;c<3;c++){unsigned value=(p[c]*255u+a/2)/a;p[c]=(unsigned char)(value>255?255:value);}
        }
        HcHereMediaSource *source=hc_here_media_alloc(error,cap);
        if(!source){free(pixels);return NULL;}
        hc_here_media_publish(source,pixels,width,height);return source;
    }
}
static HcHereMediaSource *hc_here_media_open_camera(char *error,size_t cap) {
    hc_here_media_error(error,cap,"");
    @autoreleasepool {
        NSString *usage=[[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSCameraUsageDescription"];
        if(![usage isKindOfClass:NSString.class] || !usage.length) {
            hc_here_media_error(error,cap,"Camera preview requires Hardcore's embedded camera permission metadata. Rebuild with the normal Makefile.");return NULL;
        }
        HcHereMediaSource *source=hc_here_media_alloc(error,cap);if(!source)return NULL;
        source->camera=1;
        HCHereMediaCamera *controller=[[HCHereMediaCamera alloc]initWithSource:source];
        if(!controller){pthread_mutex_destroy(&source->lock);free(source);hc_here_media_error(error,cap,"Could not initialize camera preview.");return NULL;}
        source->controller=controller;[controller prepare];return source;
    }
}
static void hc_here_media_close(HcHereMediaSource *source) {
    if(!source)return;
    @autoreleasepool {
        HCHereMediaCamera *controller=(HCHereMediaCamera *)source->controller;
        [controller shutdown];[controller release];
        free(source->pixels);pthread_mutex_destroy(&source->lock);free(source);
    }
}
#else
static HcHereMediaSource *hc_here_media_open_image(const char *path,char *error,size_t cap) {
    (void)path;hc_here_media_error(error,cap,"Native image decoding is currently available on macOS.");return NULL;
}
static HcHereMediaSource *hc_here_media_open_camera(char *error,size_t cap) {
    hc_here_media_error(error,cap,"Native webcam preview is currently available on macOS.");return NULL;
}
static void hc_here_media_close(HcHereMediaSource *source) {
    if(source){free(source->pixels);pthread_mutex_destroy(&source->lock);free(source);}
}
#endif
#endif
