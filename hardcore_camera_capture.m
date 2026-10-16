/* Included by hardcore_mac.m; also compiles as an independent Objective-C TU.
 * AVFoundation owns capture/conversion and its system H.264/AAC encoders. Only
 * camera and microphone media enter the movie; UI replay is a separate journal.
 * This file intentionally supports the application's manual retain/release. */
#import "hardcore_camera_capture.h"
#import <Speech/Speech.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <AudioToolbox/AudioToolbox.h>
#include <math.h>

static NSString *const HCCameraErrorDomain = @"HardcoreCameraCapture";
static NSString *const HCCameraMicrophonePendingWarning = @"Microphone access will be requested when recording starts.";
static NSString *const HCCameraMicrophoneSilentWarning = @"Microphone unavailable or permission denied; this recording will be silent.";
static void *HCCameraQueueKey = &HCCameraQueueKey;

typedef struct {
    double origin, pauseBegan, pausedTotal, segmentBegan, stoppedAt;
    BOOL started, paused, stopped;
} HCCameraClock;

static double hc_camera_host_seconds(void) {
    return CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock()));
}
static void hc_camera_clock_start(HCCameraClock *clock, double now) {
    *clock = (HCCameraClock){ .origin=now, .segmentBegan=now, .started=YES };
}
static double hc_camera_clock_elapsed(const HCCameraClock *clock, double now) {
    if (!clock->started) return 0;
    double end = clock->stopped ? clock->stoppedAt : clock->paused ? clock->pauseBegan : now;
    return fmax(0, end-clock->origin-clock->pausedTotal);
}
static void hc_camera_clock_pause(HCCameraClock *clock, double now) {
    if (!clock->started || clock->stopped || clock->paused) return;
    clock->paused = YES; clock->pauseBegan = fmax(now, clock->segmentBegan);
}
static void hc_camera_clock_resume(HCCameraClock *clock, double now) {
    if (!clock->started || clock->stopped || !clock->paused) return;
    now = fmax(now, clock->pauseBegan);
    clock->pausedTotal += now-clock->pauseBegan;
    clock->segmentBegan = now; clock->paused = NO;
}
static void hc_camera_clock_stop(HCCameraClock *clock, double now) {
    if (!clock->started || clock->stopped) return;
    clock->stoppedAt = clock->paused ? clock->pauseBegan : fmax(now, clock->segmentBegan);
    clock->stopped = YES;
}
/* Reject stale callbacks from before start/resume and buffers from pauses. */
static double hc_camera_clock_sample(const HCCameraClock *clock, double host) {
    if (!clock->started || clock->paused || clock->stopped || host < clock->segmentBegan)
        return -1;
    return host-clock->origin-clock->pausedTotal;
}
static NSError *hc_camera_error(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:HCCameraErrorDomain code:code
        userInfo:@{NSLocalizedDescriptionKey:message}];
}
static void hc_camera_main(void (^block)(void)) {
    if (block) dispatch_async(dispatch_get_main_queue(), block);
}

@interface HCCameraCapture () <AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureAudioDataOutputSampleBufferDelegate> {
    dispatch_queue_t _captureQueue;
    AVCaptureSession *_session;
    AVCaptureVideoPreviewLayer *_previewLayer;
    AVCaptureVideoDataOutput *_videoOutput;
    AVCaptureAudioDataOutput *_audioOutput;
    AVAssetWriter *_writer;
    AVAssetWriterInput *_videoInput, *_audioInput;
    NSURL *_directory, *_movieURL;
    HCCameraClock _clock;
    HCCameraCaptureState _state;
    NSString *_cameraName, *_warning;
    NSError *_lastError;
    BOOL _hasMicrophone, _captionsEnabled, _closed, _ownsMovie, _ownsCaptions;
    BOOL _speechAuthorized, _testMode, _interrupted, _startPending;
    NSUInteger _generation, _videoFrames, _audioBuffers, _receivedVideoFrames, _startGeneration;
    CMTime _lastVideoPTS, _lastAudioPTS;
    void (^_stateChanged)(void);
    void (^_pendingStartCompletion)(NSError *);
    void (^_stopCompletion)(NSURL *, NSArray *, NSError *);
    SFSpeechRecognizer *_recognizer;
    SFSpeechRecognitionTask *_speechTask;
    NSArray *_captions;
    CMVideoDimensions _videoDimensions;
}
- (void)setCaptureState:(HCCameraCaptureState)state error:(NSError *)error;
- (void)configureWithMicrophone:(BOOL)microphone completion:(void (^)(NSError *))completion;
- (BOOL)configureMicrophoneInput;
- (AVAuthorizationStatus)cameraAuthorizationStatusForMediaType:(AVMediaType)mediaType;
- (void)requestCameraAccessForMediaType:(AVMediaType)mediaType completion:(void (^)(BOOL))completion;
- (void)startPreparedSession:(AVCaptureSession *)session completion:(void (^)(NSError *))completion;
- (NSError *)connectPreviewToSession:(AVCaptureSession *)session;
- (NSError *)runPreparedSession:(AVCaptureSession *)session;
- (NSError *)beginWriterToDirectory:(NSURL *)directory hostTime:(double)host;
- (void)appendSampleBuffer:(CMSampleBufferRef)sample video:(BOOL)video hostTime:(double)host;
- (void)finishCaptionsForGeneration:(NSUInteger)generation;
- (void)completeStopForGeneration:(NSUInteger)generation error:(NSError *)error;
@end

@implementation HCCameraCapture
@synthesize captionsEnabled = _captionsEnabled;
@synthesize stateChanged = _stateChanged;

- (instancetype)init {
    if ((self = [super init])) {
        _captureQueue = dispatch_queue_create("app.hardcore.camera", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(_captureQueue, HCCameraQueueKey, self, NULL);
        _previewLayer = [[AVCaptureVideoPreviewLayer alloc] init];
        _previewLayer.videoGravity = AVLayerVideoGravityResizeAspect;
        _captions = [[NSArray alloc] init];
        _state = HCCameraCaptureIdle;
    }
    return self;
}
- (AVCaptureVideoPreviewLayer *)previewLayer { return _previewLayer; }
- (HCCameraCaptureState)state { @synchronized(self) { return _state; } }
- (NSString *)stateDescription {
    switch (self.state) {
        case HCCameraCaptureIdle: return @"Camera closed";
        case HCCameraCapturePreparing: return @"Opening camera…";
        case HCCameraCaptureReady: return @"Camera ready";
        case HCCameraCaptureRecording: return @"Recording";
        case HCCameraCapturePaused: return @"Paused";
        case HCCameraCaptureFinishing: return @"Saving recording…";
        case HCCameraCaptureFailed: return @"Camera unavailable";
    }
    return @"Camera";
}
- (NSString *)cameraName { @synchronized(self) { return [[_cameraName copy] autorelease]; } }
- (NSString *)warning { @synchronized(self) { return [[_warning copy] autorelease]; } }
- (NSError *)lastError { @synchronized(self) { return [[_lastError retain] autorelease]; } }
- (BOOL)hasMicrophone { @synchronized(self) { return _hasMicrophone; } }
- (NSUInteger)receivedVideoFrames { @synchronized(self) { return _receivedVideoFrames; } }
- (AVAuthorizationStatus)cameraAuthorizationStatusForMediaType:(AVMediaType)mediaType {
    return [AVCaptureDevice authorizationStatusForMediaType:mediaType];
}
- (void)requestCameraAccessForMediaType:(AVMediaType)mediaType completion:(void (^)(BOOL))completion {
    [AVCaptureDevice requestAccessForMediaType:mediaType completionHandler:completion];
}
- (void)clearMicrophoneWarning {
    @synchronized(self) {
        NSString *next = [[_warning stringByReplacingOccurrencesOfString:HCCameraMicrophonePendingWarning withString:@""]
            stringByReplacingOccurrencesOfString:HCCameraMicrophoneSilentWarning withString:@""];
        next = [next stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        NSString *copy = [next copy];
        [_warning release]; _warning = copy;
    }
}
- (void)setCaptureState:(HCCameraCaptureState)state error:(NSError *)error {
    @synchronized(self) {
        _state = state;
        [_lastError release]; _lastError = [error retain];
    }
    hc_camera_main(^{ if (self->_stateChanged) self->_stateChanged(); });
}
- (void)addWarning:(NSString *)warning {
    if (!warning.length) return;
    @synchronized(self) {
        NSString *next = _warning.length ? [_warning stringByAppendingFormat:@" %@", warning] : warning;
        [_warning release]; _warning = [next copy];
    }
}
- (void)syncCapture:(void (^)(void))block {
    if (dispatch_get_specific(HCCameraQueueKey) == self) block();
    else dispatch_sync(_captureQueue, block);
}
- (NSTimeInterval)elapsedSeconds {
    @synchronized(self) { return hc_camera_clock_elapsed(&_clock, hc_camera_host_seconds()); }
}
- (void)prepareWithCompletion:(void (^)(NSError *))completion {
    if (_closed || (self.state != HCCameraCaptureIdle && self.state != HCCameraCaptureFailed)) {
        hc_camera_main(^{ if (completion) completion(hc_camera_error(1, @"The camera is already open.")); });
        return;
    }
    [self setCaptureState:HCCameraCapturePreparing error:nil];
    void (^cameraAllowed)(BOOL) = ^(BOOL allowed) {
        if (!allowed) {
            NSError *error = hc_camera_error(2, @"Camera access is disabled. Allow Hardcore in System Settings → Privacy & Security → Camera.");
            [self setCaptureState:HCCameraCaptureFailed error:error];
            hc_camera_main(^{ if (completion) completion(error); });
            return;
        }
        /* Permission for an optional microphone must never gate the preview.
         * Already-authorized audio can join now; otherwise ask only on Rec. */
        BOOL microphone = [self cameraAuthorizationStatusForMediaType:AVMediaTypeAudio] == AVAuthorizationStatusAuthorized;
        dispatch_async(self->_captureQueue, ^{
            if (self->_closed) {
                hc_camera_main(^{ if (completion) completion(hc_camera_error(3, @"The camera was closed.")); });
                return;
            }
            [self configureWithMicrophone:microphone completion:completion];
        });
    };
    AVAuthorizationStatus status = [self cameraAuthorizationStatusForMediaType:AVMediaTypeVideo];
    if (status == AVAuthorizationStatusNotDetermined)
        [self requestCameraAccessForMediaType:AVMediaTypeVideo completion:cameraAllowed];
    else cameraAllowed(status == AVAuthorizationStatusAuthorized);
}
- (void)configureWithMicrophone:(BOOL)microphone completion:(void (^)(NSError *))completion {
    @autoreleasepool {
        NSError *error = nil;
        if (_session) {
            [_session stopRunning];
            [[NSNotificationCenter defaultCenter] removeObserver:self name:nil object:_session];
            [_videoOutput setSampleBufferDelegate:nil queue:NULL];
            [_audioOutput setSampleBufferDelegate:nil queue:NULL];
            [_videoOutput release]; _videoOutput = nil;
            [_audioOutput release]; _audioOutput = nil;
            [_session release]; _session = nil;
        }
        @synchronized(self) { [_warning release]; _warning = nil; }
        AVCaptureDevice *camera = nil, *builtIn = nil;
        NSArray *devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
        for (AVCaptureDevice *device in devices) {
            if (device.position == AVCaptureDevicePositionFront) { camera = device; break; }
            if ([device.deviceType isEqualToString:AVCaptureDeviceTypeBuiltInWideAngleCamera]) builtIn = device;
        }
        if (!camera) camera = builtIn ?: [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (!camera) error = hc_camera_error(4, @"No camera is available. Connect a camera and try again.");
        AVCaptureDeviceInput *cameraInput = camera ? [AVCaptureDeviceInput deviceInputWithDevice:camera error:&error] : nil;
        if (!cameraInput) {
            [self setCaptureState:HCCameraCaptureFailed error:error];
            hc_camera_main(^{ if (completion) completion(error); });
            return;
        }
        _session = [[AVCaptureSession alloc] init];
        [_session beginConfiguration];
        if ([_session canSetSessionPreset:AVCaptureSessionPreset1280x720])
            _session.sessionPreset = AVCaptureSessionPreset1280x720;
        else if ([_session canSetSessionPreset:AVCaptureSessionPresetHigh])
            _session.sessionPreset = AVCaptureSessionPresetHigh;
        if ([_session canAddInput:cameraInput]) [_session addInput:cameraInput];
        else error = hc_camera_error(5, @"This camera cannot be added to the recording session.");
        _videoOutput = [[AVCaptureVideoDataOutput alloc] init];
        _videoOutput.alwaysDiscardsLateVideoFrames = YES;
        NSNumber *nativeFormat = @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        if ([_videoOutput.availableVideoCVPixelFormatTypes containsObject:nativeFormat])
            _videoOutput.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey:nativeFormat};
        [_videoOutput setSampleBufferDelegate:self queue:_captureQueue];
        if ([_session canAddOutput:_videoOutput]) [_session addOutput:_videoOutput];
        else error = hc_camera_error(6, @"The camera does not provide a supported video stream.");
        AVCaptureConnection *videoConnection = [_videoOutput connectionWithMediaType:AVMediaTypeVideo];
        if (videoConnection.isVideoMirroringSupported) {
            videoConnection.automaticallyAdjustsVideoMirroring = NO;
            videoConnection.videoMirrored = NO;
        }
        @synchronized(self) { _hasMicrophone = NO; _receivedVideoFrames = 0; }
        BOOL audioReady = microphone && [self configureMicrophoneInput];
        if (!audioReady) [self addWarning:[self cameraAuthorizationStatusForMediaType:AVMediaTypeAudio] == AVAuthorizationStatusNotDetermined ?
            HCCameraMicrophonePendingWarning : HCCameraMicrophoneSilentWarning];
        [_session commitConfiguration];
        if (error) {
            [self setCaptureState:HCCameraCaptureFailed error:error];
            hc_camera_main(^{ if (completion) completion(error); });
            return;
        }
        /* Cap capture to 30 fps where supported, without forcing a new format. */
        if ([camera lockForConfiguration:NULL]) {
            for (AVFrameRateRange *range in camera.activeFormat.videoSupportedFrameRateRanges) {
                if (range.minFrameRate <= 30 && range.maxFrameRate >= 30) {
                    camera.activeVideoMinFrameDuration = CMTimeMake(1, 30);
                    camera.activeVideoMaxFrameDuration = CMTimeMake(1, 30);
                    break;
                }
            }
            [camera unlockForConfiguration];
        }
        _videoDimensions = CMVideoFormatDescriptionGetDimensions(camera.activeFormat.formatDescription);
        @synchronized(self) {
            [_cameraName release]; _cameraName = [camera.localizedName copy];
            _hasMicrophone = audioReady;
        }
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sessionError:)
            name:AVCaptureSessionRuntimeErrorNotification object:_session];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sessionInterrupted:)
            name:AVCaptureSessionWasInterruptedNotification object:_session];
        [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sessionInterruptionEnded:)
            name:AVCaptureSessionInterruptionEndedNotification object:_session];
        [self startPreparedSession:_session completion:completion];
    }
}
/* The preview connection is part of the session before capture starts. Layer
 * mutations stay on main; startRunning remains on the serial capture queue.
 * Neither queue synchronously waits for the other. */
- (NSError *)connectPreviewToSession:(AVCaptureSession *)session {
    _previewLayer.session = session;
    AVCaptureConnection *connection = _previewLayer.connection;
    if (!connection) return hc_camera_error(20, @"The camera could not connect to its preview. Close cam and open it again.");
    connection.enabled = YES;
    if (connection.isVideoMirroringSupported) {
        connection.automaticallyAdjustsVideoMirroring = NO;
        connection.videoMirrored = YES;
    }
    return nil;
}
- (NSError *)runPreparedSession:(AVCaptureSession *)session {
    [session startRunning];
    return session.isRunning ? nil : hc_camera_error(7, @"The camera could not start. It may be in use by another application.");
}
- (void)startPreparedSession:(AVCaptureSession *)session completion:(void (^)(NSError *))completion {
    hc_camera_main(^{
        if (self->_closed || self->_session != session) {
            if (completion) completion(hc_camera_error(3, @"The camera was closed."));
            return;
        }
        NSError *connectionError = [self connectPreviewToSession:session];
        if (connectionError) {
            [self setCaptureState:HCCameraCaptureFailed error:connectionError];
            if (completion) completion(connectionError);
            return;
        }
        dispatch_async(self->_captureQueue, ^{
            if (self->_closed || self->_session != session) {
                hc_camera_main(^{ if (completion) completion(hc_camera_error(3, @"The camera was closed.")); });
                return;
            }
            NSError *error = [self runPreparedSession:session];
            [self setCaptureState:error ? HCCameraCaptureFailed : HCCameraCaptureReady error:error];
            hc_camera_main(^{ if (completion) completion(error); });
        });
    });
}
/* Called inside begin/commitConfiguration, only after microphone permission. */
- (BOOL)configureMicrophoneInput {
    if (_hasMicrophone) return YES;
    AVCaptureDevice *microphone = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
    AVCaptureDeviceInput *input = microphone ? [AVCaptureDeviceInput deviceInputWithDevice:microphone error:NULL] : nil;
    if (!input || ![_session canAddInput:input]) return NO;
    AVCaptureAudioDataOutput *output = [[[AVCaptureAudioDataOutput alloc] init] autorelease];
    [output setSampleBufferDelegate:self queue:_captureQueue];
    [_session addInput:input];
    if (![_session canAddOutput:output]) {
        [output setSampleBufferDelegate:nil queue:NULL];
        [_session removeInput:input];
        return NO;
    }
    [_session addOutput:output];
    [_audioOutput release]; _audioOutput = [output retain];
    @synchronized(self) { _hasMicrophone = YES; }
    [self clearMicrophoneWarning];
    return YES;
}
- (void)sessionError:(NSNotification *)notification {
    NSError *error = notification.userInfo[AVCaptureSessionErrorKey] ?: hc_camera_error(8, @"The camera stopped unexpectedly.");
    dispatch_async(_captureQueue, ^{
        @synchronized(self) { hc_camera_clock_pause(&self->_clock, hc_camera_host_seconds()); }
        [self setCaptureState:HCCameraCaptureFailed error:error];
    });
}
- (void)sessionInterrupted:(NSNotification *)notification {
    dispatch_async(_captureQueue, ^{
        self->_interrupted = YES;
        if (self.state == HCCameraCaptureRecording) {
            @synchronized(self) { hc_camera_clock_pause(&self->_clock, hc_camera_host_seconds()); }
            [self addWarning:@"Camera capture was interrupted. Resume when the camera becomes available, or save the recording."];
            [self setCaptureState:HCCameraCapturePaused error:nil];
        }
    });
}
- (void)sessionInterruptionEnded:(NSNotification *)notification {
    dispatch_async(_captureQueue, ^{ self->_interrupted = NO; });
}
- (BOOL)pendingStartMatches:(NSUInteger)generation {
    @synchronized(self) { return _startPending && !_closed && generation == _startGeneration; }
}
- (void)cancelPendingStart {
    @synchronized(self) { _startGeneration++; }
    if (_pendingStartCompletion) _pendingStartCompletion(hc_camera_error(19, @"The recording start was cancelled."));
}
- (SFSpeechRecognizerAuthorizationStatus)speechAuthorizationStatus {
    return [SFSpeechRecognizer authorizationStatus];
}
- (void)requestSpeechAccess:(void (^)(SFSpeechRecognizerAuthorizationStatus))completion {
    [SFSpeechRecognizer requestAuthorization:completion];
}
- (void)startRecordingToDirectory:(NSURL *)directory completion:(void (^)(NSError *))completion {
    if (self.state != HCCameraCaptureReady || _startPending) {
        hc_camera_main(^{ if (completion) completion(hc_camera_error(9, @"Open the camera and wait for it to be ready before recording.")); });
        return;
    }
    NSUInteger generation;
    @synchronized(self) { _startPending = YES; generation = ++_startGeneration; }
    __block BOOL completed = NO;
    void (^finish)(NSError *) = ^(NSError *error) {
        hc_camera_main(^{
            if (completed) return;
            completed = YES;
            @synchronized(self) { self->_startPending = NO; }
            [self->_pendingStartCompletion release]; self->_pendingStartCompletion = nil;
            if (completion) completion(error);
        });
    };
    _pendingStartCompletion = [finish copy];
    void (^begin)(BOOL) = ^(BOOL authorized) {
        dispatch_async(self->_captureQueue, ^{
            if (![self pendingStartMatches:generation]) return;
            self->_speechAuthorized = authorized;
            if (self->_closed || self.state != HCCameraCaptureReady) {
                finish(hc_camera_error(10, @"The camera is no longer ready."));
                return;
            }
            if (self->_captionsEnabled && !authorized)
                [self addWarning:@"Speech permission is unavailable; captions will be empty."];
            NSError *error = [self beginWriterToDirectory:directory hostTime:hc_camera_host_seconds()];
            if (error) { [self discardOnCaptureQueue]; [self setCaptureState:HCCameraCaptureReady error:error]; }
            else [self setCaptureState:HCCameraCaptureRecording error:nil];
            finish(error);
        });
    };
    void (^microphoneAllowed)(BOOL) = ^(BOOL allowed) {
        dispatch_async(self->_captureQueue, ^{
            if (![self pendingStartMatches:generation]) return;
            if (self->_closed || self.state != HCCameraCaptureReady) {
                finish(hc_camera_error(10, @"The camera is no longer ready."));
                return;
            }
            if (allowed && !self->_hasMicrophone) {
                [self->_session beginConfiguration];
                [self configureMicrophoneInput];
                [self->_session commitConfiguration];
            }
            [self clearMicrophoneWarning];
            if (!self->_hasMicrophone) [self addWarning:HCCameraMicrophoneSilentWarning];
            hc_camera_main(^{
                if (![self pendingStartMatches:generation]) return;
                if (self->_captionsEnabled && self->_hasMicrophone && [self speechAuthorizationStatus] == SFSpeechRecognizerAuthorizationStatusNotDetermined)
                    [self requestSpeechAccess:^(SFSpeechRecognizerAuthorizationStatus status) { begin(status == SFSpeechRecognizerAuthorizationStatusAuthorized); }];
                else begin(self->_captionsEnabled && self->_hasMicrophone && [self speechAuthorizationStatus] == SFSpeechRecognizerAuthorizationStatusAuthorized);
            });
        });
    };
    AVAuthorizationStatus microphone = [self cameraAuthorizationStatusForMediaType:AVMediaTypeAudio];
    if (microphone == AVAuthorizationStatusNotDetermined)
        [self requestCameraAccessForMediaType:AVMediaTypeAudio completion:microphoneAllowed];
    else microphoneAllowed(microphone == AVAuthorizationStatusAuthorized);
}
- (NSError *)beginWriterToDirectory:(NSURL *)directory hostTime:(double)host {
    if (!directory.isFileURL) return hc_camera_error(11, @"Choose a local recording directory.");
    NSError *error = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:&error]) return error;
    NSURL *movie = [directory URLByAppendingPathComponent:@"camera.mp4"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:movie.path] ||
        [[NSFileManager defaultManager] fileExistsAtPath:[[directory URLByAppendingPathComponent:@"captions.json"] path]])
        return hc_camera_error(12, @"This directory already contains a recording. Choose a new directory.");
    [_writer release]; _writer = [[AVAssetWriter alloc] initWithURL:movie fileType:AVFileTypeMPEG4 error:&error];
    if (!_writer) return error;
    _writer.shouldOptimizeForNetworkUse = YES;
    NSDictionary *recommended = [_videoOutput recommendedVideoSettingsForAssetWriterWithOutputFileType:AVFileTypeMPEG4];
    NSInteger width = [recommended[AVVideoWidthKey] integerValue];
    NSInteger height = [recommended[AVVideoHeightKey] integerValue];
    if (width <= 0) width = _videoDimensions.width > 0 ? _videoDimensions.width : 1280;
    if (height <= 0) height = _videoDimensions.height > 0 ? _videoDimensions.height : 720;
    NSDictionary *video = @{AVVideoCodecKey:AVVideoCodecTypeH264, AVVideoWidthKey:@(width), AVVideoHeightKey:@(height),
        AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@(2500000), AVVideoExpectedSourceFrameRateKey:@30,
            AVVideoMaxKeyFrameIntervalKey:@60, AVVideoProfileLevelKey:AVVideoProfileLevelH264HighAutoLevel}};
    [_videoInput release]; _videoInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:video];
    _videoInput.expectsMediaDataInRealTime = YES;
    if (![_writer canAddInput:_videoInput]) return hc_camera_error(13, @"The system H.264 encoder is unavailable.");
    [_writer addInput:_videoInput];
    [_audioInput release]; _audioInput = nil;
    if (_hasMicrophone) {
        NSDictionary *audio = @{AVFormatIDKey:@(kAudioFormatMPEG4AAC), AVSampleRateKey:@48000,
            AVNumberOfChannelsKey:@1, AVEncoderBitRateKey:@96000};
        _audioInput = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio outputSettings:audio];
        _audioInput.expectsMediaDataInRealTime = YES;
        if (![_writer canAddInput:_audioInput]) return hc_camera_error(14, @"The system AAC encoder is unavailable.");
        [_writer addInput:_audioInput];
    }
    [_directory release]; _directory = [directory copy];
    [_movieURL release]; _movieURL = [movie copy];
    _ownsMovie = YES; _ownsCaptions = NO;
    if (![_writer startWriting]) return _writer.error ?: hc_camera_error(15, @"The recording file could not be opened.");
    [_writer startSessionAtSourceTime:kCMTimeZero];
    _generation++;
    _videoFrames = _audioBuffers = 0;
    _lastVideoPTS = _lastAudioPTS = kCMTimeInvalid;
    [_captions release]; _captions = [[NSArray alloc] init];
    @synchronized(self) { hc_camera_clock_start(&_clock, _testMode ? host : hc_camera_host_seconds()); }
    return nil;
}
- (void)pause {
    [self syncCapture:^{
        if (self.state != HCCameraCaptureRecording) return;
        @synchronized(self) { hc_camera_clock_pause(&self->_clock, hc_camera_host_seconds()); }
        [self setCaptureState:HCCameraCapturePaused error:nil];
    }];
}
- (void)resume {
    [self syncCapture:^{
        if (self.state != HCCameraCapturePaused) return;
        if (!self->_testMode && (!self->_session.isRunning || self->_interrupted)) {
            [self addWarning:@"The camera is still unavailable."]; return;
        }
        @synchronized(self) { hc_camera_clock_resume(&self->_clock, hc_camera_host_seconds()); }
        [self setCaptureState:HCCameraCaptureRecording error:nil];
    }];
}
- (void)captureOutput:(AVCaptureOutput *)output didOutputSampleBuffer:(CMSampleBufferRef)sample fromConnection:(AVCaptureConnection *)connection {
    @autoreleasepool {
        if (output == _videoOutput) {
            BOOL first;
            @synchronized(self) { first = ++_receivedVideoFrames == 1; }
            if (first) hc_camera_main(^{ if (self->_stateChanged) self->_stateChanged(); });
        }
        if (self.state != HCCameraCaptureRecording) return;
        CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
        CMClockRef sourceClock = _session.masterClock;
        CMTime host = sourceClock ? CMSyncConvertTime(pts, sourceClock, CMClockGetHostTimeClock()) : pts;
        [self appendSampleBuffer:sample video:(output == _videoOutput) hostTime:CMTimeGetSeconds(host)];
    }
}
- (void)appendSampleBuffer:(CMSampleBufferRef)sample video:(BOOL)video hostTime:(double)host {
    if (!_writer || _writer.status != AVAssetWriterStatusWriting || !CMSampleBufferDataIsReady(sample)) return;
    double seconds;
    @synchronized(self) { seconds = hc_camera_clock_sample(&_clock, host); }
    if (!isfinite(seconds) || seconds < 0) return;
    AVAssetWriterInput *input = video ? _videoInput : _audioInput;
    if (!input || !input.readyForMoreMediaData) return;
    CMTime newPTS = CMTimeMakeWithSeconds(seconds, 1000000000);
    CMTime oldPTS = CMSampleBufferGetPresentationTimeStamp(sample);
    CMTime previous = video ? _lastVideoPTS : _lastAudioPTS;
    if (CMTIME_IS_VALID(previous) && CMTimeCompare(newPTS, previous) <= 0) return;
    CMItemCount count = 0;
    if (CMSampleBufferGetSampleTimingInfoArray(sample, 0, NULL, &count) || count < 1) return;
    CMSampleTimingInfo *timings = calloc((size_t)count, sizeof(*timings));
    if (!timings) return;
    OSStatus status = CMSampleBufferGetSampleTimingInfoArray(sample, count, timings, &count);
    CMTime offset = CMTimeSubtract(oldPTS, newPTS);
    for (CMItemCount index = 0; !status && index < count; index++) {
        timings[index].presentationTimeStamp = CMTimeSubtract(timings[index].presentationTimeStamp, offset);
        if (CMTIME_IS_VALID(timings[index].decodeTimeStamp))
            timings[index].decodeTimeStamp = CMTimeSubtract(timings[index].decodeTimeStamp, offset);
    }
    CMSampleBufferRef adjusted = NULL;
    if (!status) status = CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, sample, count, timings, &adjusted);
    free(timings);
    if (status || !adjusted) return;
    BOOL appended = [input appendSampleBuffer:adjusted];
    CFRelease(adjusted);
    if (appended) {
        if (video) { _lastVideoPTS = newPTS; _videoFrames++; }
        else { _lastAudioPTS = newPTS; _audioBuffers++; }
    } else if (_writer.status == AVAssetWriterStatusFailed) {
        @synchronized(self) { hc_camera_clock_pause(&_clock, hc_camera_host_seconds()); }
        [self setCaptureState:HCCameraCaptureFailed error:_writer.error];
    }
}
- (void)stopWithCompletion:(void (^)(NSURL *, NSArray *, NSError *))completion {
    /* Freeze first so the caller's final UI event uses the precise movie end. */
    [self syncCapture:^{
        if (!self->_writer || (self.state != HCCameraCaptureRecording && self.state != HCCameraCapturePaused && self.state != HCCameraCaptureFailed)) {
            hc_camera_main(^{ if (completion) completion(nil, @[], hc_camera_error(16, @"No recording is in progress.")); });
            return;
        }
        @synchronized(self) { hc_camera_clock_stop(&self->_clock, hc_camera_host_seconds()); }
        [self->_stopCompletion release]; self->_stopCompletion = [completion copy];
        [self setCaptureState:HCCameraCaptureFinishing error:nil];
        NSUInteger generation = self->_generation;
        if (self->_writer.status != AVAssetWriterStatusWriting || !self->_videoFrames) {
            NSError *error = self->_writer.error ?: hc_camera_error(17, @"No camera frames were recorded. Wait for the camera preview, then try again.");
            [self->_writer cancelWriting];
            [self completeStopForGeneration:generation error:error];
            return;
        }
        [self->_writer endSessionAtSourceTime:CMTimeMakeWithSeconds([self elapsedSeconds], 1000000000)];
        [self->_videoInput markAsFinished];
        [self->_audioInput markAsFinished];
        [self->_writer finishWritingWithCompletionHandler:^{
            dispatch_async(self->_captureQueue, ^{
                if (generation != self->_generation) return;
                NSError *error = self->_writer.status == AVAssetWriterStatusCompleted ? nil : self->_writer.error;
                if (!error && self->_writer.status != AVAssetWriterStatusCompleted) error = hc_camera_error(18, @"The movie could not be finalized.");
                if (error) [self completeStopForGeneration:generation error:error];
                else [self finishCaptionsForGeneration:generation];
            });
        }];
    }];
}
/* Compact word timings into readable caption cues, retaining exact start/end. */
- (NSArray *)captionsFromTranscription:(SFTranscription *)transcription {
    NSMutableArray *result = [NSMutableArray array];
    NSMutableString *text = [NSMutableString string];
    double start = 0, end = 0, duration = [self elapsedSeconds];
    for (SFTranscriptionSegment *segment in transcription.segments) {
        double nextStart = fmax(0, segment.timestamp), nextEnd = fmin(duration, segment.timestamp+segment.duration);
        if (nextStart >= duration || nextEnd <= nextStart) continue;
        if (text.length && (text.length + segment.substring.length > 48 || nextEnd-start > 4.0 || nextStart-end > 0.8)) {
            [result addObject:@{@"start":@(start), @"end":@(end), @"text":[NSString stringWithString:text]}];
            [text setString:@""];
        }
        if (!text.length) start = nextStart;
        else [text appendString:@" "];
        [text appendString:segment.substring]; end = nextEnd;
    }
    if (text.length) [result addObject:@{@"start":@(start), @"end":@(end), @"text":[NSString stringWithString:text]}];
    return result;
}
- (void)finishCaptionsForGeneration:(NSUInteger)generation {
    if (!_captionsEnabled || !_speechAuthorized || !_audioBuffers) {
        [self completeStopForGeneration:generation error:nil]; return;
    }
    [_recognizer release]; _recognizer = [[SFSpeechRecognizer alloc] initWithLocale:[NSLocale currentLocale]];
    if (!_recognizer || !_recognizer.supportsOnDeviceRecognition || !_recognizer.isAvailable) {
        [self addWarning:@"On-device captions are unavailable for this language. The camera movie was saved."];
        [self completeStopForGeneration:generation error:nil]; return;
    }
    SFSpeechURLRecognitionRequest *request = [[[SFSpeechURLRecognitionRequest alloc] initWithURL:_movieURL] autorelease];
    request.requiresOnDeviceRecognition = YES;
    request.shouldReportPartialResults = YES;
    request.taskHint = SFSpeechRecognitionTaskHintDictation;
    _speechTask = [[_recognizer recognitionTaskWithRequest:request resultHandler:^(SFSpeechRecognitionResult *result, NSError *error) {
        dispatch_async(self->_captureQueue, ^{
            if (generation != self->_generation || self.state != HCCameraCaptureFinishing) return;
            if (result) {
                NSArray *captions = [self captionsFromTranscription:result.bestTranscription];
                [self->_captions release]; self->_captions = [captions copy];
            }
            if (result.isFinal || error) {
                if (error) [self addWarning:@"On-device transcription did not complete; available captions were saved."];
                [self completeStopForGeneration:generation error:nil];
            }
        });
    }] retain];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), _captureQueue, ^{
        if (generation == self->_generation && self.state == HCCameraCaptureFinishing) {
            [self addWarning:@"On-device transcription timed out; available captions were saved."];
            [self completeStopForGeneration:generation error:nil];
        }
    });
}
- (void)completeStopForGeneration:(NSUInteger)generation error:(NSError *)error {
    if (generation != _generation || self.state != HCCameraCaptureFinishing) return;
    [_speechTask cancel]; [_speechTask release]; _speechTask = nil;
    if (!error) {
        NSData *json = [NSJSONSerialization dataWithJSONObject:_captions options:NSJSONWritingPrettyPrinted error:&error];
        if (json) {
            NSURL *captionsURL = [_directory URLByAppendingPathComponent:@"captions.json"];
            if ([json writeToURL:captionsURL options:NSDataWritingAtomic error:&error]) _ownsCaptions = YES;
        }
    }
    void (^completion)(NSURL *, NSArray *, NSError *) = [[_stopCompletion copy] autorelease];
    [_stopCompletion release]; _stopCompletion = nil;
    NSURL *movie = [[_movieURL retain] autorelease];
    NSArray *captions = [[_captions copy] autorelease];
    /* Stop transfers the take to its caller, even on finalization error, so a
     * recoverable draft is not deleted when the camera preview closes. */
    _ownsMovie = _ownsCaptions = NO;
    [self setCaptureState:error ? HCCameraCaptureFailed : HCCameraCaptureReady error:error];
    hc_camera_main(^{ if (completion) completion(error ? nil : movie, captions, error); });
}
- (void)discardOnCaptureQueue {
    _generation++;
    @synchronized(self) { hc_camera_clock_stop(&_clock, hc_camera_host_seconds()); }
    [_speechTask cancel]; [_speechTask release]; _speechTask = nil;
    if (_writer.status == AVAssetWriterStatusWriting || _writer.status == AVAssetWriterStatusUnknown) [_writer cancelWriting];
    if (_ownsMovie && _movieURL) [[NSFileManager defaultManager] removeItemAtURL:_movieURL error:NULL];
    if (_ownsCaptions && _directory) [[NSFileManager defaultManager] removeItemAtURL:[_directory URLByAppendingPathComponent:@"captions.json"] error:NULL];
    _ownsMovie = _ownsCaptions = NO;
    [_writer release]; _writer = nil;
    [_videoInput release]; _videoInput = nil;
    [_audioInput release]; _audioInput = nil;
    if (_stopCompletion) {
        void (^completion)(NSURL *, NSArray *, NSError *) = [[_stopCompletion copy] autorelease];
        [_stopCompletion release]; _stopCompletion = nil;
        hc_camera_main(^{ completion(nil, @[], hc_camera_error(19, @"The recording was discarded.")); });
    }
}
- (void)discardWithCompletion:(void (^)(void))completion {
    [self cancelPendingStart];
    dispatch_async(_captureQueue, ^{
        [self discardOnCaptureQueue];
        [self setCaptureState:self->_session.isRunning || self->_testMode ? HCCameraCaptureReady : HCCameraCaptureIdle error:nil];
        hc_camera_main(completion);
    });
}
- (void)shutdown {
    @synchronized(self) { _closed = YES; }
    [self cancelPendingStart];
    self.stateChanged = nil;
    _previewLayer.session = nil;
    dispatch_async(_captureQueue, ^{
        [self discardOnCaptureQueue];
        [self->_videoOutput setSampleBufferDelegate:nil queue:NULL];
        [self->_audioOutput setSampleBufferDelegate:nil queue:NULL];
        [self->_session stopRunning];
        [self setCaptureState:HCCameraCaptureIdle error:nil];
    });
}
- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_videoOutput setSampleBufferDelegate:nil queue:NULL];
    [_audioOutput setSampleBufferDelegate:nil queue:NULL];
    [_session stopRunning];
    [_speechTask cancel]; [_speechTask release]; [_recognizer release];
    [_stateChanged release]; [_stopCompletion release]; [_pendingStartCompletion release];
    [_session release]; [_previewLayer release]; [_videoOutput release]; [_audioOutput release];
    [_writer release]; [_videoInput release]; [_audioInput release];
    [_directory release]; [_movieURL release]; [_cameraName release]; [_warning release]; [_lastError release]; [_captions release];
    dispatch_release(_captureQueue);
    [super dealloc];
}
@end
