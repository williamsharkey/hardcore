/* No windows, browser, capture devices, or installed helper are used here.
 * Tiny child processes exercise production ownership and output publication. */
#import <Foundation/Foundation.h>
#include <math.h>
static NSString *testRoot,*testTool,*lastStatus;
static int notifications;
static NSString *record_root(void) {return testRoot;}
static NSString *record_resource(NSString *name) {(void)name;return testTool;}
static void record_status(NSString *message) {[lastStatus release];lastStatus=[message copy];}
static void record_jobs_changed(void) {notifications++;}
#include "../../hardcore_recording_jobs.m"
#define CHECK(test) do {if(!(test)){fprintf(stderr,"recording jobs check failed at line %d: %s\n",__LINE__,#test);return 1;}} while(0)
static void pump(double seconds) {
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(deadline.timeIntervalSinceNow>0)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
}
static BOOL waitJobs(void) {
    NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:10];
    while(mac_record_exports_active() && deadline.timeIntervalSinceNow>0)pump(.01);
    return !mac_record_exports_active();
}
static NSDictionary *latest(void) {return mac_record_jobs().lastObject;}
static BOOL writeJSON(NSString *path,id value) {return [[NSJSONSerialization dataWithJSONObject:value options:0 error:nil] writeToFile:path atomically:YES];}
static NSString *bundle(NSString *name,NSString *title,NSString *date) {
    NSString *path=[testRoot stringByAppendingPathComponent:name];
    [NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil];
    writeJSON([path stringByAppendingPathComponent:@"manifest.json"],@{@"title":title,@"createdAt":date,@"durationMs":@2000});
    return path;
}
static int lowLevelJobs(void) {
    NSFileManager *fm=NSFileManager.defaultManager;
    NSString *root=record_library_temporary();
    NSString *destination=[root stringByAppendingPathComponent:@"kept.txt"],*source=[root stringByAppendingPathComponent:@"source.txt"];
    [@"previous" writeToFile:destination atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [@"replacement" writeToFile:source atomically:YES encoding:NSUTF8StringEncoding error:nil];
    for(int pass=0;pass<5;pass++) {
        NSString *temporary=record_library_temporary(),*staged=record_library_stage(destination);
        HCRecordingJob *job=[[HCRecordingJob alloc] init];job.replaceExisting=pass!=2;
        __block BOOL done=NO,success=NO,hasMessage=NO;
        NSString *tool=pass==0?@"/usr/bin/false":pass==3?@"/nonexistent/hardcore-export-test":pass==4?@"/usr/bin/true":@"/bin/cp";
        [job start:tool arguments:pass==1 || pass==2?@[source,staged]:@[] staged:staged destination:destination temporary:temporary completion:^(BOOL ok,NSString *message){hasMessage=message.length>0;success=ok;done=YES;}];
        NSDate *deadline=[NSDate dateWithTimeIntervalSinceNow:10];
        while(!done && deadline.timeIntervalSinceNow>0)pump(.01);
        CHECK(done && hasMessage && success==(pass==1));
        NSString *content=[NSString stringWithContentsOfFile:destination encoding:NSUTF8StringEncoding error:nil];
        CHECK([content isEqual:pass?@"replacement":@"previous"]);
        CHECK(![fm fileExistsAtPath:temporary] && ![fm fileExistsAtPath:staged]);
        [job release];
    }
    [fm removeItemAtPath:root error:nil];return 0;
}
int main(void) { @autoreleasepool {
    NSString *why=nil;
    NSDictionary *edits=record_library_edits(@{@"startMs":@100,@"endMs":@1200,@"crop":@[@0,@0,@100,@100],@"pointer":@NO,@"captionItems":@[@{@"start":@0,@"end":@1000,@"text":@"Hello\n\nWorld"}]},&why);
    CHECK(edits && [record_library_srt(edits[@"captionItems"]) isEqual:@"1\n00:00:00,000 --> 00:00:01,000\nHello\nWorld\n\n"]);
    CHECK(!record_library_edits(@{@"startMs":@200,@"endMs":@100},&why));
    CHECK(!record_library_edits(@{@"startMs":@0,@"endMs":@100,@"crop":@[@0,@0,@0,@10]},&why));
    CHECK(!record_library_edits(@{@"startMs":@0,@"endMs":@100,@"pointer":@"yes"},&why));
    CHECK(!record_library_edits(@{@"startMs":@{},@"endMs":@100},&why));
    CHECK(record_library_local_document([NSURL fileURLWithPath:@"/tmp/example.hcrec/index.html"],@"/tmp/example.hcrec"));
    CHECK(!record_library_local_document([NSURL URLWithString:@"https://example.com/index.html"],@"/tmp/example.hcrec"));
    CHECK(!record_library_local_document([NSURL fileURLWithPath:@"/tmp/other.hcrec/index.html"],@"/tmp/example.hcrec"));
    CHECK(!record_library_local_document([NSURL fileURLWithPath:@"/tmp/example.hcrec/player.js"],@"/tmp/example.hcrec"));
    CHECK(record_jobs_progress(@"noise\nhardcore export: 2/10 frames\nhardcore export: 7/10 frames\nhardcore export: 8/")==.7);
    CHECK(record_jobs_progress(@"hardcore export: 0/0 frames\nhardcore export: -1/10 frames\nhardcore export: 2/1 frames\nhardcore export: 2x/10 frames\nhardcore export: 1/2frames\n")==-1);
    CHECK(record_jobs_progress(@"hardcore export: 10/10 frames\n")==1);
    CHECK(lowLevelJobs()==0);

    NSFileManager *fm=NSFileManager.defaultManager;
    testRoot=[record_library_temporary() copy];CHECK(testRoot);
    testTool=[[testRoot stringByAppendingPathComponent:@"synthetic-exporter"] copy];
    NSString *script=@"#!/bin/sh\n"
      "printf 'hardcore export: 2/10 frames\\n' >&2\n"
      "if [ -f \"$1/slow\" ]; then sleep 1.2; fi\n"
      "if [ -f \"$1/fail\" ]; then printf 'Synthetic encoder failure\\n' >&2; exit 7; fi\n"
      "out=$2\n"
      "printf '%s\\n' \"$@\" > \"$out\"\n"
      "while [ \"$#\" -gt 0 ]; do\n"
      "  if [ \"$1\" = --captions ]; then shift; cat \"$1\" >> \"$out\"; fi\n"
      "  shift\n"
      "done\n"
      "printf 'hardcore export: 10/10 frames\\n' >&2\n";
    CHECK([fm createFileAtPath:testTool contents:[script dataUsingEncoding:NSUTF8StringEncoding] attributes:@{NSFilePosixPermissions:@0700}]);
    NSString *first=bundle(@"older.hcrec",@"First clip",@"2026-01-01T00:00:00Z");
    NSString *second=bundle(@"newer.hcrec",@"Second clip",@"2026-02-01T00:00:00Z");
    bundle(@"unfinished.partial.hcrec",@"Partial",@"2026-03-01T00:00:00Z");
    NSString *malformed=bundle(@"bad.hcrec",@"Bad",@"2026-03-01T00:00:00Z");
    writeJSON([malformed stringByAppendingPathComponent:@"manifest.json"],@{@"durationMs":@(-1)});
    NSArray *items=mac_record_items();CHECK(items.count==2 && [items[0][@"path"] isEqual:second]);
    CHECK([items[0][@"duration"] isEqual:@"0:02"] && [items[0][@"durationMs"] doubleValue]==2000);

    [fm createFileAtPath:[first stringByAppendingPathComponent:@"slow"] contents:[NSData data] attributes:nil];
    int initialNotifications=notifications;
    @autoreleasepool {CHECK(mac_record_export(first,nil,NO));}
    CHECK(mac_record_exports_active()==1 && g_record_job_timer);
    CHECK(!mac_record_export(first,nil,NO));
    CHECK([lastStatus containsString:@"already running"]);
    NSArray *initial=[mac_record_jobs() retain];CHECK(mac_record_jobs()==initial);
    CHECK(!initial[0][@"runner"] && [initial[0][@"state"] isEqual:@"running"]);
    pump(.7); /* No owner or UI calls tick: the manager timer publishes progress. */
    CHECK([latest()[@"progress"] doubleValue]==.2 && notifications>initialNotifications+1);
    CHECK([initial[0][@"progress"] doubleValue]==-1);[initial release];
    CHECK(waitJobs() && !g_record_job_timer && [latest()[@"state"] isEqual:@"done"]);
    NSString *firstOutput=[latest()[@"output"] copy];CHECK([fm fileExistsAtPath:firstOutput]);
    NSString *text=[NSString stringWithContentsOfFile:firstOutput encoding:NSUTF8StringEncoding error:nil];
    CHECK(![text containsString:@"--captions"] && [text containsString:@"2.000000"]);
    CHECK([[fm attributesOfItemAtPath:firstOutput error:nil] filePosixPermissions]==0600);
    [fm removeItemAtPath:[first stringByAppendingPathComponent:@"slow"] error:nil];

    CHECK(writeJSON([first stringByAppendingPathComponent:@"edits.json"],edits));
    CHECK(mac_record_export(first,nil,NO) && waitJobs());
    CHECK([latest()[@"state"] isEqual:@"done"] && ![latest()[@"output"] isEqual:firstOutput]);
    CHECK([fm fileExistsAtPath:firstOutput]);
    text=[NSString stringWithContentsOfFile:latest()[@"output"] encoding:NSUTF8StringEncoding error:nil];
    CHECK([text containsString:@"--captions"] && [text containsString:@"Hello\nWorld"] && [text containsString:@"--no-pointer"] && [text containsString:@"0.100000"] && [text containsString:@"1.200000"] && [text containsString:@"0,0,100,100"]);
    CHECK(!mac_record_export(first,@{@"startMs":@0,@"endMs":@3000},NO));
    CHECK([lastStatus containsString:@"exceeds"]);

    [fm createFileAtPath:[second stringByAppendingPathComponent:@"fail"] contents:[NSData data] attributes:nil];
    CHECK(mac_record_export(second,nil,NO) && waitJobs());
    CHECK([latest()[@"state"] isEqual:@"failed"] && [latest()[@"message"] containsString:@"Synthetic encoder failure"]);
    CHECK(![fm fileExistsAtPath:latest()[@"output"]] && !g_record_job_timer);
    for(NSString *name in [fm contentsOfDirectoryAtPath:[testRoot stringByAppendingPathComponent:@"Exports"] error:nil])CHECK(![name hasPrefix:@".hardcore-"]);

    /* A concurrent writer must survive the job's final publication intact. */
    [fm removeItemAtPath:[second stringByAppendingPathComponent:@"fail"] error:nil];
    [fm createFileAtPath:[second stringByAppendingPathComponent:@"slow"] contents:[NSData data] attributes:nil];
    CHECK(mac_record_export(second,nil,NO));
    NSString *collision=[latest()[@"output"] copy];
    CHECK([@"keep this output" writeToFile:collision atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    CHECK(waitJobs() && [latest()[@"state"] isEqual:@"failed"]);
    CHECK([[NSString stringWithContentsOfFile:collision encoding:NSUTF8StringEncoding error:nil] isEqual:@"keep this output"]);[collision release];

    CHECK(mac_record_export(first,nil,YES) && waitJobs());
    CHECK([latest()[@"state"] isEqual:@"done"] && [latest()[@"kind"] isEqual:@"zip"]);
    NSData *zip=[NSData dataWithContentsOfFile:latest()[@"output"]];CHECK(zip.length>4 && !memcmp(zip.bytes,"PK",2));
    NSString *oldIdentifier=[mac_record_jobs()[0][@"id"] copy];
    for(int i=0;i<33;i++)CHECK(mac_record_export(first,nil,NO) && waitJobs());
    CHECK(mac_record_jobs().count==32 && !mac_record_exports_active());
    for(NSDictionary *job in mac_record_jobs())CHECK(![job[@"id"] isEqual:oldIdentifier] && [job[@"state"] isEqual:@"done"]);
    CHECK([fm fileExistsAtPath:firstOutput]);[oldIdentifier release];
    [firstOutput release];[fm removeItemAtPath:testRoot error:nil];[testRoot release];[testTool release];[lastStatus release];
    [g_record_jobs release];g_record_jobs=nil;[g_record_job_snapshots release];g_record_job_snapshots=nil;
    puts("recording jobs: edit validation, bridge boundaries, progress, UI-independent lifetime, saved edits, ZIP, atomic outputs and failures passed (no windows or devices)");
    return 0;
} }
