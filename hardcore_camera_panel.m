/* Camera TV: grid-drawn controls around a native AVFoundation preview. The
 * capture object and recording callbacks are owned by hardcore_recording.m. */
static int g_camera_visible, g_camera_placed, g_camera_manual, g_camera_drag, g_camera_down;
static HcTvRect g_camera_rect, g_camera_requested, g_camera_drag_origin;
static int g_camera_drag_x, g_camera_drag_y;
static HcTvPlacement g_camera_placement;
static NSView *g_camera_preview_view;
static NSTimer *g_camera_timer;
static NSUInteger g_camera_library_count;
static int g_camera_was_recording;

enum { MAC_CAMERA_NONE, MAC_CAMERA_MOVE, MAC_CAMERA_MINIMIZE, MAC_CAMERA_RECORD,
    MAC_CAMERA_PAUSE, MAC_CAMERA_SAVE, MAC_CAMERA_DISCARD, MAC_CAMERA_CAPTIONS,
    MAC_CAMERA_LIBRARY, MAC_CAMERA_RESIZE, MAC_CAMERA_BODY };

@interface HCCameraPreviewView : NSView
@end
@implementation HCCameraPreviewView
- (BOOL)isFlipped { return YES; }
- (NSView *)hitTest:(NSPoint)point { (void)point; return nil; }
@end

static HcTvRect mac_camera_primary(void) {
    return (HcTvRect){sidebar_x()/CW,g_chrome.top,SB_COLS,g_chrome.bottom-g_chrome.top};
}
static int mac_camera_rect_available(HcTvRect rect,Term *term) {
    if(!hc_tv_rect_fits(rect,term->cols,term->rows) || hc_tv_rect_overlap(rect,mac_camera_primary()))return 0;
    for(int i=0;i<HC_TV_COUNT;i++)
        if(g_tvs[i].state.open && g_tvs[i].state.placed && hc_tv_rect_overlap(rect,g_tvs[i].state.rect))return 0;
    return 1;
}
static NSRect mac_camera_body_pixels(void) {
    HcTvRect r=g_camera_rect;
    if(r.w<=2 || r.h<=5)return NSMakeRect(0,0,1,1);
    return NSMakeRect((r.x+1)*CW,(r.y+1)*CH,(r.w-2)*CW,chrome_max(0,r.h-5)*CH);
}
static void mac_camera_preview_sync(void) {
    if(!g_view)return;
    BOOL show=g_camera_visible && g_camera_placed && g_camera_capture &&
        g_camera_capture.state!=HCCameraCapturePreparing && g_camera_capture.state!=HCCameraCaptureFailed;
    if(show && !g_camera_preview_view) {
        g_camera_preview_view=[[HCCameraPreviewView alloc] initWithFrame:NSZeroRect];
        g_camera_preview_view.wantsLayer=YES;
        g_camera_preview_view.layer.masksToBounds=YES;
        g_camera_preview_view.layer.backgroundColor=[[NSColor blackColor] CGColor];
        [g_view addSubview:g_camera_preview_view];
    }
    [g_camera_preview_view setHidden:!show];
    if(!show)return;
    /* Timer/permission callbacks also enter here, outside UI_FONT. */
    UI_FONT;NSRect pixels=mac_camera_body_pixels();UI_DONE;
    /* These are renderer pixels with a top-left origin, not AppKit backing
     * coordinates. Rect conversion flips Y again on HCView and puts the
     * preview above the window. Convert the units, preserving the origin. */
    NSSize unit=[g_view convertSizeFromBacking:NSMakeSize(1,1)];
    NSRect frame=NSMakeRect(pixels.origin.x*unit.width,pixels.origin.y*unit.height,
        pixels.size.width*unit.width,pixels.size.height*unit.height);
    [CATransaction begin];[CATransaction setDisableActions:YES];
    g_camera_preview_view.frame=frame;
    AVCaptureVideoPreviewLayer *preview=g_camera_capture.previewLayer;
    if(preview.superlayer!=g_camera_preview_view.layer) {
        [preview removeFromSuperlayer];[g_camera_preview_view.layer addSublayer:preview];
    }
    preview.frame=g_camera_preview_view.bounds;
    [CATransaction commit];
}
static void mac_camera_release_hidden(void) {
    if(!g_camera_visible && !mac_record_active() && g_camera_capture) {
        [g_camera_capture.previewLayer removeFromSuperlayer];
        [g_camera_capture shutdown];[g_camera_capture release];g_camera_capture=nil;
    }
}
static void mac_camera_library_refresh(void) {
    NSUInteger count=0;
    for(NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:record_root() error:NULL])
        if([name hasSuffix:@".hcrec"] && ![name hasSuffix:@".partial.hcrec"])count++;
    g_camera_library_count=count;
}
static void mac_camera_tick_start(void) {
    if(g_camera_timer)return;
    g_camera_timer=[NSTimer scheduledTimerWithTimeInterval:.2 repeats:YES block:^(NSTimer *timer) {
        int active=mac_record_active();
        if(g_camera_drag && !g_view.window.isKeyWindow)g_camera_drag=g_camera_down=0;
        if(g_camera_was_recording && !active)mac_camera_library_refresh();
        g_camera_was_recording=active;
        if(!g_camera_visible && !active) {
            mac_camera_release_hidden();[timer invalidate];g_camera_timer=nil;return;
        }
        mac_camera_preview_sync();plat_invalidate();
    }];
    [[NSRunLoop mainRunLoop] addTimer:g_camera_timer forMode:NSRunLoopCommonModes];
}
static void mac_camera_toggle(void) {
    g_camera_visible=!g_camera_visible;
    g_camera_drag=g_camera_down=0;
    if(g_camera_visible) {
        mac_camera_library_refresh();mac_camera_tick_start();
        if(!g_camera_capture) {
            g_camera_capture=[[HCCameraCapture alloc] init];
            g_camera_capture.captionsEnabled=[[NSUserDefaults standardUserDefaults] boolForKey:@"HCCameraCaptions"];
            g_camera_capture.stateChanged=^{ mac_camera_preview_sync();plat_invalidate(); };
        }
        if(!mac_record_active() && (g_camera_capture.state==HCCameraCaptureIdle || g_camera_capture.state==HCCameraCaptureFailed)) {
            [g_camera_capture prepareWithCompletion:^(NSError *error) { (void)error;mac_camera_preview_sync();plat_invalidate(); }];
        }
    } else mac_camera_release_hidden();
    mac_camera_preview_sync();plat_invalidate();
}
static void mac_camera_layout(Term *term) {
    g_camera_placed=0;
    if(!g_camera_visible || !term || term->cols<32 || term->rows<9) {
        mac_camera_preview_sync();return;
    }
    UI_FONT;
    if(!g_camera_requested.w) {
        int width=32;
        int body=chrome_max(4,(int)ceil((double)(width-2)*CW*9/(16*CH)));
        g_camera_requested=(HcTvRect){0,0,width,body+5};
    }
    HcTvRect requested=hc_tv_requested_rect(g_camera_requested,term->cols,term->rows);
    requested.w=chrome_max(32,requested.w);requested.h=chrome_max(9,requested.h);
    if(g_camera_manual && mac_camera_rect_available(requested,term)) {
        g_camera_rect=requested;g_camera_placement=(HcTvPlacement){.rect=requested,.valid=1};g_camera_placed=1;
    } else if(g_camera_placement.valid && g_camera_rect.w==requested.w && g_camera_rect.h==requested.h &&
              mac_camera_rect_available(g_camera_rect,term)) {
        g_camera_placed=1;
    } else {
        ChromeTvCostContext context={.term=term,.primary=mac_camera_primary()};
        HcTvPlacement placement=hc_tv_place(term->cols,term->rows,context.primary,requested.w,requested.h,
            g_camera_placement.valid?&g_camera_placement:NULL,chrome_tv_cost,&context);
        if(placement.valid) {
            g_camera_rect=placement.rect;g_camera_placement=placement;g_camera_placed=1;
        }
    }
    if(g_camera_placed) {
        HcTvRect prior[HC_TV_COUNT+1];int count=0;
        prior[count++]=mac_camera_primary();
        for(int i=0;i<HC_TV_COUNT;i++)if(g_tvs[i].state.open && g_tvs[i].state.placed)
            prior[count++]=g_tvs[i].state.rect;
        chrome_tv_reflow_apply(term,g_camera_rect,prior,count);
    }
    mac_camera_preview_sync();UI_DONE;
}
static int mac_camera_hit(int x,int y) {
    if(!g_camera_visible || !g_camera_placed || x<0 || y<0)return MAC_CAMERA_NONE;
    UI_FONT;
    HcTvRect r=g_camera_rect;int col=x/CW-r.x,row=y/CH-r.y;
    UI_DONE;
    if(col<0 || col>=r.w || row<0 || row>=r.h)return MAC_CAMERA_NONE;
    if(row==0)return col>=r.w-3?MAC_CAMERA_MINIMIZE:MAC_CAMERA_MOVE;
    if(row==r.h-1 && col>=r.w-3)return MAC_CAMERA_RESIZE;
    if(row==r.h-4) {
        if(col>=1 && col<8)return MAC_CAMERA_RECORD;
        if(col>=9 && col<16)return MAC_CAMERA_PAUSE;
        if(col>=17 && col<22)return MAC_CAMERA_SAVE;
        if(col>=23 && col<30)return MAC_CAMERA_DISCARD;
    }
    if(row==r.h-3) {
        if(col>=1 && col<17)return MAC_CAMERA_CAPTIONS;
        if(col>=19 && col<27)return MAC_CAMERA_LIBRARY;
    }
    return MAC_CAMERA_BODY;
}
static void mac_camera_label(const wchar_t *label,int col,int width,int row,uint32_t color,int hit) {
    if(mac_camera_hit(g_mouse_x,g_mouse_y)==hit)
        fill_rect(col*CW,row*CH,(col+width)*CW,(row+1)*CH,TAB_HOVER,1);
    chrome_label(label,col,col+width,row,color);
}
static void mac_camera_draw(void) {
    if(!g_camera_visible || !g_camera_placed)return;
    UI_FONT;
    int origin=g_chrome_x,surface=g_chrome_surface_cols;
    float alpha=g_draw_alpha;int capture=g_chrome_capture;
    HcTvRect r=g_camera_rect;
    g_chrome_x=r.x*CW;g_chrome_surface_cols=r.w;g_draw_alpha=1;g_chrome_capture=0;
    fill_rect(0,r.y*CH,r.w*CW,(r.y+r.h)*CH,BAR_BG,1);
    fill_rect(0,r.y*CH,r.w*CW,r.y*CH+1,g_look.dim,1);
    fill_rect(0,(r.y+r.h)*CH-1,r.w*CW,(r.y+r.h)*CH,g_look.dim,1);
    fill_rect(0,r.y*CH,1,(r.y+r.h)*CH,g_look.dim,1);
    fill_rect(r.w*CW-1,r.y*CH,r.w*CW,(r.y+r.h)*CH,g_look.dim,1);
    HCCameraCaptureState state=g_camera_capture?g_camera_capture.state:HCCameraCaptureIdle;
    BOOL busy=mac_record_active(),recording=state==HCCameraCaptureRecording,paused=state==HCCameraCapturePaused;
    wchar_t title[96];
    if(busy) {
        unsigned elapsed=(unsigned)g_camera_capture.elapsedSeconds;
        _snwprintf(title,96,L"↕ Camera %02u:%02u:%02u",elapsed/3600,elapsed/60%60,elapsed%60);
    } else _snwprintf(title,96,L"↕ Camera");
    chrome_label(title,1,r.w-3,r.y,recording?g_look.red:g_look.text);
    mac_camera_label(L"−",r.w-2,1,r.y,g_look.text,MAC_CAMERA_MINIMIZE);
    fill_rect(CW,(r.y+1)*CH,(r.w-1)*CW,(r.y+r.h-4)*CH,DEF_BG,1);
    if(state==HCCameraCapturePreparing || state==HCCameraCaptureFailed || state==HCCameraCaptureIdle) {
        const char *message=state==HCCameraCapturePreparing?"Opening camera…":state==HCCameraCaptureFailed?"Camera unavailable":"Camera closed";
        wchar_t text[96];utf8_to_wide(message,text,96);chrome_label(text,2,r.w-2,r.y+2,g_look.detail);
        if(state==HCCameraCaptureFailed)chrome_label(L"Close and reopen cam to retry",2,r.w-2,r.y+3,g_look.dim);
    }
    int row=r.y+r.h-4;
    mac_camera_label(L"● Rec",1,7,row,!busy && state==HCCameraCaptureReady?g_look.red:g_look.dim,MAC_CAMERA_RECORD);
    mac_camera_label(paused?L"Resume":L"Pause",9,7,row,recording||paused?g_look.text:g_look.dim,MAC_CAMERA_PAUSE);
    mac_camera_label(L"Save",17,5,row,recording||paused||state==HCCameraCaptureFailed?AMBER:g_look.dim,MAC_CAMERA_SAVE);
    mac_camera_label(L"Discard",23,7,row,busy && (recording||paused||state==HCCameraCaptureFailed)?g_look.text_lo:g_look.dim,MAC_CAMERA_DISCARD);
    row++;
    mac_camera_label(g_camera_capture.captionsEnabled?L"[x] Captions":L"[ ] Captions",1,16,row,busy?g_look.dim:g_look.text_lo,MAC_CAMERA_CAPTIONS);
    wchar_t library[32];
    _snwprintf(library,32,L"Clips %lu",(unsigned long)chrome_min((int)g_camera_library_count,999));
    mac_camera_label(library,19,8,row,g_look.text_lo,MAC_CAMERA_LIBRARY);
    NSString *status=g_record_status.length?g_record_status:g_camera_capture.stateDescription;
    if(g_camera_capture.lastError)status=g_camera_capture.lastError.localizedDescription;
    wchar_t text[512];utf8_to_wide(status.UTF8String ?: "",text,512);
    chrome_label(text,1,r.w-1,row+1,g_camera_capture.lastError?g_look.red:g_look.detail);
    NSString *foot=g_camera_capture.warning.length?g_camera_capture.warning:g_camera_capture.hasMicrophone?@"Camera + microphone":@"Camera only · no microphone";
    utf8_to_wide(foot.UTF8String ?: "",text,512);chrome_label(text,1,r.w-3,row+2,g_look.dim);
    int grip=chrome_min(CW-3,CH-5),bottom=(r.y+r.h)*CH;
    for(int length=grip;length>=2;length-=3)for(int step=0;step<length;step++) {
        int x=r.w*CW-3-step,y=bottom-3-(length-1-step);
        fill_rect(x,y,x+1,y+1,g_look.detail,1);
    }
    g_chrome_x=origin;g_chrome_surface_cols=surface;g_draw_alpha=alpha;g_chrome_capture=capture;UI_DONE;
}
static void mac_camera_drag_move(int x,int y) {
    Tab *tab=active_tab();if(!tab)return;
    UI_FONT;
    HcTvRect r=g_camera_drag_origin;
    int dx=(int)lround((double)(x-g_camera_drag_x)/CW),dy=(int)lround((double)(y-g_camera_drag_y)/CH);
    if(g_camera_drag==MAC_CAMERA_RESIZE) {
        r.w=chrome_min(chrome_max(32,r.w+dx),chrome_min(HC_TV_MAX_WIDTH,tab->t.cols-r.x));
        r.h=chrome_min(chrome_max(9,r.h+dy),chrome_min(HC_TV_MAX_HEIGHT,tab->t.rows-r.y));
    } else {
        r.x=chrome_min(chrome_max(0,r.x+dx),chrome_max(0,tab->t.cols-r.w));
        r.y=chrome_min(chrome_max(0,r.y+dy),chrome_max(0,tab->t.rows-r.h));
    }
    if(mac_camera_rect_available(r,&tab->t)) {
        g_camera_manual=1;g_camera_requested=g_camera_rect=r;
        g_camera_placement=(HcTvPlacement){.rect=r,.valid=1};mac_camera_preview_sync();plat_invalidate();
    }
    UI_DONE;
    [(g_camera_drag==MAC_CAMERA_RESIZE?mac_tv_resize_cursor():[NSCursor closedHandCursor]) set];
}
static int mac_camera_mouse(NSEvent *event,int x,int y) {
    NSEventType type=event.type;
    if(type==NSEventTypeKeyDown) {
        if(g_camera_drag && event.keyCode==53){g_camera_drag=0;plat_invalidate();return 1;}
        return 0;
    }
    if(g_camera_drag && (type==NSEventTypeLeftMouseDragged || type==NSEventTypeLeftMouseUp)) {
        mac_camera_drag_move(x,y);
        if(type==NSEventTypeLeftMouseUp){g_camera_drag=g_camera_down=0;[[NSCursor arrowCursor] set];}
        return 1;
    }
    if(g_camera_down && (type==NSEventTypeLeftMouseDragged || type==NSEventTypeLeftMouseUp)) {
        if(type==NSEventTypeLeftMouseUp)g_camera_down=0;
        return 1;
    }
    int hit=mac_camera_hit(x,y);
    if(!hit)return 0;
    if(type==NSEventTypeMouseMoved) {
        g_mouse_x=x;g_mouse_y=y;plat_invalidate();return 0;
    }
    if(type!=NSEventTypeLeftMouseDown)
        return type==NSEventTypeScrollWheel || type==NSEventTypeRightMouseDown || type==NSEventTypeRightMouseUp ||
            type==NSEventTypeOtherMouseDown || type==NSEventTypeOtherMouseUp;
    g_mouse_x=x;g_mouse_y=y;g_camera_down=1;
    HCCameraCaptureState state=g_camera_capture.state;
    BOOL busy=mac_record_active();
    if(hit==MAC_CAMERA_MOVE || hit==MAC_CAMERA_RESIZE) {
        mac_cutie_blur();chrome_preview_cancel();g_sel_drag=g_sb_drag=0;
        g_camera_drag=hit;g_camera_drag_origin=g_camera_rect;g_camera_drag_x=x;g_camera_drag_y=y;
        [(hit==MAC_CAMERA_RESIZE?mac_tv_resize_cursor():[NSCursor closedHandCursor]) set];
    } else if(hit==MAC_CAMERA_MINIMIZE) {mac_camera_toggle();g_camera_down=1;}
    else if(hit==MAC_CAMERA_RECORD && !busy && state==HCCameraCaptureReady)mac_record_start();
    else if(hit==MAC_CAMERA_PAUSE && (state==HCCameraCaptureRecording || state==HCCameraCapturePaused))mac_record_pause();
    else if(hit==MAC_CAMERA_SAVE && busy && (state==HCCameraCaptureRecording || state==HCCameraCapturePaused || state==HCCameraCaptureFailed))mac_record_stop();
    else if(hit==MAC_CAMERA_DISCARD && busy && (state==HCCameraCaptureRecording || state==HCCameraCapturePaused || state==HCCameraCaptureFailed))mac_record_discard();
    else if(hit==MAC_CAMERA_CAPTIONS && !busy) {
        g_camera_capture.captionsEnabled=!g_camera_capture.captionsEnabled;
        [[NSUserDefaults standardUserDefaults] setBool:g_camera_capture.captionsEnabled forKey:@"HCCameraCaptions"];
    } else if(hit==MAC_CAMERA_LIBRARY){mac_camera_library_refresh();mac_record_library();}
    plat_invalidate();return 1;
}
static int mac_camera_cursor(int x,int y) {
    if(g_camera_drag){[(g_camera_drag==MAC_CAMERA_RESIZE?mac_tv_resize_cursor():[NSCursor closedHandCursor]) set];return 1;}
    int hit=mac_camera_hit(x,y);if(!hit)return 0;
    [(hit==MAC_CAMERA_MOVE?[NSCursor openHandCursor]:hit==MAC_CAMERA_RESIZE?mac_tv_resize_cursor():
        hit==MAC_CAMERA_BODY?[NSCursor arrowCursor]:[NSCursor pointingHandCursor]) set];
    return 1;
}
static NSDictionary *mac_camera_metadata(void) {
    UI_FONT;NSRect rect=mac_camera_body_pixels();UI_DONE;
    return @{@"visible":@(g_camera_visible && g_camera_placed),@"mirrored":@YES,
        @"rect":@[@(rect.origin.x),@(rect.origin.y),@(rect.size.width),@(rect.size.height)]};
}

/* Read-only diagnostics: counters/geometry only, never camera images or audio. */
static NSDictionary *mac_camera_status(void) {
    HCCameraCapture *camera=g_camera_capture;
    AVCaptureVideoPreviewLayer *preview=camera.previewLayer;
    NSView *view=g_camera_preview_view;
    return @{@"visible":@(g_camera_visible),@"placed":@(g_camera_placed),
        @"recording":@(mac_record_active()),@"state":camera.stateDescription ?: @"Camera closed",
        @"camera":camera.cameraName ?: @"",
        @"error":camera.lastError.localizedDescription ?: @"",@"warning":camera.warning ?: @"",
        @"video_frames":@(camera.receivedVideoFrames),
        @"session_running":@(preview.session.isRunning),@"connection_active":@(preview.connection.active),
        @"connection_enabled":@(preview.connection.enabled),
        @"hidden":@(!view || view.isHiddenOrHasHiddenAncestor),@"attached":@(view && preview.superlayer==view.layer),
        @"view_frame":NSStringFromRect(view.frame),@"view_bounds":NSStringFromRect(view.bounds),
        @"layer_frame":NSStringFromRect(preview.frame)};
}
