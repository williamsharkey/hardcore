#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <math.h>
static NSString *record_root(void) {return @"/tmp/hardcore-fixture-recordings";}
static NSString *record_resource(NSString *name) {return name;}
static void record_status(NSString *message) {(void)message;}
#include "../../hardcore_recording_library.m"

/* Offscreen/inactive AppKit lifecycle probe: never activates or creates a
 * browser/capture view. It checks ownership and ordering, not active OS clicks. */
static void pump(double seconds) {
    NSDate *until=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(until.timeIntervalSinceNow>0) {
        NSEvent *event=[NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate dateWithTimeIntervalSinceNow:.005] inMode:NSDefaultRunLoopMode dequeue:YES];
        if(event)[NSApp sendEvent:event];
        [NSApp updateWindows];
    }
}
static NSWindow *frontVisibleWindow(void) {
    for(NSWindow *window in NSApp.orderedWindows)if(window.visible)return window;
    return nil;
}
static BOOL postMouse(NSWindow *window,NSEventType type) {
    NSEvent *event=[NSEvent mouseEventWithType:type location:NSMakePoint(100,100) modifierFlags:0 timestamp:NSProcessInfo.processInfo.systemUptime windowNumber:window.windowNumber context:nil eventNumber:1 clickCount:1 pressure:type==NSEventTypeLeftMouseDown?1:0];
    if(!event)return NO;
    [NSApp postEvent:event atStart:NO];pump(.03);return YES;
}
static int library_window_check(void) {
    @autoreleasepool {
        [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];[NSApp finishLaunching];
        NSWindow *main=[[NSWindow alloc] initWithContentRect:NSMakeRect(-10000,-10000,1000,800) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:NO];
        main.title=@"Library test main window";main.alphaValue=0;main.releasedWhenClosed=NO;
        /* Prepare the production singleton before opening so it never flashes
           onscreen. An inner pool catches accidental autorelease ownership. */
        @autoreleasepool {
            g_record_library=[[HCRecordingLibrary alloc] init];
            g_record_library.window.alphaValue=0;
            [g_record_library.window setFrameOrigin:NSMakePoint(-10000,-10000)];
        }
        NSWindow *library=g_record_library.window;
        if(!library || library.releasedWhenClosed || library.windowController!=g_record_library)return 1;
        [main makeKeyAndOrderFront:nil];pump(.03);
        __block NSUInteger down=0,up=0,closes=0;
        __block BOOL openOnRelease=NO,armed=NO;
        id observer=[NSNotificationCenter.defaultCenter addObserverForName:NSWindowWillCloseNotification object:library queue:nil usingBlock:^(NSNotification *note){(void)note;closes++;}];
        id monitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown|NSEventMaskLeftMouseUp handler:^NSEvent *(NSEvent *event){
            if(event.window!=main)return event;
            if(event.type==NSEventTypeLeftMouseDown){down++;armed=YES;if(!openOnRelease)mac_record_library();}
            else {up++;if(openOnRelease && armed)mac_record_library();armed=NO;}
            return nil;
        }];
        for(int cycle=0;cycle<4;cycle++) {
            openOnRelease=cycle%2;
            NSUInteger closedBefore=closes;
            @autoreleasepool {
                if(!postMouse(main,NSEventTypeLeftMouseDown))return 2;
                if(!openOnRelease && (!library.visible || frontVisibleWindow()!=library))return 3;
                if(openOnRelease && library.visible)return 4;
                if(!postMouse(main,NSEventTypeLeftMouseUp))return 5;
            }
            pump(.03);
            if(!library.visible || frontVisibleWindow()!=library || closes!=closedBefore)return 6;
            if(g_record_library.window!=library || library.windowController!=g_record_library)return 7;
            [library close];pump(.03);
            if(library.visible || closes!=closedBefore+1)return 8;
        }
        [NSEvent removeMonitor:monitor];[NSNotificationCenter.defaultCenter removeObserver:observer];
        BOOL remainedInactive=!NSApp.active;
        [main orderOut:nil];[main close];[main release];[g_record_library release];g_record_library=nil;
        if(down!=4 || up!=4 || closes!=4 || !remainedInactive)return 9;
        puts("recording library window: retained across autorelease pools, consumed press/release, front ordering, and four close/reopen cycles passed (inactive app; no devices)");
    }
    return 0;
}

int main(void) { @autoreleasepool {
    NSString *why=nil;
    NSDictionary *edits=record_library_edits(@{@"startMs":@100,@"endMs":@200,@"crop":@[@0,@0,@100,@100],@"captionItems":@[@{@"start":@0,@"end":@1000,@"text":@"Hello\n\nWorld"}]},&why);
    if(!edits || ![record_library_srt(edits[@"captionItems"]) isEqual:@"1\n00:00:00,000 --> 00:00:01,000\nHello\nWorld\n\n"])return 1;
    if(record_library_edits(@{@"startMs":@200,@"endMs":@100},&why))return 2;
    if(record_library_edits(@{@"startMs":@0,@"endMs":@100,@"crop":@[@0,@0,@0,@10]},&why))return 3;
    if(record_library_edits(@{@"startMs":@0,@"endMs":@100,@"pointer":@"yes"},&why))return 4;
    if(!record_library_local_document([NSURL fileURLWithPath:@"/tmp/example.hcrec/index.html"],@"/tmp/example.hcrec"))return 5;
    if(record_library_local_document([NSURL URLWithString:@"https://example.com/index.html"],@"/tmp/example.hcrec"))return 6;
    if(record_library_local_document([NSURL fileURLWithPath:@"/tmp/other.hcrec/index.html"],@"/tmp/example.hcrec"))return 7;
    if(record_library_local_document([NSURL fileURLWithPath:@"/tmp/example.hcrec/player.js"],@"/tmp/example.hcrec"))return 8;
    if(record_library_edits(@{@"startMs":@{},@"endMs":@100},&why))return 9;
    NSFileManager *fm=NSFileManager.defaultManager;
    NSString *root=record_library_temporary();
    NSString *destination=[root stringByAppendingPathComponent:@"kept.txt"],*source=[root stringByAppendingPathComponent:@"source.txt"];
    [@"previous" writeToFile:destination atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [@"replacement" writeToFile:source atomically:YES encoding:NSUTF8StringEncoding error:nil];
    for(int pass=0;pass<2;pass++) {
        NSString *temporary=record_library_temporary(),*staged=record_library_stage(destination);
        HCRecordingJob *job=[[HCRecordingJob alloc] init];
        __block BOOL done=NO,success=NO;
        [job start:pass?@"/bin/cp":@"/usr/bin/false" arguments:pass?@[source,staged]:@[] staged:staged destination:destination temporary:temporary completion:^(BOOL ok,NSString *message){(void)message;success=ok;done=YES;}];
        NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:10];
        while(!done && [deadline timeIntervalSinceNow]>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
        if(!done || success!=(pass==1))return 10+pass;
        NSString *content=[NSString stringWithContentsOfFile:destination encoding:NSUTF8StringEncoding error:nil];
        if(![content isEqual:pass?@"replacement":@"previous"])return 12+pass;
        if([fm fileExistsAtPath:temporary] || [fm fileExistsAtPath:staged])return 14+pass;
        [job release];
    }
    [fm removeItemAtPath:root error:nil];
    int windowResult=library_window_check();
    if(windowResult){fprintf(stderr,"library window check failed: %d\n",windowResult);return 20+windowResult;}
    puts("recording library: edit validation, local bridge boundaries and atomic export jobs passed");return 0;
} }
