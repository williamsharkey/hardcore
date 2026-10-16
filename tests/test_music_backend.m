/* Deterministic transport/catalog tests: no network, speakers, or devices. */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#include <stdio.h>
#include <math.h>

@interface MusicTestItem : NSObject
@property AVPlayerItemStatus status;
@property CMTime duration;
@property NSTimeInterval preferredForwardBufferDuration;
@end
@implementation MusicTestItem
@end
static int playersMade,playCalls,pauseCalls,seeks,notifications;
static NSURL *lastSource;
@interface MusicTestTask : NSObject
@property BOOL cancelled;
- (void)cancel;
@end
@implementation MusicTestTask
- (void)cancel {self.cancelled=YES;}
@end
@interface MusicTestPlayer : NSObject
@property(retain) MusicTestItem *currentItem;
@property BOOL automaticallyWaitsToMinimizeStalling;
@property(getter=isMuted) BOOL muted;
@property AVPlayerTimeControlStatus timeControlStatus;
@property CMTime currentTime;
- (void)play;
- (void)pause;
- (void)seekToTime:(CMTime)t toleranceBefore:(CMTime)b toleranceAfter:(CMTime)a completionHandler:(void (^)(BOOL))completion;
@end
@implementation MusicTestPlayer
- (void)play {playCalls++;self.timeControlStatus=AVPlayerTimeControlStatusPlaying;}
- (void)pause {pauseCalls++;self.timeControlStatus=AVPlayerTimeControlStatusPaused;}
- (void)seekToTime:(CMTime)t toleranceBefore:(CMTime)b toleranceAfter:(CMTime)a completionHandler:(void (^)(BOOL))completion {
    (void)b;(void)a;seeks++;self.currentTime=t;completion(YES);
}
- (void)dealloc {[_currentItem release];[super dealloc];}
@end
static id testItem(NSURL *url) {
    [lastSource release];lastSource=[url copy];
    MusicTestItem *item=[[[MusicTestItem alloc] init] autorelease];item.status=AVPlayerItemStatusReadyToPlay;item.duration=CMTimeMake(90,1);return item;
}
static id testPlayer(id item) {
    playersMade++;MusicTestPlayer *player=[[[MusicTestPlayer alloc] init] autorelease];player.currentItem=item;player.currentTime=kCMTimeZero;return player;
}
#define MUSIC_PLAYER_ITEM(url) ((AVPlayerItem *)testItem(url))
#define MUSIC_PLAYER(item) ((AVPlayer *)testPlayer(item))
static void mac_music_changed(void) {notifications++;}
#include "../hardcore_mac_music.m"
#define CHECK(condition) do{if(!(condition)){fprintf(stderr,"music check failed line %d: %s\n",__LINE__,#condition);return 1;}}while(0)

static NSString *itemXML(NSString *title,NSString *file,NSString *duration) {
    return [NSString stringWithFormat:@"<item><title>%@</title><link>http://musicforprogramming.net/one</link><itunes:duration>%@</itunes:duration><enclosure url=\"%@\" type=\"audio/mpeg\"/></item>",title,duration,file];
}
static NSData *rss(NSString *body) {
    return [[NSString stringWithFormat:@"<rss xmlns:itunes=\"http://www.itunes.com/dtds/podcast-1.0.dtd\"><channel>%@</channel></rss>",body] dataUsingEncoding:NSUTF8StringEncoding];
}
static void pump(double seconds) {
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(deadline.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
}
int main(int argc,char **argv) {@autoreleasepool {
    if(argc==2){NSArray *feed=music_parse_feed(music_read_bounded(@(argv[1])));CHECK(feed.count>0);printf("Official RSS: %lu episodes; %s\n",(unsigned long)feed.count,[feed.firstObject[@"title"] UTF8String]);return 0;}
    CHECK(music_duration(@"4:00:00")==14400 && music_duration(@"1:30")==90);
    CHECK(music_duration(@"1:60")==0 && music_duration(@"-20")==0 && music_duration(@"Infinity")==0 && music_duration(@"1::2")==0);
    CHECK([music_time(3661) isEqual:@"1:01:01"] && [music_time(NAN) isEqual:@"0:00"]);
    CHECK([music_url(@"http://datashat.net/test.mp3",YES).scheme isEqual:@"https"]);
    CHECK(!music_url(@"https://evil.test/file.mp3",YES) && !music_url(@"file:///tmp/file.mp3",YES));
    CHECK(!music_url(@"https://user:password@datashat.net/file.mp3",YES) && !music_url(@"https://datashat.net:443/file.mp3",YES));
    CHECK(!music_url(@"https://datashat.net/file.html",YES) && !music_url(@"https://musicforprogramming.net.evil.test/one",NO));
    NSString *one=itemXML(@"Episode 79: Café &amp; Space",@"https://datashat.net/music_for_programming_79.mp3",@"4:00:00");
    NSString *two=itemXML(@"Episode 78: Datassette",@"https://datashat.net/music_for_programming_78.mp3",@"1:30:00");
    NSData *feed=rss([one stringByAppendingString:two]);NSArray *catalog=music_parse_feed(feed);
    CHECK(catalog.count==2 && [catalog[0][@"shortTitle"] isEqual:@"79: Café & Space"] && [catalog[0][@"episode"] intValue]==79);
    CHECK([catalog[0][@"duration"] intValue]==14400 && [catalog[0][@"page"] hasPrefix:@"https://"]);
    CHECK(music_parse_feed(rss([one stringByAppendingString:one])).count==1);
    CHECK(!music_parse_feed(rss(itemXML(@"Bad",@"https://attacker.test/test.mp3",@"1:00"))));
    CHECK(!music_parse_feed([@"<!DOCTYPE rss [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><rss/>" dataUsingEncoding:NSUTF8StringEncoding]));
    CHECK(!music_parse_feed([@"<rss><item><title>broken" dataUsingEncoding:NSUTF8StringEncoding]));
    CHECK(!music_parse_feed([NSMutableData dataWithLength:MUSIC_FEED_LIMIT+1]));
    NSMutableString *many=[NSMutableString string];for(int i=0;i<513;i++)[many appendString:one];CHECK(!music_parse_feed(rss(many)));
    NSMutableString *deep=[NSMutableString string];for(int i=0;i<20;i++)[deep appendString:@"<x>"];for(int i=0;i<20;i++)[deep appendString:@"</x>"];CHECK(!music_parse_feed(rss(deep)));
    HCMusic *transport=[[[HCMusic alloc] init] autorelease];
    NSURLSession *testSession=NSURLSession.sharedSession;MusicTestTask *task=[[[MusicTestTask alloc] init] autorelease];
    __block NSURLSessionResponseDisposition disposition=NSURLSessionResponseAllow;
    NSURLResponse *large=[[[NSURLResponse alloc] initWithURL:[NSURL URLWithString:music_feed_url] MIMEType:@"application/xml" expectedContentLength:MUSIC_FEED_LIMIT+1 textEncodingName:@"utf-8"] autorelease];
    [transport URLSession:testSession dataTask:(NSURLSessionDataTask *)task didReceiveResponse:large completionHandler:^(NSURLSessionResponseDisposition answer){disposition=answer;}];CHECK(disposition==NSURLSessionResponseCancel);
    NSHTTPURLResponse *bad=[[[NSHTTPURLResponse alloc] initWithURL:[NSURL URLWithString:music_feed_url] statusCode:503 HTTPVersion:@"HTTP/1.1" headerFields:@{}] autorelease];
    [transport URLSession:testSession dataTask:(NSURLSessionDataTask *)task didReceiveResponse:bad completionHandler:^(NSURLSessionResponseDisposition answer){disposition=answer;}];CHECK(disposition==NSURLSessionResponseCancel);
    __block BOOL redirected=YES;
    [transport URLSession:testSession task:(NSURLSessionTask *)task willPerformHTTPRedirection:bad newRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://other.test/feed"]] completionHandler:^(NSURLRequest *request){redirected=request!=nil;}];CHECK(!redirected);
    [transport URLSession:testSession task:(NSURLSessionTask *)task willPerformHTTPRedirection:bad newRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:music_feed_url]] completionHandler:^(NSURLRequest *request){redirected=request!=nil;}];CHECK(redirected);
    transport->received=[[NSMutableData alloc] initWithLength:MUSIC_FEED_LIMIT];
    [transport URLSession:testSession dataTask:(NSURLSessionDataTask *)task didReceiveData:[NSData dataWithBytes:"x" length:1]];CHECK(task.cancelled && transport->received.length==MUSIC_FEED_LIMIT);

    /* Test-only catalog path is honored only inside the owned control harness. */
    NSString *directory=[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *path=[directory stringByAppendingPathComponent:@"feed.xml"];CHECK([feed writeToFile:path atomically:YES]);
    unsetenv("HARDCORE_CTL");setenv("HARDCORE_TEST_MUSIC_CATALOG",path.fileSystemRepresentation,1);
    CHECK(!music_test_path("HARDCORE_TEST_MUSIC_CATALOG"));setenv("HARDCORE_CTL","owned-music-test",1);
    mac_music_load();CHECK([mac_music_status()[@"loading"] boolValue]);
    for(int i=0;i<200 && [mac_music_status()[@"loading"] boolValue];i++)pump(.01);
    CHECK(![mac_music_status()[@"loading"] boolValue] && mac_music_items().count==2);
    CHECK(playersMade==0 && ![mac_music_status()[@"playing"] boolValue]);
    mac_music_select(1,0);CHECK([mac_music_status()[@"index"] intValue]==1 && playersMade==0);
    mac_music_next(1);CHECK([mac_music_status()[@"index"] intValue]==0 && playersMade==0);
    mac_music_next(-1);CHECK([mac_music_status()[@"index"] intValue]==1);
    mac_music_select(-1,1);mac_music_select(500,1);CHECK(playersMade==0);
    mac_music_seek(30);NSDictionary *saved=[[mac_music_pack() copy] autorelease];
    CHECK([saved[@"position"] intValue]==30 && ![saved[@"playing"] boolValue]);
    mac_music_shutdown();mac_music_restore(saved);CHECK(playersMade==0 && [mac_music_status()[@"position"] intValue]==30);

    setenv("HARDCORE_TEST_MUSIC_MEDIA","/tmp/owned-silent-audio.wav",1);
    mac_music_play_pause();CHECK(playersMade==1 && [mac_music_status()[@"playing"] boolValue]);
    CHECK(lastSource.isFileURL && g_music_service->player.muted);
    [g_music_service tick];pump(.02);CHECK(seeks==1 && playCalls==1 && [mac_music_status()[@"position"] intValue]==30);
    mac_music_play_pause();CHECK(![mac_music_status()[@"playing"] boolValue] && !g_music_service->timer);
    mac_music_next(1);CHECK([mac_music_status()[@"index"] intValue]==0 && playersMade==1);
    mac_music_play_pause();CHECK(playersMade==2 && [mac_music_status()[@"playing"] boolValue]);
    [g_music_service tick];CHECK(![mac_music_status()[@"buffering"] boolValue]);
    NSDictionary *playing=[[mac_music_pack() copy] autorelease];
    mac_music_shutdown();mac_music_restore(playing);CHECK(playersMade==3 && [mac_music_status()[@"playing"] boolValue]);
    [NSNotificationCenter.defaultCenter postNotificationName:AVPlayerItemDidPlayToEndTimeNotification object:g_music_service->player.currentItem];
    CHECK([mac_music_status()[@"index"] intValue]==1 && playersMade==4 && [mac_music_status()[@"playing"] boolValue]);
    ((MusicTestItem *)g_music_service->player.currentItem).status=AVPlayerItemStatusFailed;[g_music_service tick];
    CHECK(![mac_music_status()[@"playing"] boolValue] && [mac_music_status()[@"error"] length]>0);
    mac_music_play_pause();CHECK(playersMade==5 && [mac_music_status()[@"error"] length]==0);
    [g_music_service acceptItems:@[catalog[1],catalog[0]]];CHECK([mac_music_status()[@"index"] intValue]==0 && playersMade==5);
    [g_music_service acceptItems:@[catalog[0]]];CHECK([mac_music_status()[@"index"] intValue]==1 && mac_music_items().count==2 && playersMade==5);
    mac_music_stop();CHECK(![mac_music_status()[@"playing"] boolValue] && [mac_music_status()[@"position"] intValue]==0 && !g_music_service->player);
    mac_music_seek(NAN);CHECK([mac_music_status()[@"position"] intValue]==0);
    NSDictionary *before=[[mac_music_pack() copy] autorelease];
    mac_music_restore(@{@"items":@[catalog[0]],@"index":@100,@"position":@0,@"playing":@YES});CHECK([before isEqual:mac_music_pack()]);
    mac_music_restore(@{@"items":@[catalog[0]],@"index":@0,@"position":@(NAN),@"playing":@YES});CHECK([before isEqual:mac_music_pack()]);
    mac_music_restore(@{@"items":@[@{@"audio":@"file:///tmp/test.mp3",@"title":@"bad"}],@"index":@0,@"position":@0});CHECK([before isEqual:mac_music_pack()]);
    mac_music_shutdown();

    /* Failed refresh does not discard usable old catalog or start playback. */
    mac_music_init();[g_music_service acceptItems:catalog];
    CHECK([@"invalid" writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    mac_music_load();for(int i=0;i<200 && [mac_music_status()[@"loading"] boolValue];i++)pump(.01);
    CHECK(mac_music_items().count==2 && [mac_music_status()[@"catalogError"] length]>0 && ![mac_music_status()[@"playing"] boolValue]);
    mac_music_shutdown();
    mac_music_play_pause();for(int i=0;i<200 && [mac_music_status()[@"loading"] boolValue];i++)pump(.01);
    CHECK(![mac_music_status()[@"playing"] boolValue] && [mac_music_status()[@"catalogError"] length]>0);
    mac_music_shutdown();
    /* A queued async read cannot resurrect a shut-down service. */
    mac_music_load();mac_music_shutdown();pump(.05);CHECK(!g_music_service);
    mac_music_init();[g_music_service acceptItems:catalog];mac_music_select(0,1);
    HCMusic *service=g_music_service;AVPlayerItem *ended=service->player.currentItem;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{
        [NSNotificationCenter.defaultCenter postNotificationName:AVPlayerItemDidPlayToEndTimeNotification object:ended];
    });
    for(int i=0;i<100 && [mac_music_status()[@"index"] intValue]!=1;i++)pump(.005);
    CHECK([mac_music_status()[@"index"] intValue]==1 && [mac_music_status()[@"playing"] boolValue]);
    mac_music_shutdown();
    [NSFileManager.defaultManager removeItemAtPath:directory error:nil];[lastSource release];
    printf("Music backend: parser, bounded catalog, explicit playback, transport, retry, end advance, and reload state passed (%d changes).\n",notifications);
    return 0;
}}
