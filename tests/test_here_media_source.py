#!/usr/bin/env python3
"""Bounded native image/camera source tests. No camera permission/device access.

ImageIO decodes generated PNG/TIFF fixtures; camera frames are synthetic pixel
buffers, and authorization callbacks use a subclass with no device operations.
Python only compiles and drives the tests, never the shipped media preview.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "hardcore_here_media_source.h"
static void require(int ok,const char *message) {
    if(!ok){fprintf(stderr,"FAIL: %s\n",message);exit(1);}
}
static void save_image(const char *path,int width,int height,const unsigned char *pixels,int orientation) {
    CGDataProviderRef provider=CGDataProviderCreateWithData(NULL,pixels,(size_t)width*height*4,NULL);
    CGColorSpaceRef color=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image=CGImageCreate(width,height,8,32,width*4,color,kCGImageAlphaLast|kCGBitmapByteOrder32Big,
        provider,NULL,NO,kCGRenderingIntentDefault);
    require(image!=NULL,"create image fixture");
    NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef dest=CGImageDestinationCreateWithURL((CFURLRef)url,
        orientation?CFSTR("public.tiff"):CFSTR("public.png"),1,NULL);
    require(dest!=NULL,"create image destination");
    CGImageDestinationAddImage(dest,image,(CFDictionaryRef)(orientation?@{(id)kCGImagePropertyOrientation:@(orientation)}:@{}));
    require(CGImageDestinationFinalize(dest),"write image fixture");
    CFRelease(dest);CGImageRelease(image);CGColorSpaceRelease(color);CGDataProviderRelease(provider);
}
static unsigned char *frame(HcHereMediaSource *source,int *w,int *h) {
    unsigned char *pixels=NULL;size_t capacity=0;uint64_t revision=0;char error[256];
    int result=hc_here_media_copy_frame(source,&pixels,&capacity,w,h,&revision,error,sizeof(error));
    if(result!=1)fprintf(stderr,"frame error: %s\n",error);
    require(result==1 && revision>0,"copy initial frame");return pixels;
}
@interface HCHereFakePermission : HCHereMediaCamera {
@public
    AVAuthorizationStatus fakeStatus;
    void (^completion)(BOOL);
    int configured,requests;
}
- (void)drain;
@end
@implementation HCHereFakePermission
- (AVAuthorizationStatus)authorizationStatus {return fakeStatus;}
- (void)requestAccess:(void (^)(BOOL))callback {requests++;completion=[callback copy];}
- (void)configure {if(!_closed)configured++;}
- (void)drain {dispatch_sync(_queue,^{});}
- (void)dealloc {[completion release];[super dealloc];}
@end
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    @autoreleasepool {
        char path[4096],error[256];snprintf(path,sizeof(path),"%s/image.png",argv[2]);
        if(!strcmp(argv[1],"image")) {
            unsigned char values[]={255,0,0,255,0,255,0,255, 0,0,255,255,255,255,0,255};
            save_image(path,2,2,values,0);
            HcHereMediaSource *source=hc_here_media_open_image(path,error,sizeof(error));
            require(source!=NULL,error);int width,height;unsigned char *pixels=frame(source,&width,&height);
            require(width==2 && height==2,"native-sized image not upscaled");
            require(!memcmp(pixels,values,sizeof(values)),"upright top-down image and RGB channels");
            uint64_t revision=source->revision;size_t capacity=16;
            require(hc_here_media_copy_frame(source,&pixels,&capacity,&width,&height,&revision,error,sizeof(error))==0,"unchanged revision");
            free(pixels);hc_here_media_close(source);
        } else if(!strcmp(argv[1],"alpha")) {
            unsigned char values[]={255,0,0,128,0,0,0,0};save_image(path,2,1,values,0);
            HcHereMediaSource *source=hc_here_media_open_image(path,error,sizeof(error));require(source!=NULL,error);
            int width,height;unsigned char *pixels=frame(source,&width,&height);
            require(pixels[0]>=253 && pixels[1]==0 && pixels[2]==0 && pixels[3]==128,"straight alpha survives decoder");
            require(pixels[7]==0,"transparent pixel preserved");free(pixels);hc_here_media_close(source);
        } else if(!strcmp(argv[1],"orientation")) {
            unsigned char values[]={255,0,0,255,0,255,0,255, 0,0,255,255,255,255,0,255,
                255,0,255,255,0,255,255,255};
            snprintf(path,sizeof(path),"%s/oriented.tiff",argv[2]);save_image(path,2,3,values,6);
            HcHereMediaSource *source=hc_here_media_open_image(path,error,sizeof(error));require(source!=NULL,error);
            int width,height;unsigned char *pixels=frame(source,&width,&height);
            require(width==3 && height==2,"EXIF orientation swaps dimensions");
            unsigned char rotated[]={255,0,255,255,0,0,255,255,255,0,0,255,
                0,255,255,255,255,255,0,255,0,255,0,255};
            require(!memcmp(pixels,rotated,sizeof(rotated)),"EXIF rotation is clockwise and upright");
            free(pixels);hc_here_media_close(source);
        } else if(!strcmp(argv[1],"bounded")) {
            int sizes[][2]={{1600,400},{400,1600},{1,2000},{2000,1}};
            for(int n=0;n<4;n++) {
                int ow=sizes[n][0],oh=sizes[n][1];size_t count=(size_t)ow*oh;
                unsigned char *values=malloc(count*4);memset(values,255,count*4);save_image(path,ow,oh,values,0);free(values);
                HcHereMediaSource *source=hc_here_media_open_image(path,error,sizeof(error));require(source!=NULL,error);
                int width,height;unsigned char *pixels=frame(source,&width,&height);
                require(width>0 && width<=640 && height>0 && height<=480,"bounded dimensions");
                require(abs(width*oh-height*ow)<=(ow>oh?ow:oh),"aspect ratio not cropped");
                free(pixels);hc_here_media_close(source);
            }
        } else if(!strcmp(argv[1],"invalid")) {
            require(!hc_here_media_open_image("/does/not/exist",error,sizeof(error)) && error[0],"missing file");
            require(!hc_here_media_open_image(argv[2],error,sizeof(error)),"directory rejected");
            FILE *file=fopen(path,"wb");fputs("not an image",file);fclose(file);
            require(!hc_here_media_open_image(path,error,sizeof(error)),"invalid image rejected");
            file=fopen(path,"wb");fclose(file);require(!hc_here_media_open_image(path,error,sizeof(error)),"empty file rejected");
            require(truncate(path,HC_HERE_MEDIA_FILE_MAX+1)==0,"sparse oversized fixture");
            require(!hc_here_media_open_image(path,error,sizeof(error)),"oversized file rejected before reading");
            snprintf(path,sizeof(path),"%s/pipe",argv[2]);require(!mkfifo(path,0600),"create fifo");
            require(!hc_here_media_open_image(path,error,sizeof(error)),"FIFO rejected without blocking");
        } else if(!strcmp(argv[1],"latest")) {
            HcHereMediaSource *source=hc_here_media_alloc(error,sizeof(error));require(source!=NULL,error);
            for(int n=1;n<=30;n++) {unsigned char *pixels=malloc(4);memset(pixels,n,4);require(hc_here_media_publish(source,pixels,1,1),"publish frame");}
            int width,height;unsigned char *pixels=frame(source,&width,&height);require(source->revision==30 && pixels[0]==30,"latest frame only");
            free(pixels);hc_here_media_set_message(source,1,"failure\033[31m\n");
            uint64_t revision=0;size_t capacity=0;pixels=NULL;
            require(hc_here_media_copy_frame(source,&pixels,&capacity,&width,&height,&revision,error,sizeof(error))==-1,"terminal error surfaced");
            require(!strchr(error,'\033') && !strchr(error,'\n'),"status controls sanitized");
            require(!hc_here_media_publish(source,malloc(4),1,1),"late frame cannot resurrect failed source");
            hc_here_media_close(source);
        } else if(!strcmp(argv[1],"frame")) {
            HcHereMediaSource *source=hc_here_media_alloc(error,sizeof(error));require(source!=NULL,error);
            HCHereMediaCamera *controller=[[HCHereMediaCamera alloc]initWithSource:source];source->controller=controller;
            CVPixelBufferRef buffer=NULL;
            require(CVPixelBufferCreate(kCFAllocatorDefault,2,2,kCVPixelFormatType_32BGRA,NULL,&buffer)==kCVReturnSuccess,"synthetic buffer");
            CVPixelBufferLockBaseAddress(buffer,0);unsigned char *base=CVPixelBufferGetBaseAddress(buffer);size_t stride=CVPixelBufferGetBytesPerRow(buffer);
            unsigned char top[]={0,0,255,17,0,255,0,22},bottom[]={255,0,0,19,0,255,255,32};
            memcpy(base,top,8);memcpy(base+stride,bottom,8);CVPixelBufferUnlockBaseAddress(buffer,0);
            CMVideoFormatDescriptionRef format=NULL;require(CMVideoFormatDescriptionCreateForImageBuffer(NULL,buffer,&format)==noErr,"synthetic format");
            CMSampleTimingInfo timing={CMTimeMake(1,12),CMTimeMake(1,12),kCMTimeInvalid};CMSampleBufferRef sample=NULL;
            require(CMSampleBufferCreateReadyWithImageBuffer(NULL,buffer,format,&timing,&sample)==noErr,"synthetic sample");
            [controller consumeSample:sample];
            int width,height;unsigned char *pixels=frame(source,&width,&height);
            unsigned char expected[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,0,255};
            require(width==2 && height==2 && !memcmp(pixels,expected,16),"camera BGRA stride, upright conversion and opaque alpha");
            uint64_t revision=source->revision;[controller consumeSample:sample];
            require(source->revision==revision,"camera callbacks throttle duplicate immediate frames");
            free(pixels);CFRelease(sample);CFRelease(format);CVPixelBufferRelease(buffer);hc_here_media_close(source);
        } else if(!strcmp(argv[1],"permission")) {
            HcHereMediaSource *source=hc_here_media_alloc(error,sizeof(error));source->camera=1;
            HCHereFakePermission *fake=[[HCHereFakePermission alloc]initWithSource:source];source->controller=fake;
            fake->fakeStatus=AVAuthorizationStatusDenied;[fake prepare];
            require(source->failed && fake->requests==0 && fake->configured==0,"denial never requests or opens camera");hc_here_media_close(source);
            source=hc_here_media_alloc(error,sizeof(error));source->camera=1;
            fake=[[HCHereFakePermission alloc]initWithSource:source];source->controller=fake;fake->fakeStatus=AVAuthorizationStatusNotDetermined;[fake prepare];
            require(source->pending && fake->requests==1 && fake->configured==0,"permission gate precedes capture");
            [fake retain];hc_here_media_close(source);fake->completion(YES);[fake drain];
            require(fake->configured==0,"late permission cannot reopen closed camera");[fake->completion release];fake->completion=nil;[fake release];
        } else if(!strcmp(argv[1],"timeout")) {
            HcHereMediaSource *source=hc_here_media_alloc(error,sizeof(error));source->camera=source->pending=1;source->opened-=121;
            unsigned char *pixels=NULL;size_t capacity=0;uint64_t revision=0;int width=0,height=0;
            require(hc_here_media_copy_frame(source,&pixels,&capacity,&width,&height,&revision,error,sizeof(error))==-1 && strstr(error,"permission timed out"),"permission timeout");
            hc_here_media_close(source);source=hc_here_media_alloc(error,sizeof(error));source->camera=1;source->opened-=16;
            require(hc_here_media_copy_frame(source,&pixels,&capacity,&width,&height,&revision,error,sizeof(error))==-1 && strstr(error,"did not deliver"),"no-frame watchdog");hc_here_media_close(source);
        } else if(!strcmp(argv[1],"metadata")) {
            require([[NSBundle mainBundle] objectForInfoDictionaryKey:@"NSCameraUsageDescription"]==nil,"bare fixture has no TCC metadata");
            require(!hc_here_media_open_camera(error,sizeof(error)) && strstr(error,"metadata"),"bare binary refuses unsafe permission request");
        } else return 2;
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Apple ImageIO/AVFoundation source')
class MediaSource(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-media-source-')
        cls.root = Path(cls.temp.name)
        source = cls.root / 'fixture.m'
        source.write_text(SOURCE)
        cls.binary = cls.root / 'fixture'
        command = ['clang', '-O1', '-Wall', '-Wextra', '-Wno-unused-function', '-Wno-deprecated-declarations',
                   '-I', str(ROOT), str(source), '-o', str(cls.binary),
                   '-framework', 'Foundation', '-framework', 'ImageIO', '-framework', 'CoreGraphics',
                   '-framework', 'AVFoundation', '-framework', 'CoreMedia', '-framework', 'CoreVideo']
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def verify(self, name):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            result = subprocess.run([str(self.binary), name, folder], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_image_upright_and_revision(self): self.verify('image')
    def test_image_alpha(self): self.verify('alpha')
    def test_exif_orientation(self): self.verify('orientation')
    def test_bounded_aspect_ratio(self): self.verify('bounded')
    def test_bad_files_and_fifo_nonblocking(self): self.verify('invalid')
    def test_latest_frame_only_and_errors(self): self.verify('latest')
    def test_synthetic_camera_frame(self): self.verify('frame')
    def test_permission_lifecycle_without_device(self): self.verify('permission')
    def test_capture_deadlines(self): self.verify('timeout')
    def test_bare_executable_metadata_guard(self): self.verify('metadata')


if __name__ == '__main__':
    unittest.main()
