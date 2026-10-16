/* Browser chrome is drawn and hit-tested in character coordinates. */
typedef struct { int back,forward,url,urlWidth,go,close,add,mode,controlsRow,urlRow,rows; } BrowserNav;
static BrowserNav browser_nav_geometry(int cols,int lights,int baseRows) {
    BrowserNav n={0};int start=lights;
    if(cols<70){start=0;n.controlsRow=baseRows;n.urlRow=baseRows+1;}
    n.back=start;n.forward=start+3;
    n.close=cols-14;n.add=cols-11;n.mode=cols-8;
    n.url=cols<70 ? 1 : start+6;n.go=cols<70 ? cols-5 : cols-18;
    n.urlWidth=n.go-n.url-1;n.rows=MAX(baseRows,n.urlRow+1);
    return n;
}
static BrowserNav browser_nav(void) {
    int w,h;plat_client_size(&w,&h);int cols=MAX(10,w/CW),lights=0;
    for(int i=0;i<3;i++){NSRect r=mac_title_button_rect(i);int end=(int)ceil(NSMaxX(r)/CW)+1;if(end>lights)lights=end;}
    return browser_nav_geometry(cols,lights,MAX(1,(mac_title_native_height()+CH-1)/CH));
}
static int mac_browser_title_height(void) {
    if(!browser_for(active_tab()))return 0;
    return browser_nav().rows*CH;
}
static int mac_browser_nav_editing(void) { HCBrowser *b=browser_for(active_tab());return b && b->editKind==1; }
static int mac_browser_page_rows(Tab *tb) {
    HCBrowser *b=browser_for(tb);if(!b || !b->inspectOpen)return tb ? tb->t.rows : 0;
    return MAX(3,tb->t.rows-MAX(5,tb->t.rows/3));
}
static void browser_ui_text(NSString *text,int x,int y,int width,uint32_t color) {
    if(width<1 || y<0)return;wchar_t chars[4096];utf8_to_wide(text.UTF8String ?: "",chars,4096);
    draw_label(chars,(float)(x*CW),(float)(y*CH),(float)(width*CW),color);
}
static void browser_ui_box(int x,int y,int width,int height) {
    fill_rect(x*CW,y*CH,(x+width)*CW,(y+height)*CH,BAR_BG,1);
    browser_ui_text(@"┌",x,y,1,g_look.dim);browser_ui_text(@"┐",x+width-1,y,1,g_look.dim);
    browser_ui_text(@"└",x,y+height-1,1,g_look.dim);browser_ui_text(@"┘",x+width-1,y+height-1,1,g_look.dim);
    for(int i=1;i<width-1;i++){browser_ui_text(@"─",x+i,y,1,g_look.dim);browser_ui_text(@"─",x+i,y+height-1,1,g_look.dim);}
    for(int j=1;j<height-1;j++){browser_ui_text(@"│",x,y+j,1,g_look.dim);browser_ui_text(@"│",x+width-1,y+j,1,g_look.dim);}
}
static void browser_ui_menu_clear(HCBrowser *b) { b->menuKind=0;[b->menuItems release];b->menuItems=nil;[b->menuURL release];b->menuURL=nil; }
static void browser_ui_end_edit(HCBrowser *b) { b->editKind=0;[b->editText release];b->editText=nil;b->editAll=0;plat_invalidate(); }
static void browser_ui_eval_value(HCBrowser *b,int node,int revision,int documentEpoch,NSString *value) {
    if(b->epoch!=documentEpoch)return;revision=b->generation;
    NSData *data=[NSJSONSerialization dataWithJSONObject:@[value] options:0 error:nil];
    NSString *array=[[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    NSString *code=[NSString stringWithFormat:@"globalThis.hardcoreBrowser?.activate(%d,%d,%@)",node,revision,[array substringWithRange:NSMakeRange(1,array.length-2)]];
    [b->web evaluateJavaScript:code inFrame:nil inContentWorld:browser_world() completionHandler:nil];
}
static void browser_ui_commit(HCBrowser *b) {
    if(!b->editKind)return;
    NSString *value=[[b->editText copy] autorelease];int kind=b->editKind,node=b->editNode,revision=b->editRevision,epoch=b->editEpoch;
    if(kind==1) {
        NSString *url=browser_url(value);if(!url){NSBeep();return;}
        browser_ui_end_edit(b);[b load:url];
    } else {browser_ui_end_edit(b);browser_ui_eval_value(b,node,revision,epoch,value);}
    if(active_tab() && active_tab()->browser==b)mac_browser_sync();
}
static void browser_ui_begin_edit(HCBrowser *b,int kind,NSString *value) {
    [b->editText release];b->editText=[[NSMutableString alloc] initWithString:value ?: @""];
    b->editKind=kind;b->editCaret=(int)b->editText.length;b->editAll=1;
    browser_ui_menu_clear(b);[g_win makeFirstResponder:g_view];
    /* Programmatic navigation focus has no pointer target: (0,0) may now be
       occupied by the docked menu, which correctly suppresses title hover. */
    if(kind==1)mac_title_hover_at(-1,-1);plat_invalidate();
}
static void mac_browser_begin_control(Tab *tb,int node,int revision,int documentEpoch,NSDictionary *info) {
    HCBrowser *b=browser_for(tb);if(!b)return;
    NSString *tag=browser_string(info[@"tag"]),*type=browser_string(info[@"type"]);
    if([tag isEqual:@"SELECT"]) {
        browser_ui_menu_clear(b);b->menuKind=2;b->menuNode=node;b->menuRevision=revision;
        b->menuItems=[info[@"options"] copy];b->menuSelected=0;b->menuWidth=MIN(tb->t.cols,36);
        b->menuX=MAX(0,MIN(tb->t.cols-b->menuWidth,g_mouse_x/CW));b->menuY=MAX(0,MIN(tb->t.rows-4,g_mouse_y/CH));
        for(NSUInteger i=0;i<b->menuItems.count;i++)if([b->menuItems[i][@"value"] isEqual:info[@"value"]])b->menuSelected=(int)i;
        [g_win makeFirstResponder:g_view];plat_invalidate();return;
    }
    BOOL text=[tag isEqual:@"TEXTAREA"] || ([tag isEqual:@"INPUT"] && ![@[@"checkbox",@"radio",@"button",@"submit",@"reset",@"file",@"image",@"hidden"] containsObject:type]);
    if(text) {
        b->editNode=node;b->editRevision=revision;b->editEpoch=documentEpoch;b->editSecret=[type isEqual:@"password"];
        b->editWidth=MIN(tb->t.cols,MAX(16,MIN(50,tb->t.cols-2)));
        b->editHeight=[tag isEqual:@"TEXTAREA"] ? MIN(7,tb->t.rows) : 4;
        b->editX=MAX(0,MIN(tb->t.cols-b->editWidth,g_mouse_x/CW));
        b->editY=MAX(0,MIN(tb->t.rows-b->editHeight,g_mouse_y/CH));
        browser_ui_begin_edit(b,[tag isEqual:@"TEXTAREA"] ? 3 : 2,browser_string(info[@"value"]));return;
    }
    NSString *code=[NSString stringWithFormat:@"globalThis.hardcoreBrowser?.activate(%d,%d)",node,revision];
    [b->web evaluateJavaScript:code inFrame:nil inContentWorld:browser_world() completionHandler:nil];
}
static void mac_browser_address_grid(void) {
    Tab *tb=active_tab();if(!tb || tb->kind!=TK_BROWSER)tb=mac_browser_new(@"hardcore:welcome",tb ? tb->id : 0,0);
    HCBrowser *b=browser_for(tb);if(b)browser_ui_begin_edit(b,1,[NSString stringWithUTF8String:tb->browser_url]);
}
static int mac_browser_insert(NSString *text) {
    HCBrowser *b=browser_for(active_tab());if(!b || !b->editKind)return 0;
    if(b->editAll){[b->editText setString:@""];b->editCaret=0;b->editAll=0;}
    if(b->editText.length+text.length>4095){NSBeep();return 1;}
    if(b->editKind!=3)text=[[text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet] componentsJoinedByString:@" "];
    [b->editText insertString:text atIndex:b->editCaret];b->editCaret+=(int)text.length;plat_invalidate();return 1;
}
static void browser_inspect_request(HCBrowser *b,int node) {
    int revision=b->generation,epoch=b->epoch,identity=b->tabID;
    NSString *js=[NSString stringWithFormat:@"globalThis.hardcoreBrowser?.inspect(%d,%d)",node,revision];
    [b->web evaluateJavaScript:js inFrame:nil inContentWorld:browser_world() completionHandler:^(id result,NSError *error){
        Tab *tb=tab_by_id(identity);if(error || !tb || tb->browser!=b || b->generation!=revision || b->epoch!=epoch || ![result isKindOfClass:NSDictionary.class])return;
        int oldScroll=[b->inspection[@"id"] isEqual:result[@"id"]] ? b->inspectScroll : 0;
        [b->inspection release];b->inspection=[result copy];b->inspectRevision=revision;b->inspectOpen=1;b->inspectScroll=oldScroll;b->dirty=1;
        mac_browser_resize(tb);plat_invalidate();
    }];
}
static NSArray *browser_inspect_lines(HCBrowser *b) {
    NSDictionary *d=b->inspection;if(!d)return @[@{@"text":@"Choose Inspect element from the page context menu."}];
    NSMutableArray *lines=[NSMutableArray array];
    [lines addObject:@{@"text":[NSString stringWithFormat:@"<%@>",browser_string(d[@"tag"])]}];
    if(b->inspectRevision!=b->generation)[lines addObject:@{@"text":@"Page changed; inspect again to refresh this node."}];
    if(b->inspectTab==2) {
        for(NSString *line in [browser_string(d[@"text"]) componentsSeparatedByString:@"\n"]) [lines addObject:@{@"text":line}];
    } else {
        NSArray *items=b->inspectTab==1 ? d[@"style"] : d[@"attributes"];
        for(NSDictionary *item in items)[lines addObject:@{@"text":[NSString stringWithFormat:@"  %@ = %@",browser_string(item[@"name"]),browser_string(item[@"value"])]}];
        if(!b->inspectTab)for(NSDictionary *child in d[@"children"])[lines addObject:@{@"text":[NSString stringWithFormat:@"  <%@> %@",browser_string(child[@"tag"]),browser_string(child[@"text"])],@"id":child[@"id"] ?: @0}];
    }
    return lines;
}
static void browser_context_target(int mx,int my,int node,int link) {
    Tab *tb=active_tab();HCBrowser *b=browser_for(tb);if(!b)return;
    int col=mx/CW,row=my/CH;
    int revision=b->generation,epoch=b->epoch;
    browser_ui_menu_clear(b);b->menuKind=1;b->menuNode=node;b->menuRevision=revision;
    b->menuWidth=MIN(tb->t.cols,28);b->menuX=MAX(0,MIN(tb->t.cols-b->menuWidth,col));b->menuY=MAX(0,MIN(tb->t.rows-8,row));
    b->menuItems=[@[@{@"label":@"Copy selection",@"op":@3},@{@"label":@"Inspect element",@"op":@4},@{@"label":@"Back",@"op":@5},@{@"label":@"Reload",@"op":@6}] retain];
    NSString *js=[NSString stringWithFormat:@"globalThis.hardcoreBrowser?.info(%d,%d)",link ? link : node,revision];
    [b->web evaluateJavaScript:js inFrame:nil inContentWorld:browser_world() completionHandler:^(id info,NSError *error){
        if(error || b->generation!=revision || b->epoch!=epoch || b->menuKind!=1 || b->menuNode!=node || ![info isKindOfClass:NSDictionary.class])return;
        NSString *url=browser_string(info[@"href"]);if(!url.length)url=browser_string(info[@"src"]);
        if(url.length){[b->menuURL release];b->menuURL=[url copy];[b->menuItems release];b->menuItems=[@[
            @{@"label":@"Copy link location",@"op":@1},@{@"label":@"Open in new browser tab",@"op":@2},
            @{@"label":@"Copy selection",@"op":@3},@{@"label":@"Inspect element",@"op":@4},@{@"label":@"Back",@"op":@5},@{@"label":@"Reload",@"op":@6}] retain];}
        plat_invalidate();
    }];
    [g_win makeFirstResponder:g_view];plat_invalidate();
}
static void browser_ui_menu_choose(HCBrowser *b,int at) {
    if(at<0 || at>=(int)b->menuItems.count)return;
    NSDictionary *item=[[b->menuItems[at] retain] autorelease];NSString *url=[[b->menuURL copy] autorelease];
    int kind=b->menuKind,node=b->menuNode,rev=b->menuRevision;
    browser_ui_menu_clear(b);
    if(kind==2){browser_ui_eval_value(b,node,rev,b->epoch,browser_string(item[@"value"]));return;}
    switch([item[@"op"] intValue]) {
    case 1:set_clipboard_utf8(url.UTF8String,(int)[url lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);break;
    case 2:if(browser_url(url))mac_browser_new(url,active_tab()->parent_id,0);break;
    case 3:
        if(active_tab()->browser_native){
            int identity=b->tabID;
            [b->web evaluateJavaScript:@"globalThis.getSelection()?.toString() || ''" inFrame:nil inContentWorld:browser_world() completionHandler:^(id value,NSError *error){
                if(!error && tab_by_id(identity) && [value isKindOfClass:NSString.class])set_clipboard_utf8([value UTF8String],(int)[value lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
            }];
        }else sel_copy();break;
    case 4:if(rev==b->generation)browser_inspect_request(b,node);break;
    case 5:mac_browser_command(1);break;case 6:mac_browser_command(3);break;
    }
    if(active_tab() && active_tab()->browser==b && active_tab()->browser_native)mac_browser_sync();
    plat_invalidate();
}
static int browser_ui_menu_offset(HCBrowser *b,int capacity) {
    return capacity>0 ? (b->menuSelected/capacity)*capacity : 0;
}
static int mac_browser_ui_key(NSEvent *event) {
    HCBrowser *b=browser_for(active_tab());if(!b)return 0;
    int key=event.keyCode;BOOL cmd=(event.modifierFlags&NSEventModifierFlagCommand)!=0;
    if(b->menuKind) {
        if(key==53)browser_ui_menu_clear(b);
        else if(key==126)b->menuSelected=MAX(0,b->menuSelected-1);
        else if(key==125)b->menuSelected=MIN((int)b->menuItems.count-1,b->menuSelected+1);
        else if(key==36 || key==76)browser_ui_menu_choose(b,b->menuSelected);
        plat_invalidate();return 1;
    }
    if(!b->editKind)return 0;
    if(cmd) {
        NSString *keyText=event.charactersIgnoringModifiers.lowercaseString;
        if([keyText isEqual:@"a"])b->editAll=1;
        else if([keyText isEqual:@"v"]){NSString *s=[[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];if(s)mac_browser_insert(s);}
        else if([keyText isEqual:@"c"] && b->editAll && !b->editSecret)set_clipboard_utf8(b->editText.UTF8String,(int)[b->editText lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        else if(key==36 || key==76)browser_ui_commit(b);
        else return 0;
        plat_invalidate();return 1;
    }
    if(key==53)browser_ui_end_edit(b);
    else if(key==36 || key==76){if(b->editKind==3 && !(event.modifierFlags&NSEventModifierFlagControl))mac_browser_insert(@"\n");else browser_ui_commit(b);}
    else if(key==123){if(b->editAll)b->editCaret=0;else if(b->editCaret>0)b->editCaret=(int)[b->editText rangeOfComposedCharacterSequenceAtIndex:b->editCaret-1].location;b->editAll=0;}
    else if(key==124){if(b->editAll)b->editCaret=(int)b->editText.length;else if(b->editCaret<(int)b->editText.length)b->editCaret=(int)NSMaxRange([b->editText rangeOfComposedCharacterSequenceAtIndex:b->editCaret]);b->editAll=0;}
    else if(key==115){b->editCaret=0;b->editAll=0;}
    else if(key==119){b->editCaret=(int)b->editText.length;b->editAll=0;}
    else if(key==51 || key==117){
        if(b->editAll){[b->editText setString:@""];b->editCaret=0;b->editAll=0;}
        else if(key==51 && b->editCaret>0){NSRange r=[b->editText rangeOfComposedCharacterSequenceAtIndex:b->editCaret-1];[b->editText deleteCharactersInRange:r];b->editCaret=(int)r.location;}
        else if(key==117 && b->editCaret<(int)b->editText.length)[b->editText deleteCharactersInRange:[b->editText rangeOfComposedCharacterSequenceAtIndex:b->editCaret]];
    } else return 0;
    plat_invalidate();return 1;
}
static int mac_browser_ui_mouse(int mx,int my) {
    Tab *tb=active_tab();HCBrowser *b=browser_for(tb);if(!b)return 0;
    int col=mx/CW,row=my/CH;
    if(b->menuKind) {
        int cap=MAX(1,MIN(10,tb->t.rows-b->menuY-2)),offset=browser_ui_menu_offset(b,cap);
        if(col>=b->menuX && col<b->menuX+b->menuWidth && row>b->menuY && row<=b->menuY+cap)
            browser_ui_menu_choose(b,offset+row-b->menuY-1);
        else {browser_ui_menu_clear(b);if(tb->browser_native)mac_browser_sync();}
        plat_invalidate();return 1;
    }
    if(b->inspectOpen && row>=mac_browser_page_rows(tb)) {
        int top=mac_browser_page_rows(tb);
        if(row==top) {
            if(col>=tb->t.cols-3){b->inspectOpen=0;b->dirty=1;mac_browser_resize(tb);}
            else if(col<10){if(b->inspectRevision==b->generation)browser_inspect_request(b,[b->inspection[@"parent"] intValue]);}
            else {b->inspectTab=col<18 ? 0 : col<27 ? 1 : 2;b->inspectScroll=0;}
        } else {
            NSArray *lines=browser_inspect_lines(b);int at=row-top-1+b->inspectScroll;
            if(at>=0 && at<(int)lines.count && [lines[at][@"id"] intValue] && b->inspectRevision==b->generation)browser_inspect_request(b,[lines[at][@"id"] intValue]);
        }
        plat_invalidate();return 1;
    }
    if(g_mac_title_hover) {
        BrowserNav n=browser_nav();
        if(row==n.controlsRow) {
            if(col>=n.back && col<n.back+3){mac_browser_command(1);return 1;}
            if(col>=n.forward && col<n.forward+3){mac_browser_command(2);return 1;}
            if(col>=n.close && col<n.close+3){close_tab(g_active);return 1;}
            if(col>=n.add && col<n.add+3){mac_browser_open_welcome();return 1;}
            if(col>=n.mode && col<n.mode+8){mac_browser_command(0);return 1;}
        }
        if(row==n.urlRow && col>=n.url-1 && col<n.go){
            if(b->editKind==1){b->editCaret=MIN((int)b->editText.length,b->editViewStart+MAX(0,col-n.url));b->editAll=0;plat_invalidate();}
            else mac_browser_address_grid();return 1;}
        if(row==n.urlRow && col>=n.go && col<n.go+4){if(b->editKind==1)browser_ui_commit(b);else [b load:[NSString stringWithUTF8String:tb->browser_url]];return 1;}
    }
    if(b->editKind && b->editKind!=1) {
        if(col>=b->editX && col<b->editX+b->editWidth && row>=b->editY && row<b->editY+b->editHeight) {
            if(row==b->editY && col>=b->editX+b->editWidth-5)browser_ui_commit(b);
            else if(row>b->editY && row<b->editY+b->editHeight-1){
                b->editCaret=MIN((int)b->editText.length,MAX(0,b->editCaret-(b->editWidth-4))+MAX(0,col-b->editX-1));b->editAll=0;plat_invalidate();
            }
            return 1;
        }
        browser_ui_end_edit(b);
    }
    if(b->editKind==1 && !mac_title_contains(mx,my)){browser_ui_end_edit(b);mac_title_hover_at(mx,my);}
    return 0;
}
static int mac_browser_ui_contains(int mx,int my) {
    HCBrowser *b=browser_for(active_tab());if(!b)return 0;
    return b->menuKind || b->editKind>1 || (b->inspectOpen && my>=mac_browser_page_rows(active_tab())*CH);
}
static int mac_browser_ui_scroll(int mx,int my,int lines) {
    (void)mx;HCBrowser *b=browser_for(active_tab());if(!b)return 0;
    if(b->menuKind){b->menuSelected=MAX(0,MIN((int)b->menuItems.count-1,b->menuSelected+lines));plat_invalidate();return 1;}
    if(b->inspectOpen && my>=mac_browser_page_rows(active_tab())*CH){int count=(int)browser_inspect_lines(b).count;
        b->inspectScroll=MAX(0,MIN(MAX(0,count-(active_tab()->t.rows-mac_browser_page_rows(active_tab())-1)),b->inspectScroll+lines));plat_invalidate();return 1;}
    return 0;
}
static uint32_t browser_editor_cp(NSString *text,int at,int *length) {
    uint32_t cp=[text characterAtIndex:at];*length=1;
    if(cp>=0xD800 && cp<=0xDBFF && at+1<(int)text.length){uint32_t low=[text characterAtIndex:at+1];
        if(low>=0xDC00 && low<=0xDFFF){cp=0x10000+((cp-0xD800)<<10)+low-0xDC00;*length=2;}}
    return cp;
}
static void browser_ui_draw_editor(HCBrowser *b,int x,int y,int width,int height) {
    NSString *text=b->editSecret ? [@"" stringByPaddingToLength:b->editText.length withString:@"•" startingAtIndex:0] : b->editText;
    int caretX=0,caretY=0;
    for(int at=0;at<b->editCaret && at<(int)text.length;) {
        int length;uint32_t cp=browser_editor_cp(text,at,&length);int w=cp_width(cp);
        if(cp=='\n'){caretX=0;caretY++;}else {if(height>1 && caretX+w>width){caretX=0;caretY++;}caretX+=w;}
        at+=length;
    }
    int origin=height==1 ? MAX(0,caretX-width+2) : MAX(0,caretY-height+1);b->editViewStart=origin;
    if(b->editAll)fill_rect(x*CW,y*CH,(x+width)*CW,(y+height)*CH,SEL_BG,1);
    int cx=0,cy=0;
    for(int at=0;at<(int)text.length;) {
        int length;uint32_t cp=browser_editor_cp(text,at,&length);int w=cp_width(cp);
        if(cp=='\n'){cx=0;cy++;at+=length;continue;}
        if(height>1 && cx+w>width){cx=0;cy++;}
        int dx=height==1 ? cx-origin : cx,dy=height==1 ? 0 : cy-origin;
        if(w>0 && dx>=0 && dx+w<=width && dy>=0 && dy<height)
            browser_ui_text([text substringWithRange:NSMakeRange(at,length)],x+dx,y+dy,w,DEF_FG);
        cx+=w;at+=length;
    }
    int dx=height==1 ? caretX-origin : caretX,dy=height==1 ? 0 : caretY-origin;
    if(dx>=width){dx=0;dy++;}dy=MIN(height-1,MAX(0,dy));dx=MIN(width-1,MAX(0,dx));
    fill_rect((x+dx)*CW,(y+dy)*CH,(x+dx)*CW+1,(y+dy+1)*CH,AMBER,1);
}

static void mac_browser_draw_nav(void) {
    /* A preview has no interactive browser toolbar or edit cursor. Its
       real tab keeps its own responder and draft until a deliberate click. */
    if(chrome_preview_active())return;
    Tab *tb=active_tab();HCBrowser *b=browser_for(tb);if(!b || !g_mac_title_hover)return;
    BrowserNav n=browser_nav();
    browser_ui_text(@"[←]",n.back,n.controlsRow,3,b->web.canGoBack ? DEF_FG : g_look.dim);
    browser_ui_text(@"[→]",n.forward,n.controlsRow,3,b->web.canGoForward ? DEF_FG : g_look.dim);
    browser_ui_text(@"[×]",n.close,n.controlsRow,3,DEF_FG);browser_ui_text(@"[+]",n.add,n.controlsRow,3,DEF_FG);
    browser_ui_text(tb->browser_native ? @"[Native]" : @"[ Mono ]",n.mode,n.controlsRow,8,AMBER);
    browser_ui_text(@"│",n.url-1,n.urlRow,1,g_look.dim);browser_ui_text(@"│",n.go-1,n.urlRow,1,g_look.dim);
    if(b->editKind==1)browser_ui_draw_editor(b,n.url,n.urlRow,n.urlWidth,1);
    else browser_ui_text([NSString stringWithUTF8String:tb->browser_url],n.url,n.urlRow,n.urlWidth,DEF_FG);
    browser_ui_text(@"[Go]",n.go,n.urlRow,4,DEF_FG);
    fill_rect(0,n.rows*CH-1,tb->t.cols*CW,n.rows*CH,g_look.dim,1);
}
static void mac_browser_draw_overlay(void) {
    /* The cached page uses the full preview grid; inspector/edit/menu state
       remains untouched in the background and cannot cover the tab list. */
    if(chrome_preview_active())return;
    Tab *tb=active_tab();HCBrowser *b=browser_for(tb);if(!b)return;
    if(b->inspectOpen) {
        int top=mac_browser_page_rows(tb),height=tb->t.rows-top;
        fill_rect(0,top*CH,tb->t.cols*CW,tb->t.rows*CH,BAR_BG,1);
        fill_rect(0,top*CH,tb->t.cols*CW,top*CH+1,g_look.dim,1);
        browser_ui_text(@"[↑ parent]",0,top,10,DEF_FG);
        browser_ui_text(@"[DOM]",11,top,6,b->inspectTab==0 ? AMBER : DEF_FG);
        browser_ui_text(@"[Style]",18,top,8,b->inspectTab==1 ? AMBER : DEF_FG);
        browser_ui_text(@"[Text]",27,top,7,b->inspectTab==2 ? AMBER : DEF_FG);
        browser_ui_text(@"[×]",tb->t.cols-3,top,3,DEF_FG);
        NSArray *lines=browser_inspect_lines(b);
        for(int i=0;i<height-1 && i+b->inspectScroll<(int)lines.count;i++) {
            NSDictionary *line=lines[i+b->inspectScroll];browser_ui_text(browser_string(line[@"text"]),0,top+i+1,tb->t.cols,[line[@"id"] intValue] ? g_shown->pal[4] : DEF_FG);
        }
    }
    if(b->editKind>1) {
        browser_ui_box(b->editX,b->editY,b->editWidth,b->editHeight);
        browser_ui_text(b->editKind==3 ? @" Ctrl-Return applies " : @" Return applies ",b->editX+1,b->editY,b->editWidth-7,g_look.detail);
        browser_ui_text(@"[OK]",b->editX+b->editWidth-5,b->editY,4,AMBER);
        browser_ui_draw_editor(b,b->editX+1,b->editY+1,b->editWidth-2,b->editHeight-2);
    }
    if(b->menuKind) {
        int cap=MAX(1,MIN(10,tb->t.rows-b->menuY-2)),visible=MIN(cap,(int)b->menuItems.count),offset=browser_ui_menu_offset(b,cap);
        browser_ui_box(b->menuX,b->menuY,b->menuWidth,visible+2);
        for(int i=0;i<visible && offset+i<(int)b->menuItems.count;i++) {
            int selected=offset+i==b->menuSelected;
            if(selected)fill_rect((b->menuX+1)*CW,(b->menuY+i+1)*CH,(b->menuX+b->menuWidth-1)*CW,(b->menuY+i+2)*CH,TAB_HOVER,1);
            browser_ui_text(browser_string(b->menuItems[offset+i][@"label"]),b->menuX+1,b->menuY+i+1,b->menuWidth-2,selected ? AMBER : DEF_FG);
        }
    }
}
static int mac_browser_hover_link(void) {
    if(chrome_preview_active())return 0;
    Tab *tb=active_tab();HCBrowser *b=browser_for(tb);if(!b || tb->browser_native || g_mac_title_hover || b->menuKind || b->editKind || over_chrome(g_mouse_x,g_mouse_y))return 0;
    int x=g_mouse_x/CW,y=g_mouse_y/CH;
    if(g_mouse_x<0 || g_mouse_y<0 || x>=tb->t.cols || y>=b->pageRows)return 0;
    return tb->t.row[0][y][x].link;
}
static void mac_browser_draw_tile(Tab *tb,const Cell *cell,int x,int y) {
    HCBrowser *b=browser_for(tb);int id=cell->ch&255;if(!b || id<1 || id>(int)b->images.count)return;
    HCBrowserImage *image=b->images[id-1];int width=(cell->ch>>8)&255,height=(cell->ch>>16)&255;
    int scale=MAX(1,g_scale),cw=CW/scale,ch=CH/scale;
    browser_image_palette(image,width*cw,height*ch);
    if(!image->indexed) {
        if(!cell->attr)browser_ui_text(image->failed ? @"[img]" : @"[…]",x,y,MIN(width,5),g_look.dim);
        return;
    }
    int tx=cell->attr&15,ty=cell->attr>>4;
    for(int py=0;py<ch;py++) {
        int px=0;while(px<cw) {
            unsigned char color=image->indexed[(ty*ch+py)*image->qw+tx*cw+px];int end=px+1;
            while(end<cw && image->indexed[(ty*ch+py)*image->qw+tx*cw+end]==color)end++;
            fill_rect(x*CW+px*scale,y*CH+py*scale,x*CW+end*scale,y*CH+(py+1)*scale,image->palette[color],1);px=end;
        }
    }
}
static void browser_image_ready(int identity) { Tab *tb=tab_by_id(identity);HCBrowser *b=browser_for(tb);if(b){b->dirty=1;plat_invalidate();} }

static int mac_browser_editing(void) {HCBrowser *b=browser_for(active_tab());return b && b->editKind;}
static int mac_browser_copy_edit(void) {
    HCBrowser *b=browser_for(active_tab());if(!b || !b->editKind)return 0;
    if(b->editAll && !b->editSecret)set_clipboard_utf8(b->editText.UTF8String,(int)[b->editText lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    return 1;
}

static void mac_browser_context(int mx,int my) {
    Tab *tb=active_tab();HCBrowser *b=browser_for(tb);if(!b)return;
    mac_browser_prepare(tb);int x=mx/CW,y=my/CH;
    if(x<0 || x>=tb->t.cols || y<0 || y>=mac_browser_page_rows(tb))return;
    int node=b->nodeGrid && !tb->browser_native ? b->nodeGrid[y*tb->t.cols+x] : 0;
    int link=tb->browser_native ? 0 : tb->t.row[0][y][x].link;
    browser_context_target(mx,my,node,link);
}
static void mac_browser_context_native(NSDictionary *message) {
    HCBrowser *b=browser_for(active_tab());if(!b || !active_tab()->browser_native || [message[@"generation"] intValue]!=b->generation)return;
    NSPoint point=[g_view convertPoint:[g_win mouseLocationOutsideOfEventStream] fromView:nil];
    CGFloat scale=[g_win backingScaleFactor];int node=[message[@"node"] intValue],link=[message[@"link"] intValue];
    if(node<0 || node>4000 || link<0 || link>4000)return;
    browser_context_target((int)(point.x*scale),(int)(point.y*scale),node,link);
}

static void browser_inspect_refresh(HCBrowser *b) {
    if(b->inspectOpen && b->inspection)browser_inspect_request(b,[b->inspection[@"id"] intValue]);
}
static void browser_ui_reset(HCBrowser *b) {
    if(b->editKind>1)browser_ui_end_edit(b);browser_ui_menu_clear(b);[b->inspection release];b->inspection=nil;b->inspectScroll=0;
}

static int mac_browser_ui_move(int mx,int my) {
    Tab *tb=active_tab();HCBrowser *b=browser_for(tb);if(!b)return 0;
    int col=mx/CW,row=my/CH;
    if(b->menuKind){
        int cap=MAX(1,MIN(10,tb->t.rows-b->menuY-2)),offset=browser_ui_menu_offset(b,cap);
        int at=offset+row-b->menuY-1;
        if(col>b->menuX && col<b->menuX+b->menuWidth-1 && row>b->menuY && row<=b->menuY+cap && at>=0 && at<(int)b->menuItems.count){b->menuSelected=at;plat_invalidate();}
        [[NSCursor arrowCursor] set];return 1;
    }
    if(g_mac_title_hover && mac_title_contains(mx,my)){
        BrowserNav nav=browser_nav();
        if(row==nav.urlRow && col>=nav.url && col<nav.go-1)[[NSCursor IBeamCursor] set];
        else if((row==nav.controlsRow && ((col>=nav.back && col<nav.forward+3) || col>=nav.close)) || (row==nav.urlRow && col>=nav.go && col<nav.go+4))[[NSCursor pointingHandCursor] set];
        else [[NSCursor openHandCursor] set];
        return 1;
    }
    if(b->editKind>1 && col>=b->editX && col<b->editX+b->editWidth && row>=b->editY && row<b->editY+b->editHeight){[[NSCursor IBeamCursor] set];return 1;}
    if(b->inspectOpen && row>=mac_browser_page_rows(tb)){[[NSCursor arrowCursor] set];return 1;}
    return 0;
}
