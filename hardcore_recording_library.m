/* Clips replay uses Hardcore's existing tab/view lifecycle. The job manager
 * (included before this file) owns exports independently of the editor. */
#include "hardcore_recording_view_geometry.h"
static void mac_record_library(void);
static void mac_record_file_viewer(NSString *path);
static void record_library_open(NSString *bundle);
static void record_library_open_output(NSString *path);
/* Refresh only executable player assets. Replay/video/edit data are untouched;
   prepare every replacement first and roll back assets on a write failure. */
static BOOL record_library_refresh_assets(NSString *bundle) {
    NSFileManager *fm=NSFileManager.defaultManager;
    NSMutableDictionary *replacements=[NSMutableDictionary dictionary],*originals=[NSMutableDictionary dictionary];
    for(NSString *name in @[@"index.html",@"core.js",@"player.js",@"style.css"]) {
        NSString *path=[bundle stringByAppendingPathComponent:name];
        if(![path.stringByResolvingSymlinksInPath hasPrefix:[bundle stringByAppendingString:@"/"]]){record_status(@"This clip has a player asset outside its folder");return NO;}
        NSData *replacement=[NSData dataWithContentsOfFile:record_resource([@"recording/" stringByAppendingString:name])];
        if(!replacement || replacement.length>4*1024*1024){record_status(@"The Clips player assets are missing. Rebuild or reinstall Hardcore.");return NO;}
        NSData *original=[NSData dataWithContentsOfFile:path];
        if(original.length>4*1024*1024){record_status(@"This clip has an oversized player asset");return NO;}
        if([original isEqual:replacement])continue;
        replacements[name]=replacement;originals[name]=original ?: (id)NSNull.null;
    }
    NSMutableArray *written=[NSMutableArray array];
    for(NSString *name in replacements) {
        NSString *path=[bundle stringByAppendingPathComponent:name];NSError *error=nil;
        if(![replacements[name] writeToFile:path options:NSDataWritingAtomic error:&error]) {
            for(NSString *previous in written) {
                NSString *restore=[bundle stringByAppendingPathComponent:previous];id original=originals[previous];
                if(original==NSNull.null)[fm removeItemAtPath:restore error:nil];
                else [original writeToFile:restore options:NSDataWritingAtomic error:nil];
            }
            record_status(error.localizedDescription ?: @"Could not update this clip's replay controls");return NO;
        }
        [fm setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:nil];[written addObject:name];
    }
    return YES;
}

@interface HCRecordingBrowser : HCBrowser {
@public
    NSString *recordPath;
    BOOL recordMovie,recordClosed,recordReady;
    double recordDuration;
    NSTimer *recordTimer;
    NSDictionary *recordLastStatus,*recordLastTheme;
}
- (BOOL)allowsDocument:(NSURL *)url;
- (void)hostStatus:(NSDictionary *)status;
- (void)pollJobs:(NSTimer *)timer;
@end

static NSDictionary *record_browser_theme(void) {
    return @{ @"bg":[NSString stringWithFormat:@"#%06x",g_look.bg & 0xffffff],
        @"surface":[NSString stringWithFormat:@"#%06x",g_look.bar & 0xffffff],
        @"line":[NSString stringWithFormat:@"#%06x",g_look.hover & 0xffffff],
        @"text":[NSString stringWithFormat:@"#%06x",g_look.fg & 0xffffff],
        @"muted":[NSString stringWithFormat:@"#%06x",g_look.dim & 0xffffff],
        @"accent":[NSString stringWithFormat:@"#%06x",g_look.accent & 0xffffff] };
}
static NSString *record_browser_json(id value) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:nil];
    NSString *json=data?[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease]:@"null";
    return [[json stringByReplacingOccurrencesOfString:@"\u2028" withString:@"\\u2028"] stringByReplacingOccurrencesOfString:@"\u2029" withString:@"\\u2029"];
}

@implementation HCRecordingBrowser
- (void)prepareNative {
    Tab *tb=tab_by_id(tabID);if(!tb || tb->browser!=self || recordClosed)return;
    NSRect rect=[self contentRect:[g_browser_root bounds]];
    if(inspectOpen){CGFloat scale=[g_win backingScaleFactor];CGFloat pane=(tb->t.rows-mac_browser_page_rows(tb))*CH/scale;rect.origin.y+=pane;rect.size.height=MAX(1,rect.size.height-pane);}
    if(!NSEqualRects(web.frame,rect))mac_browser_resize(tb);
}
- (NSRect)contentRect:(NSRect)bounds {
    UI_FONT;int title=mac_title_height();UI_DONE;
    return hc_recording_content_rect(bounds,[g_win backingScaleFactor],title,sidebar_w(),cell_w(g_font),g_tab_underlay==1,[g_browser_root isFlipped]);
}
- (WKWebViewConfiguration *)configuration {
    WKWebViewConfiguration *config=[super configuration];
    config.websiteDataStore=WKWebsiteDataStore.nonPersistentDataStore;
    [config.userContentController addScriptMessageHandler:self name:@"hardcoreRecording"];
    return config;
}
- (id)initWithTab:(Tab *)tb {
    if(!(self=[super initWithTab:tb]))return nil;
    NSURL *url=[NSURL URLWithString:[NSString stringWithUTF8String:tb->browser_url]];
    recordMovie=[url.path.pathExtension.lowercaseString isEqual:@"mp4"];
    tb->browser_recording=recordMovie?2:1;
    recordPath=[(recordMovie?url.path:url.path.stringByDeletingLastPathComponent).stringByStandardizingPath.stringByResolvingSymlinksInPath copy];
    if(recordMovie) {
        /* Media documents need no privileged recording actions. */
        [web.configuration.userContentController removeScriptMessageHandlerForName:@"hardcoreRecording"];
    } else {
        NSDictionary *attributes=[NSFileManager.defaultManager attributesOfItemAtPath:[recordPath stringByAppendingPathComponent:@"manifest.json"] error:nil];
        NSData *data=[attributes fileSize]<=1024*1024?[NSData dataWithContentsOfFile:[recordPath stringByAppendingPathComponent:@"manifest.json"]]:nil;
        NSDictionary *manifest=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
        if([manifest isKindOfClass:NSDictionary.class] && record_library_number(manifest[@"durationMs"]))recordDuration=[manifest[@"durationMs"] doubleValue];
    }
    return self;
}
- (BOOL)allowsDocument:(NSURL *)url {
    if(!recordMovie)return record_library_local_document(url,recordPath);
    return url.isFileURL && [url.path.stringByStandardizingPath.stringByResolvingSymlinksInPath isEqualToString:recordPath];
}
- (void)load:(NSString *)url {
    NSURL *destination=[NSURL URLWithString:url];
    if(recordClosed || ![self allowsDocument:destination])return;
    recordReady=NO;
    /* No network script, iframe, or popup can borrow the local document bridge.
       Wait for the content blocker before loading any bundle assets. */
    NSString *rules=@"[{\"trigger\":{\"url-filter\":\"^https?://\"},\"action\":{\"type\":\"block\"}}]";
    [WKContentRuleListStore.defaultStore compileContentRuleListForIdentifier:@"HardcoreRecordingOffline" encodedContentRuleList:rules completionHandler:^(WKContentRuleList *list,NSError *error){
        if(recordClosed)return;
        Tab *tb=tab_by_id(tabID);if(!tb || tb->browser!=self)return;
        if(!list){tb->busy=0;record_status(error.localizedDescription ?: @"Could not prepare the Clips player");update_busy(0);plat_invalidate();return;}
        [web.configuration.userContentController addContentRuleList:list];
        [super load:url];
    }];
}
- (void)dispose {
    if(recordClosed)return;
    recordClosed=YES;[recordTimer invalidate];[recordTimer release];recordTimer=nil;
    [web.configuration.userContentController removeScriptMessageHandlerForName:@"hardcoreRecording"];
    [super dispose];
}
- (void)dealloc {
    [recordTimer invalidate];[recordTimer release];[recordPath release];[recordLastStatus release];[recordLastTheme release];[super dealloc];
}
- (void)hostStatus:(NSDictionary *)status {
    if(recordClosed || !recordReady || recordMovie || ![self allowsDocument:web.URL])return;
    [web evaluateJavaScript:[NSString stringWithFormat:@"window.hardcoreRecordingPlayer?.hostStatus(%@)",record_browser_json(status)] completionHandler:nil];
}
- (void)accept:(NSDictionary *)snapshot {
    [super accept:snapshot];
    Tab *tb=tab_by_id(tabID);if(!tb || tb->browser!=self)return;
    NSString *title=[snapshot[@"title"] isKindOfClass:NSString.class]?snapshot[@"title"]:recordPath.lastPathComponent;
    mac_browser_metadata(tb,title,recordMovie?@"Video":@"Clips",nil);
}
- (void)pollJobs:(NSTimer *)timer {
    (void)timer;if(recordClosed || recordMovie)return;
    NSDictionary *latest=nil;BOOL exporting=NO;
    for(NSDictionary *job in mac_record_jobs())if([job[@"bundle"] isEqual:recordPath]){latest=job;if([job[@"state"] isEqual:@"running"])exporting=YES;}
    Tab *tb=tab_by_id(tabID);BOOL busy=web.loading || exporting;
    if(tb && tb->browser==self && tb->busy!=busy){tb->busy=busy;update_busy(0);plat_invalidate();}
    if(latest && ![latest isEqual:recordLastStatus]) {
        [recordLastStatus release];recordLastStatus=[latest copy];[self hostStatus:latest];
    }
    /* Theme changes follow the surrounding Hardcore controls. */
    NSDictionary *theme=record_browser_theme();
    if(recordReady && ![theme isEqual:recordLastTheme]) {
        [recordLastTheme release];recordLastTheme=[theme copy];
        [web evaluateJavaScript:[NSString stringWithFormat:@"window.hardcoreRecordingPlayer?.hostTheme(%@)",record_browser_json(theme)] completionHandler:nil];
    }
}
- (void)webView:(WKWebView *)view decidePolicyForNavigationAction:(WKNavigationAction *)action decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)view;
    handler(!recordClosed && action.targetFrame.isMainFrame && [self allowsDocument:action.request.URL]?WKNavigationActionPolicyAllow:WKNavigationActionPolicyCancel);
}
- (WKWebView *)webView:(WKWebView *)view createWebViewWithConfiguration:(WKWebViewConfiguration *)config forNavigationAction:(WKNavigationAction *)action windowFeatures:(WKWindowFeatures *)features {
    (void)view;(void)config;(void)action;(void)features;return nil;
}
- (void)webView:(WKWebView *)view runJavaScriptAlertPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completion {
    (void)view;(void)message;(void)frame;completion();
}
- (void)webView:(WKWebView *)view runJavaScriptConfirmPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))completion {
    (void)view;(void)message;(void)frame;completion(NO);
}
- (void)webView:(WKWebView *)view runOpenPanelWithParameters:(WKOpenPanelParameters *)parameters initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSArray<NSURL *> *))completion {
    (void)view;(void)parameters;(void)frame;completion(nil);
}
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation {
    [super webView:view didFinishNavigation:navigation];
    if(recordClosed || ![self allowsDocument:view.URL])return;
    recordReady=YES;
    Tab *tb=tab_by_id(tabID);
    if(tb)mac_browser_metadata(tb,[recordPath.lastPathComponent stringByDeletingPathExtension],recordMovie?@"Video":@"Clips",nil);
    if(recordMovie)return;
    NSDictionary *attributes=[NSFileManager.defaultManager attributesOfItemAtPath:[recordPath stringByAppendingPathComponent:@"edits.json"] error:nil];
    NSData *data=[attributes fileSize]<=4*1024*1024?[NSData dataWithContentsOfFile:[recordPath stringByAppendingPathComponent:@"edits.json"]]:nil;
    id value=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
    NSString *why=nil;NSDictionary *edits=value?record_library_edits(value,&why):nil;
    if(edits && (!recordDuration || [edits[@"endMs"] doubleValue]<=recordDuration+.001))
        [view evaluateJavaScript:[NSString stringWithFormat:@"window.hardcoreRecordingPlayer?.applyEdits(%@)",record_browser_json(edits)] completionHandler:nil];
    [recordLastStatus release];recordLastStatus=nil;[recordLastTheme release];recordLastTheme=nil;[self pollJobs:nil];
    if(!recordTimer){recordTimer=[[NSTimer timerWithTimeInterval:.5 target:self selector:@selector(pollJobs:) userInfo:nil repeats:YES] retain];[NSRunLoop.mainRunLoop addTimer:recordTimer forMode:NSRunLoopCommonModes];}
}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    if([message.name isEqual:@"hardcoreBrowser"]){[super userContentController:controller didReceiveScriptMessage:message];return;}
    if(recordClosed || recordMovie || message.webView!=web || ![message.name isEqual:@"hardcoreRecording"] || !message.frameInfo.isMainFrame ||
       ![self allowsDocument:message.frameInfo.request.URL] || ![self allowsDocument:web.URL])return;
    id body=message.body;if(![body isKindOfClass:NSDictionary.class])return;
    NSString *action=body[@"action"];
    if([action isEqual:@"library"] || [action isEqual:@"open"]){mac_record_library();return;}
    if([action isEqual:@"files"]){mac_record_file_viewer(recordPath);return;}
    if([action isEqual:@"output"]) {
        /* Accept a known job id, never a page-supplied filesystem path. */
        for(NSDictionary *job in mac_record_jobs())if([job[@"bundle"] isEqual:recordPath] && [job[@"id"] isEqual:body[@"id"]] && [job[@"state"] isEqual:@"done"]){record_library_open_output(job[@"output"]);break;}
        return;
    }
    if(![action isEqual:@"export"] && ![action isEqual:@"saveEdits"] && ![action isEqual:@"zip"])return;
    NSString *why=nil;NSDictionary *edits=record_library_edits(body,&why);
    if(!edits || (recordDuration>0 && [edits[@"endMs"] doubleValue]>recordDuration+.001)) {
        [self hostStatus:@{@"state":@"failed",@"message":why ?: @"The clip end exceeds this recording's duration"}];return;
    }
    if([action isEqual:@"saveEdits"]) {
        NSError *error=nil;NSString *path=[recordPath stringByAppendingPathComponent:@"edits.json"];
        NSData *data=[NSJSONSerialization dataWithJSONObject:edits options:NSJSONWritingPrettyPrinted error:&error];
        BOOL ok=data && [data writeToFile:path options:NSDataWritingAtomic error:&error];
        if(ok) {
            [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:nil];
            NSString *script=[NSString stringWithFormat:@"window.HARDCORE_EDITS = %@;\n",record_browser_json(edits)];
            NSString *scriptPath=[recordPath stringByAppendingPathComponent:@"edits.js"];
            ok=[script writeToFile:scriptPath atomically:YES encoding:NSUTF8StringEncoding error:&error];
            if(ok)[NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:scriptPath error:nil];
        }
        [self hostStatus:@{@"state":ok?@"saved":@"failed",@"message":ok?@"Saved edits to this clip":(error.localizedDescription ?: @"Could not save clip edits")}];return;
    }
    if(mac_record_export(recordPath,edits,[action isEqual:@"zip"]))[self pollJobs:nil];
    else [self hostStatus:@{@"state":@"failed",@"message":@"The export could not start. Check Clips for details."}];
}
@end

static HCBrowser *record_library_restore(Tab *tb) {
    if(!tb || (tb->browser_recording!=1 && tb->browser_recording!=2))return nil;
    NSURL *url=[NSURL URLWithString:[NSString stringWithUTF8String:tb->browser_url]];
    BOOL movie=tb->browser_recording==2;
    BOOL valid=url.isFileURL && [NSFileManager.defaultManager isReadableFileAtPath:url.path] &&
        (movie?[url.path.pathExtension.lowercaseString isEqual:@"mp4"]:
            ([url.lastPathComponent isEqual:@"index.html"] && record_library_local_document(url,url.path.stringByDeletingLastPathComponent)));
    if(!valid || (!movie && !record_library_refresh_assets(url.path.stringByDeletingLastPathComponent.stringByResolvingSymlinksInPath))){tb->browser_recording=0;return nil;}
    return [[HCRecordingBrowser alloc] initWithTab:tb];
}

static BOOL record_library_activate(NSString *path,BOOL movie) {
    for(int i=0;i<g_ntabs;i++) {
        HCBrowser *browser=browser_for(g_tabs[i]);
        if([browser isKindOfClass:HCRecordingBrowser.class]) {
            HCRecordingBrowser *clip=(HCRecordingBrowser *)browser;
            if(clip->recordMovie==movie && [clip->recordPath isEqual:path]){g_tabs[i]->browser_native=!mac_record_active();activate(i);return YES;}
        }
    }
    return NO;
}
static void record_library_open(NSString *bundle) {
    if(![bundle isKindOfClass:NSString.class])return;
    NSString *path=bundle.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    BOOL directory=NO;NSString *index=[path stringByAppendingPathComponent:@"index.html"];
    if(![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory] || !directory ||
       ![NSFileManager.defaultManager isReadableFileAtPath:index] || !record_library_local_document([NSURL fileURLWithPath:index],path)) {
        record_status(@"This clip has no portable replay editor");return;
    }
    if(!record_library_refresh_assets(path))return;
    if(!record_library_activate(path,NO) && !mac_browser_new_with_class([NSURL fileURLWithPath:index].absoluteString,0,1,HCRecordingBrowser.class))record_status(@"Cannot open another replay tab");
}
static void record_library_open_output(NSString *path) {
    if(![path isKindOfClass:NSString.class] || !path.length)return;
    path=path.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    BOOL directory=NO;
    if(![NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&directory]){record_status(@"This output file is no longer available");return;}
    if(directory || ![path.pathExtension.lowercaseString isEqual:@"mp4"]){mac_record_file_viewer(path);return;}
    if(!record_library_activate(path,YES) && !mac_browser_new_with_class([NSURL fileURLWithPath:path].absoluteString,0,1,HCRecordingBrowser.class))record_status(@"Cannot open another video tab");
}
