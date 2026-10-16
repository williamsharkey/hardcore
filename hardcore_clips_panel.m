/* The camera TV's library face. All chrome uses the current bitmap font and
 * theme. Jobs belong to the application, never to the lifetime of this TV. */
enum { MAC_CLIPS_CAMERA=100,MAC_CLIPS_RECORDINGS,MAC_CLIPS_EXPORTS,MAC_CLIPS_REPLAY,
    MAC_CLIPS_MP4,MAC_CLIPS_ZIP,MAC_CLIPS_OUTPUT,MAC_CLIPS_FILES,MAC_CLIPS_REFRESH,
    MAC_CLIPS_PREVIOUS,MAC_CLIPS_NEXT,MAC_CLIPS_ITEM=1000 };
static NSArray *g_clips_items;
static int g_clips_exports,g_clips_selected,g_clips_offset;
static NSString *g_clips_selected_identity;
static int g_clips_identity_exports;
static NSArray *mac_clips_rows(void) {return g_clips_exports?mac_record_jobs():g_clips_items;}
static int mac_clips_capacity(void) {return chrome_max(1,g_camera_rect.h-8);}
static void mac_clips_forget_selection(void) {
    [g_clips_selected_identity release];g_clips_selected_identity=nil;
    g_clips_identity_exports=g_clips_exports;
}
static void mac_clips_clamp(void) {
    NSArray *rows=mac_clips_rows();int count=(int)rows.count,capacity=mac_clips_capacity();
    NSString *key=g_clips_exports?@"id":@"path";
    if(g_clips_identity_exports!=g_clips_exports)mac_clips_forget_selection();
    if(g_clips_selected_identity) {
        g_clips_selected=-1;
        for(int i=0;i<count;i++)if([rows[i][key] isEqual:g_clips_selected_identity]){g_clips_selected=i;break;}
        /* A removed clip/job leaves no selection, rather than letting a
           pending Replay/Output click act on its former neighbor. */
    } else {
        g_clips_selected=chrome_min(chrome_max(0,g_clips_selected),chrome_max(0,count-1));
        if(count)g_clips_selected_identity=[rows[g_clips_selected][key] copy];
    }
    if(g_clips_selected>=0 && g_clips_selected<g_clips_offset)g_clips_offset=g_clips_selected;
    if(g_clips_selected>=0 && g_clips_selected>=g_clips_offset+capacity)g_clips_offset=g_clips_selected-capacity+1;
    g_clips_offset=chrome_min(chrome_max(0,g_clips_offset),chrome_max(0,count-capacity));
}
static void mac_clips_select_index(int index) {
    mac_clips_forget_selection();g_clips_selected=index;mac_clips_clamp();
}
static void mac_clips_refresh(void) {
    mac_clips_clamp();
    [g_clips_items release];g_clips_items=[mac_record_items() copy];
    g_camera_library_count=g_clips_items.count;mac_clips_clamp();
}
static NSDictionary *mac_clips_selected(void) {
    mac_clips_clamp();NSArray *rows=mac_clips_rows();
    return g_clips_selected>=0 && g_clips_selected<(int)rows.count?rows[g_clips_selected]:nil;
}
static NSDictionary *mac_clips_job(NSDictionary *item) {
    if(g_clips_exports)return item;
    NSDictionary *last=nil;
    for(NSDictionary *job in mac_record_jobs())if([job[@"bundle"] isEqual:item[@"path"]]) {
        if([job[@"state"] isEqual:@"running"])return job;
        last=job;
    }
    return last;
}
static void mac_record_library(void) {
    g_camera_clips=g_camera_visible=1;g_camera_pending_action=g_camera_drag=0;
    if(g_camera_requested.w<48)g_camera_requested.w=48;
    if(g_camera_requested.h<16)g_camera_requested.h=16;
    mac_clips_refresh();mac_camera_release_hidden();mac_camera_tick_start();
    mac_camera_preview_sync();plat_invalidate();
}
static int mac_clips_hit(int col,int row) {
    int h=g_camera_rect.h;
    if(row==1) {
        if(col>=1 && col<9)return MAC_CLIPS_CAMERA;
        if(col>=10 && col<18)return MAC_CLIPS_RECORDINGS;
        if(col>=19 && col<29)return MAC_CLIPS_EXPORTS;
    }
    if(row>=2 && row<2+mac_clips_capacity() && col>0 && col<g_camera_rect.w-1) {
        int item=g_clips_offset+row-2;
        if(item<(int)mac_clips_rows().count)return MAC_CLIPS_ITEM+item;
    }
    if(row==h-4) {
        if(col>=1 && col<8)return MAC_CLIPS_REPLAY;
        if(col>=9 && col<14)return MAC_CLIPS_MP4;
        if(col>=15 && col<19)return MAC_CLIPS_ZIP;
        if(col>=20 && col<28)return MAC_CLIPS_OUTPUT;
    }
    if(row==h-3) {
        if(col>=1 && col<8)return MAC_CLIPS_FILES;
        if(col>=9 && col<18)return MAC_CLIPS_REFRESH;
        if(col>=20 && col<23)return MAC_CLIPS_PREVIOUS;
        if(col>=24 && col<27)return MAC_CLIPS_NEXT;
    }
    return MAC_CAMERA_BODY;
}
static void mac_clips_action(int hit) {
    if(hit==MAC_CLIPS_CAMERA){mac_camera_open_preview();return;}
    mac_clips_clamp();
    if(hit==MAC_CLIPS_RECORDINGS || hit==MAC_CLIPS_EXPORTS) {
        g_clips_exports=hit==MAC_CLIPS_EXPORTS;mac_clips_forget_selection();
        g_clips_selected=g_clips_offset=0;mac_clips_refresh();mac_clips_select_index(0);
    } else if(hit==MAC_CLIPS_REFRESH)mac_clips_refresh();
    else if(hit==MAC_CLIPS_PREVIOUS || hit==MAC_CLIPS_NEXT)mac_clips_select_index(g_clips_selected+(hit==MAC_CLIPS_NEXT?1:-1));
    else if(hit>=MAC_CLIPS_ITEM)mac_clips_select_index(hit-MAC_CLIPS_ITEM);
    else {
        NSDictionary *item=mac_clips_selected(),*job=mac_clips_job(item);
        NSString *bundle=g_clips_exports?item[@"bundle"]:item[@"path"];
        if(hit==MAC_CLIPS_FILES)mac_record_file_viewer(job[@"output"] ?: bundle ?: record_root());
        else if(hit==MAC_CLIPS_OUTPUT && [job[@"state"] isEqual:@"done"])record_library_open_output(job[@"output"]);
        else if(bundle && hit==MAC_CLIPS_REPLAY)record_library_open(bundle);
        else if(bundle && (hit==MAC_CLIPS_MP4 || hit==MAC_CLIPS_ZIP)) {
            if(mac_record_export(bundle,nil,hit==MAC_CLIPS_ZIP)) {
                g_clips_exports=1;g_clips_offset=0;mac_clips_select_index((int)mac_record_jobs().count-1);
            }
        }
    }
    mac_clips_clamp();plat_invalidate();
}
static void mac_clips_text(NSString *text,int col,int end,int row,uint32_t color) {
    wchar_t wide[1024];utf8_to_wide(text.UTF8String ?: "",wide,1024);chrome_label(wide,col,end,row,color);
}
static void mac_clips_draw(void) {
    mac_clips_clamp();HcTvRect r=g_camera_rect;int h=r.h,y=r.y;
    NSDictionary *item=mac_clips_selected(),*job=mac_clips_job(item);
    chrome_label(L"↕ Clips",1,r.w-3,y,g_look.text);
    if(mac_record_exports_active()) {
        wchar_t drip[2]={g_ndrip?g_drip[(GetTickCount()/DRIP_MS)%g_ndrip]:0x2800,0};
        chrome_label(drip,9,10,y,AMBER);
    }
    mac_camera_label(L"−",r.w-2,1,y,g_look.text,MAC_CAMERA_MINIMIZE);
    mac_camera_label(L"Camera",1,8,y+1,g_look.text_lo,MAC_CLIPS_CAMERA);
    mac_camera_label(L"Clips",10,8,y+1,!g_clips_exports?AMBER:g_look.text_lo,MAC_CLIPS_RECORDINGS);
    mac_camera_label(L"Exports",19,10,y+1,g_clips_exports?AMBER:g_look.text_lo,MAC_CLIPS_EXPORTS);
    NSArray *rows=mac_clips_rows();int capacity=mac_clips_capacity();
    for(int i=0;i<capacity && g_clips_offset+i<(int)rows.count;i++) {
        int n=g_clips_offset+i;NSDictionary *entry=rows[n];
        if(n==g_clips_selected)fill_rect(CW,(y+2+i)*CH,(r.w-1)*CW,(y+3+i)*CH,TAB_HOVER,1);
        NSString *mark=n==g_clips_selected?@"›":@" ";
        if(g_clips_exports) {
            if([entry[@"state"] isEqual:@"running"])mark=[NSString stringWithFormat:@"%C",(unichar)(g_ndrip?g_drip[(GetTickCount()/DRIP_MS)%g_ndrip]:0x2800)];
            else mark=[entry[@"state"] isEqual:@"done"]?@"✓":@"!";
        }
        NSString *label=[NSString stringWithFormat:@"%@ %@ %@",mark,g_clips_exports?[entry[@"kind"] uppercaseString]:entry[@"duration"],entry[@"title"]];
        mac_clips_text(label,1,r.w-1,y+2+i,n==g_clips_selected?g_look.text:g_look.text_lo);
    }
    if(!rows.count)mac_clips_text(g_clips_exports?@"No exports yet":@"No saved clips yet",2,r.w-2,y+2,g_look.dim);
    mac_clips_text(g_clips_exports?[job[@"output"] lastPathComponent]:item[@"date"],1,r.w-1,y+h-6,g_look.detail);
    if(job) {
        double progress=[job[@"progress"] doubleValue];int width=chrome_max(4,r.w-10);
        wchar_t bar[256];int count=chrome_min(width,240),filled=(int)floor(fmax(0,fmin(1,progress))*count);
        bar[0]='[';for(int i=0;i<count;i++)bar[i+1]=i<filled?'=':' ';bar[count+1]=']';bar[count+2]=0;
        chrome_label(bar,1,count+3,y+h-5,AMBER);
        mac_clips_text(progress<0?@"…":[NSString stringWithFormat:@"%d%%",(int)floor(progress*100)],count+4,r.w-1,y+h-5,g_look.text);
    }
    uint32_t available=item?g_look.text_lo:g_look.dim;
    mac_camera_label(L"Replay",1,7,y+h-4,available,MAC_CLIPS_REPLAY);
    mac_camera_label(L"MP4",9,5,y+h-4,available,MAC_CLIPS_MP4);
    mac_camera_label(L"ZIP",15,4,y+h-4,available,MAC_CLIPS_ZIP);
    mac_camera_label(L"Output",20,8,y+h-4,[job[@"state"] isEqual:@"done"]?AMBER:g_look.dim,MAC_CLIPS_OUTPUT);
    mac_camera_label(L"Files",1,7,y+h-3,g_look.text_lo,MAC_CLIPS_FILES);
    mac_camera_label(L"Refresh",9,9,y+h-3,g_look.text_lo,MAC_CLIPS_REFRESH);
    mac_camera_label(L"↑",20,3,y+h-3,g_look.text_lo,MAC_CLIPS_PREVIOUS);
    mac_camera_label(L"↓",24,3,y+h-3,g_look.text_lo,MAC_CLIPS_NEXT);
    mac_clips_text(job?job[@"message"]:g_record_status,1,r.w-1,y+h-2,g_look.detail);
    mac_clips_text([NSString stringWithFormat:@"%d/%lu · exports keep running",rows.count?g_clips_selected+1:0,(unsigned long)rows.count],1,r.w-3,y+h-1,g_look.dim);
    int grip=chrome_min(CW-3,CH-5),bottom=(y+h)*CH;
    for(int len=grip;len>=2;len-=3)for(int step=0;step<len;step++) {
        int x=r.w*CW-3-step,py=bottom-3-(len-1-step);fill_rect(x,py,x+1,py+1,g_look.detail,1);
    }
}
