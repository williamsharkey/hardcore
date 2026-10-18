/* One consented connection to the user's own Chrome, shared by every Hardcore
   process of this login. Chrome asks the user to Allow each new remote-debugging
   connection and closes hidden targets when their connection ends, so a
   background relay (`hardcore chrome relay`) holds that single connection.
   Windows, `hardcore here` and in-place reloads reconnect here without another
   prompt, and a reloaded window re-adopts its still-open pages.

   Clients speak plain CDP JSON, each message framed by a 4-byte big-endian
   length. The relay renumbers request ids, routes session messages to the
   client that attached the session, and broadcasts browser-level events.
   Pages created through the relay that no client re-attaches within a grace
   period are closed (a crashed window); the relay exits when idle or when
   Chrome goes away. The socket lives in the 0700 runtime directory and accepts
   only this user: the same trust as the window control sockets. */
#ifndef HARDCORE_CHROME_RELAY_H
#define HARDCORE_CHROME_RELAY_H
#include <sys/un.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <mach-o/dyld.h>

#define HC_RELAY_FRAME_MAX (32*1024*1024)
#define HC_RELAY_ORPHAN_GRACE 60.0   /* seconds a reload has to re-adopt its pages */
#define HC_RELAY_IDLE_EXIT 600.0     /* no clients, no pages: drop Chrome's banner */
extern char **environ;
/* Tests shorten the relay's timers. */
static double hc_relay_seconds(const char *name,double fallback) {
    const char *value=getenv(name);double seconds=value ? atof(value) : 0;
    return seconds>0 ? seconds : fallback;
}

static NSString *hc_chrome_runtime(void) {
    const char *override=getenv("HARDCORE_RUNTIME_DIR");
    NSString *dir=override && *override ? @(override) : [NSString stringWithFormat:@"/tmp/hardcore-%u",getuid()];
    struct stat st;
    if(mkdir(dir.fileSystemRepresentation,0700) && errno!=EEXIST)return nil;
    if(lstat(dir.fileSystemRepresentation,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode & 077))return nil;
    return dir;
}
static NSString *hc_chrome_relay_path(void) {
    NSString *dir=hc_chrome_runtime();
    return dir ? [dir stringByAppendingPathComponent:@"chrome-relay.sock"] : nil;
}
/* HARDCORE_CHROME_ATTACH names another browser's endpoint file (or its profile
   directory); otherwise the Dock profile's. */
static NSString *hc_chrome_attach_endpoint_path(void) {
    NSString *attach=NSProcessInfo.processInfo.environment[@"HARDCORE_CHROME_ATTACH"];
    if(!attach.length)return hc_chrome_regular_endpoint();
    NSString *path=attach.stringByExpandingTildeInPath;BOOL isDir=NO;
    if([NSFileManager.defaultManager fileExistsAtPath:path isDirectory:&isDir] && isDir)path=[path stringByAppendingPathComponent:@"DevToolsActivePort"];
    return path;
}

/* Writes may meet a full socket buffer: wait for room rather than drop a frame. */
static BOOL hc_relay_write_all(int fd,const void *bytes,size_t length) {
    const char *p=bytes;
    while(length) {
        ssize_t n=write(fd,p,length);
        if(n>0){p+=n;length-=(size_t)n;continue;}
        if(n<0 && errno==EINTR)continue;
        if(n<0 && errno==EAGAIN){struct pollfd w={fd,POLLOUT,0};if(poll(&w,1,10000)>0)continue;}
        return NO;
    }
    return YES;
}
static BOOL hc_relay_send_frame(int fd,NSData *data) {
    if(data.length>HC_RELAY_FRAME_MAX)return NO;
    uint32_t n=htonl((uint32_t)data.length);
    return hc_relay_write_all(fd,&n,sizeof(n)) && hc_relay_write_all(fd,data.bytes,data.length);
}
/* Drains a nonblocking socket into buffer and hands out whole frames.
   NO means end of stream or a malformed frame. */
static BOOL hc_relay_read_frames(int fd,NSMutableData *buffer,void (^frame)(NSData *)) {
    char chunk[65536];
    for(;;) {
        ssize_t n=read(fd,chunk,sizeof(chunk));
        if(n>0){[buffer appendBytes:chunk length:(NSUInteger)n];continue;}
        if(n<0 && errno==EINTR)continue;
        if(n<0 && errno==EAGAIN)break;
        return NO;
    }
    NSUInteger offset=0;const unsigned char *bytes=buffer.bytes;
    while(buffer.length-offset>=4) {
        uint32_t length;memcpy(&length,bytes+offset,4);length=ntohl(length);
        if(length>HC_RELAY_FRAME_MAX)return NO;
        if(buffer.length-offset-4<length)break;
        frame([buffer subdataWithRange:NSMakeRange(offset+4,length)]);
        offset+=4+length;
    }
    if(offset)[buffer replaceBytesInRange:NSMakeRange(0,offset) withBytes:NULL length:0];
    return YES;
}
static void hc_relay_socket_setup(int fd) {
    int yes=1;
    fcntl(fd,F_SETFD,FD_CLOEXEC);fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
    setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes));
}
static int hc_relay_dial(NSString *path) {
    struct sockaddr_un a={0};const char *p=path.fileSystemRepresentation;
    if(!p || strlen(p)>=sizeof(a.sun_path))return -1;
    a.sun_family=AF_UNIX;strcpy(a.sun_path,p);
    int fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0)return -1;
    uid_t uid;gid_t gid;
    if(connect(fd,(struct sockaddr *)&a,sizeof(a)) || getpeereid(fd,&uid,&gid) || uid!=getuid()){close(fd);return -1;}
    hc_relay_socket_setup(fd);return fd;
}
static int hc_relay_spawn(const char *mode) {
    char exe[PATH_MAX];uint32_t size=sizeof(exe);
    if(_NSGetExecutablePath(exe,&size))return -1;
    char *argv[]={exe,"chrome",(char *)mode,NULL};
    posix_spawnattr_t attr;posix_spawn_file_actions_t actions;
    posix_spawnattr_init(&attr);posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETSID|POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_init(&actions);
    for(int i=0;i<3;i++)posix_spawn_file_actions_addopen(&actions,i,"/dev/null",i ? O_WRONLY : O_RDONLY,0);
    pid_t pid=-1;int failed=posix_spawn(&pid,exe,&actions,&attr,argv,environ);
    posix_spawn_file_actions_destroy(&actions);posix_spawnattr_destroy(&attr);
    return failed ? -1 : pid;
}
/* Client side: reach the relay, starting it if needed. The relay is spawned
   through a short-lived launcher so it belongs to launchd, not this window. */
static int hc_chrome_relay_connect(NSError **error) {
    NSString *path=hc_chrome_relay_path();
    if(!path){if(error)*error=hc_chrome_error(@"Hardcore's runtime directory is unavailable.");return -1;}
    int fd=hc_relay_dial(path);if(fd>=0)return fd;
    pid_t launcher=hc_relay_spawn("relay-launch");
    if(launcher>0){int status;while(waitpid(launcher,&status,0)<0 && errno==EINTR){}}
    NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+5;
    while(NSDate.timeIntervalSinceReferenceDate<deadline) {
        if((fd=hc_relay_dial(path))>=0)return fd;
        [NSThread sleepForTimeInterval:0.05];
    }
    if(error)*error=hc_chrome_error(@"Hardcore could not start its Chrome connection helper.");
    return -1;
}

@interface HCRelayClient : NSObject {
@public int fd;BOOL gone;
    dispatch_source_t source;dispatch_queue_t writer;NSMutableData *buffer;
}
@end
@implementation HCRelayClient
- (void)dealloc {[buffer release];if(writer)dispatch_release(writer);if(source)dispatch_release(source);[super dealloc];}
@end

@interface HCRelay : NSObject {
@public
    NSURLSession *session;NSURLSessionWebSocketTask *socket;
    BOOL ready;
    NSMutableArray *queued;               /* @[client,data] waiting for Chrome's Allow */
    NSMutableSet *clients;
    NSMutableDictionary *routes;          /* relay id -> @[client|NSNull, client id, method, targetId] */
    NSMutableDictionary *sessionOwner;    /* sessionId -> client */
    NSMutableDictionary *sessionTarget;   /* sessionId -> targetId */
    NSMutableDictionary *pendingAttach;   /* targetId -> client */
    NSMutableDictionary *owned;           /* targetId -> orphaned-since date, or NSNull while attached */
    NSUInteger serial;
    NSTimeInterval idleSince;
    NSString *failure;                    /* set once the relay is going away */
}
@end
@implementation HCRelay
- (id)init {
    if((self=[super init])) {
        queued=[NSMutableArray new];clients=[NSMutableSet new];routes=[NSMutableDictionary new];
        sessionOwner=[NSMutableDictionary new];sessionTarget=[NSMutableDictionary new];
        pendingAttach=[NSMutableDictionary new];owned=[NSMutableDictionary new];
        idleSince=NSDate.timeIntervalSinceReferenceDate;
    }return self;
}
- (void)toClient:(HCRelayClient *)client value:(NSDictionary *)value {
    if(client->gone)return;
    NSData *data=[NSJSONSerialization dataWithJSONObject:value options:0 error:nil];if(!data)return;
    int fd=client->fd;
    dispatch_async(client->writer,^{
        if(!hc_relay_send_frame(fd,data))dispatch_async(dispatch_get_main_queue(),^{[self drop:client];});
    });
}
- (void)toChrome:(NSDictionary *)message {
    NSData *data=[NSJSONSerialization dataWithJSONObject:message options:0 error:nil];if(!data)return;
    NSString *string=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    [socket sendMessage:[[[NSURLSessionWebSocketMessage alloc] initWithString:string] autorelease] completionHandler:^(NSError *e){
        if(e)dispatch_async(dispatch_get_main_queue(),^{[self finish:@"Chrome closed Hardcore's connection."];});
    }];
}
- (void)internal:(NSString *)method params:(NSDictionary *)params {
    NSNumber *rid=@(++serial);routes[rid]=@[NSNull.null,@0,method,@""];
    [self toChrome:@{@"id":rid,@"method":method,@"params":params}];
}
/* Chrome refused, went away, or the user did not Allow in time. Clients see
   the reason and reconnect later through a fresh relay (a fresh prompt). The
   relay lingers briefly so a client still dialing in also gets the reason. */
- (void)finish:(NSString *)reason {
    if(failure)return;failure=[reason copy];
    for(HCRelayClient *client in clients.allObjects)[self refuse:client];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(3*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        unlink(hc_chrome_relay_path().fileSystemRepresentation);exit(0);
    });
}
- (void)refuse:(HCRelayClient *)client {
    [self toClient:client value:@{@"relayError":failure}];
    dispatch_sync(client->writer,^{});
    [self drop:client];
}
- (void)drop:(HCRelayClient *)client {
    if(client->gone)return;client->gone=YES;
    dispatch_source_cancel(client->source);
    for(NSString *sid in [sessionOwner allKeysForObject:client]) {
        [sessionOwner removeObjectForKey:sid];[sessionTarget removeObjectForKey:sid];
        if(ready && !failure)[self internal:@"Target.detachFromTarget" params:@{@"sessionId":sid}];
    }
    [pendingAttach removeObjectsForKeys:[pendingAttach allKeysForObject:client]];
    [clients removeObject:client];
}
- (void)client:(HCRelayClient *)client message:(NSData *)data {
    if(client->gone || failure)return;
    if(!ready){[queued addObject:@[client,data]];return;}
    NSMutableDictionary *message=[NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
    if(![message isKindOfClass:NSMutableDictionary.class] || !message[@"id"] || ![message[@"method"] isKindOfClass:NSString.class])return;
    NSDictionary *params=[message[@"params"] isKindOfClass:NSDictionary.class] ? message[@"params"] : @{};
    NSString *target=[params[@"targetId"] isKindOfClass:NSString.class] ? params[@"targetId"] : @"";
    NSNumber *rid=@(++serial);routes[rid]=@[client,message[@"id"],message[@"method"],target];
    if([message[@"method"] isEqualToString:@"Target.attachToTarget"] && target.length)pendingAttach[target]=client;
    message[@"id"]=rid;[self toChrome:message];
}
- (void)claim:(NSString *)sid target:(NSString *)target client:(HCRelayClient *)client {
    if(!sid.length || client->gone)return;
    sessionOwner[sid]=client;if(target.length)sessionTarget[sid]=target;
    if(target.length && owned[target])owned[target]=NSNull.null;
}
- (void)chrome:(NSData *)data {
    NSMutableDictionary *message=[NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil];
    if(![message isKindOfClass:NSMutableDictionary.class])return;
    NSNumber *rid=message[@"id"];
    if(rid) {
        NSArray *route=[[routes[rid] retain] autorelease];[routes removeObjectForKey:rid];
        if(route && route[0]==NSNull.null && [route[2] isEqualToString:@"Browser.getVersion"] && !ready) {
            /* Chrome answering means the user allowed this connection. */
            ready=YES;
            NSArray *backlog=[[queued copy] autorelease];[queued removeAllObjects];
            for(NSArray *item in backlog)[self client:item[0] message:item[1]];
        }
        if(!route || route[0]==NSNull.null)return;
        HCRelayClient *client=route[0];NSString *method=route[2];NSDictionary *result=message[@"result"];
        if([method isEqualToString:@"Target.createTarget"] && [result[@"targetId"] isKindOfClass:NSString.class])
            owned[result[@"targetId"]]=@(NSDate.timeIntervalSinceReferenceDate);
        if([method isEqualToString:@"Target.attachToTarget"] && [result[@"sessionId"] isKindOfClass:NSString.class]) {
            [self claim:result[@"sessionId"] target:route[3] client:client];
            if(client->gone)[self internal:@"Target.detachFromTarget" params:@{@"sessionId":result[@"sessionId"]}];
        }
        message[@"id"]=route[1];[self toClient:client value:message];return;
    }
    NSString *method=message[@"method"];NSDictionary *params=[message[@"params"] isKindOfClass:NSDictionary.class] ? message[@"params"] : @{};
    if([method isEqualToString:@"Target.attachedToTarget"]) {
        NSString *target=params[@"targetInfo"][@"targetId"];HCRelayClient *client=target ? pendingAttach[target] : nil;
        if(client){[self claim:params[@"sessionId"] target:target client:client];[pendingAttach removeObjectForKey:target];}
    } else if([method isEqualToString:@"Target.detachedFromTarget"] && params[@"sessionId"]) {
        [sessionOwner removeObjectForKey:params[@"sessionId"]];[sessionTarget removeObjectForKey:params[@"sessionId"]];
    } else if([method isEqualToString:@"Target.targetDestroyed"] && params[@"targetId"]) [owned removeObjectForKey:params[@"targetId"]];
    NSString *sid=message[@"sessionId"];
    if(sid){HCRelayClient *client=sessionOwner[sid];if(client)[self toClient:client value:message];}
    else for(HCRelayClient *client in clients)[self toClient:client value:message];
}
- (void)receive {
    [socket receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message,NSError *error){
        NSData *data=message.type==NSURLSessionWebSocketMessageTypeData ? message.data : [message.string dataUsingEncoding:NSUTF8StringEncoding];
        dispatch_async(dispatch_get_main_queue(),^{@autoreleasepool {
            if(error){[self finish:self->ready ? @"Chrome closed Hardcore's connection." : HC_CHROME_ALLOW_HELP];return;}
            [self chrome:data];
        }});
        if(!error)[self receive];
    }];
}
- (void)sweep {
    NSTimeInterval now=NSDate.timeIntervalSinceReferenceDate;
    NSSet *attached=[NSSet setWithArray:sessionTarget.allValues];
    for(NSString *target in owned.allKeys) {
        if([attached containsObject:target]){owned[target]=NSNull.null;continue;}
        if(owned[target]==NSNull.null){owned[target]=@(now);continue;}
        if(now-[owned[target] doubleValue]>=hc_relay_seconds("HARDCORE_CHROME_RELAY_GRACE",HC_RELAY_ORPHAN_GRACE)) {
            [owned removeObjectForKey:target];
            if(ready && !failure)[self internal:@"Target.closeTarget" params:@{@"targetId":target}];
        }
    }
    if(clients.count || owned.count)idleSince=now;
    else if(now-idleSince>=hc_relay_seconds("HARDCORE_CHROME_RELAY_IDLE",HC_RELAY_IDLE_EXIT))[self finish:@"Hardcore's Chrome connection helper exited while idle."];
}
- (void)accept:(int)listener {
    int fd=accept(listener,NULL,NULL);if(fd<0)return;
    uid_t uid;gid_t gid;
    if(getpeereid(fd,&uid,&gid) || uid!=getuid()){close(fd);return;}
    hc_relay_socket_setup(fd);
    HCRelayClient *client=[[HCRelayClient new] autorelease];
    client->fd=fd;client->buffer=[NSMutableData new];
    client->writer=dispatch_queue_create("hardcore.chrome.relay.client",DISPATCH_QUEUE_SERIAL);
    client->source=dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,(uintptr_t)fd,0,dispatch_get_main_queue());
    dispatch_queue_t writer=client->writer;dispatch_retain(writer);
    dispatch_source_set_event_handler(client->source,^{@autoreleasepool {
        if(client->gone)return;
        if(!hc_relay_read_frames(fd,client->buffer,^(NSData *frame){[self client:client message:frame];}))[self drop:client];
    }});
    /* Close only after queued writes, so a reused descriptor never gets them. */
    dispatch_source_set_cancel_handler(client->source,^{dispatch_async(writer,^{close(fd);});dispatch_release(writer);});
    [clients addObject:client];idleSince=NSDate.timeIntervalSinceReferenceDate;
    dispatch_resume(client->source);
    if(failure)[self refuse:client];
}
- (void)connectChrome {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY,0),^{@autoreleasepool {
        NSString *path=hc_chrome_attach_endpoint_path();
        BOOL regular=![NSProcessInfo.processInfo.environment[@"HARDCORE_CHROME_ATTACH"] length];
        NSURL *endpoint=hc_chrome_endpoint(path);
        if(!hc_chrome_listening(endpoint) && regular && !hc_chrome_regular_app()) {
            /* Start Chrome the way the Dock does, without taking focus; its
               remote-debugging allowance lives in the profile. */
            [NSTask launchedTaskWithExecutableURL:[NSURL fileURLWithPath:@"/usr/bin/open"] arguments:@[@"-g",@"-b",@"com.google.Chrome"] error:nil terminationHandler:nil];
            NSTimeInterval deadline=NSDate.timeIntervalSinceReferenceDate+15;
            while(!hc_chrome_listening(endpoint=hc_chrome_endpoint(path)) && NSDate.timeIntervalSinceReferenceDate<deadline)
                [NSThread sleepForTimeInterval:0.1];
        }
        BOOL listening=hc_chrome_listening(endpoint);
        dispatch_async(dispatch_get_main_queue(),^{
            if(!listening){[self finish:regular ? HC_CHROME_ALLOW_HELP : @"Chrome attach is unavailable. Enable chrome://inspect/#remote-debugging in Chrome, then retry."];return;}
            NSURLSessionConfiguration *config=[NSURLSessionConfiguration ephemeralSessionConfiguration];
            /* Chrome holds the handshake open until the user clicks Allow. */
            config.timeoutIntervalForRequest=120;config.timeoutIntervalForResource=DBL_MAX;
            self->session=[[NSURLSession sessionWithConfiguration:config] retain];
            self->socket=[[self->session webSocketTaskWithURL:endpoint] retain];
            self->socket.maximumMessageSize=HC_RELAY_FRAME_MAX;
            [self->socket resume];[self receive];
            NSNumber *rid=@(++self->serial);self->routes[rid]=@[NSNull.null,@0,@"Browser.getVersion",@""];
            [self->socket sendMessage:[[[NSURLSessionWebSocketMessage alloc] initWithString:[NSString stringWithFormat:@"{\"id\":%@,\"method\":\"Browser.getVersion\"}",rid]] autorelease]
                completionHandler:^(NSError *e){if(e)dispatch_async(dispatch_get_main_queue(),^{[self finish:HC_CHROME_ALLOW_HELP];});}];
        });
    }});
}
@end

/* `hardcore chrome relay-launch`: start the relay detached, then exit, so the
   relay is reparented to launchd rather than lingering as a window's child. */
static int hc_chrome_relay_launch_main(void) {
    return hc_relay_spawn("relay")>0 ? 0 : 1;
}
static int hc_chrome_relay_main(void) {
    @autoreleasepool {
        NSString *path=hc_chrome_relay_path();if(!path)return 1;
        NSString *lockPath=[path stringByAppendingString:@".lock"];
        int lock=open(lockPath.fileSystemRepresentation,O_CREAT|O_RDWR|O_CLOEXEC,0600);
        if(lock<0)return 1;
        if(flock(lock,LOCK_EX|LOCK_NB))return 0;          /* another relay already serves this login */
        signal(SIGPIPE,SIG_IGN);
        struct sockaddr_un a={0};const char *p=path.fileSystemRepresentation;
        if(strlen(p)>=sizeof(a.sun_path))return 1;
        a.sun_family=AF_UNIX;strcpy(a.sun_path,p);
        unlink(p);
        int listener=socket(AF_UNIX,SOCK_STREAM,0);
        mode_t old=umask(077);
        int bound=listener>=0 ? bind(listener,(struct sockaddr *)&a,sizeof(a)) : -1;
        umask(old);
        if(bound || listen(listener,16)){if(listener>=0)close(listener);return 1;}
        fcntl(listener,F_SETFD,FD_CLOEXEC);fcntl(listener,F_SETFL,O_NONBLOCK);
        HCRelay *relay=[HCRelay new];
        dispatch_source_t accepting=dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,(uintptr_t)listener,0,dispatch_get_main_queue());
        dispatch_source_set_event_handler(accepting,^{@autoreleasepool {[relay accept:listener];}});
        dispatch_resume(accepting);
        dispatch_source_t sweeping=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
        uint64_t period=(uint64_t)(MIN(5.0,hc_relay_seconds("HARDCORE_CHROME_RELAY_GRACE",HC_RELAY_ORPHAN_GRACE)/4)*NSEC_PER_SEC);
        dispatch_source_set_timer(sweeping,dispatch_time(DISPATCH_TIME_NOW,(int64_t)period),period,period/10);
        dispatch_source_set_event_handler(sweeping,^{@autoreleasepool {[relay sweep];}});
        dispatch_resume(sweeping);
        [relay connectChrome];
        dispatch_main();
    }
}
#endif
