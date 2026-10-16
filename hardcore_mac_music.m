/* Music For Programming's public RSS is the catalog; AVPlayer streams only
 * after a Play action. No browser, shell command, or full-track download. */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <math.h>

#define MUSIC_FEED_LIMIT (2u*1024u*1024u)
#define MUSIC_ITEM_LIMIT 512
static void mac_music_changed(void);
static NSString *const music_feed_url=@"https://musicforprogramming.net/rss.xml";

static NSString *music_text(id value,NSUInteger limit) {
    if(![value isKindOfClass:NSString.class])return @"";
    NSMutableString *clean=[NSMutableString string];
    for(NSString *part in [value componentsSeparatedByCharactersInSet:NSCharacterSet.controlCharacterSet]) {
        if(clean.length && part.length)[clean appendString:@" "];
        [clean appendString:part];
        if(clean.length>=limit)break;
    }
    NSString *result=[clean stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return result.length>limit?[result substringToIndex:limit]:result;
}
static NSURL *music_url(id value,BOOL audio) {
    if(![value isKindOfClass:NSString.class] || [value length]>2048)return nil;
    NSURLComponents *u=[NSURLComponents componentsWithString:value];
    NSString *host=u.host.lowercaseString;
    if(![@[@"http",@"https"] containsObject:u.scheme.lowercaseString] || u.user || u.password || u.port)return nil;
    if(audio) {
        if(![@[@"datashat.net",@"musicforprogramming.net"] containsObject:host] || ![u.path.lowercaseString hasSuffix:@".mp3"])return nil;
    } else if(![host isEqual:@"musicforprogramming.net"])return nil;
    u.scheme=@"https";u.fragment=nil;return u.URL;
}
static double music_duration(id value) {
    if(![value isKindOfClass:NSString.class] || [value length]>16)return 0;
    NSArray *parts=[value componentsSeparatedByString:@":"];
    if(parts.count<1 || parts.count>3)return 0;
    double total=0;
    for(NSUInteger i=0;i<parts.count;i++) {
        NSString *s=parts[i];
        if(!s.length || [s rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location!=NSNotFound)return 0;
        double n=s.doubleValue;if(!isfinite(n) || n<0 || (i && n>=60))return 0;
        total=total*60+n;
    }
    return total<=7*24*3600?total:0;
}
static NSString *music_time(double seconds) {
    long n=(long)(isfinite(seconds)?fmax(0,fmin(seconds,7*24*3600)):0);
    return n>=3600?[NSString stringWithFormat:@"%ld:%02ld:%02ld",n/3600,(n/60)%60,n%60]:[NSString stringWithFormat:@"%ld:%02ld",n/60,n%60];
}
static NSDictionary *music_item(id raw) {
    if(![raw isKindOfClass:NSDictionary.class])return nil;
    NSURL *audio=music_url(raw[@"audio"],YES),*page=music_url(raw[@"page"],NO);
    NSString *title=music_text(raw[@"title"],180);
    if(!audio || !title.length)return nil;
    NSString *shortTitle=[title hasPrefix:@"Episode "]?[title substringFromIndex:8]:title;
    NSScanner *scanner=[NSScanner scannerWithString:shortTitle];int episode=0;
    if(![scanner scanInt:&episode] || episode<1 || episode>100000)episode=0;
    double duration=[raw[@"duration"] isKindOfClass:NSNumber.class]?[raw[@"duration"] doubleValue]:0;
    if(!isfinite(duration) || duration<0 || duration>7*24*3600)duration=0;
    return @{@"id":audio.absoluteString,@"audio":audio.absoluteString,@"page":page.absoluteString?:@"https://musicforprogramming.net/",
        @"title":title,@"shortTitle":shortTitle,@"episode":@(episode),@"duration":@(duration),@"durationText":music_time(duration)};
}

@interface HCMusicFeedParser : NSObject <NSXMLParserDelegate> {
@public NSMutableArray *items;NSMutableDictionary *item;NSMutableString *text;NSString *field;NSMutableSet *seen;int depth,itemDepth,fieldDepth,itemCount;BOOL invalid;
}
@end
@implementation HCMusicFeedParser
- (id)init {if((self=[super init])){items=[[NSMutableArray alloc] init];seen=[[NSMutableSet alloc] init];}return self;}
- (void)dealloc {[items release];[item release];[text release];[field release];[seen release];[super dealloc];}
- (void)parser:(NSXMLParser *)parser didStartElement:(NSString *)name namespaceURI:(NSString *)uri qualifiedName:(NSString *)q attributes:(NSDictionary *)attrs {
    (void)uri;(void)q;depth++;
    if(depth>16){invalid=YES;[parser abortParsing];return;}
    if([name isEqual:@"item"]) {
        if(item || ++itemCount>MUSIC_ITEM_LIMIT){invalid=YES;[parser abortParsing];return;}
        item=[[NSMutableDictionary alloc] init];itemDepth=depth;
    } else if(item && depth==itemDepth+1) {
        if([@[@"title",@"link",@"itunes:duration"] containsObject:name]) {
            [field release];field=[name copy];fieldDepth=depth;[text release];text=[[NSMutableString alloc] init];
        } else if([name isEqual:@"enclosure"])item[@"audio"]=attrs[@"url"]?:@"";
    }
}
- (void)parser:(NSXMLParser *)parser foundCharacters:(NSString *)s {
    if(field) {if(text.length+s.length>8192){invalid=YES;[parser abortParsing];return;}[text appendString:s];}
}
- (void)parser:(NSXMLParser *)parser foundCDATA:(NSData *)data {
    NSString *s=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];if(s)[self parser:parser foundCharacters:s];
}
- (void)parser:(NSXMLParser *)parser didEndElement:(NSString *)name namespaceURI:(NSString *)uri qualifiedName:(NSString *)q {
    (void)parser;(void)uri;(void)q;
    if(field && fieldDepth==depth) {
        if([field isEqual:@"title"])item[@"title"]=text;
        else if([field isEqual:@"link"])item[@"page"]=text;
        else item[@"duration"]=@(music_duration([text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]));
        [field release];field=nil;[text release];text=nil;
    }
    if([name isEqual:@"item"] && itemDepth==depth) {
        NSDictionary *entry=music_item(item);
        if(entry && ![seen containsObject:entry[@"id"]]){[items addObject:entry];[seen addObject:entry[@"id"]];}
        [item release];item=nil;
    }
    depth--;
}
@end
static NSArray *music_parse_feed(NSData *data) {
    if(!data.length || data.length>MUSIC_FEED_LIMIT)return nil;
    NSString *xml=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    if(!xml || [xml rangeOfString:@"<!DOCTYPE" options:NSCaseInsensitiveSearch].location!=NSNotFound ||
       [xml rangeOfString:@"<!ENTITY" options:NSCaseInsensitiveSearch].location!=NSNotFound)return nil;
    HCMusicFeedParser *delegate=[[[HCMusicFeedParser alloc] init] autorelease];
    NSXMLParser *parser=[[[NSXMLParser alloc] initWithData:data] autorelease];parser.delegate=delegate;parser.shouldResolveExternalEntities=NO;
    return [parser parse] && !delegate->invalid && delegate->items.count?[[delegate->items copy] autorelease]:nil;
}
static NSString *music_cache_path(void) {
    return [[NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:@"hardcore/music"] stringByAppendingPathComponent:@"catalog.xml"];
}
static NSString *music_test_path(const char *name) {
    const char *value=getenv("HARDCORE_CTL")?getenv(name):NULL;
    return value && *value?[NSString stringWithUTF8String:value]:nil;
}
static NSData *music_read_bounded(NSString *path) {
    if(!path.length)return nil;
    NSFileHandle *file=[NSFileHandle fileHandleForReadingAtPath:path];if(!file)return nil;
    NSData *data=nil;
    @try {data=[file readDataOfLength:MUSIC_FEED_LIMIT+1];[file closeFile];} @catch(NSException *exception){(void)exception;return nil;}
    return data.length<=MUSIC_FEED_LIMIT?data:nil;
}

/* Factory seams permit deterministic silent tests without networking/audio. */
#ifndef MUSIC_PLAYER_ITEM
#define MUSIC_PLAYER_ITEM(url) [AVPlayerItem playerItemWithURL:(url)]
#endif
#ifndef MUSIC_PLAYER
#define MUSIC_PLAYER(item) [AVPlayer playerWithPlayerItem:(item)]
#endif

@interface HCMusic : NSObject <NSURLSessionDataDelegate> {
@public
    NSArray *items;NSInteger selected;AVPlayer *player;NSTimer *timer;
    NSURLSession *session;NSMutableData *received;NSString *error,*catalogError;
    BOOL loading,wantsPlayback,buffering,dead,seekPending,seeking;double position,duration;unsigned generation,seekGeneration;
    NSDictionary *lastState;
}
- (void)load;
- (void)startPlayer;
- (void)notify;
- (void)acceptItems:(NSArray *)catalog;
- (void)select:(NSInteger)index play:(BOOL)play;
- (void)playPause;
- (void)next:(int)delta;
- (void)stop;
- (void)seek:(double)seconds;
- (void)tick;
- (NSDictionary *)state;
- (void)shutdown;
@end
@implementation HCMusic
- (id)init {if((self=[super init])){items=[@[] retain];selected=-1;}return self;}
- (void)releasePlayer {
    seekGeneration++;seeking=NO;
    if(player){[NSNotificationCenter.defaultCenter removeObserver:self name:AVPlayerItemDidPlayToEndTimeNotification object:player.currentItem];[player pause];[player release];player=nil;}
    buffering=NO;
}
- (void)notify {
    NSDictionary *state=[self state];
    if(![state isEqual:lastState]){[lastState release];lastState=[state copy];mac_music_changed();}
}
- (void)ensureTimer {
    if(timer || dead)return;
    timer=[[NSTimer timerWithTimeInterval:.25 target:self selector:@selector(tick) userInfo:nil repeats:YES] retain];
    [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
}
- (NSDictionary *)current {return selected>=0 && selected<(NSInteger)items.count?items[selected]:nil;}
- (void)acceptItems:(NSArray *)catalog {
    if(dead || !catalog.count)return;
    NSString *currentID=[[[self current][@"id"] copy] autorelease];
    NSMutableArray *merged=[catalog mutableCopy];NSInteger index=-1;
    for(NSUInteger i=0;i<merged.count;i++)if([merged[i][@"id"] isEqual:currentID]){index=(NSInteger)i;break;}
    /* Keep the current stream if a refreshed feed drops an older episode. */
    if(index<0 && currentID && [self current]){if(merged.count>=MUSIC_ITEM_LIMIT)[merged removeLastObject];[merged addObject:[self current]];index=(NSInteger)merged.count-1;}
    [items release];items=[merged copy];[merged release];selected=index>=0?index:0;
    if(!duration)duration=[[self current][@"duration"] doubleValue];
    mac_music_changed();[self notify];
}
- (void)networkFinished:(NSString *)message data:(NSData *)data {
    NSURLSession *old=session;session=nil;[old finishTasksAndInvalidate];[old release];
    [received release];received=nil;
    if(dead)return;
    if(message){loading=NO;[catalogError release];catalogError=[message copy];if(!items.count)wantsPlayback=NO;[self notify];return;}
    unsigned request=generation;NSString *path=music_cache_path();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool {
        NSArray *catalog=[music_parse_feed(data) retain];
        if(catalog) {
            NSString *directory=[path stringByDeletingLastPathComponent];
            [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:nil];
            [data writeToFile:path options:NSDataWritingAtomic error:nil];
            [NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path error:nil];
        }
        dispatch_async(dispatch_get_main_queue(),^{
            if(!dead && generation==request) {
                loading=NO;[catalogError release];catalogError=catalog?nil:[@"Catalog could not be read; choose Refresh to retry." copy];
                if(catalog)[self acceptItems:catalog];
                if(wantsPlayback && !player && items.count)[self startPlayer];
                if(!items.count)wantsPlayback=NO;
                [self notify];
            }
            [catalog release];
        });
    }});
}
- (void)startNetwork {
    if(dead)return;
    received=[[NSMutableData alloc] init];
    NSURLSessionConfiguration *config=NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForRequest=15;config.timeoutIntervalForResource=25;config.HTTPMaximumConnectionsPerHost=1;
    config.requestCachePolicy=NSURLRequestReloadIgnoringLocalCacheData;
    config.HTTPAdditionalHeaders=@{@"Accept":@"application/rss+xml, application/xml, text/xml",@"User-Agent":@"Hardcore Music/1.0 (+https://github.com/williamsharkey/hardcore)"};
    session=[[NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:NSOperationQueue.mainQueue] retain];
    [[session dataTaskWithURL:[NSURL URLWithString:music_feed_url]] resume];
}
- (void)load {
    if(loading || dead)return;loading=YES;generation++;[catalogError release];catalogError=nil;[self notify];
    NSString *fixture=music_test_path("HARDCORE_TEST_MUSIC_CATALOG");
    if(items.count && !fixture){[self startNetwork];return;}
    unsigned request=generation;NSString *path=fixture?:music_cache_path();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool {
        NSData *data=music_read_bounded(path);
        NSArray *catalog=[music_parse_feed(data) retain];
        dispatch_async(dispatch_get_main_queue(),^{
            if(!dead && generation==request){
                if(catalog)[self acceptItems:catalog];
                if(wantsPlayback && !player && items.count)[self startPlayer];
                if(fixture){loading=NO;catalogError=catalog?nil:[@"Test catalog could not be read." copy];if(!items.count)wantsPlayback=NO;[self notify];}
                else [self startNetwork];
            }
            [catalog release];
        });
    }});
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))completion {
    (void)s;(void)task;(void)response;
    completion([request.URL.scheme.lowercaseString isEqual:@"https"] && [request.URL.host.lowercaseString isEqual:@"musicforprogramming.net"]?request:nil);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    (void)s;(void)task;BOOL ok=response.expectedContentLength<=MUSIC_FEED_LIMIT;
    if([response isKindOfClass:NSHTTPURLResponse.class])ok=ok && [(NSHTTPURLResponse *)response statusCode]>=200 && [(NSHTTPURLResponse *)response statusCode]<300;
    completion(ok?NSURLSessionResponseAllow:NSURLSessionResponseCancel);
}
- (void)URLSession:(NSURLSession *)s dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)s;if(received.length+data.length>MUSIC_FEED_LIMIT){[task cancel];return;}[received appendData:data];
}
- (void)URLSession:(NSURLSession *)s task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)failure {
    (void)task;if(s!=session)return;
    NSData *data=[[received copy] autorelease];
    [self networkFinished:failure?@"Cannot refresh Music For Programming; choose Refresh to retry.":nil data:data];
}
- (void)startPlayer {
    if(dead || ![self current])return;
    [self releasePlayer];[error release];error=nil;
    NSString *fixture=music_test_path("HARDCORE_TEST_MUSIC_MEDIA");
    NSURL *source=fixture?[NSURL fileURLWithPath:fixture]:music_url([self current][@"audio"],YES);
    AVPlayerItem *item=MUSIC_PLAYER_ITEM(source);
    /* Limit prefetch rather than treating an episode as a download. */
    item.preferredForwardBufferDuration=10;
    player=[MUSIC_PLAYER(item) retain];player.automaticallyWaitsToMinimizeStalling=YES;
    if(fixture)player.muted=YES;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(ended:) name:AVPlayerItemDidPlayToEndTimeNotification object:item];
    seekPending=position>0;buffering=wantsPlayback;
    if(wantsPlayback && !seekPending)[player play];
    [self ensureTimer];[self notify];
}
- (void)select:(NSInteger)index play:(BOOL)play {
    if(dead || index<0 || index>=(NSInteger)items.count)return;
    BOOL same=index==selected;wantsPlayback=play;
    if(!same){[self releasePlayer];selected=index;position=0;duration=[[self current][@"duration"] doubleValue];seekPending=NO;}
    [error release];error=nil;
    if(play){[self ensureTimer];if(!player || player.currentItem.status==AVPlayerItemStatusFailed)[self startPlayer];else if(!seekPending && !seeking)[player play];}
    else {[player pause];buffering=NO;}
    [self notify];
}
- (void)playPause {
    if(dead)return;
    if(wantsPlayback){wantsPlayback=NO;[player pause];buffering=NO;[self tick];return;}
    wantsPlayback=YES;
    if(!items.count){[self load];[self notify];return;}
    [self select:selected>=0?selected:0 play:YES];
}
- (void)next:(int)delta {
    if(dead)return;
    if(!items.count){[self load];return;}
    NSInteger n=(NSInteger)items.count,index=selected>=0?selected:0;
    NSInteger step=(NSInteger)delta%n;index=(index+step+n)%n;
    [self select:index play:wantsPlayback];
}
- (void)ended:(NSNotification *)note {
    /* AVFoundation may post this notification from a playback thread. The
       playlist, player ownership, and redraw callback all belong to main. */
    if(!NSThread.isMainThread){dispatch_async(dispatch_get_main_queue(),^{[self ended:note];});return;}
    if(dead || note.object!=player.currentItem || !wantsPlayback)return;
    [self next:1];
    /* A one-episode feed still restarts when its only episode ends. */
    if(items.count==1){position=0;[self startPlayer];}
}
- (void)seek:(double)seconds {
    if(dead || !isfinite(seconds) || ![self current])return;
    position=fmax(0,fmin(seconds,duration>0?duration:7*24*3600));
    if(player){seekPending=YES;[self ensureTimer];[self tick];}
    [self notify];
}
- (void)stop {wantsPlayback=NO;position=0;seekPending=NO;[self releasePlayer];[error release];error=nil;[self notify];}
- (void)tick {
    if(dead)return;
    if(player) {
        AVPlayerItem *item=player.currentItem;
        if(item.status==AVPlayerItemStatusFailed) {
            wantsPlayback=NO;buffering=NO;
            if(!error)error=[@"Playback unavailable; press Play to retry." copy];
            [self releasePlayer];
        } else {
            double length=CMTimeGetSeconds(item.duration);if(isfinite(length) && length>0)duration=length;
            if(seekPending && item.status==AVPlayerItemStatusReadyToPlay) {
                seekPending=NO;seeking=YES;unsigned seekID=++seekGeneration;
                double target=duration>0?fmin(position,duration):position;AVPlayer *active=player;
                [player seekToTime:CMTimeMakeWithSeconds(target,600) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero completionHandler:^(BOOL finished){
                    dispatch_async(dispatch_get_main_queue(),^{if(!dead && player==active && seekGeneration==seekID){seeking=NO;if(finished){position=target;if(wantsPlayback)[player play];}}});
                }];
            } else if(!seekPending && !seeking) {double at=CMTimeGetSeconds(player.currentTime);if(isfinite(at) && at>=0)position=at;}
            buffering=wantsPlayback && player.timeControlStatus!=AVPlayerTimeControlStatusPlaying;
        }
    }
    [self notify];
    if(!player || (!wantsPlayback && !seekPending && !seeking && player.currentItem.status==AVPlayerItemStatusReadyToPlay)) {[timer invalidate];[timer release];timer=nil;}
}
- (NSDictionary *)state {
    NSDictionary *entry=[self current];
    return @{@"loaded":@(items.count>0),@"loading":@(loading),@"index":@(selected),@"count":@(items.count),
      @"title":entry[@"title"]?:@"Music For Programming",@"shortTitle":entry[@"shortTitle"]?:@"Choose an episode",@"episode":entry[@"episode"]?:@0,
      @"playing":@(wantsPlayback),@"buffering":@(buffering),@"position":@(floor(position)),@"duration":@(floor(duration)),
      @"positionText":music_time(position),@"durationText":music_time(duration),@"progress":@(duration>0?fmax(0,fmin(1,position/duration)):0),
      @"error":error?:@"",@"catalogError":catalogError?:@""};
}
- (void)shutdown {
    dead=YES;generation++;[timer invalidate];[timer release];timer=nil;
    [session invalidateAndCancel];[session release];session=nil;[received release];received=nil;
    [self releasePlayer];
}
- (void)dealloc {[self shutdown];[items release];[error release];[catalogError release];[lastState release];[super dealloc];}
@end

static HCMusic *g_music_service;
static void mac_music_init(void) {if(!g_music_service)g_music_service=[[HCMusic alloc] init];}
static void mac_music_load(void) {mac_music_init();[g_music_service load];}
static NSArray *mac_music_items(void) {return g_music_service?g_music_service->items:@[];}
static NSDictionary *mac_music_status(void) {mac_music_init();return [g_music_service state];}
static void mac_music_play_pause(void) {mac_music_init();[g_music_service playPause];}
static void mac_music_next(int delta) {mac_music_init();[g_music_service next:delta];}
static void mac_music_select(int index,int play) {mac_music_init();[g_music_service select:index play:play!=0];}
static void mac_music_seek(double seconds) {mac_music_init();[g_music_service seek:seconds];}
static void mac_music_stop(void) {if(g_music_service)[g_music_service stop];}
static NSDictionary *mac_music_pack(void) {
    if(!g_music_service || !g_music_service->items.count)return @{};
    HCMusic *m=g_music_service;
    return @{@"items":m->items,@"index":@(m->selected),@"position":@(m->position),@"playing":@(m->wantsPlayback)};
}
static void mac_music_restore(id value) {
    if(![value isKindOfClass:NSDictionary.class] || ![value[@"items"] isKindOfClass:NSArray.class] || [value[@"items"] count]>MUSIC_ITEM_LIMIT)return;
    NSMutableArray *items=[NSMutableArray array];
    for(id raw in value[@"items"]){NSDictionary *item=music_item(raw);if(!item)return;[items addObject:item];}
    if(!items.count || ![value[@"index"] isKindOfClass:NSNumber.class] || ![value[@"position"] isKindOfClass:NSNumber.class])return;
    NSInteger index=[value[@"index"] integerValue];double position=[value[@"position"] doubleValue];
    if(index<0 || index>=(NSInteger)items.count || !isfinite(position) || position<0 || position>7*24*3600)return;
    mac_music_init();[g_music_service stop];[g_music_service acceptItems:items];[g_music_service select:index play:NO];[g_music_service seek:position];
    if([value[@"playing"] isKindOfClass:NSNumber.class] && [value[@"playing"] boolValue])[g_music_service playPause];
}
static void mac_music_shutdown(void) {[g_music_service shutdown];[g_music_service release];g_music_service=nil;}
