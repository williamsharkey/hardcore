/* Full installed Chrome, shared by GUI and terminal-hosted browser tabs.
   This native CDP client deliberately never reads/copies cookies or profiles.
   Its private Chrome profile persists sign-ins, history and installed extensions. */
#ifndef HARDCORE_CHROME_BROWSER_H
#define HARDCORE_CHROME_BROWSER_H
#ifdef __APPLE__
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

typedef void (^HCChromeReply)(NSDictionary *, NSError *);
static NSError *hc_chrome_error(NSString *message) {
    return [NSError errorWithDomain:@"hardcore.chrome" code:1 userInfo:@{NSLocalizedDescriptionKey:message ?: @"Chrome connection failed"}];
}
static NSString *hc_chrome_profile(void) {
    NSString *override=NSProcessInfo.processInfo.environment[@"HARDCORE_CHROME_PROFILE"];
    return (override.length ? override : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/hardcore/Chrome"]).stringByExpandingTildeInPath;
}
static NSString *hc_chrome_executable(void) {
    NSString *override=NSProcessInfo.processInfo.environment[@"HARDCORE_CHROME_EXECUTABLE"];
    if(override.length)return override.stringByExpandingTildeInPath;
    for(NSString *p in @[@"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        [NSHomeDirectory() stringByAppendingPathComponent:@"Applications/Google Chrome.app/Contents/MacOS/Google Chrome"],
        @"/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
        @"/Applications/Chromium.app/Contents/MacOS/Chromium"])
        if([NSFileManager.defaultManager isExecutableFileAtPath:p])return p;
    return nil;
}
static BOOL hc_chrome_is_regular_profile(NSString *profile) {
    NSString *path=profile.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    for(NSString *suffix in @[@"Library/Application Support/Google/Chrome",
        @"Library/Application Support/Google/Chrome Beta",@"Library/Application Support/Google/Chrome Dev",
        @"Library/Application Support/Google/Chrome Canary",@"Library/Application Support/Microsoft Edge",
        @"Library/Application Support/Microsoft Edge Beta",@"Library/Application Support/Microsoft Edge Dev",
        @"Library/Application Support/Microsoft Edge Canary",@"Library/Application Support/Chromium"]) {
        NSString *regular=[[NSHomeDirectory() stringByAppendingPathComponent:suffix] stringByResolvingSymlinksInPath];
        if([path isEqualToString:regular] || [path hasPrefix:[regular stringByAppendingString:@"/"]])return YES;
    }
    return NO;
}
/* Only the two-line endpoint file is read, never browser profile contents. */
static NSURL *hc_chrome_endpoint(NSString *path) {
    NSData *data=[NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    if(!data.length || data.length>4096)return nil;
    NSString *s=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    NSArray *lines=[s componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet];
    if(lines.count<2)return nil;
    NSString *number=[lines[0] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSString *route=[lines[1] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if(!number.length || [number rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location!=NSNotFound)return nil;
    NSInteger port=number.integerValue;
    if(port<1 || port>65535 || ![route hasPrefix:@"/devtools/browser/"] || route.length>512 ||
       [route rangeOfCharacterFromSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].location!=NSNotFound)return nil;
    return [NSURL URLWithString:[NSString stringWithFormat:@"ws://127.0.0.1:%ld%@",(long)port,route]];
}
static BOOL hc_chrome_listening(NSURL *endpoint) {
    if(!endpoint)return NO;
    int fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0)return NO;
    struct sockaddr_in addr={0};addr.sin_family=AF_INET;addr.sin_port=htons(endpoint.port.intValue);addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    fcntl(fd,F_SETFL,O_NONBLOCK);
    int result=connect(fd,(struct sockaddr *)&addr,sizeof(addr));
    if(result<0 && errno==EINPROGRESS) {
        fd_set fds;FD_ZERO(&fds);FD_SET(fd,&fds);struct timeval timeout={0,200000};
        if(select(fd+1,NULL,&fds,NULL,&timeout)>0) {int e=0;socklen_t size=sizeof(e);getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&size);result=e ? -1 : 0;}
    }
    close(fd);return result==0;
}

@class HCChromePage,HCChromeBroker;
static HCChromeBroker *hc_chrome_shared_broker;
@interface HCChromePending : NSObject {
@public HCChromeReply callback;
}
@end
@implementation HCChromePending
- (void)dealloc {[callback release];[super dealloc];}
@end

@interface HCChromeBroker : NSObject {
@public BOOL attached,headed;
    NSURLSession *session;
    NSURLSessionWebSocketTask *socket;
    NSMutableDictionary *pending,*pages,*targets;
    NSMutableArray *waiting;
    NSUInteger serial,connectionGeneration;
    BOOL connecting,connected,shuttingDown;
    NSUInteger openingCount;
    NSMutableSet *closingTargets;
    NSMutableSet *quitReplies;   /* request ids, guarded by @synchronized: drained on the socket's queue */
    NSTask *process;
}
+ (HCChromeBroker *)shared;
- (void)connect:(void (^)(NSError *))done;
- (NSNumber *)send:(NSString *)method params:(NSDictionary *)params session:(NSString *)sessionID completion:(HCChromeReply)done;
- (void)receive;
- (void)receiveOn:(NSURLSessionWebSocketTask *)task generation:(NSUInteger)generation;
- (void)fail:(NSError *)error generation:(NSUInteger)generation;
- (void)closeTarget:(NSString *)identifier completion:(void (^)(NSError *))done;
- (void)closeTarget:(NSString *)identifier session:(NSString *)pageSession completion:(void (^)(NSError *))done;
- (void)verifyClosed:(NSString *)identifier attempts:(int)attempts completion:(void (^)(NSError *))done;
- (void)quitClose:(NSString *)identifier session:(NSString *)pageSession;
@end

@interface HCChromePage : NSObject {
@public
    NSString *url,*title,*targetID;
    BOOL ready,closed,canGoBack,canGoForward;
@private
    HCChromeBroker *broker;
    NSString *sessionID,*source,*frameID;
    NSMutableDictionary *contextFrames;
    NSInteger contextID,preparedContext;
    int viewportWidth,viewportHeight;
    int sentWidth,sentHeight;
    BOOL metadataPending,ownsTarget,closeSent;
    void (^changed)(NSDictionary *);
}
- (id)initWithScript:(NSString *)script changed:(void (^)(NSDictionary *))callback;
- (void)start:(NSString *)location completion:(void (^)(NSError *))done;
- (void)adoptTarget:(NSString *)identifier completion:(void (^)(NSError *))done;
- (void)evaluate:(NSString *)js completion:(void (^)(id,NSError *))done;
- (void)command:(NSString *)method params:(NSDictionary *)params completion:(HCChromeReply)done;
- (void)navigate:(NSString *)location;
- (void)go:(int)delta;
- (void)reload;
- (void)setWidth:(int)width height:(int)height;
- (void)capture:(void (^)(NSData *,NSError *))done;
- (void)close;
- (void)closeWithCompletion:(void (^)(NSError *))done;
- (void)event:(NSString *)method params:(NSDictionary *)params;
- (void)refreshMetadata;
- (void)notify:(NSDictionary *)event;
@end

@implementation HCChromeBroker
+ (HCChromeBroker *)shared {
    static dispatch_once_t once;dispatch_once(&once,^{hc_chrome_shared_broker=[HCChromeBroker new];});return hc_chrome_shared_broker;
}
- (id)init {
    if((self=[super init])) {
        pending=[NSMutableDictionary new];pages=[NSMutableDictionary new];targets=[NSMutableDictionary new];waiting=[NSMutableArray new];
        closingTargets=[NSMutableSet new];quitReplies=[NSMutableSet new];
        NSURLSessionConfiguration *config=[NSURLSessionConfiguration ephemeralSessionConfiguration];
        config.timeoutIntervalForRequest=20;config.timeoutIntervalForResource=30;
        session=[[NSURLSession sessionWithConfiguration:config] retain];
    }return self;
}
- (void)finishConnect:(NSError *)error {
    connecting=NO;connected=error==nil;
    if(error)[socket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
    NSArray *callbacks=[[waiting copy] autorelease];[waiting removeAllObjects];
    for(HCChromePending *p in callbacks)if(p->callback)p->callback(nil,error);
}
- (void)connect:(void (^)(NSError *))done {
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{[self connect:done];});return;}
    if(shuttingDown){if(done)done(hc_chrome_error(@"Chrome browsing is shutting down."));return;}
    if(connected){if(done)done(nil);return;}
    HCChromePending *p=[HCChromePending new];p->callback=[^(NSDictionary *r,NSError *e){(void)r;if(done)done(e);} copy];[waiting addObject:p];[p release];
    if(connecting)return;connecting=YES;
    NSUInteger generation=++connectionGeneration;
    NSString *attach=NSProcessInfo.processInfo.environment[@"HARDCORE_CHROME_ATTACH"];
    attached=attach.length>0;
    NSString *profile=hc_chrome_profile();
    NSString *endpointPath=attached ? attach.stringByExpandingTildeInPath : [profile stringByAppendingPathComponent:@"DevToolsActivePort"];
    BOOL isDir=NO;if(attached && [NSFileManager.defaultManager fileExistsAtPath:endpointPath isDirectory:&isDir] && isDir)endpointPath=[endpointPath stringByAppendingPathComponent:@"DevToolsActivePort"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool {
        NSError *failure=nil;NSURL *endpoint=nil;NSTask *launched=nil;int lock=-1;
        if(!self->attached) {
            NSString *exe=hc_chrome_executable();
            if(!exe.length)failure=hc_chrome_error(@"Install Google Chrome to browse inside Hardcore.");
            if(hc_chrome_is_regular_profile(profile))failure=hc_chrome_error(@"HARDCORE_CHROME_PROFILE must be a separate profile. Use HARDCORE_CHROME_ATTACH with Chrome's explicit remote-debugging consent to attach your regular browser.");
            if(!failure && ![NSFileManager.defaultManager createDirectoryAtPath:profile withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&failure]){}
            if(!failure) {
                lock=open([[profile stringByAppendingPathComponent:@"hardcore-launch.lock"] fileSystemRepresentation],O_CREAT|O_RDWR,0600);
                if(lock<0)failure=hc_chrome_error(@"Cannot lock the Hardcore Chrome profile.");
                else {
                    NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+25;
                    while(flock(lock,LOCK_EX|LOCK_NB)<0) {
                        if(NSDate.timeIntervalSinceReferenceDate>deadline){failure=hc_chrome_error(@"Chrome profile startup timed out.");break;}
                        [NSThread sleepForTimeInterval:0.05];
                    }
                }
            }
            if(!failure) {
                endpoint=hc_chrome_endpoint(endpointPath);
                if(!hc_chrome_listening(endpoint)) {
                    launched=[NSTask new];launched.executableURL=[NSURL fileURLWithPath:exe];
                    launched.arguments=@[@"--headless=new",@"--remote-debugging-address=127.0.0.1",@"--remote-debugging-port=0",
                        [@"--user-data-dir=" stringByAppendingString:profile],@"--no-first-run",@"--no-default-browser-check",@"about:blank"];
                    launched.standardOutput=[NSFileHandle fileHandleWithNullDevice];launched.standardError=[NSFileHandle fileHandleWithNullDevice];launched.standardInput=[NSFileHandle fileHandleWithNullDevice];
                    if(![launched launchAndReturnError:&failure])endpoint=nil;
                    else {
                        NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+25;endpoint=nil;
                        while(NSDate.timeIntervalSinceReferenceDate<deadline) {
                            NSURL *candidate=hc_chrome_endpoint(endpointPath);
                            if(hc_chrome_listening(candidate)){endpoint=candidate;break;}
                            if(!launched.running){failure=hc_chrome_error(@"Chrome could not open its Hardcore profile. Close a separate Chrome setup window using that profile, then retry.");break;}
                            [NSThread sleepForTimeInterval:0.05];
                        }
                        if(!endpoint && !failure)failure=hc_chrome_error(@"Chrome did not publish a debugging endpoint in time.");
                    }
                }
            }
        } else {
            endpoint=hc_chrome_endpoint(endpointPath);
            if(!hc_chrome_listening(endpoint))failure=hc_chrome_error(@"Chrome attach is unavailable. Enable chrome://inspect/#remote-debugging in Chrome, then retry.");
        }
        if(lock>=0){flock(lock,LOCK_UN);close(lock);}
        dispatch_async(dispatch_get_main_queue(),^{
            if(generation!=self->connectionGeneration)return;
            if(failure){[self finishConnect:failure];return;}
            [self->process release];self->process=[launched retain];
            [self->socket release];self->socket=[[self->session webSocketTaskWithURL:endpoint] retain];
            self->socket.maximumMessageSize=16*1024*1024;
            [self->socket resume];[self receive];
            [self send:@"Browser.getVersion" params:@{} session:nil completion:^(NSDictionary *r,NSError *e){
                if(generation!=self->connectionGeneration)return;
                self->headed=[r[@"userAgent"] rangeOfString:@"HeadlessChrome/"].location==NSNotFound;
                [self finishConnect:e];
                if(!e)[self send:@"Target.setDiscoverTargets" params:@{@"discover":@YES} session:nil completion:nil];
            }];
        });
        [launched release];
    }});
}
- (void)fail:(NSError *)error generation:(NSUInteger)generation {
    if(generation!=connectionGeneration || (!connected && !connecting))return;
    connected=NO;[socket cancelWithCloseCode:NSURLSessionWebSocketCloseCodeGoingAway reason:nil];
    NSArray *callbacks=[[pending.allValues copy] autorelease];[pending removeAllObjects];
    for(HCChromePending *p in callbacks)if(p->callback)p->callback(nil,error);
    if(connecting)[self finishConnect:error];
    for(HCChromePage *page in [[pages.allValues copy] autorelease]){page->ready=NO;[page notify:@{@"type":@"error",@"message":error.localizedDescription ?: @"Chrome disconnected"}];}
}
- (NSNumber *)send:(NSString *)method params:(NSDictionary *)params session:(NSString *)pageSession completion:(HCChromeReply)done {
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{[self send:method params:params session:pageSession completion:done];});return nil;}
    if(!socket || (!connected && !connecting)){if(done)done(nil,hc_chrome_error(@"Chrome is not connected."));return nil;}
    if(pending.count>=512){if(done)done(nil,hc_chrome_error(@"Chrome is busy; retry the operation."));return nil;}
    NSNumber *key=@(++serial);NSMutableDictionary *message=[NSMutableDictionary dictionaryWithDictionary:@{@"id":key,@"method":method,@"params":params ?: @{}}];
    if(pageSession.length)message[@"sessionId"]=pageSession;
    NSError *error=nil;NSData *data=[NSJSONSerialization dataWithJSONObject:message options:0 error:&error];
    if(!data || data.length>8*1024*1024){if(done)done(nil,error ?: hc_chrome_error(@"Chrome request exceeds the size limit."));return nil;}
    HCChromePending *p=[HCChromePending new];p->callback=[done copy];pending[key]=p;[p release];
    NSUInteger generation=connectionGeneration;
    NSString *string=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    NSURLSessionWebSocketMessage *frame=[[[NSURLSessionWebSocketMessage alloc] initWithString:string] autorelease];
    [socket sendMessage:frame completionHandler:^(NSError *e){if(e)dispatch_async(dispatch_get_main_queue(),^{[self fail:e generation:generation];});}];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(20*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        HCChromePending *expired=[[self->pending objectForKey:key] retain];
        if(expired){[self->pending removeObjectForKey:key];if(expired->callback)expired->callback(nil,hc_chrome_error([NSString stringWithFormat:@"Chrome timed out: %@",method]));[expired release];}
    });
    return key;
}
- (void)receive {[self receiveOn:socket generation:connectionGeneration];}
/* Re-armed on the socket's own serial queue, not the main queue: quit can run
   inside a main-queue source (the test channel, a shell exiting in the last
   tab), and its close replies must still arrive. Main still sees messages in order. */
- (void)receiveOn:(NSURLSessionWebSocketTask *)task generation:(NSUInteger)generation {
    [task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message,NSError *error){
        if(!error && self->shuttingDown) {
            NSData *raw=message.type==NSURLSessionWebSocketMessageTypeData ? message.data : [message.string dataUsingEncoding:NSUTF8StringEncoding];
            id reply=raw.length<=16*1024*1024 ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
            if([reply isKindOfClass:NSDictionary.class] && reply[@"id"])@synchronized(self->quitReplies){[self->quitReplies removeObject:reply[@"id"]];}
        }
        dispatch_async(dispatch_get_main_queue(),^{@autoreleasepool {
            if(generation!=self->connectionGeneration)return;
            if(error){[self fail:error generation:generation];return;}
            NSData *data=message.type==NSURLSessionWebSocketMessageTypeData ? message.data : [message.string dataUsingEncoding:NSUTF8StringEncoding];
            id value=data.length<=16*1024*1024 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            if(![value isKindOfClass:NSDictionary.class]){[self fail:hc_chrome_error(@"Chrome returned an invalid message.") generation:generation];return;}
            NSDictionary *reply=value;NSNumber *key=reply[@"id"];
            if(key) {
                HCChromePending *p=[[self->pending objectForKey:key] retain];[self->pending removeObjectForKey:key];
                if(p){NSDictionary *failure=reply[@"error"];if(p->callback)p->callback(reply[@"result"],failure ? hc_chrome_error(failure[@"message"]) : nil);[p release];}
            } else {
                NSString *method=reply[@"method"];NSDictionary *params=reply[@"params"] ?: @{};
                HCChromePage *page=self->pages[reply[@"sessionId"] ?: @""];
                if(page)[page event:method params:params];
                if([method isEqualToString:@"Target.targetInfoChanged"]) {
                    NSDictionary *info=params[@"targetInfo"];HCChromePage *owner=self->targets[info[@"targetId"] ?: @""];
                    if(owner) {
                        if([info[@"url"] isKindOfClass:NSString.class]){[owner->url release];owner->url=[info[@"url"] copy];}
                        if([info[@"title"] isKindOfClass:NSString.class]){[owner->title release];owner->title=[info[@"title"] copy];}
                        [owner notify:@{@"type":@"changed"}];
                    }
                }
                if([method isEqualToString:@"Target.targetCreated"]) {
                    NSDictionary *info=params[@"targetInfo"];
                    HCChromePage *opener=self->targets[info[@"openerId"] ?: @""];
                    if(opener && [info[@"type"] isEqualToString:@"page"])
                        [opener notify:@{@"type":@"popup",@"targetID":info[@"targetId"] ?: @"",@"url":info[@"url"] ?: @"about:blank"}];
                }
                if([method isEqualToString:@"Target.targetDestroyed"]) {
                    HCChromePage *owner=self->targets[params[@"targetId"] ?: @""];
                    if(owner && !owner->closed){owner->ready=NO;[owner notify:@{@"type":@"error",@"message":@"This Chrome tab was closed."}];}
                }
            }
        }});
        if(!error)[self receiveOn:task generation:generation];
    }];
}
- (void)closeTarget:(NSString *)identifier completion:(void (^)(NSError *))done {
    [self closeTarget:identifier session:nil completion:done];
}
- (void)closeTarget:(NSString *)identifier session:(NSString *)pageSession completion:(void (^)(NSError *))done {
    if(shuttingDown){[self quitClose:identifier session:pageSession];if(done)done(nil);return;}
    if(identifier.length)[closingTargets addObject:identifier];
    void (^close)(void)=^{
        [self send:@"Target.closeTarget" params:@{@"targetId":identifier} session:nil completion:^(NSDictionary *r,NSError *e){
            (void)r;
            void (^finished)(void)=^{[self verifyClosed:identifier attempts:8 completion:^(NSError *failure){[self->closingTargets removeObject:identifier];if(done)done(failure ?: e);}];};
            if(pageSession.length)[self send:@"Target.detachFromTarget" params:@{@"sessionId":pageSession} session:nil completion:^(NSDictionary *d,NSError *de){(void)d;(void)de;finished();}];
            else finished();
        }];
    };
    if(pageSession.length)[self send:@"Page.stopLoading" params:@{} session:pageSession completion:^(NSDictionary *r,NSError *e){(void)r;(void)e;close();}];
    else close();
}
/* Quit sends the close itself instead of chaining it behind Page.stopLoading's
   reply, and counts the reply on the socket's queue (see receiveOn:). Holding
   the lock across send keeps a fast reply from arriving uncounted. */
- (void)quitClose:(NSString *)identifier session:(NSString *)pageSession {
    if(!identifier.length)return;
    if(pageSession.length)[self send:@"Page.stopLoading" params:@{} session:pageSession completion:nil];
    @synchronized(quitReplies) {
        NSNumber *key=[self send:@"Target.closeTarget" params:@{@"targetId":identifier} session:nil completion:nil];
        if(key)[quitReplies addObject:key];
    }
}
- (void)verifyClosed:(NSString *)identifier attempts:(int)attempts completion:(void (^)(NSError *))done {
    /* Chrome can acknowledge close during navigation before actually removing
       the target. Retry only this owned ID, with a small bounded deadline. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.05*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        [self send:@"Target.getTargetInfo" params:@{@"targetId":identifier} session:nil completion:^(NSDictionary *r,NSError *e){
            if(e){if(done)done(self->connected ? nil : e);return;}
            if(!r[@"targetInfo"]){if(done)done(nil);return;}
            if(attempts<=0){if(done)done(hc_chrome_error(@"Chrome did not finish closing its page."));return;}
            [self send:@"Target.closeTarget" params:@{@"targetId":identifier} session:nil completion:^(NSDictionary *closedResult,NSError *closeError){
                (void)closedResult;(void)closeError;[self verifyClosed:identifier attempts:attempts-1 completion:done];
            }];
        }];
    });
}
@end

@implementation HCChromePage
- (id)initWithScript:(NSString *)script changed:(void (^)(NSDictionary *))callback {
    if((self=[super init])) {
        broker=[[HCChromeBroker shared] retain];changed=[callback copy];viewportWidth=1000;viewportHeight=700;
        source=[[NSString stringWithFormat:@"globalThis.webkit={messageHandlers:{hardcoreBrowser:{postMessage:(message)=>globalThis.hardcoreChromeNotify(JSON.stringify(message))}}};\n%@",script ?: @""] copy];
        contextFrames=[NSMutableDictionary new];
        url=[@"about:blank" copy];title=[@"" copy];
    }return self;
}
- (void)dealloc {[broker release];[sessionID release];[source release];[frameID release];[contextFrames release];[changed release];[url release];[title release];[targetID release];[super dealloc];}
- (void)notify:(NSDictionary *)event {if(!closed && changed)changed(event);}
- (void)start:(NSString *)location completion:(void (^)(NSError *))done {
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{[self start:location completion:done];});return;}
    if(closed || targetID || broker->shuttingDown){if(done)done(hc_chrome_error(@"Chrome page has already started or closed."));return;}
    [broker connect:^(NSError *error){
        if(error || self->closed || self->broker->shuttingDown){if(done)done(error ?: hc_chrome_error(@"Chrome page closed."));return;}
        NSMutableDictionary *params=[NSMutableDictionary dictionaryWithDictionary:@{@"url":@"about:blank",@"background":@YES}];
        if(self->broker->attached || self->broker->headed)params[@"hidden"]=@YES;
        self->broker->openingCount++;
        [self->broker send:@"Target.createTarget" params:params session:nil completion:^(NSDictionary *r,NSError *e){
            if(self->broker->openingCount)self->broker->openingCount--;
            if(e){if(done)done(e);return;}
            if(self->closed || self->broker->shuttingDown){[self->broker closeTarget:r[@"targetId"] ?: @"" completion:nil];if(done)done(hc_chrome_error(@"Chrome page closed."));return;}
            self->ownsTarget=YES;
            [self adoptTarget:r[@"targetId"] completion:^(NSError *ae){
                if(ae){if(done)done(ae);return;}
                [self command:@"Page.navigate" params:@{@"url":location.length ? location : @"about:blank"} completion:^(NSDictionary *n,NSError *ne){
                    if(n[@"errorText"])ne=hc_chrome_error(n[@"errorText"]);
                    if(done)done(ne);if(ne)[self notify:@{@"type":@"error",@"message":ne.localizedDescription}];else [self refreshMetadata];
                }];
            }];
        }];
    }];
}
- (void)adoptTarget:(NSString *)identifier completion:(void (^)(NSError *))done {
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{[self adoptTarget:identifier completion:done];});return;}
    if(closed || targetID || !identifier.length || broker->shuttingDown){if(done)done(hc_chrome_error(@"Chrome target is unavailable."));return;}
    if(!broker->connected) {
        [broker connect:^(NSError *e){if(e){if(done)done(e);}else [self adoptTarget:identifier completion:done];}];return;
    }
    targetID=[identifier copy];broker->targets[targetID]=self;
    broker->openingCount++;
    [broker send:@"Target.attachToTarget" params:@{@"targetId":targetID,@"flatten":@YES} session:nil completion:^(NSDictionary *ar,NSError *ae){
        if(self->broker->openingCount)self->broker->openingCount--;
        if(ae){[self close];if(done)done(ae);return;}
        self->ownsTarget=YES;
        if(self->closed || self->broker->shuttingDown) {
            if(!self->closeSent){self->closeSent=YES;[self->broker closeTarget:self->targetID session:ar[@"sessionId"] completion:nil];}
            [self close];if(done)done(hc_chrome_error(@"Chrome page closed."));return;
        }
        self->sessionID=[ar[@"sessionId"] copy];self->broker->pages[self->sessionID]=self;
        [self command:@"Page.enable" params:@{} completion:^(NSDictionary *p,NSError *pe){
            (void)p;if(pe){[self close];if(done)done(pe);return;}
            [self command:@"Runtime.enable" params:@{} completion:^(NSDictionary *q,NSError *qe){
                (void)q;if(qe){[self close];if(done)done(qe);return;}
                [self command:@"Runtime.addBinding" params:@{@"name":@"hardcoreChromeNotify",@"executionContextName":@"hardcore.browser"} completion:^(NSDictionary *b,NSError *be){
                    (void)b;if(be){[self close];if(done)done(be);return;}
                    [self command:@"Page.addScriptToEvaluateOnNewDocument" params:@{@"source":self->source,@"worldName":@"hardcore.browser"} completion:^(NSDictionary *s,NSError *se){
                        (void)s;if(se){[self close];if(done)done(se);return;}
                        self->ready=YES;[self setWidth:self->viewportWidth height:self->viewportHeight];
                        [self refreshMetadata];if(done)done(nil);
                    }];
                }];
            }];
        }];
    }];
}
- (void)command:(NSString *)method params:(NSDictionary *)params completion:(HCChromeReply)done {
    if(closed || !sessionID){if(done)done(nil,hc_chrome_error(@"Chrome page is not ready."));return;}
    [broker send:method params:params session:sessionID completion:done];
}
- (void)evaluate:(NSString *)js completion:(void (^)(id,NSError *))done {
    if(closed || !ready){if(done)done(nil,hc_chrome_error(@"Chrome page is not ready."));return;}
    void (^run)(NSInteger)=^(NSInteger context){
        BOOL prepare=self->preparedContext!=context;
        NSString *expression=prepare ? [NSString stringWithFormat:@"%@;\n%@",self->source,js ?: @"undefined"] : js;
        [self command:@"Runtime.evaluate" params:@{@"expression":expression ?: @"undefined",@"contextId":@(context),@"returnByValue":@YES,@"awaitPromise":@YES,@"userGesture":@YES} completion:^(NSDictionary *r,NSError *e){
            if(!e && r[@"exceptionDetails"])e=hc_chrome_error(r[@"exceptionDetails"][@"text"] ?: @"Page script failed.");
            if(!e)self->preparedContext=context;
            if(done)done(r[@"result"][@"value"],e);
        }];
    };
    if(contextID){run(contextID);return;}
    [self command:@"Page.getFrameTree" params:@{} completion:^(NSDictionary *r,NSError *e){
        NSString *fid=r[@"frameTree"][@"frame"][@"id"];
        if(e || !fid){if(done)done(nil,e ?: hc_chrome_error(@"Chrome has no document yet."));return;}
        [self->frameID release];self->frameID=[fid copy];
        [self command:@"Page.createIsolatedWorld" params:@{@"frameId":fid,@"worldName":@"hardcore.browser"} completion:^(NSDictionary *w,NSError *we){
            NSInteger context=[w[@"executionContextId"] integerValue];
            if(we || !context){if(done)done(nil,we ?: hc_chrome_error(@"Chrome document changed."));return;}
            self->contextID=context;run(context);
        }];
    }];
}
- (void)refreshMetadata {
    if(metadataPending || closed || !ready)return;metadataPending=YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.1*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        self->metadataPending=NO;if(self->closed || !self->ready)return;
        [self command:@"Page.getNavigationHistory" params:@{} completion:^(NSDictionary *r,NSError *e){
            if(e)return;
            NSArray *entries=r[@"entries"];NSInteger current=[r[@"currentIndex"] integerValue];
            self->canGoBack=current>0;self->canGoForward=current+1<(NSInteger)entries.count;
            if(current>=0 && current<(NSInteger)entries.count) {
                NSDictionary *entry=entries[(NSUInteger)current];
                if([entry[@"url"] isKindOfClass:NSString.class]){[self->url release];self->url=[entry[@"url"] copy];}
                if([entry[@"title"] isKindOfClass:NSString.class]){[self->title release];self->title=[entry[@"title"] copy];}
            }
            [self notify:@{@"type":@"changed"}];
        }];
    });
}
- (void)event:(NSString *)method params:(NSDictionary *)params {
    if(closed)return;
    if([method isEqualToString:@"Runtime.bindingCalled"] && [params[@"name"] isEqualToString:@"hardcoreChromeNotify"]) {
        /* Ignore page-world/subframe messages, even if a page discovers the binding name. */
        NSString *senderFrame=contextFrames[params[@"executionContextId"] ?: @0];
        if(!frameID || ![senderFrame isEqualToString:frameID])return;
        NSString *payload=params[@"payload"];
        if([payload isKindOfClass:NSString.class] && payload.length<1024*1024) {
            id event=[NSJSONSerialization JSONObjectWithData:[payload dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil];
            if([event isKindOfClass:NSDictionary.class])[self notify:event];
        }
    } else if([method isEqualToString:@"Runtime.executionContextCreated"]) {
        NSDictionary *context=params[@"context"];
        if([context[@"name"] isEqualToString:@"hardcore.browser"] && context[@"id"] && context[@"auxData"][@"frameId"])
            contextFrames[context[@"id"]]=context[@"auxData"][@"frameId"];
    } else if([method isEqualToString:@"Runtime.executionContextsCleared"]) {contextID=preparedContext=0;[contextFrames removeAllObjects];}
    else if([method isEqualToString:@"Runtime.executionContextDestroyed"]) {
        NSNumber *identifier=params[@"executionContextId"];
        if(identifier)[contextFrames removeObjectForKey:identifier];
        if(identifier.integerValue==contextID)contextID=preparedContext=0;
    }
    else if([method isEqualToString:@"Page.frameNavigated"] && !params[@"frame"][@"parentId"]) {
        contextID=preparedContext=0;[frameID release];frameID=[params[@"frame"][@"id"] copy];
        NSString *next=params[@"frame"][@"url"];if(next){[url release];url=[next copy];}
        [self notify:@{@"type":@"navigation"}];[self refreshMetadata];
    } else if([method isEqualToString:@"Page.loadEventFired"] || [method isEqualToString:@"Page.domContentEventFired"] || [method isEqualToString:@"Page.navigatedWithinDocument"]) {[self refreshMetadata];[self notify:@{@"type":@"changed"}];}
    else if([method isEqualToString:@"Inspector.targetCrashed"]){ready=NO;[self notify:@{@"type":@"error",@"message":@"Chrome page crashed. Open the page again."}];}
}
- (void)navigate:(NSString *)location {
    if(!location.length)return;
    [self command:@"Page.navigate" params:@{@"url":location} completion:^(NSDictionary *r,NSError *e){
        if(r[@"errorText"])e=hc_chrome_error(r[@"errorText"]);
        if(e)[self notify:@{@"type":@"error",@"message":e.localizedDescription}];else [self refreshMetadata];
    }];
}
- (void)go:(int)delta {
    [self command:@"Page.getNavigationHistory" params:@{} completion:^(NSDictionary *r,NSError *e){
        if(e)return;NSArray *entries=r[@"entries"];NSInteger index=[r[@"currentIndex"] integerValue]+delta;
        if(index>=0 && index<(NSInteger)entries.count)[self command:@"Page.navigateToHistoryEntry" params:@{@"entryId":entries[(NSUInteger)index][@"id"]} completion:nil];
    }];
}
- (void)reload {[self command:@"Page.reload" params:@{} completion:nil];}
- (void)setWidth:(int)width height:(int)height {
    viewportWidth=MAX(1,MIN(width,8192));viewportHeight=MAX(1,MIN(height,8192));
    if(ready && (sentWidth!=viewportWidth || sentHeight!=viewportHeight)) {
        sentWidth=viewportWidth;sentHeight=viewportHeight;
        [self command:@"Emulation.setDeviceMetricsOverride" params:@{@"width":@(viewportWidth),@"height":@(viewportHeight),@"deviceScaleFactor":@1,@"mobile":@NO} completion:^(NSDictionary *r,NSError *e){(void)r;if(e)self->sentWidth=self->sentHeight=0;}];
    }
}
- (void)capture:(void (^)(NSData *,NSError *))done {
    [self command:@"Page.captureScreenshot" params:@{@"format":@"png",@"fromSurface":@YES,@"captureBeyondViewport":@NO} completion:^(NSDictionary *r,NSError *e){
        NSData *data=nil;NSString *encoded=r[@"data"];
        if(!e && [encoded isKindOfClass:NSString.class] && encoded.length<=16*1024*1024)data=[[[NSData alloc] initWithBase64EncodedString:encoded options:0] autorelease];
        if(!data && !e)e=hc_chrome_error(@"Chrome did not return an image.");if(done)done(data,e);
    }];
}
- (void)close {[self closeWithCompletion:nil];}
- (void)closeWithCompletion:(void (^)(NSError *))done {
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{[self closeWithCompletion:done];});return;}
    if(closed){if(done)done(nil);return;}closed=YES;ready=NO;
    [self retain];
    if(sessionID)[broker->pages removeObjectForKey:sessionID];
    if(targetID)[broker->targets removeObjectForKey:targetID];
    if(targetID && ownsTarget && !closeSent){closeSent=YES;[broker closeTarget:targetID session:sessionID completion:done];}
    else if(done)done(nil);
    [changed release];changed=nil;
    [self release];
}
@end

/* Called only on real application quit, never exec/reload. A terminal-only
   window does not instantiate/connect Chrome just because it is quitting. */
static void hc_chrome_close_pages_main(void) {
    HCChromeBroker *broker=hc_chrome_shared_broker;if(!broker)return;
    broker->shuttingDown=YES;
    NSMutableSet *owned=[NSMutableSet setWithArray:broker->targets.allValues];
    [owned addObjectsFromArray:broker->pages.allValues];
    /* Closes already chained behind Page.stopLoading are resent directly. */
    for(NSString *identifier in [[broker->closingTargets copy] autorelease])[broker quitClose:identifier session:nil];
    for(HCChromePage *page in owned)[page close];
    /* Replies are counted on the socket's queue, so this wait works even when
       quit runs inside a main-queue block that the run loop cannot re-enter. */
    NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+1.5;
    for(;;) {
        NSUInteger left;@synchronized(broker->quitReplies){left=broker->quitReplies.count;}
        if((!left && !broker->openingCount) || NSDate.timeIntervalSinceReferenceDate>=deadline)break;
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    }
}

/* An explicit, visible profile setup command. It never closes another profile,
   or a profile containing a browsing page. Ordinary WWW browsing stays hidden. */
static int hc_chrome_setup_main(int argc,char **argv) {
    @autoreleasepool {
        const char *action=argc>2 ? argv[2] : "";
        NSString *profile=hc_chrome_profile();
        if(!strcmp(action,"profile")){printf("%s\n",profile.fileSystemRepresentation);return 0;}
        if(strcmp(action,"setup")){fprintf(stderr,"Usage: hardcore chrome setup | profile\n");return 2;}
        if([NSProcessInfo.processInfo.environment[@"HARDCORE_CHROME_ATTACH"] length]) {
            fprintf(stderr,"hardcore: attached Chrome uses your existing profile. Open Chrome's settings/extensions directly.\n");return 2;
        }
        if(hc_chrome_is_regular_profile(profile)) {
            fprintf(stderr,"hardcore: choose a separate HARDCORE_CHROME_PROFILE; the regular Chrome profile is never launched or changed by setup.\n");return 2;
        }
        NSString *exe=hc_chrome_executable();
        if(!exe.length || ![NSFileManager.defaultManager isExecutableFileAtPath:exe]){fprintf(stderr,"hardcore: install Google Chrome before setting up its Hardcore profile.\n");return 2;}
        NSError *error=nil;
        if(![NSFileManager.defaultManager createDirectoryAtPath:profile withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&error]) {
            fprintf(stderr,"hardcore: %s\n",error.localizedDescription.UTF8String);return 2;
        }
        NSString *endpointPath=[profile stringByAppendingPathComponent:@"DevToolsActivePort"];
        NSURL *endpoint=hc_chrome_endpoint(endpointPath);
        if(hc_chrome_listening(endpoint)) {
            __block BOOL finished=NO,occupied=NO;
            __block NSError *failure=nil;
            HCChromeBroker *broker=[HCChromeBroker shared];
            [broker connect:^(NSError *e){
                if(e){failure=[e retain];finished=YES;return;}
                [broker send:@"Target.getTargets" params:@{} session:nil completion:^(NSDictionary *r,NSError *getError){
                    if(getError){failure=[getError retain];finished=YES;return;}
                    for(NSDictionary *target in r[@"targetInfos"]) {
                        NSString *location=target[@"url"];
                        if([target[@"type"] isEqualToString:@"page"] && location.length && ![location isEqualToString:@"about:blank"]){occupied=YES;finished=YES;return;}
                    }
                    [broker send:@"Browser.close" params:@{} session:nil completion:^(NSDictionary *unused,NSError *closeError){
                        (void)unused;(void)closeError;finished=YES;
                    }];
                }];
            }];
            NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+25;
            while(!finished && NSDate.timeIntervalSinceReferenceDate<deadline)
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            if(!finished || failure){fprintf(stderr,"hardcore: %s\n",failure ? failure.localizedDescription.UTF8String : "Chrome setup timed out.");[failure release];return 2;}
            if(occupied){fprintf(stderr,"hardcore: close the WWW tabs and other pages using the Hardcore Chrome profile, then run setup again. Your pages were left open.\n");return 2;}
            deadline=NSDate.timeIntervalSinceReferenceDate+5;
            while(hc_chrome_listening(endpoint) && NSDate.timeIntervalSinceReferenceDate<deadline)
                [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
            if(hc_chrome_listening(endpoint)){fprintf(stderr,"hardcore: the idle Chrome profile has not exited yet; retry setup shortly.\n");return 2;}
        }
        NSTask *task=[[[NSTask alloc] init] autorelease];task.executableURL=[NSURL fileURLWithPath:exe];
        task.arguments=@[[ @"--user-data-dir=" stringByAppendingString:profile],@"--remote-debugging-address=127.0.0.1",@"--remote-debugging-port=0",@"--no-first-run",@"--no-default-browser-check",@"chrome://extensions/"];
        task.standardOutput=[NSFileHandle fileHandleWithNullDevice];task.standardError=[NSFileHandle fileHandleWithNullDevice];task.standardInput=[NSFileHandle fileHandleWithNullDevice];
        if(![task launchAndReturnError:&error]){fprintf(stderr,"hardcore: %s\n",error.localizedDescription.UTF8String);return 2;}
        /* LaunchServices accepting an executable does not mean Chrome accepted
           the profile. Wait for its actual debugging listener before success. */
        BOOL running=NO;NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+15;
        do {
            NSURL *candidate=hc_chrome_endpoint(endpointPath);
            if(hc_chrome_listening(candidate)){running=YES;break;}
            if(!task.running)break;
            [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        } while(NSDate.timeIntervalSinceReferenceDate<deadline);
        if(!running) {
            fprintf(stderr,"hardcore: Chrome setup did not open its profile. Close another Chrome instance using this Hardcore profile and retry.\n");return 2;
        }
        printf("Opened Hardcore's Chrome profile for sign-in and extensions.\nProfile: %s\nQuit this Chrome instance when setup is finished; WWW tabs reuse the saved profile.\n",profile.fileSystemRepresentation);
        return 0;
    }
}
#endif
#endif
