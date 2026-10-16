#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#include <math.h>
static NSString *record_root(void) {return @"/tmp/hardcore-fixture-recordings";}
static NSString *record_resource(NSString *name) {return name;}
static void record_status(NSString *message) {(void)message;}
#include "../../hardcore_recording_library.m"
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
    puts("recording library: edit validation, local bridge boundaries and atomic export jobs passed");return 0;
} }
