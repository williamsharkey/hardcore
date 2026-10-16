/* Saved recordings and the native host for the portable replay editor.
 * Included after hardcore_recording.m. The bridge accepts only the local main
 * document, validates every argument, and invokes fixed executables without a shell. */
#import <WebKit/WebKit.h>
static void mac_record_library(void);
static NSMutableSet *g_record_editors;
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
static NSTextField *record_library_label(NSRect rect,NSString *text) {
    NSTextField *label=[[[NSTextField alloc] initWithFrame:rect] autorelease];
    label.stringValue=text ?: @"";label.editable=NO;label.selectable=NO;label.bordered=NO;label.drawsBackground=NO;
    label.font=[NSFont systemFontOfSize:12];return label;
}
static NSButton *record_library_button(NSRect rect,NSString *title,id target,SEL action) {
    NSButton *button=[[[NSButton alloc] initWithFrame:rect] autorelease];button.title=title;button.bezelStyle=NSBezelStyleRounded;
    button.target=target;button.action=action;return button;
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
}
@property(readonly) NSString *log;
- (BOOL)start:(NSString *)tool arguments:(NSArray *)arguments staged:(NSString *)staged destination:(NSString *)destination temporary:(NSString *)temporary completion:(void (^)(BOOL,NSString *))completion;
@end
@implementation HCRecordingJob
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
    _task=[[NSTask alloc] init];_task.executableURL=[NSURL fileURLWithPath:tool];_task.arguments=arguments;
    _task.standardOutput=output;_task.standardError=output;_task.standardInput=[NSFileHandle fileHandleWithNullDevice];
    _task.terminationHandler=^(NSTask *task){
        int status=task.terminationStatus;
        dispatch_async(dispatch_get_main_queue(),^{
            NSError *error=nil;BOOL ok=status==0;
            if(ok) {
                if([fm fileExistsAtPath:_destination])ok=[fm replaceItemAtURL:[NSURL fileURLWithPath:_destination] withItemAtURL:[NSURL fileURLWithPath:_staged] backupItemName:nil options:0 resultingItemURL:NULL error:&error];
                else ok=[fm moveItemAtPath:_staged toPath:_destination error:&error];
            }
            NSString *message=ok?[NSString stringWithFormat:@"Saved %@",_destination.lastPathComponent]:error.localizedDescription;
            if(!message.length)message=record_library_log(_log);
            if(!message.length)message=[NSString stringWithFormat:@"Export stopped with status %d",status];
            if(_completion)_completion(ok,message);
            [fm removeItemAtPath:_temporary error:nil];[fm removeItemAtPath:_staged error:nil];
            _task.terminationHandler=nil;
        });
    };
    NSError *error=nil;
    if(![_task launchAndReturnError:&error]) {
        _task.terminationHandler=nil;[output closeFile];
        if(_completion)_completion(NO,error.localizedDescription);
        [fm removeItemAtPath:_temporary error:nil];[fm removeItemAtPath:_staged error:nil];return NO;
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
@interface HCRecordingEditor : NSWindowController <NSWindowDelegate,WKNavigationDelegate,WKUIDelegate,WKScriptMessageHandler> {
    NSString *_bundle;
    WKWebView *_web;
    NSTextField *_status;
    NSProgressIndicator *_progress;
    HCRecordingJob *_job;
    NSTimer *_timer;
    BOOL _closed;
    double _durationMs;
}
- (instancetype)initWithBundle:(NSString *)bundle;
- (void)exportEdits:(NSDictionary *)edits;
- (void)updateProgress:(NSTimer *)timer;
@end
static void record_library_open(NSString *path) {
    BOOL directory=NO;
    if(![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] || !directory ||
       ![NSFileManager.defaultManager fileExistsAtPath:[path stringByAppendingPathComponent:@"index.html"]]) {
        record_status(@"This recording has no portable player");return;
    }
    if(!g_record_editors)g_record_editors=[[NSMutableSet alloc] init];
    HCRecordingEditor *editor=[[[HCRecordingEditor alloc] initWithBundle:path] autorelease];
    [g_record_editors addObject:editor];[editor showWindow:nil];[editor.window makeKeyAndOrderFront:nil];
}
@implementation HCRecordingEditor
- (instancetype)initWithBundle:(NSString *)bundle {
    NSWindow *window=[[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,1120,780) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO] autorelease];
    if(!(self=[super initWithWindow:window]))return nil;
    _bundle=[bundle.stringByStandardizingPath.stringByResolvingSymlinksInPath copy];
    NSData *manifestData=[NSData dataWithContentsOfFile:[_bundle stringByAppendingPathComponent:@"manifest.json"]];
    NSDictionary *manifest=manifestData.length<1024*1024 && manifestData?[NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nil]:nil;
    if([manifest isKindOfClass:NSDictionary.class] && record_library_number(manifest[@"durationMs"]))_durationMs=[manifest[@"durationMs"] doubleValue];
    window.title=[@"Recording · " stringByAppendingString:bundle.lastPathComponent];window.delegate=self;window.releasedWhenClosed=NO;window.minSize=NSMakeSize(720,500);[window center];
    WKWebViewConfiguration *config=[[[WKWebViewConfiguration alloc] init] autorelease];
    config.websiteDataStore=WKWebsiteDataStore.nonPersistentDataStore;
    [config.userContentController addScriptMessageHandler:self name:@"hardcoreRecording"];
    _web=[[WKWebView alloc] initWithFrame:NSMakeRect(0,32,1120,748) configuration:config];
    _web.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;_web.navigationDelegate=self;_web.UIDelegate=self;[window.contentView addSubview:_web];
    _status=[record_library_label(NSMakeRect(14,7,1050,18),@"Trim, crop, add captions, then export a video or save your edits.") retain];
    _status.autoresizingMask=NSViewWidthSizable;[window.contentView addSubview:_status];
    _progress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(1086,8,16,16)];_progress.style=NSProgressIndicatorStyleSpinning;_progress.displayedWhenStopped=NO;_progress.autoresizingMask=NSViewMinXMargin;[window.contentView addSubview:_progress];
    /* Local replay assets are self-contained. Blocking network subresources
       also prevents remote scripts from borrowing the local bridge origin. */
    NSString *rules=@"[{\"trigger\":{\"url-filter\":\"^https?://\"},\"action\":{\"type\":\"block\"}}]";
    [WKContentRuleListStore.defaultStore compileContentRuleListForIdentifier:@"HardcoreRecordingOffline" encodedContentRuleList:rules completionHandler:^(WKContentRuleList *list,NSError *error){
        if(_closed)return;
        if(!list){_status.stringValue=error.localizedDescription ?: @"Could not prepare the offline player";return;}
        [_web.configuration.userContentController addContentRuleList:list];
        [_web loadFileURL:[NSURL fileURLWithPath:[_bundle stringByAppendingPathComponent:@"index.html"]] allowingReadAccessToURL:[NSURL fileURLWithPath:_bundle isDirectory:YES]];
    }];
    return self;
}
- (void)dealloc {
    [_timer invalidate];[_timer release];[_web.configuration.userContentController removeScriptMessageHandlerForName:@"hardcoreRecording"];
    [_web release];[_bundle release];[_status release];[_progress release];[_job release];[super dealloc];
}
- (void)windowWillClose:(NSNotification *)notification {
    (void)notification;_closed=YES;[_web.configuration.userContentController removeScriptMessageHandlerForName:@"hardcoreRecording"];
    _web.navigationDelegate=nil;_web.UIDelegate=nil;[_web stopLoading];
    [[self retain] autorelease];if(!_job)[g_record_editors removeObject:self];
}
- (void)webView:(WKWebView *)web decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)web;
    NSURL *url=action.request.URL;
    BOOL local=action.targetFrame.isMainFrame && record_library_local_document(url,_bundle);
    if(!local && ([url.scheme.lowercaseString isEqual:@"https"] || [url.scheme.lowercaseString isEqual:@"http"]) && action.navigationType==WKNavigationTypeLinkActivated)
        [NSWorkspace.sharedWorkspace openURL:url];
    handler(local?WKNavigationActionPolicyAllow:WKNavigationActionPolicyCancel);
}
- (WKWebView *)webView:(WKWebView *)web createWebViewWithConfiguration:(WKWebViewConfiguration *)config forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    (void)web;(void)config;(void)features;
    NSURL *url=action.request.URL;
    if(action.navigationType==WKNavigationTypeLinkActivated && ([url.scheme.lowercaseString isEqual:@"https"] || [url.scheme.lowercaseString isEqual:@"http"]))[NSWorkspace.sharedWorkspace openURL:url];
    return nil;
}
- (void)webView:(WKWebView *)web didFinishNavigation:(WKNavigation *)navigation {
    (void)navigation;
    if(!record_library_local_document(web.URL,_bundle))return;
    NSData *data=[NSData dataWithContentsOfFile:[_bundle stringByAppendingPathComponent:@"edits.json"]];
    if(data.length>4*1024*1024)return;
    id value=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
    NSString *why=nil;NSDictionary *edits=value?record_library_edits(value,&why):nil;
    if(!edits)return;
    NSData *json=[NSJSONSerialization dataWithJSONObject:edits options:0 error:nil];
    NSString *source=[[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding] autorelease];
    [web evaluateJavaScript:[NSString stringWithFormat:@"window.hardcoreRecordingPlayer && window.hardcoreRecordingPlayer.applyEdits(%@)",source] completionHandler:nil];
}
- (void)webView:(WKWebView *)web didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)web;(void)navigation;_status.stringValue=error.localizedDescription;
}
- (void)webView:(WKWebView *)web runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSArray<NSURL *> *))completion {
    (void)web;
    if(!frame.isMainFrame || !record_library_local_document(frame.request.URL,_bundle)){completion(nil);return;}
    NSOpenPanel *panel=[NSOpenPanel openPanel];panel.canChooseFiles=YES;panel.canChooseDirectories=NO;
    panel.allowsMultipleSelection=parameters.allowsMultipleSelection;panel.allowedFileTypes=@[@"srt",@"vtt"];panel.prompt=@"Add captions";
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response){completion(response==NSModalResponseOK?panel.URLs:nil);}];
}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller;
    if(_closed || !message.frameInfo.isMainFrame || !record_library_local_document(message.frameInfo.request.URL,_bundle) || !record_library_local_document(_web.URL,_bundle))return;
    id body=message.body;if(![body isKindOfClass:NSDictionary.class])return;
    NSString *action=body[@"action"];
    if([action isEqual:@"library"]){mac_record_library();return;}
    if([action isEqual:@"open"]) {
        NSOpenPanel *panel=[NSOpenPanel openPanel];panel.canChooseDirectories=YES;panel.canChooseFiles=NO;panel.allowsMultipleSelection=NO;panel.prompt=@"Open recording";panel.directoryURL=[NSURL fileURLWithPath:record_root()];
        [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response){if(response==NSModalResponseOK)record_library_open(panel.URL.path);}];return;
    }
    if(![action isEqual:@"export"] && ![action isEqual:@"saveEdits"])return;
    NSString *why=nil;NSDictionary *edits=record_library_edits(body,&why);
    if(!edits){_status.stringValue=why;return;}
    if(_durationMs>0 && [edits[@"endMs"] doubleValue]>_durationMs+.001){_status.stringValue=@"The clip end exceeds this recording's duration";return;}
    if([action isEqual:@"saveEdits"]) {
        NSError *error=nil;NSString *path=[_bundle stringByAppendingPathComponent:@"edits.json"];
        NSData *data=[NSJSONSerialization dataWithJSONObject:edits options:NSJSONWritingPrettyPrinted error:&error];
        BOOL ok=data && [data writeToFile:path options:NSDataWritingAtomic error:&error];
        if(ok) {
            [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:nil];
            NSString *json=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
            json=[[json stringByReplacingOccurrencesOfString:@"\u2028" withString:@"\\u2028"] stringByReplacingOccurrencesOfString:@"\u2029" withString:@"\\u2029"];
            NSString *script=[NSString stringWithFormat:@"window.HARDCORE_EDITS = %@;\n",json];
            NSString *scriptPath=[_bundle stringByAppendingPathComponent:@"edits.js"];
            ok=[script writeToFile:scriptPath atomically:YES encoding:NSUTF8StringEncoding error:&error];
            if(ok)[NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:scriptPath error:nil];
        }
        _status.stringValue=ok?@"Saved edits to this recording":(error.localizedDescription ?: @"Could not save recording edits");return;
    }
    [self exportEdits:edits];
}
- (void)updateProgress:(NSTimer *)timer {
    (void)timer;
    NSString *log=_job.log;
    NSArray *lines=[log componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
    for(NSString *line in lines.reverseObjectEnumerator)if([line hasPrefix:@"hardcore export:"]){_status.stringValue=line;break;}
}
- (void)exportEdits:(NSDictionary *)edits {
    if(_job){_status.stringValue=@"A video export is already running";return;}
    NSString *tool=record_resource(@"recording-export");
    if(![NSFileManager.defaultManager isExecutableFileAtPath:tool])tool=record_resource(@"build/recording-export");
    if(![NSFileManager.defaultManager isExecutableFileAtPath:tool]){_status.stringValue=@"The video exporter is missing. Rebuild or reinstall Hardcore.";return;}
    NSSavePanel *panel=[NSSavePanel savePanel];panel.allowedFileTypes=@[@"mp4"];panel.nameFieldStringValue=[[_bundle.lastPathComponent stringByDeletingPathExtension] stringByAppendingPathExtension:@"mp4"];
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response){
        if(response!=NSModalResponseOK || _job)return;
        NSString *temp=record_library_temporary();if(!temp){_status.stringValue=@"Cannot create temporary export files";return;}
        NSString *destination=panel.URL.path,*staged=record_library_stage(destination);
        NSMutableArray *args=[NSMutableArray arrayWithObjects:_bundle,staged,@"--start",[NSString stringWithFormat:@"%.6f",[edits[@"startMs"] doubleValue]/1000],@"--end",[NSString stringWithFormat:@"%.6f",[edits[@"endMs"] doubleValue]/1000],nil];
        if(edits[@"crop"]!=NSNull.null)[args addObjectsFromArray:@[@"--crop",[edits[@"crop"] componentsJoinedByString:@","]]];
        if(![edits[@"pointer"] boolValue])[args addObject:@"--no-pointer"];
        if(![edits[@"captions"] boolValue])[args addObject:@"--no-captions"];
        NSString *srt=[temp stringByAppendingPathComponent:@"captions.srt"];
        NSError *error=nil;
        if(![record_library_srt(edits[@"captionItems"]) writeToFile:srt atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
            _status.stringValue=error.localizedDescription;[NSFileManager.defaultManager removeItemAtPath:temp error:nil];return;
        }
        [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:srt error:nil];
        [args addObjectsFromArray:@[@"--captions",srt]];
        _job=[[HCRecordingJob alloc] init];_status.stringValue=@"Exporting MP4…";[_progress startAnimation:nil];
        _timer=[[NSTimer scheduledTimerWithTimeInterval:.5 target:self selector:@selector(updateProgress:) userInfo:nil repeats:YES] retain];
        [_job start:tool arguments:args staged:staged destination:destination temporary:temp completion:^(BOOL ok,NSString *message){
            [_timer invalidate];[_timer release];_timer=nil;[_progress stopAnimation:nil];_status.stringValue=message;
            record_status(ok?[@"Exported " stringByAppendingString:destination.lastPathComponent]:@"Video export failed");
            if(!ok && !_closed){NSAlert *alert=[[[NSAlert alloc] init] autorelease];alert.messageText=@"Video export failed";alert.informativeText=message;[alert beginSheetModalForWindow:self.window completionHandler:nil];}
            [_job autorelease];_job=nil;
            if(_closed)[g_record_editors removeObject:self];
        }];
    }];
}
@end

@interface HCRecordingLibrary : NSWindowController <NSTableViewDataSource,NSTableViewDelegate> {
    NSTableView *_table;
    NSArray *_items;
    NSTextField *_status;
    NSProgressIndicator *_progress;
    HCRecordingJob *_job;
}
- (void)refresh:(id)sender;
- (void)openSelected:(id)sender;
- (void)shareSelected:(id)sender;
- (void)revealSelected:(id)sender;
@end
@implementation HCRecordingLibrary
- (instancetype)init {
    NSWindow *window=[[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,840,480) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO] autorelease];
    if(!(self=[super initWithWindow:window]))return nil;
    window.title=@"Hardcore recordings";window.releasedWhenClosed=NO;window.minSize=NSMakeSize(640,300);[window center];
    NSScrollView *scroll=[[[NSScrollView alloc] initWithFrame:NSMakeRect(16,78,808,386)] autorelease];scroll.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;scroll.hasVerticalScroller=YES;scroll.borderType=NSBezelBorder;
    _table=[[NSTableView alloc] initWithFrame:scroll.bounds];_table.delegate=self;_table.dataSource=self;_table.rowHeight=28;_table.target=self;_table.doubleAction=@selector(openSelected:);_table.allowsMultipleSelection=NO;
    for(NSArray *definition in @[@[@"title",@"Recording",@440],@[@"date",@"Created",@235],@[@"duration",@"Length",@90]]) {
        NSTableColumn *column=[[[NSTableColumn alloc] initWithIdentifier:definition[0]] autorelease];column.title=definition[1];column.width=[definition[2] doubleValue];[_table addTableColumn:column];
    }
    _table.columnAutoresizingStyle=NSTableViewFirstColumnOnlyAutoresizingStyle;scroll.documentView=_table;[window.contentView addSubview:scroll];
    [window.contentView addSubview:record_library_button(NSMakeRect(14,40,122,30),@"Replay / edit",self,@selector(openSelected:))];
    [window.contentView addSubview:record_library_button(NSMakeRect(140,40,112,30),@"Export MP4",self,@selector(openSelected:))];
    [window.contentView addSubview:record_library_button(NSMakeRect(256,40,104,30),@"Share ZIP",self,@selector(shareSelected:))];
    [window.contentView addSubview:record_library_button(NSMakeRect(364,40,92,30),@"Reveal",self,@selector(revealSelected:))];
    NSButton *refresh=record_library_button(NSMakeRect(726,40,100,30),@"Refresh",self,@selector(refresh:));refresh.autoresizingMask=NSViewMinXMargin;[window.contentView addSubview:refresh];
    _status=[record_library_label(NSMakeRect(18,14,770,18),@"") retain];_status.autoresizingMask=NSViewWidthSizable;[window.contentView addSubview:_status];
    _progress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(802,14,16,16)];_progress.style=NSProgressIndicatorStyleSpinning;_progress.displayedWhenStopped=NO;_progress.autoresizingMask=NSViewMinXMargin;[window.contentView addSubview:_progress];
    [self refresh:nil];return self;
}
- (void)dealloc {[_table release];[_items release];[_status release];[_progress release];[_job release];[super dealloc];}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {(void)table;return _items.count;}
- (id)tableView:(NSTableView *)table objectValueForTableColumn:(NSTableColumn *)column row:(NSInteger)row {(void)table;return row>=0 && (NSUInteger)row<_items.count?_items[row][column.identifier]:@"";}
- (NSDictionary *)selected {
    NSInteger row=_table.selectedRow;
    if(row<0 || (NSUInteger)row>=_items.count){_status.stringValue=@"Select a recording first";return nil;}
    return _items[row];
}
- (void)refresh:(id)sender {
    (void)sender;NSMutableArray *items=[NSMutableArray array];
    NSFileManager *fm=NSFileManager.defaultManager;
    for(NSString *name in [fm contentsOfDirectoryAtPath:record_root() error:nil]) {
        if(![name.pathExtension isEqual:@"hcrec"] || [name hasSuffix:@".partial.hcrec"])continue;
        NSString *path=[record_root() stringByAppendingPathComponent:name];
        NSData *data=[NSData dataWithContentsOfFile:[path stringByAppendingPathComponent:@"manifest.json"]];
        if(data.length>1024*1024)continue;
        NSDictionary *meta=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
        if(![meta isKindOfClass:NSDictionary.class])continue;
        NSString *title=[meta[@"title"] isKindOfClass:NSString.class]?meta[@"title"]:name.stringByDeletingPathExtension;
        NSString *date=[meta[@"createdAt"] isKindOfClass:NSString.class]?meta[@"createdAt"]:@"";
        double duration=record_library_number(meta[@"durationMs"])?fmax(0,[meta[@"durationMs"] doubleValue]/1000):0;
        [items addObject:@{@"path":path,@"title":title,@"date":date,@"duration":[NSString stringWithFormat:@"%d:%02d",(int)duration/60,(int)duration%60]}];
    }
    [items sortUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b){return [b[@"date"] compare:a[@"date"]];}];
    [_items release];_items=[items copy];[_table reloadData];
    if(_items.count)[_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    if(!_job)_status.stringValue=_items.count?[NSString stringWithFormat:@"%lu recordings · %@",(unsigned long)_items.count,record_root()]:@"No saved recordings yet. Start a recording from the camera panel.";
}
- (void)openSelected:(id)sender {(void)sender;NSDictionary *item=[self selected];if(item)record_library_open(item[@"path"]);}
- (void)revealSelected:(id)sender {(void)sender;NSDictionary *item=[self selected];if(item)[NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:item[@"path"]]]];}
- (void)shareSelected:(id)sender {
    (void)sender;if(_job){_status.stringValue=@"A ZIP export is already running";return;}
    NSDictionary *item=[self selected];if(!item)return;
    NSSavePanel *panel=[NSSavePanel savePanel];panel.allowedFileTypes=@[@"zip"];panel.nameFieldStringValue=[[[item[@"path"] lastPathComponent] stringByDeletingPathExtension] stringByAppendingPathExtension:@"zip"];
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response){
        if(response!=NSModalResponseOK || _job)return;
        NSString *temp=record_library_temporary();if(!temp){_status.stringValue=@"Cannot prepare the ZIP export";return;}
        NSString *destination=panel.URL.path,*staged=record_library_stage(destination);
        _job=[[HCRecordingJob alloc] init];_status.stringValue=@"Creating portable ZIP…";[_progress startAnimation:nil];
        [_job start:@"/usr/bin/ditto" arguments:@[@"-c",@"-k",@"--sequesterRsrc",@"--keepParent",item[@"path"],staged] staged:staged destination:destination temporary:temp completion:^(BOOL ok,NSString *message){
            (void)ok;[_progress stopAnimation:nil];_status.stringValue=message;[_job autorelease];_job=nil;
        }];
    }];
}
@end
static HCRecordingLibrary *g_record_library;
static void mac_record_library(void) {
    if(!g_record_library)g_record_library=[[HCRecordingLibrary alloc] init];
    [g_record_library refresh:nil];[g_record_library showWindow:nil];[g_record_library.window makeKeyAndOrderFront:nil];
}
