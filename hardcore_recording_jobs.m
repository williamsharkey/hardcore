/* Persistent recording exports. Included after hardcore_recording.m. All manager
 * APIs run on the main thread; no window, camera, or replay view owns a job.
 * The host supplies record_root/resource/status and record_jobs_changed. */
#ifndef HARDCORE_RECORDING_JOBS_M
#define HARDCORE_RECORDING_JOBS_M
#import <Foundation/Foundation.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

static BOOL record_library_number(id value) {
    return [value isKindOfClass:NSNumber.class] && isfinite([value doubleValue]);
}
static BOOL record_library_local_document(NSURL *url,NSString *bundle) {
    if(!url.isFileURL)return NO;
    NSString *root=bundle.stringByResolvingSymlinksInPath;
    NSString *expected=[[root stringByAppendingPathComponent:@"index.html"] stringByResolvingSymlinksInPath];
    return [expected hasPrefix:[root stringByAppendingString:@"/"]] && [url.path.stringByResolvingSymlinksInPath isEqualToString:expected];
}
static NSDictionary *record_library_edits(id body,NSString **error) {
    if(![body isKindOfClass:NSDictionary.class]){*error=@"Invalid export settings";return nil;}
    if(!record_library_number(body[@"startMs"]) || !record_library_number(body[@"endMs"])) {
        *error=@"Choose a valid start and end for the clip";return nil;
    }
    double start=[body[@"startMs"] doubleValue],end=[body[@"endMs"] doubleValue];
    if(start<0 || end<=start || end>7*24*3600*1000.0) {
        *error=@"Choose a valid start and end for the clip";return nil;
    }
    id crop=body[@"crop"];
    if(crop && crop!=NSNull.null) {
        if(![crop isKindOfClass:NSArray.class] || [crop count]!=4){*error=@"Invalid crop rectangle";return nil;}
        for(id n in crop)if(!record_library_number(n) || [n doubleValue]<0 || [n doubleValue]>8192){*error=@"Invalid crop rectangle";return nil;}
        if([crop[2] doubleValue]<=0 || [crop[3] doubleValue]<=0){*error=@"Crop dimensions must be positive";return nil;}
    }
    id captions=body[@"captionItems"] ?: @[];
    if(![captions isKindOfClass:NSArray.class] || [captions count]>10000){*error=@"Too many captions";return nil;}
    NSMutableArray *clean=[NSMutableArray array];NSUInteger textLength=0;
    for(id cue in captions) {
        if(![cue isKindOfClass:NSDictionary.class] || !record_library_number(cue[@"start"]) || !record_library_number(cue[@"end"]) ||
           [cue[@"start"] doubleValue]<0 || [cue[@"end"] doubleValue]<=[cue[@"start"] doubleValue] || [cue[@"end"] doubleValue]>7*24*3600*1000.0 ||
           ![cue[@"text"] isKindOfClass:NSString.class] || [cue[@"text"] length]>10000) {*error=@"Invalid caption";return nil;}
        textLength+=[cue[@"text"] length];
        if(textLength>512*1024){*error=@"Caption text is too large";return nil;}
        [clean addObject:@{@"start":cue[@"start"],@"end":cue[@"end"],@"text":cue[@"text"]}];
    }
    for(NSString *key in @[@"captions",@"pointer"])if(body[key] && ![body[key] isKindOfClass:NSNumber.class]){*error=@"Invalid display setting";return nil;}
    return @{@"version":@1,@"startMs":@(start),@"endMs":@(end),@"crop":crop ?: NSNull.null,
        @"captions":body[@"captions"] ?: @YES,@"pointer":body[@"pointer"] ?: @YES,@"captionItems":clean};
}
static NSString *record_library_srt_time(double milliseconds) {
    long long value=llround(milliseconds);
    return [NSString stringWithFormat:@"%02lld:%02lld:%02lld,%03lld",value/3600000,value/60000%60,value/1000%60,value%1000];
}
static NSString *record_library_srt(NSArray *captions) {
    NSMutableString *s=[NSMutableString string];int index=1;
    for(NSDictionary *cue in captions) {
        NSString *text=[cue[@"text"] stringByReplacingOccurrencesOfString:@"\r" withString:@""];
        /* SRT blank lines separate cues; preserve the visible paragraph break
           as a single line break, rather than accidentally creating a new cue. */
        while([text rangeOfString:@"\n\n"].location!=NSNotFound)text=[text stringByReplacingOccurrencesOfString:@"\n\n" withString:@"\n"];
        [s appendFormat:@"%d\n%@ --> %@\n%@\n\n",index++,record_library_srt_time([cue[@"start"] doubleValue]),record_library_srt_time([cue[@"end"] doubleValue]),text];
    }
    return s;
}
static NSString *record_library_log(NSString *path) {
    NSFileHandle *file=[NSFileHandle fileHandleForReadingAtPath:path];
    if(!file)return @"";
    unsigned long long size=[file seekToEndOfFile];[file seekToFileOffset:size>16384?size-16384:0];
    NSData *data=[file readDataToEndOfFile];[file closeFile];
    return [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease] ?: @"";
}

@interface HCRecordingJob : NSObject {
    NSTask *_task;
    NSString *_temporary;
    NSString *_staged;
    NSString *_destination;
    NSString *_log;
    void (^_completion)(BOOL,NSString *);
    BOOL _replaceExisting;
}
@property(readonly) NSString *log;
@property BOOL replaceExisting;
- (BOOL)start:(NSString *)tool arguments:(NSArray *)arguments staged:(NSString *)staged destination:(NSString *)destination temporary:(NSString *)temporary completion:(void (^)(BOOL,NSString *))completion;
@end
@implementation HCRecordingJob
@synthesize replaceExisting=_replaceExisting;
- (instancetype)init {if((self=[super init]))_replaceExisting=YES;return self;}
- (NSString *)log {return record_library_log(_log);}
- (void)dealloc {
    [_task release];[_temporary release];[_staged release];[_destination release];[_log release];[_completion release];[super dealloc];
}
- (BOOL)start:(NSString *)tool arguments:(NSArray *)arguments staged:(NSString *)staged destination:(NSString *)destination temporary:(NSString *)temporary completion:(void (^)(BOOL,NSString *))completion {
    _temporary=[temporary copy];_staged=[staged copy];_destination=[destination copy];_completion=[completion copy];
    _log=[[_temporary stringByAppendingPathComponent:@"export.log"] copy];
    NSFileManager *fm=NSFileManager.defaultManager;
    [fm createFileAtPath:_log contents:[NSData data] attributes:@{NSFilePosixPermissions:@0600}];
    NSFileHandle *output=[NSFileHandle fileHandleForWritingAtPath:_log];
    if(!output) {
        [fm removeItemAtPath:_temporary error:nil];[fm removeItemAtPath:_staged error:nil];
        if(_completion)_completion(NO,@"Cannot create the export log");return NO;
    }
    _task=[[NSTask alloc] init];_task.executableURL=[NSURL fileURLWithPath:tool];_task.arguments=arguments;
    _task.standardOutput=output;_task.standardError=output;_task.standardInput=[NSFileHandle fileHandleWithNullDevice];
    _task.terminationHandler=^(NSTask *task){
        int status=task.terminationStatus;
        dispatch_async(dispatch_get_main_queue(),^{
            NSError *error=nil;BOOL ok=status==0;
            if(ok) {
                if(!_replaceExisting) {
                    /* Same-volume hard-link publication is atomic and fails
                     * with EEXIST even if another writer races completion. */
                    chmod(_staged.fileSystemRepresentation,0600);
                    ok=link(_staged.fileSystemRepresentation,_destination.fileSystemRepresentation)==0;
                    if(ok)unlink(_staged.fileSystemRepresentation);
                    else error=[NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
                } else if([fm fileExistsAtPath:_destination])ok=[fm replaceItemAtURL:[NSURL fileURLWithPath:_destination] withItemAtURL:[NSURL fileURLWithPath:_staged] backupItemName:nil options:0 resultingItemURL:NULL error:&error];
                else ok=[fm moveItemAtPath:_staged toPath:_destination error:&error];
            }
            NSString *message=ok?[NSString stringWithFormat:@"Saved %@",_destination.lastPathComponent]:error.localizedDescription;
            if(!message.length)message=record_library_log(_log);
            if(!message.length)message=[NSString stringWithFormat:@"Export stopped with status %d",status];
            [fm removeItemAtPath:_temporary error:nil];[fm removeItemAtPath:_staged error:nil];
            if(_completion)_completion(ok,message);
            _task.terminationHandler=nil;
        });
    };
    NSError *error=nil;
    if(![_task launchAndReturnError:&error]) {
        _task.terminationHandler=nil;[output closeFile];
        [fm removeItemAtPath:_temporary error:nil];[fm removeItemAtPath:_staged error:nil];
        if(_completion)_completion(NO,error.localizedDescription);return NO;
    }
    [output closeFile];return YES;
}
@end

static NSString *record_library_temporary(void) {
    NSString *path=[NSTemporaryDirectory() stringByAppendingPathComponent:[@"hardcore-export-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    return [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:nil]?path:nil;
}
static NSString *record_library_stage(NSString *destination) {
    return [[destination stringByDeletingLastPathComponent] stringByAppendingPathComponent:[NSString stringWithFormat:@".hardcore-%@-%@",NSUUID.UUID.UUIDString,destination.lastPathComponent]];
}
/* Only complete, well-formed progress lines count. A log may end halfway
 * through a write, or contain unrelated decoder diagnostics. */
static double record_jobs_progress(NSString *log) {
    double result=-1;
    for(NSString *line in [log componentsSeparatedByString:@"\n"]) {
        if(![line hasPrefix:@"hardcore export: "] || ![line hasSuffix:@" frames"])continue;
        NSString *numbers=[line substringWithRange:NSMakeRange(17,line.length-17-7)];
        NSArray *parts=[numbers componentsSeparatedByString:@"/"];
        if(parts.count!=2)continue;
        NSCharacterSet *other=[[NSCharacterSet characterSetWithCharactersInString:@"0123456789"] invertedSet];
        if(![parts[0] length] || ![parts[1] length] || [parts[0] rangeOfCharacterFromSet:other].location!=NSNotFound || [parts[1] rangeOfCharacterFromSet:other].location!=NSNotFound)continue;
        double current=[parts[0] doubleValue],total=[parts[1] doubleValue];
        if(isfinite(current) && isfinite(total) && total>0 && current<=total)result=current/total;
    }
    return result;
}
static id record_jobs_json(NSString *path,NSUInteger limit) {
    NSDictionary *attributes=[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if(!attributes || [attributes fileSize]>limit)return nil;
    NSData *data=[NSData dataWithContentsOfFile:path];
    return data && data.length<=limit?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
}
static NSDictionary *record_jobs_manifest(NSString *bundle) {
    id value=record_jobs_json([bundle stringByAppendingPathComponent:@"manifest.json"],1024*1024);
    if(![value isKindOfClass:NSDictionary.class] || !record_library_number(value[@"durationMs"]))return nil;
    double duration=[value[@"durationMs"] doubleValue];
    return duration>0 && duration<=7*24*3600*1000.0?value:nil;
}
static NSString *record_jobs_title(NSString *bundle,NSDictionary *manifest) {
    id title=manifest[@"title"];
    return [title isKindOfClass:NSString.class] && [title length]?title:bundle.lastPathComponent.stringByDeletingPathExtension;
}
static NSArray *mac_record_items(void) {
    NSMutableArray *items=[NSMutableArray array];
    NSFileManager *fm=NSFileManager.defaultManager;
    for(NSString *name in [fm contentsOfDirectoryAtPath:record_root() error:nil]) {
        if(![name hasSuffix:@".hcrec"] || [name hasSuffix:@".partial.hcrec"] || [name hasPrefix:@"."])continue;
        NSString *path=[record_root() stringByAppendingPathComponent:name];BOOL directory=NO;
        if(![fm fileExistsAtPath:path isDirectory:&directory] || !directory)continue;
        NSDictionary *manifest=record_jobs_manifest(path);if(!manifest)continue;
        id date=manifest[@"createdAt"];if(![date isKindOfClass:NSString.class])date=@"";
        double duration=[manifest[@"durationMs"] doubleValue];
        long long seconds=(long long)floor(duration/1000);
        NSString *formatted=seconds>=3600?[NSString stringWithFormat:@"%lld:%02lld:%02lld",seconds/3600,seconds/60%60,seconds%60]:[NSString stringWithFormat:@"%lld:%02lld",seconds/60,seconds%60];
        [items addObject:@{@"path":path,@"title":record_jobs_title(path,manifest),@"date":date,@"duration":formatted,@"durationMs":@(duration)}];
    }
    [items sortUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b){
        NSComparisonResult result=[b[@"date"] compare:a[@"date"]];
        return result==NSOrderedSame?[a[@"path"] compare:b[@"path"]]:result;
    }];
    return [[items copy] autorelease];
}

static NSMutableArray *g_record_jobs;
static NSArray *g_record_job_snapshots;
static NSTimer *g_record_job_timer;
static void mac_record_jobs_tick(void);
static int mac_record_exports_active(void) {
    int active=0;for(NSDictionary *job in g_record_jobs)if([job[@"state"] isEqual:@"running"])active++;return active;
}
static void record_jobs_notify(void) {
    [g_record_job_snapshots release];g_record_job_snapshots=nil;
    record_jobs_changed();
}
static NSArray *mac_record_jobs(void) {
    if(!g_record_job_snapshots) {
        NSMutableArray *snapshots=[NSMutableArray array];
        for(NSDictionary *job in g_record_jobs) {
            NSMutableDictionary *snapshot=[[job mutableCopy] autorelease];[snapshot removeObjectForKey:@"runner"];
            [snapshots addObject:[[snapshot copy] autorelease]];
        }
        g_record_job_snapshots=[snapshots copy];
    }
    return [[g_record_job_snapshots retain] autorelease];
}
static void record_jobs_timer_sync(void) {
    if(mac_record_exports_active()) {
        if(!g_record_job_timer) {
            g_record_job_timer=[[NSTimer timerWithTimeInterval:.5 repeats:YES block:^(NSTimer *timer){(void)timer;mac_record_jobs_tick();}] retain];
            [NSRunLoop.mainRunLoop addTimer:g_record_job_timer forMode:NSRunLoopCommonModes];
        }
    } else { [g_record_job_timer invalidate];[g_record_job_timer release];g_record_job_timer=nil; }
}
static void mac_record_jobs_tick(void) {
    BOOL changed=NO;
    for(NSMutableDictionary *job in g_record_jobs) {
        if(![job[@"state"] isEqual:@"running"] || ![job[@"kind"] isEqual:@"mp4"])continue;
        double progress=record_jobs_progress([(HCRecordingJob *)job[@"runner"] log]);
        if(progress<0 || progress<=[job[@"progress"] doubleValue])continue;
        job[@"progress"]=@(progress);
        job[@"message"]=progress>=1?@"Finishing MP4…":[NSString stringWithFormat:@"Exporting MP4 · %.0f%%",floor(progress*100)];
        changed=YES;
    }
    if(changed)record_jobs_notify();
}
static BOOL record_jobs_reject(NSString *message) {record_status(message ?: @"Could not start export");return NO;}
static NSString *record_jobs_short_message(NSString *message) {
    message=[message stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if(!message.length)return @"Export failed";
    return message.length>1024?[message substringFromIndex:message.length-1024]:message;
}
static BOOL mac_record_export(NSString *bundle,NSDictionary *edits,BOOL zip) {
    if(![bundle isKindOfClass:NSString.class] || !bundle.length)return record_jobs_reject(@"Choose a saved recording");
    bundle=bundle.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    BOOL directory=NO;NSFileManager *fm=NSFileManager.defaultManager;
    if(![fm fileExistsAtPath:bundle isDirectory:&directory] || !directory || [bundle hasSuffix:@".partial.hcrec"])return record_jobs_reject(@"Choose a finished recording");
    NSDictionary *manifest=record_jobs_manifest(bundle);
    if(!manifest)return record_jobs_reject(@"This recording has no valid manifest");
    NSString *kind=zip?@"zip":@"mp4";
    for(NSDictionary *job in g_record_jobs)
        if([job[@"state"] isEqual:@"running"] && [job[@"bundle"] isEqual:bundle] && [job[@"kind"] isEqual:kind])return record_jobs_reject(@"That export is already running");
    if(mac_record_exports_active()>=32)return record_jobs_reject(@"Wait for an export to finish before starting another");
    BOOL overrideCaptions=NO;
    NSDictionary *settings=nil;
    NSString *tool=@"/usr/bin/ditto";
    if(!zip) {
        tool=record_resource(@"recording-export");
        if(![fm isExecutableFileAtPath:tool])tool=record_resource(@"build/recording-export");
        if(![fm isExecutableFileAtPath:tool])return record_jobs_reject(@"The recording exporter is missing; rebuild Hardcore");
        id input=edits;
        if(!input) {
            NSString *saved=[bundle stringByAppendingPathComponent:@"edits.json"];
            if([fm fileExistsAtPath:saved]) {
                input=record_jobs_json(saved,4*1024*1024);
                if(!input)return record_jobs_reject(@"The saved recording edits could not be read");
            }
        }
        if(input) {
            overrideCaptions=[input isKindOfClass:NSDictionary.class] && input[@"captionItems"]!=nil;
            NSString *why=nil;settings=record_library_edits(input,&why);
            if(!settings)return record_jobs_reject(why);
        } else settings=@{@"startMs":@0,@"endMs":manifest[@"durationMs"],@"crop":NSNull.null,@"captions":@YES,@"pointer":@YES};
        if([settings[@"endMs"] doubleValue]>[manifest[@"durationMs"] doubleValue]+.001)return record_jobs_reject(@"The trim range exceeds this recording");
    }
    NSString *exports=[record_root() stringByAppendingPathComponent:@"Exports"];
    NSError *error=nil;
    if(![fm createDirectoryAtPath:exports withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&error])return record_jobs_reject(error.localizedDescription);
    NSString *identifier=NSUUID.UUID.UUIDString;
    NSString *stem=bundle.lastPathComponent.stringByDeletingPathExtension;
    if(stem.length>100)stem=[stem substringToIndex:100];
    NSString *destination=[exports stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@.%@",stem,identifier.lowercaseString,kind]];
    NSString *staged=record_library_stage(destination),*temporary=record_library_temporary();
    if(!temporary)return record_jobs_reject(@"Cannot create a private export directory");
    NSMutableArray *arguments;
    if(zip)arguments=[NSMutableArray arrayWithArray:@[@"-c",@"-k",@"--sequesterRsrc",@"--keepParent",bundle,staged]];
    else {
        arguments=[NSMutableArray arrayWithArray:@[bundle,staged,@"--start",[NSString stringWithFormat:@"%.6f",[settings[@"startMs"] doubleValue]/1000],@"--end",[NSString stringWithFormat:@"%.6f",[settings[@"endMs"] doubleValue]/1000]]];
        id crop=settings[@"crop"];if(crop && crop!=NSNull.null)[arguments addObjectsFromArray:@[@"--crop",[crop componentsJoinedByString:@","]]];
        if(![settings[@"pointer"] boolValue])[arguments addObject:@"--no-pointer"];
        if(![settings[@"captions"] boolValue])[arguments addObject:@"--no-captions"];
        if(overrideCaptions) {
            NSString *captions=[temporary stringByAppendingPathComponent:@"captions.srt"];
            NSData *data=[record_library_srt(settings[@"captionItems"]) dataUsingEncoding:NSUTF8StringEncoding];
            if(![fm createFileAtPath:captions contents:data attributes:@{NSFilePosixPermissions:@0600}]) {
                [fm removeItemAtPath:temporary error:nil];return record_jobs_reject(@"Could not prepare export captions");
            }
            [arguments addObjectsFromArray:@[@"--captions",captions]];
        }
    }
    if(!g_record_jobs)g_record_jobs=[[NSMutableArray alloc] init];
    while(g_record_jobs.count>=32) {
        NSUInteger old=[g_record_jobs indexOfObjectPassingTest:^BOOL(NSDictionary *job,NSUInteger index,BOOL *stop){(void)index;(void)stop;return ![job[@"state"] isEqual:@"running"];}];
        if(old==NSNotFound)break;[g_record_jobs removeObjectAtIndex:old];
    }
    HCRecordingJob *runner=[[[HCRecordingJob alloc] init] autorelease];runner.replaceExisting=NO;
    NSMutableDictionary *job=[NSMutableDictionary dictionaryWithDictionary:@{@"id":identifier,@"bundle":bundle,@"title":record_jobs_title(bundle,manifest),@"kind":kind,@"state":@"running",@"progress":@(-1),@"message":zip?@"Creating portable ZIP…":@"Preparing MP4…",@"output":destination,@"runner":runner}];
    [g_record_jobs addObject:job];record_jobs_timer_sync();record_jobs_notify();record_status(job[@"message"]);
    return [runner start:tool arguments:arguments staged:staged destination:destination temporary:temporary completion:^(BOOL success,NSString *message){
        job[@"state"]=success?@"done":@"failed";if(success)job[@"progress"]=@1;
        job[@"message"]=record_jobs_short_message(message);[job removeObjectForKey:@"runner"];
        record_jobs_timer_sync();record_jobs_notify();record_status(job[@"message"]);
    }];
}
#endif
