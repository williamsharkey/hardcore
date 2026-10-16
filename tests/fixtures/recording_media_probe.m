/* Offline fixture inspector: never opens capture devices. */
#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
int main(int argc,const char **argv) { @autoreleasepool {
    if (argc<2) return 2;
    AVURLAsset *asset=[AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]] options:nil];
    AVAssetTrack *video=[asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    AVAssetTrack *audio=[asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
    NSMutableDictionary *info=[@{@"duration":@(CMTimeGetSeconds(asset.duration)),@"width":@(video.naturalSize.width),@"height":@(video.naturalSize.height),@"fps":@(video.nominalFrameRate),@"audio":@(audio!=nil)} mutableCopy];
    if (video.formatDescriptions.count) info[@"videoCodec"]=@(CMFormatDescriptionGetMediaSubType((__bridge CMFormatDescriptionRef)video.formatDescriptions.firstObject));
    if (audio.formatDescriptions.count) {
        CMAudioFormatDescriptionRef desc=(__bridge CMAudioFormatDescriptionRef)audio.formatDescriptions.firstObject;
        const AudioStreamBasicDescription *format=CMAudioFormatDescriptionGetStreamBasicDescription(desc);
        info[@"audioCodec"]=@(format->mFormatID); info[@"sampleRate"]=@(format->mSampleRate); info[@"channels"]=@(format->mChannelsPerFrame);
    }
    NSError *error;
    if (audio) {
        AVAssetReader *reader=[[AVAssetReader alloc] initWithAsset:asset error:&error];
        AVAssetReaderTrackOutput *out=[AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:audio outputSettings:@{AVFormatIDKey:@(kAudioFormatLinearPCM),AVLinearPCMBitDepthKey:@16,AVLinearPCMIsFloatKey:@NO,AVLinearPCMIsBigEndianKey:@NO,AVLinearPCMIsNonInterleaved:@NO}];
        [reader addOutput:out];
        if (![reader startReading]) return 1;
        double first=-1; int peak=0;
        CMSampleBufferRef sample;
        while ((sample=[out copyNextSampleBuffer])) {
            CMBlockBufferRef data=CMSampleBufferGetDataBuffer(sample);
            size_t size=CMBlockBufferGetDataLength(data);
            int16_t *values=malloc(size);
            if (CMBlockBufferCopyDataBytes(data,0,size,values)) return 1;
            CMAudioFormatDescriptionRef desc=CMSampleBufferGetFormatDescription(sample);
            const AudioStreamBasicDescription *format=CMAudioFormatDescriptionGetStreamBasicDescription(desc);
            for (size_t i=0;i<size/2;i++) {
                int amplitude=abs(values[i]);
                if (amplitude>peak) peak=amplitude;
                if (amplitude>100 && first<0) first=CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample))+(double)(i/format->mChannelsPerFrame)/format->mSampleRate;
            }
            free(values); CFRelease(sample);
        }
        info[@"firstAudible"]=@(first); info[@"audioPeak"]=@(peak);
    }
    if (argc==4) {
        AVAssetImageGenerator *gen=[AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
        gen.requestedTimeToleranceBefore=kCMTimeZero; gen.requestedTimeToleranceAfter=kCMTimeZero;
        CGImageRef image=[gen copyCGImageAtTime:CMTimeMakeWithSeconds(atof(argv[2]),60000) actualTime:NULL error:&error];
        if (!image) { fprintf(stderr,"%s\n",error.description.UTF8String); return 1; }
        CGImageDestinationRef out=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[3]]],CFSTR("public.png"),1,NULL);
        CGImageDestinationAddImage(out,image,NULL); BOOL ok=CGImageDestinationFinalize(out); CFRelease(out); CGImageRelease(image);
        if (!ok) return 1;
    }
    NSData *json=[NSJSONSerialization dataWithJSONObject:info options:NSJSONWritingSortedKeys error:&error];
    if (!json) return 1;
    fwrite(json.bytes,1,json.length,stdout); putchar('\n'); return 0;
} }
