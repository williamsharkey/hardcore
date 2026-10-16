/* No devices, camera/microphone authorization, or Speech APIs are accessed.
 * Exercises the actual H.264/AAC writer with synthesized camera/audio buffers.
 * clang -O1 -Wno-deprecated-declarations tests/test_camera_capture.m -o build/test_camera_capture \
 *   -framework Cocoa -framework AVFoundation -framework CoreMedia -framework CoreVideo \
 *   -framework AudioToolbox -framework Speech -framework QuartzCore
 */
#import "../hardcore_camera_capture.m"
#include <assert.h>
#include <unistd.h>

@interface HCCameraCapture (SyntheticTest)
- (NSError *)testBegin:(NSURL *)directory;
- (void)testPause:(double)host;
- (void)testResume:(double)host;
- (void)testFreeze:(double)host;
- (void)testAppend:(CMSampleBufferRef)sample video:(BOOL)video host:(double)host;
- (NSUInteger)testVideoFrames;
- (AVCaptureSession *)testEmptySessionWithMicrophone:(BOOL)microphone;
- (void)testSetMicrophone:(BOOL)microphone;
@end
@implementation HCCameraCapture (SyntheticTest)
- (NSError *)testBegin:(NSURL *)directory {
    __block NSError *error = nil;
    [self syncCapture:^{
        self->_testMode = YES; self->_hasMicrophone = YES;
        self->_videoDimensions = (CMVideoDimensions){640,360};
        error = [[self beginWriterToDirectory:directory hostTime:1000] retain];
        if (!error) [self setCaptureState:HCCameraCaptureRecording error:nil];
    }];
    return [error autorelease];
}
- (void)testPause:(double)host {
    [self syncCapture:^{ @synchronized(self) { hc_camera_clock_pause(&self->_clock, host); } }];
}
- (void)testResume:(double)host {
    [self syncCapture:^{ @synchronized(self) { hc_camera_clock_resume(&self->_clock, host); } }];
}
- (void)testFreeze:(double)host {
    [self syncCapture:^{ @synchronized(self) { hc_camera_clock_stop(&self->_clock, host); } }];
}
- (void)testAppend:(CMSampleBufferRef)sample video:(BOOL)video host:(double)host {
    double deadline = hc_camera_host_seconds()+5;
    __block BOOL ready = NO;
    do {
        [self syncCapture:^{ ready = (video ? self->_videoInput : self->_audioInput).readyForMoreMediaData; }];
        if (!ready) usleep(1000);
    } while (!ready && hc_camera_host_seconds() < deadline);
    if (!ready) [self syncCapture:^{ fprintf(stderr, "writer ready timeout, video=%d status=%ld error=%s frames=%lu audio=%lu\n", video, (long)self->_writer.status, self->_writer.error.description.UTF8String, (unsigned long)self->_videoFrames, (unsigned long)self->_audioBuffers); }];
    assert(ready);
    [self syncCapture:^{ [self appendSampleBuffer:sample video:video hostTime:host]; }];
}
- (NSUInteger)testVideoFrames {
    __block NSUInteger result = 0;
    [self syncCapture:^{ result = self->_videoFrames; }];
    return result;
}
- (AVCaptureSession *)testEmptySessionWithMicrophone:(BOOL)microphone {
    @synchronized(self) { _hasMicrophone = microphone; }
    if (!_session) _session = [[AVCaptureSession alloc] init];
    return _session;
}
- (void)testSetMicrophone:(BOOL)microphone {
    @synchronized(self) { _hasMicrophone = microphone; }
}
@end

static void wait_for(BOOL *done) {
    double deadline = hc_camera_host_seconds()+15;
    while (!*done && hc_camera_host_seconds() < deadline)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    assert(*done);
}
static CMSampleBufferRef video_sample(double host, BOOL afterPause) {
    CVPixelBufferRef pixel = NULL;
    assert(CVPixelBufferCreate(kCFAllocatorDefault, 640, 360, kCVPixelFormatType_32BGRA,
        (CFDictionaryRef)@{(id)kCVPixelBufferIOSurfacePropertiesKey:@{}}, &pixel) == noErr);
    CVPixelBufferLockBaseAddress(pixel, 0);
    uint8_t *base = CVPixelBufferGetBaseAddress(pixel);
    size_t stride = CVPixelBufferGetBytesPerRow(pixel);
    for (int y=0; y<360; y++) for (int x=0; x<640; x++) {
        uint8_t *p = base+y*stride+x*4;
        p[0] = afterPause ? 220 : 25; p[1] = (x*255)/640; p[2] = afterPause ? 25 : 220; p[3] = 255;
    }
    CVPixelBufferUnlockBaseAddress(pixel, 0);
    CMVideoFormatDescriptionRef format = NULL;
    assert(CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, pixel, &format) == noErr);
    CMSampleTimingInfo timing = { CMTimeMake(1,30), CMTimeMakeWithSeconds(host, 1000000000), kCMTimeInvalid };
    CMSampleBufferRef sample = NULL;
    assert(CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, pixel, format, &timing, &sample) == noErr);
    CFRelease(format); CFRelease(pixel);
    return sample;
}
static CMSampleBufferRef audio_sample(double host, NSInteger index) {
    AudioStreamBasicDescription asbd = {0};
    asbd.mSampleRate = 48000;
    asbd.mFormatID = kAudioFormatLinearPCM;
    asbd.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    asbd.mBytesPerPacket = asbd.mBytesPerFrame = 2;
    asbd.mFramesPerPacket = asbd.mChannelsPerFrame = 1;
    asbd.mBitsPerChannel = 16;
    CMAudioFormatDescriptionRef format = NULL;
    assert(CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, 0, NULL, 0, NULL, NULL, &format) == noErr);
    CMBlockBufferRef block = NULL;
    assert(CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, 3200, kCFAllocatorDefault, NULL, 0, 3200, 0, &block) == noErr);
    int16_t samples[1600];
    for (int i=0; i<1600; i++) samples[i] = (int16_t)(sin(2*M_PI*440*(i+index*1600)/48000.0)*8000);
    assert(CMBlockBufferReplaceDataBytes(samples, block, 0, sizeof(samples)) == noErr);
    CMSampleBufferRef sample = NULL;
    assert(CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault, block, format, 1600,
        CMTimeMakeWithSeconds(host, 1000000000), NULL, &sample) == noErr);
    CFRelease(block); CFRelease(format);
    return sample;
}
static void test_clock(void) {
    HCCameraClock clock = {0};
    assert(hc_camera_clock_elapsed(&clock, 100) == 0);
    hc_camera_clock_start(&clock, 100);
    assert(hc_camera_clock_sample(&clock, 99) < 0);
    assert(hc_camera_clock_elapsed(&clock, 102) == 2);
    hc_camera_clock_pause(&clock, 102);
    hc_camera_clock_pause(&clock, 104); /* idempotent */
    assert(hc_camera_clock_elapsed(&clock, 120) == 2);
    assert(hc_camera_clock_sample(&clock, 108) < 0);
    hc_camera_clock_resume(&clock, 112);
    hc_camera_clock_resume(&clock, 114);
    assert(hc_camera_clock_sample(&clock, 111.9) < 0);
    assert(fabs(hc_camera_clock_sample(&clock, 112.25)-2.25) < 1e-9);
    hc_camera_clock_pause(&clock, 114);
    hc_camera_clock_resume(&clock, 119);
    assert(hc_camera_clock_elapsed(&clock, 120) == 5);
    hc_camera_clock_pause(&clock, 121);
    hc_camera_clock_stop(&clock, 150);
    assert(hc_camera_clock_elapsed(&clock, 200) == 6);
    hc_camera_clock_resume(&clock, 300);
    assert(hc_camera_clock_elapsed(&clock, 301) == 6);
}
static void inspect_movie(NSURL *url) {
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    NSArray *video = [asset tracksWithMediaType:AVMediaTypeVideo];
    NSArray *audio = [asset tracksWithMediaType:AVMediaTypeAudio];
    assert(video.count == 1 && audio.count == 1);
    AVAssetTrack *videoTrack = video.firstObject, *audioTrack = audio.firstObject;
    assert(CMFormatDescriptionGetMediaSubType((CMFormatDescriptionRef)videoTrack.formatDescriptions.firstObject) == kCMVideoCodecType_H264);
    assert(CMFormatDescriptionGetMediaSubType((CMFormatDescriptionRef)audioTrack.formatDescriptions.firstObject) == kAudioFormatMPEG4AAC);
    assert(fabs(CMTimeGetSeconds(asset.duration)-2) < 0.1);
    NSError *error = nil;
    AVAssetReader *reader = [[[AVAssetReader alloc] initWithAsset:asset error:&error] autorelease];
    assert(reader && !error);
    AVAssetReaderTrackOutput *output = [[[AVAssetReaderTrackOutput alloc] initWithTrack:videoTrack
        outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)}] autorelease];
    [reader addOutput:output]; assert([reader startReading]);
    NSInteger count=0; double previous=-1;
    CMSampleBufferRef sample;
    while ((sample = [output copyNextSampleBuffer])) {
        double now = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
        assert(now > previous);
        if (previous >= 0) assert(now-previous < 0.05); /* no 20-second pause in movie */
        CVPixelBufferRef pixel = CMSampleBufferGetImageBuffer(sample);
        CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
        uint8_t *p = CVPixelBufferGetBaseAddress(pixel);
        if (count < 30) assert(p[2] > p[0]+100);
        else assert(p[0] > p[2]+100);
        CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
        previous=now; count++; CFRelease(sample);
    }
    assert(reader.status == AVAssetReaderStatusCompleted && count == 60);
    reader = [[[AVAssetReader alloc] initWithAsset:asset error:&error] autorelease];
    output = [[[AVAssetReaderTrackOutput alloc] initWithTrack:audioTrack
        outputSettings:@{AVFormatIDKey:@(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey:@16,
            AVLinearPCMIsFloatKey:@NO, AVLinearPCMIsBigEndianKey:@NO}] autorelease];
    [reader addOutput:output]; assert([reader startReading]);
    count=0; previous=-1;
    while ((sample=[output copyNextSampleBuffer])) {
        double now=CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
        if (previous >= 0) assert(fabs(now-previous) < .002);
        previous=now+CMTimeGetSeconds(CMSampleBufferGetDuration(sample)); count++; CFRelease(sample);
    }
    assert(reader.status == AVAssetReaderStatusCompleted && count > 10 && previous > 1.9);
}
/* Control-flow regression fixture: every permission/device operation is
 * replaced, while the real prepare/start sequencing and queue hops execute. */
@interface HCCameraPermissionFixture : HCCameraCapture {
@public
    AVAuthorizationStatus videoAuthorization, audioAuthorization;
    NSUInteger videoRequests, audioRequests, configurations, microphoneConfigurations, writerStarts;
    BOOL configuredWithAudio, previewConnected, sessionStarted;
    NSError *previewFailure;
    void (^pendingAudio)(BOOL);
    void (^pendingVideo)(BOOL);
    void (^pendingSpeech)(SFSpeechRecognizerAuthorizationStatus);
}
@end
@implementation HCCameraPermissionFixture
- (instancetype)init {
    if ((self = [super init])) {
        videoAuthorization = AVAuthorizationStatusAuthorized;
        audioAuthorization = AVAuthorizationStatusNotDetermined;
    }
    return self;
}
- (AVAuthorizationStatus)cameraAuthorizationStatusForMediaType:(AVMediaType)type {
    return [type isEqualToString:AVMediaTypeVideo] ? videoAuthorization : audioAuthorization;
}
- (void)requestCameraAccessForMediaType:(AVMediaType)type completion:(void (^)(BOOL))completion {
    if ([type isEqualToString:AVMediaTypeVideo]) { videoRequests++; [pendingVideo release]; pendingVideo = [completion copy]; }
    else { audioRequests++; [pendingAudio release]; pendingAudio = [completion copy]; }
}
- (void)configureWithMicrophone:(BOOL)microphone completion:(void (^)(NSError *))completion {
    configurations++; configuredWithAudio = microphone;
    if (!microphone) [self addWarning:HCCameraMicrophonePendingWarning];
    AVCaptureSession *session = [self testEmptySessionWithMicrophone:microphone];
    [self startPreparedSession:session completion:completion];
}
- (NSError *)connectPreviewToSession:(AVCaptureSession *)session {
    assert([NSThread isMainThread]); assert(!sessionStarted);
    previewConnected = YES;
    return previewFailure;
}
- (NSError *)runPreparedSession:(AVCaptureSession *)session {
    assert(![NSThread isMainThread]); assert(previewConnected);
    sessionStarted = YES; /* deliberately never calls AVFoundation startRunning */
    return nil;
}
- (BOOL)configureMicrophoneInput {
    microphoneConfigurations++;
    [self testSetMicrophone:YES];
    return YES;
}
- (NSError *)beginWriterToDirectory:(NSURL *)directory hostTime:(double)host {
    writerStarts++; return nil; /* no files or real recording */
}
- (SFSpeechRecognizerAuthorizationStatus)speechAuthorizationStatus { return SFSpeechRecognizerAuthorizationStatusNotDetermined; }
- (void)requestSpeechAccess:(void (^)(SFSpeechRecognizerAuthorizationStatus))completion { pendingSpeech = [completion copy]; }
- (void)dealloc { [previewFailure release]; [pendingAudio release]; [pendingVideo release]; [pendingSpeech release]; [super dealloc]; }
@end

static void test_preview_authorization(void) {
    HCCameraPermissionFixture *capture = [[HCCameraPermissionFixture alloc] init];
    capture->videoAuthorization = AVAuthorizationStatusNotDetermined;
    __block BOOL ready = NO;
    [capture prepareWithCompletion:^(NSError *error) {
        assert([NSThread isMainThread] && !error); ready = YES;
    }];
    assert(capture->videoRequests == 1 && capture->configurations == 0 && capture->audioRequests == 0);
    capture->videoAuthorization = AVAuthorizationStatusAuthorized;
    capture->pendingVideo(YES);
    [capture->pendingVideo release]; capture->pendingVideo = nil;
    wait_for(&ready);
    assert(capture.state == HCCameraCaptureReady && capture->sessionStarted);
    assert(capture->configurations == 1 && !capture->configuredWithAudio);
    assert(capture->audioRequests == 0 && capture->videoRequests == 1);
    assert(capture->previewConnected && !capture.hasMicrophone);
    __block BOOL started = NO;
    [capture startRecordingToDirectory:[NSURL fileURLWithPath:@"/unused-permission-fixture"] completion:^(NSError *error) {
        assert(!error); started = YES;
    }];
    assert(capture->audioRequests == 1 && capture->writerStarts == 0);
    assert(capture.state == HCCameraCaptureReady && capture->sessionStarted);
    __block BOOL duplicateRejected = NO;
    [capture startRecordingToDirectory:[NSURL fileURLWithPath:@"/unused-permission-fixture"] completion:^(NSError *error) {
        assert(error); duplicateRejected = YES;
    }];
    wait_for(&duplicateRejected);
    assert(capture->audioRequests == 1);
    capture->audioAuthorization = AVAuthorizationStatusAuthorized;
    capture->pendingAudio(YES);
    [capture->pendingAudio release]; capture->pendingAudio = nil;
    wait_for(&started);
    assert(capture->writerStarts == 1 && capture->microphoneConfigurations == 1 && capture.hasMicrophone);
    assert([capture.warning rangeOfString:HCCameraMicrophonePendingWarning].location == NSNotFound);
    [capture shutdown]; [capture syncCapture:^{}]; [capture release];

    /* A rejected preview connection cannot claim Ready or start the session. */
    capture = [[HCCameraPermissionFixture alloc] init];
    capture->previewFailure = [hc_camera_error(100, @"fixture preview failure") retain];
    ready = NO;
    [capture prepareWithCompletion:^(NSError *error) { assert(error.code == 100); ready = YES; }];
    wait_for(&ready);
    assert(capture.state == HCCameraCaptureFailed && !capture->sessionStarted);
    assert(capture->audioRequests == 0);
    [capture shutdown]; [capture syncCapture:^{}]; [capture release];

    /* Denied microphone access still leaves a usable camera-only recording. */
    capture = [[HCCameraPermissionFixture alloc] init];
    capture->audioAuthorization = AVAuthorizationStatusDenied;
    ready = NO;
    [capture prepareWithCompletion:^(NSError *error) { assert(!error); ready = YES; }];
    wait_for(&ready);
    started = NO;
    [capture startRecordingToDirectory:[NSURL fileURLWithPath:@"/unused-permission-fixture"] completion:^(NSError *error) { assert(!error); started = YES; }];
    wait_for(&started);
    assert(capture->writerStarts == 1 && !capture.hasMicrophone && capture->audioRequests == 0);
    assert([capture.warning containsString:@"silent"]);
    [capture shutdown]; [capture syncCapture:^{}]; [capture release];
}

static void test_pending_start_cancellation(void) {
    /* Cover deferred microphone and speech prompts, and both explicit discard
     * and closing the camera. All authorization providers remain fake. */
    for (int speech = 0; speech < 2; speech++) for (int close = 0; close < 2; close++) {
        HCCameraPermissionFixture *capture = [[HCCameraPermissionFixture alloc] init];
        capture.captionsEnabled = speech;
        if (speech) capture->audioAuthorization = AVAuthorizationStatusAuthorized;
        __block BOOL ready = NO, cancelled = NO, discarded = NO;
        __block NSUInteger callbacks = 0;
        [capture prepareWithCompletion:^(NSError *error) { assert(!error); ready = YES; }];
        wait_for(&ready);
        [capture startRecordingToDirectory:[NSURL fileURLWithPath:@"/unused-cancel-fixture"] completion:^(NSError *error) {
            assert(error && [NSThread isMainThread]); callbacks++; cancelled = YES;
        }];
        if (speech) {
            double deadline = hc_camera_host_seconds()+5;
            while (!capture->pendingSpeech && hc_camera_host_seconds() < deadline)
                [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.001]];
            assert(capture->pendingSpeech);
        } else assert(capture->pendingAudio);
        if (close) { [capture shutdown]; [capture syncCapture:^{}]; }
        else { [capture discardWithCompletion:^{ discarded = YES; }]; wait_for(&discarded); }
        wait_for(&cancelled);
        assert(callbacks == 1 && capture->writerStarts == 0);
        if (speech) { capture->pendingSpeech(SFSpeechRecognizerAuthorizationStatusAuthorized); [capture->pendingSpeech release]; capture->pendingSpeech = nil; }
        else { capture->pendingAudio(YES); [capture->pendingAudio release]; capture->pendingAudio = nil; }
        [capture syncCapture:^{}];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
        [capture syncCapture:^{}];
        assert(callbacks == 1 && capture->writerStarts == 0);
        if (!close) [capture shutdown];
        [capture syncCapture:^{}]; [capture release];
    }
}

int main(void) {
    @autoreleasepool {
        test_clock();
        test_preview_authorization();
        test_pending_start_cancellation();
        NSURL *root = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:
            [@"hardcore-camera-test-" stringByAppendingString:[NSUUID UUID].UUIDString]] isDirectory:YES];
        NSURL *take = [root URLByAppendingPathComponent:@"take" isDirectory:YES];
        HCCameraCapture *capture = [[HCCameraCapture alloc] init];
        assert([capture testBegin:take] == nil);
        for (int i=0; i<60; i++) {
            if (i == 30) {
                [capture testPause:1001];
                CMSampleBufferRef paused=video_sample(1010, NO);
                [capture testAppend:paused video:YES host:1010]; CFRelease(paused);
                assert(capture.testVideoFrames == 30);
                [capture testResume:1021];
                CMSampleBufferRef stale=video_sample(1000.99, NO);
                [capture testAppend:stale video:YES host:1000.99]; CFRelease(stale);
                assert(capture.testVideoFrames == 30);
            }
            double host=1000+i/30.0+(i>=30 ? 20 : 0);
            CMSampleBufferRef video=video_sample(host,i>=30), audio=audio_sample(host,i);
            [capture testAppend:video video:YES host:host];
            [capture testAppend:audio video:NO host:host];
            CFRelease(video); CFRelease(audio);
        }
        [capture testFreeze:1022]; assert(fabs(capture.elapsedSeconds-2)<1e-9);
        __block BOOL done=NO;
        [capture stopWithCompletion:^(NSURL *url, NSArray *captions, NSError *error) {
            if (error) fprintf(stderr,"capture error: %s\n",error.description.UTF8String);
            assert(!error && url && !captions.count); done=YES;
        }];
        wait_for(&done);
        NSURL *movie=[take URLByAppendingPathComponent:@"camera.mp4"];
        inspect_movie(movie);
        NSData *captionData=[NSData dataWithContentsOfURL:[take URLByAppendingPathComponent:@"captions.json"]];
        NSArray *captions=[NSJSONSerialization JSONObjectWithData:captionData options:0 error:NULL];
        assert([captions isKindOfClass:[NSArray class]] && captions.count==0);
        assert([capture testBegin:take] != nil); /* never overwrite a saved take */
        done=NO;
        [capture discardWithCompletion:^{ done=YES; }]; wait_for(&done);
        assert([[NSFileManager defaultManager] fileExistsAtPath:movie.path]);
        NSURL *discard=[root URLByAppendingPathComponent:@"discard" isDirectory:YES];
        assert([capture testBegin:discard] == nil);
        NSURL *sentinel=[discard URLByAppendingPathComponent:@"keep.txt"];
        [@"unrelated" writeToURL:sentinel atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        CMSampleBufferRef first=video_sample(1000,NO);
        [capture testAppend:first video:YES host:1000]; CFRelease(first);
        done=NO; [capture discardWithCompletion:^{ done=YES; }]; wait_for(&done);
        assert(![[NSFileManager defaultManager] fileExistsAtPath:[[discard URLByAppendingPathComponent:@"camera.mp4"] path]]);
        assert([[NSFileManager defaultManager] fileExistsAtPath:sentinel.path]);
        [capture shutdown]; [capture syncCapture:^{}]; [capture release];
        [[NSFileManager defaultManager] removeItemAtURL:root error:NULL];
        puts("camera capture: independent preview permission, connection-before-start ordering, pause clock, H.264/AAC encoding/decoding, timestamp continuity, save and discard passed (no devices opened)");
    }
    return 0;
}
