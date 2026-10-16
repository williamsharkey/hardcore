/* Camera movies plus a lossless visual replay journal. Only bitmap font atlas
 * resources and drawing/event metadata are captured, never desktop screenshots.
 * Disk serialization runs on a bounded serial queue; failure preserves the draft. */
static dispatch_queue_t g_record_queue;
static NSString *g_record_dir;
static FILE *g_record_frames, *g_record_pointer;
static NSMutableArray *g_record_atlas;
static NSData *g_record_last_quads;
static NSDictionary *g_record_last_camera;
static int g_record_w,g_record_h,g_record_bg,g_record_writing,g_record_finishing,g_record_failed;
static size_t g_record_pending;
static unsigned g_record_generation;
static int g_record_disk_error,g_record_fixture;
static void record_fixture_finish(void);
static double g_record_duration;
static NSMutableDictionary *g_record_info;
static NSMutableArray *g_record_native_tabs;
static NSString *record_root(void) {
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Movies/Hardcore"];
}
static void record_status(NSString *s) {
    [g_record_status release];g_record_status=[s copy];plat_invalidate();
}
static int mac_record_active(void) {return g_record_writing || g_record_finishing;}
static double record_time(void) {return g_record_fixture ? 0 : g_camera_capture ? [g_camera_capture elapsedSeconds]*1000 : g_record_duration;}
static void record_fail(NSString *reason) {
    if(g_record_failed || !mac_record_active())return;
    g_record_failed=1;
    [g_camera_capture pause];
    record_status([@"Recording paused: " stringByAppendingString:reason]);
}
static BOOL record_write(FILE *f,NSData *data) {
    return f && fwrite(data.bytes,1,data.length,f)==data.length;
}
static NSData *record_json(id object) {return [NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingFragmentsAllowed error:nil];}
static void record_enqueue(FILE *file,NSDictionary *value,size_t cost) {
    if(!g_record_queue || g_record_failed)return;
    if(cost>32*1024*1024 || __atomic_load_n(&g_record_pending,__ATOMIC_RELAXED)+cost>32*1024*1024) {
        record_fail(@"disk queue full; save this clip before recording again");return;
    }
    unsigned generation=g_record_generation;
    __atomic_add_fetch(&g_record_pending,cost,__ATOMIC_RELAXED);
    dispatch_async(g_record_queue,^{ @autoreleasepool {
        NSData *json=record_json(value);
        BOOL ok=json && record_write(file,json) && fputc('\n',file)!=EOF;
        __atomic_sub_fetch(&g_record_pending,cost,__ATOMIC_RELAXED);
        if(!ok){__atomic_store_n(&g_record_disk_error,1,__ATOMIC_RELAXED);dispatch_async(dispatch_get_main_queue(),^{if(generation==g_record_generation)record_fail(@"could not write replay data");});}
    }});
}
static NSDictionary *record_atlas_item(int x,int y,int w,int h,NSData *data) {
    return @{@"x":@(x),@"y":@(y),@"w":@(w),@"h":@(h),@"data":data};
}
static void mac_record_upload(int x,int y,const unsigned char *bitmap) {
    if(!g_record_writing || g_record_failed)return;
    /* Keep glyph updates during a pause too: the first resumed frame may use
       slots populated while the paused application continued changing. */
    [g_record_atlas addObject:record_atlas_item(x,y,SLOT,SLOT,[NSData dataWithBytes:bitmap length:SLOT*SLOT])];
    if(g_record_atlas.count>MAX_SLOTS*2)record_fail(@"too many pending glyph changes");
}
static NSString *record_visible_text(void) {
    Tab *tab=display_tab();if(!tab)return @"";
    NSMutableString *text=[NSMutableString string];Term *t=&tab->t;
    for(int row=0;row<t->rows;row++) {
        const Cell *cells;int count;display_line(t,row,&cells,&count);
        for(int col=0;col<count;col++) {
            if(cells[col].wflag==W_CONT)continue;
            char utf[8]={0};int n=utf8_encode(cells[col].ch ? cells[col].ch : ' ',utf);utf[n]=0;
            [text appendString:[NSString stringWithUTF8String:utf] ?: @"�"];
        }
        [text appendString:@"\n"];
    }
    return text;
}
static void mac_record_frame(Frame *f) {
    if(!g_record_writing || g_record_failed || (!g_record_fixture && g_camera_capture.state!=HCCameraCaptureRecording))return;
    NSDictionary *camera=mac_camera_metadata() ?: @{@"visible":@NO};
    NSData *quads=[NSData dataWithBytes:f->v length:(size_t)f->n*sizeof(Inst)];
    if(!g_record_atlas.count && g_record_w==f->w && g_record_h==f->h && g_record_bg==(int)DEF_BG &&
       [g_record_last_quads isEqual:quads] && [g_record_last_camera isEqual:camera])return;
    [g_record_last_quads release];g_record_last_quads=[quads retain];
    [g_record_last_camera release];g_record_last_camera=[camera copy];
    g_record_w=f->w;g_record_h=f->h;g_record_bg=DEF_BG;
    NSArray *updates=[[g_record_atlas copy] autorelease];[g_record_atlas removeAllObjects];
    double time=record_time();g_record_duration=time;
    /* Copying this frame is bounded by the renderer's existing instance count.
       Expensive Base64 and JSON work stays off the AppKit thread. */
    size_t cost=quads.length+1024;
    for(NSDictionary *a in updates)cost+=[a[@"data"] length];
    if(cost>32*1024*1024 || __atomic_load_n(&g_record_pending,__ATOMIC_RELAXED)+cost>32*1024*1024) {
        record_fail(@"disk queue full; save this clip before recording again");return;
    }
    unsigned generation=g_record_generation;
    __atomic_add_fetch(&g_record_pending,cost,__ATOMIC_RELAXED);
    FILE *file=g_record_frames;int width=f->w,height=f->h;uint32_t background=DEF_BG;
    NSString *visibleText=record_visible_text();int tabID=display_tab()?display_tab()->id:0;
    dispatch_async(g_record_queue,^{ @autoreleasepool {
        NSMutableArray *atlas=[NSMutableArray arrayWithCapacity:updates.count];
        for(NSDictionary *a in updates) {
            NSMutableDictionary *item=[[a mutableCopy] autorelease];
            item[@"data"]=[a[@"data"] base64EncodedStringWithOptions:0];[atlas addObject:item];
        }
        NSDictionary *frame=@{@"t":@(time),@"w":@(width),@"h":@(height),@"bg":@(background),@"transparent":@NO,
            @"quads":[quads base64EncodedStringWithOptions:0],@"atlas":atlas,@"camera":camera,@"text":visibleText,@"tabId":@(tabID)};
        NSData *json=record_json(frame);
        BOOL ok=json && record_write(file,json) && fputc('\n',file)!=EOF;
        __atomic_sub_fetch(&g_record_pending,cost,__ATOMIC_RELAXED);
        if(!ok){__atomic_store_n(&g_record_disk_error,1,__ATOMIC_RELAXED);dispatch_async(dispatch_get_main_queue(),^{if(generation==g_record_generation)record_fail(@"could not write replay frame");});}
    }});
    if(g_record_fixture)record_fixture_finish();
}
static void mac_record_hide_pointer(void) {
    if(!g_record_writing || g_record_failed || g_camera_capture.state!=HCCameraCaptureRecording)return;
    record_enqueue(g_record_pointer,@{@"t":@(record_time()),@"x":@(-1),@"y":@(-1),@"visible":@NO,@"buttons":@0,@"type":@"move"},256);
}
static void mac_record_event(NSEvent *event,int x,int y) {
    if(!g_record_writing || g_record_failed || g_camera_capture.state!=HCCameraCaptureRecording)return;
    NSString *type=nil;
    switch(event.type) {
    case NSEventTypeMouseMoved:case NSEventTypeLeftMouseDragged:case NSEventTypeRightMouseDragged:case NSEventTypeOtherMouseDragged:type=@"move";break;
    case NSEventTypeLeftMouseDown:case NSEventTypeRightMouseDown:case NSEventTypeOtherMouseDown:type=@"down";break;
    case NSEventTypeLeftMouseUp:case NSEventTypeRightMouseUp:case NSEventTypeOtherMouseUp:type=@"up";break;
    case NSEventTypeScrollWheel:type=@"scroll";break;
    default:return;
    }
    int w,h;plat_client_size(&w,&h);
    record_enqueue(g_record_pointer,@{@"t":@(record_time()),@"x":@(x),@"y":@(y),@"visible":@(x>=0 && y>=0 && x<w && y<h),
        @"buttons":@([NSEvent pressedMouseButtons]),@"type":type,@"dx":@(event.type==NSEventTypeScrollWheel?event.scrollingDeltaX:0),
        @"dy":@(event.type==NSEventTypeScrollWheel?event.scrollingDeltaY:0)},512);
}
static NSString *record_resource(NSString *name) {
    NSString *bundle=[[NSBundle mainBundle].resourcePath stringByAppendingPathComponent:name];
    if([[NSFileManager defaultManager] fileExistsAtPath:bundle])return bundle;
    NSString *repo=[[@(g_mac_exe_path) stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    return [repo stringByAppendingPathComponent:name];
}
static NSString *record_title(void) {
    Tab *tab=active_tab();if(!tab)return @"Hardcore";
    char title[2048];int n=0;
    for(int i=0;i<255 && tab->t.title[i] && n<2000;i++)n+=utf8_encode(tab->t.title[i],title+n);
    title[n]=0;
    if(n)return [NSString stringWithUTF8String:title] ?: @"Hardcore";
    return [[NSString stringWithUTF8String:tab->t.cwd] lastPathComponent] ?: @"Hardcore";
}
static void record_browser_restore(void) {
    for(NSNumber *ident in g_record_native_tabs){Tab *tb=tab_by_id(ident.intValue);if(tb)tb->browser_native=1;}
    [g_record_native_tabs release];g_record_native_tabs=nil;mac_browser_sync();
}
static void record_clear(void) {
    g_record_writing=g_record_finishing=g_record_fixture=0;
    [g_record_dir release];g_record_dir=nil;
    [g_record_atlas release];g_record_atlas=nil;
    [g_record_last_quads release];g_record_last_quads=nil;
    [g_record_last_camera release];g_record_last_camera=nil;
    [g_record_info release];g_record_info=nil;
    record_browser_restore();plat_invalidate();
}
static void mac_record_start(void) {
    if(mac_record_active() || g_camera_capture.state!=HCCameraCaptureReady)return;
    NSFileManager *fm=NSFileManager.defaultManager;
    NSError *error=nil;
    if(![fm createDirectoryAtPath:record_root() withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&error]) {
        record_status(error.localizedDescription);return;
    }
    NSDateFormatter *date=[[[NSDateFormatter alloc] init] autorelease];date.dateFormat=@"yyyy-MM-dd_HH-mm-ss";
    NSString *base=[NSString stringWithFormat:@"%@-%@.partial.hcrec",[date stringFromDate:NSDate.date],[NSUUID.UUID.UUIDString substringToIndex:8]];
    g_record_dir=[[record_root() stringByAppendingPathComponent:base] copy];
    if(![fm createDirectoryAtPath:g_record_dir withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:&error]) {
        record_status(error.localizedDescription);[g_record_dir release];g_record_dir=nil;return;
    }
    if(!g_record_queue)g_record_queue=dispatch_queue_create("hardcore.recording.journal",DISPATCH_QUEUE_SERIAL);
    g_record_frames=fopen([[g_record_dir stringByAppendingPathComponent:@"frames.jsonl"] fileSystemRepresentation],"wx");
    g_record_pointer=fopen([[g_record_dir stringByAppendingPathComponent:@"pointer.jsonl"] fileSystemRepresentation],"wx");
    if(!g_record_frames || !g_record_pointer) {
        if(g_record_frames)fclose(g_record_frames);if(g_record_pointer)fclose(g_record_pointer);
        g_record_frames=g_record_pointer=NULL;record_status(@"Cannot create replay journal");record_clear();return;
    }
    fchmod(fileno(g_record_frames),0600);fchmod(fileno(g_record_pointer),0600);
    g_record_info=[@{@"version":@1,@"title":record_title(),@"createdAt":@""} mutableCopy];
    NSISO8601DateFormatter *iso=[[[NSISO8601DateFormatter alloc] init] autorelease];g_record_info[@"createdAt"]=[iso stringFromDate:NSDate.date];
    g_record_generation++;g_record_disk_error=0;
    g_record_atlas=[[NSMutableArray alloc] init];g_record_failed=0;g_record_finishing=1;g_record_duration=0;
    record_status(@"Starting camera recording…");
    [g_camera_capture startRecordingToDirectory:[NSURL fileURLWithPath:g_record_dir isDirectory:YES] completion:^(NSError *captureError){
        if(captureError){fclose(g_record_frames);fclose(g_record_pointer);g_record_frames=g_record_pointer=NULL;record_status(captureError.localizedDescription);record_clear();return;}
        /* Snapshot font resources once, then journal only changed atlas slots. */
        for(int slot=0;slot<g_nslots;slot++) {
            int x=(slot%SLOTS_PER_ROW)*SLOT,y=(slot/SLOTS_PER_ROW)*SLOT;unsigned char bitmap[SLOT*SLOT];
            [g_atlas getBytes:bitmap bytesPerRow:SLOT fromRegion:MTLRegionMake2D(x,y,SLOT,SLOT) mipmapLevel:0];
            [g_record_atlas addObject:record_atlas_item(x,y,SLOT,SLOT,[NSData dataWithBytes:bitmap length:sizeof(bitmap)])];
        }
        g_record_native_tabs=[[NSMutableArray alloc] init];
        for(int i=0;i<g_ntabs;i++)if(g_tabs[i]->kind==TK_BROWSER && g_tabs[i]->browser_native) {
            [g_record_native_tabs addObject:@(g_tabs[i]->id)];g_tabs[i]->browser_native=0;
        }
        mac_browser_sync();g_record_finishing=0;g_record_writing=1;
        record_status(@"Recording · browser tabs use grid view");plat_invalidate();
    }];
}
static void mac_record_pause(void) {
    if(!g_record_writing || g_record_failed)return;
    if(g_camera_capture.state==HCCameraCaptureRecording){[g_camera_capture pause];record_status(@"Paused");}
    else if(g_camera_capture.state==HCCameraCapturePaused){[g_camera_capture resume];record_status(@"Recording");plat_invalidate();}
}
static BOOL record_copy_journal(FILE *out,NSString *file) {
    FILE *input=fopen(file.fileSystemRepresentation,"r");if(!input)return NO;
    char *line=NULL;size_t capacity=0;ssize_t len;BOOL first=YES,ok=YES;
    while((len=getline(&line,&capacity,input))>=0) {
        if(!len || line[len-1]!='\n'){ok=NO;break;}
        if(!first && fputc(',',out)==EOF){ok=NO;break;}first=NO;
        if(fwrite(line,1,(size_t)len-1,out)!=(size_t)len-1){ok=NO;break;}
    }
    if(ferror(input))ok=NO;free(line);fclose(input);return ok;
}
static NSString *record_finalize(NSString *directory,NSDictionary *info,NSArray *captions,double duration,NSString **failure) {
    NSFileManager *fm=NSFileManager.defaultManager;
    NSDictionary *frameAttributes=[fm attributesOfItemAtPath:[directory stringByAppendingPathComponent:@"frames.jsonl"] error:nil];
    if(![frameAttributes[NSFileSize] unsignedLongLongValue]){*failure=@"No terminal frame was captured; private draft retained";return nil;}
    NSString *replay=[directory stringByAppendingPathComponent:@"replay.json"];
    FILE *out=fopen(replay.fileSystemRepresentation,"wx");if(!out){*failure=@"Cannot create replay";return nil;}
    fchmod(fileno(out),0600);
    NSMutableDictionary *meta=[[info mutableCopy] autorelease];meta[@"durationMs"]=@(duration);
    meta[@"camera"]=[info[@"synthetic"] boolValue]?(id)[NSNull null]:@{@"file":@"camera.mp4",@"offsetMs":@0,@"mirrored":@YES};meta[@"captions"]=captions;
    NSData *header=record_json(meta);
    BOOL ok=header.length>1 && fwrite(header.bytes,1,header.length-1,out)==header.length-1;
    ok=ok && fputs(",\"frames\":[",out)!=EOF && record_copy_journal(out,[directory stringByAppendingPathComponent:@"frames.jsonl"]);
    ok=ok && fputs("],\"pointer\":[",out)!=EOF && record_copy_journal(out,[directory stringByAppendingPathComponent:@"pointer.jsonl"]);
    ok=ok && fputs("]}",out)!=EOF; if(fclose(out))ok=NO;
    if(!ok){*failure=@"Replay journal could not be finalized; draft retained";return nil;}
    [meta removeObjectForKey:@"captions"];meta[@"format"]=@"hardcore-recording";meta[@"replay"]=@"replay.json";
    if(![record_json(meta) writeToFile:[directory stringByAppendingPathComponent:@"manifest.json"] options:NSDataWritingAtomic error:nil]){*failure=@"Cannot save recording manifest";return nil;}
    /* External script permits portable file:// playback without fetch/CORS. */
    FILE *js=fopen([[directory stringByAppendingPathComponent:@"recording.js"] fileSystemRepresentation],"wx");
    FILE *input=fopen(replay.fileSystemRepresentation,"r");ok=js && input;
    if(ok) {fchmod(fileno(js),0600);ok=fputs("window.HARDCORE_RECORDING=",js)!=EOF;char buffer[65536];size_t n;
        while(ok && (n=fread(buffer,1,sizeof(buffer),input)))ok=fwrite(buffer,1,n,js)==n;
        if(ferror(input))ok=NO;if(fputs(";\n",js)==EOF)ok=NO;}
    if(input)fclose(input);if(js && fclose(js))ok=NO;
    if(!ok){*failure=@"Cannot write portable player data";return nil;}
    for(NSString *asset in @[@"index.html",@"core.js",@"player.js",@"style.css",@"edits.js"]) {
        NSError *e=nil;
        if(![fm copyItemAtPath:record_resource([@"recording/" stringByAppendingString:asset]) toPath:[directory stringByAppendingPathComponent:asset] error:&e]) {
            *failure=e.localizedDescription;return nil;
        }
    }
    NSString *final=[directory stringByReplacingOccurrencesOfString:@".partial.hcrec" withString:@".hcrec"];
    NSError *e=nil;if(![fm moveItemAtPath:directory toPath:final error:&e]){*failure=e.localizedDescription;return nil;}
    [fm removeItemAtPath:[final stringByAppendingPathComponent:@"frames.jsonl"] error:nil];
    [fm removeItemAtPath:[final stringByAppendingPathComponent:@"pointer.jsonl"] error:nil];
    return final;
}
static void mac_record_stop(void) {
    if(!g_record_writing || g_record_finishing)return;
    g_record_duration=record_time();g_record_writing=0;g_record_finishing=1;
    record_status(@"Saving camera and replay…");
    [g_camera_capture stopWithCompletion:^(NSURL *movie,NSArray *captions,NSError *error){
        (void)movie;
        NSString *directory=[[g_record_dir copy] autorelease];NSDictionary *info=[[g_record_info copy] autorelease];
        double duration=g_record_duration;BOOL failed=g_record_failed;
        NSMutableArray *timed=[NSMutableArray array];
        for(NSDictionary *c in captions)[timed addObject:@{@"start":@([c[@"start"] doubleValue]*1000),@"end":@([c[@"end"] doubleValue]*1000),@"text":c[@"text"] ?: @""}];
        dispatch_async(g_record_queue,^{@autoreleasepool {
            BOOL diskOK=fclose(g_record_frames)==0;diskOK=(fclose(g_record_pointer)==0)&&diskOK;g_record_frames=g_record_pointer=NULL;
            NSString *why=error.localizedDescription;NSString *saved=nil;
            if(!why && !failed && !__atomic_load_n(&g_record_disk_error,__ATOMIC_RELAXED) && diskOK)saved=record_finalize(directory,info,timed,duration,&why);
            if(!why && !saved)why=@"Incomplete replay retained as a private draft";
            dispatch_async(dispatch_get_main_queue(),^{
                if(saved)record_status([NSString stringWithFormat:@"Saved %@",saved.lastPathComponent]);else record_status(why);
                record_clear();
            });
        }});
    }];
    g_record_duration=record_time(); /* stop synchronously freezes the media clock */
}
static void mac_record_discard(void) {
    if(!g_record_writing || g_record_finishing)return;
    g_record_writing=0;g_record_finishing=1;record_status(@"Discarding recording…");
    NSString *directory=[[g_record_dir copy] autorelease];
    [g_camera_capture discardWithCompletion:^{
        dispatch_async(g_record_queue,^{@autoreleasepool {
            fclose(g_record_frames);fclose(g_record_pointer);g_record_frames=g_record_pointer=NULL;
            [NSFileManager.defaultManager removeItemAtPath:directory error:nil];
            dispatch_async(dispatch_get_main_queue(),^{record_status(@"Recording discarded");record_clear();});
        }});
    }];
}

/* Private control-channel fixture: exercises the production renderer/journal
 * without constructing a capture session or touching camera/microphone APIs. */
static void mac_record_fixture(NSString *directory) {
    if(mac_record_active() || !getenv("HARDCORE_CTL") || ![directory hasSuffix:@".partial.hcrec"])return;
    if(![NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:@{NSFilePosixPermissions:@0700} error:nil])return;
    g_record_dir=[directory copy];
    if(!g_record_queue)g_record_queue=dispatch_queue_create("hardcore.recording.journal",DISPATCH_QUEUE_SERIAL);
    g_record_frames=fopen([[directory stringByAppendingPathComponent:@"frames.jsonl"] fileSystemRepresentation],"wx");
    g_record_pointer=fopen([[directory stringByAppendingPathComponent:@"pointer.jsonl"] fileSystemRepresentation],"wx");
    if(!g_record_frames || !g_record_pointer){if(g_record_frames)fclose(g_record_frames);if(g_record_pointer)fclose(g_record_pointer);record_clear();return;}
    g_record_info=[@{@"version":@1,@"title":@"Synthetic renderer verification",@"synthetic":@YES,@"createdAt":@"2026-10-16T00:00:00Z"} mutableCopy];
    g_record_generation++;g_record_disk_error=g_record_failed=0;g_record_fixture=g_record_writing=1;
    g_record_atlas=[[NSMutableArray alloc] init];
    for(int slot=0;slot<g_nslots;slot++) {
        int x=(slot%SLOTS_PER_ROW)*SLOT,y=(slot/SLOTS_PER_ROW)*SLOT;unsigned char bitmap[SLOT*SLOT];
        [g_atlas getBytes:bitmap bytesPerRow:SLOT fromRegion:MTLRegionMake2D(x,y,SLOT,SLOT) mipmapLevel:0];
        [g_record_atlas addObject:record_atlas_item(x,y,SLOT,SLOT,[NSData dataWithBytes:bitmap length:sizeof(bitmap)])];
    }
    plat_invalidate();
}
static void record_fixture_finish(void) {
    g_record_writing=0;g_record_finishing=1;
    NSString *directory=[[g_record_dir copy] autorelease];NSDictionary *info=[[g_record_info copy] autorelease];
    dispatch_async(g_record_queue,^{@autoreleasepool {
        fclose(g_record_frames);fclose(g_record_pointer);g_record_frames=g_record_pointer=NULL;
        NSString *why=nil;NSString *saved=record_finalize(directory,info,@[],1000,&why);
        dispatch_async(dispatch_get_main_queue(),^{record_status(saved ?: why);record_clear();});
    }});
}
