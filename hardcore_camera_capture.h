/* Native camera-only recording. All control methods are called on the main
 * thread. Completion/state callbacks run on the main thread. Constructing the
 * recorder has no privacy side effects; prepare is the explicit permission and
 * capture boundary. Call shutdown before releasing an active recorder. */
#ifndef HARDCORE_CAMERA_CAPTURE_H
#define HARDCORE_CAMERA_CAPTURE_H
#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>

typedef NS_ENUM(NSInteger, HCCameraCaptureState) {
    HCCameraCaptureIdle, HCCameraCapturePreparing, HCCameraCaptureReady,
    HCCameraCaptureRecording, HCCameraCapturePaused, HCCameraCaptureFinishing,
    HCCameraCaptureFailed
};

@interface HCCameraCapture : NSObject
@property(nonatomic, readonly) AVCaptureVideoPreviewLayer *previewLayer;
@property(nonatomic, readonly) HCCameraCaptureState state;
@property(nonatomic, readonly, copy) NSString *stateDescription;
@property(nonatomic, readonly, copy) NSString *cameraName;
@property(nonatomic, readonly, copy) NSString *warning;
@property(nonatomic, readonly, retain) NSError *lastError;
@property(nonatomic, readonly) BOOL hasMicrophone;
/* Observability for distinguishing a streaming camera from preview layout. */
@property(nonatomic, readonly) NSUInteger receivedVideoFrames;
/* Set before start. Only on-device recognition is permitted; unavailable
 * recognition leaves a valid movie and empty captions with a warning. */
@property(nonatomic) BOOL captionsEnabled;
@property(nonatomic, copy) void (^stateChanged)(void);
/* Camera preview starts after camera permission alone. Microphone permission,
 * when not yet decided, is requested on the first recording attempt. */
- (void)prepareWithCompletion:(void (^)(NSError *error))completion;
- (void)startRecordingToDirectory:(NSURL *)directory completion:(void (^)(NSError *error))completion;
/* These transitions take effect before returning. elapsedSeconds shares the
 * exact host clock and pause offsets used by the media sample timestamps. */
- (void)pause;
- (void)resume;
- (NSTimeInterval)elapsedSeconds;
/* Saves camera.mp4 and captions.json. Each caption is {start,end,text}, seconds
 * on the same pause-free timeline. Optional transcription runs after encoding. */
- (void)stopWithCompletion:(void (^)(NSURL *movieURL, NSArray *captions, NSError *error))completion;
/* Deletes only files this recorder created, never the containing directory. */
- (void)discardWithCompletion:(void (^)(void))completion;
- (void)shutdown;
@end
#endif
