/* Live WebKit documents with an embedded DOM -> bitmap-grid projection.
   Included after HCView. Runtime assets are compiled into the executable. */
#include "build/browser.inc"
#include "hardcore_browser_grid.h"
#include "hardcore_browser_label.h"

static NSString *browser_page_title(Tab *tb) {
    char utf[1024];int length=0;
    for(int i=0;i<255 && tb->t.title[i];i++)length+=utf8_encode(tb->t.title[i],utf+length);
    utf[length]=0;return [NSString stringWithUTF8String:utf] ?: @"";
}
static void mac_browser_tab_label(Tab *tb,wchar_t *out,int cap,int cols) {
    if(cap<1)return;
    out[0]=0;if(cols<1)return;
    cols=MIN(cols,MIN(cap-1,255));
    if(tb->browser_label_cols!=cols || !tb->browser_label[0]) {
        NSString *label=browser_label_format([NSString stringWithUTF8String:tb->browser_url] ?: @"",browser_page_title(tb),
            [NSString stringWithUTF8String:tb->browser_site] ?: @"",cols,tb->parent_id!=0);
        utf8_to_wide(label.UTF8String,tb->browser_label,256);tb->browser_label_cols=cols;
    }
    wcsncpy(out,tb->browser_label,(size_t)cap-1);out[cap-1]=0;
}
static void mac_browser_metadata(Tab *tb,NSString *title,NSString *site,NSString *url) {
    utf8_to_wide(title.UTF8String ?: "",tb->t.title,256);
    snprintf(tb->browser_site,sizeof(tb->browser_site),"%s",site.UTF8String ?: "");
    if(url.length)snprintf(tb->browser_url,sizeof(tb->browser_url),"%s",url.UTF8String);
    tb->browser_label_cols=0;
}
static WKContentWorld *browser_world(void) {
    return [WKContentWorld worldWithName:@"hardcore.browser"];
}
static NSString *browser_string(id value) {
    return [value isKindOfClass:NSString.class] ? value : @"";
}
static NSString *browser_url_bound(NSString *url) {
    return url && [url lengthOfBytesUsingEncoding:NSUTF8StringEncoding]<4096 ? url : nil;
}
static NSString *browser_url(NSString *input) {
    NSString *s = [input stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!s.length || [s isEqualToString:@"hardcore:welcome"]) return @"hardcore:welcome";
    if (s.length > 4095 || [s rangeOfCharacterFromSet:NSCharacterSet.controlCharacterSet].location != NSNotFound) return nil;
    if ([s hasPrefix:@"/"]) return browser_url_bound([[NSURL fileURLWithPath:s] absoluteString]);
    if (![s containsString:@"://"] && ![s hasPrefix:@"about:"]) {
        NSString *authority=[[s componentsSeparatedByString:@"/"] firstObject];
        NSRange colon=[authority rangeOfString:@":" options:NSBackwardsSearch];
        BOOL hostPort=NO;
        if (colon.location!=NSNotFound && colon.location+1<authority.length) {
            NSString *port=[authority substringFromIndex:colon.location+1];
            hostPort=[port rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location==NSNotFound;
        }
        if (colon.location==NSNotFound || hostPort) {
            BOOL local=[authority isEqualToString:@"localhost"] || [authority hasPrefix:@"localhost:"] || [authority hasPrefix:@"127.0.0.1:"];
            s=[(local ? @"http://" : @"https://") stringByAppendingString:s];
        }
    }
    NSURL *u = [NSURL URLWithString:s];
    NSString *scheme = u.scheme.lowercaseString;
    if (([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"]) && u.host.length) return browser_url_bound(u.absoluteString);
    if ([scheme isEqualToString:@"file"] && (!u.host.length || [u.host isEqualToString:@"localhost"])) return browser_url_bound(u.absoluteString);
    return [s isEqualToString:@"about:blank"] ? s : nil;
}

#include "hardcore_browser_images.h"

@interface HCBrowser : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler> {
@public
    int tabID, generation, epoch, scroll, total, dirty, requesting, changedAgain, focusedAction;
    int lastCols, lastRows, lastX, lastY, lastHeight, lastSide, lastPreview;
    WKWebView *web;
    BrowserBlock *blocks;
    int blockCount;
    NSMutableDictionary *imageCache, *anchorBlocks;
    NSMutableArray *images;
    uint16_t *nodeGrid;
    int nodeCols,nodeRows,pageRows;
    NSMutableString *editText;
    int editKind,editNode,editRevision,editEpoch,editCaret,editAll,editSecret,editX,editY,editWidth,editHeight,editViewStart;
    int menuX,menuY,menuWidth,menuKind,menuSelected,menuNode,menuRevision;
    NSArray *menuItems;
    NSString *menuURL;
    int inspectOpen,inspectTab,inspectScroll,inspectRevision;
    NSDictionary *inspection;
    NSArray *inspectLines;
    int hoverLink;
}
- (id)initWithTab:(Tab *)tb;
- (void)load:(NSString *)url;
- (void)project;
- (void)accept:(NSDictionary *)snapshot;
- (void)dispose;
@end

static void mac_browser_begin_control(Tab *tb,int node,int revision,int documentEpoch,NSDictionary *info);
static void mac_browser_address_grid(void);
static void mac_browser_context_native(NSDictionary *message);
static void browser_inspect_refresh(HCBrowser *b);
static void browser_ui_reset(HCBrowser *b);
static NSView *g_browser_root;
static void mac_browser_edit(SEL action, id sender) {
    /* Native selection and text editing belong to WebKit's responder chain. */
    if ([g_win firstResponder]!=(NSResponder *)g_view) [NSApp sendAction:action to:nil from:sender];
}
static Tab *mac_browser_new(NSString *url, int parent, int native);
static HCBrowser *browser_for(Tab *tb) { return tb && tb->kind == TK_BROWSER ? (HCBrowser *)tb->browser : nil; }
static void browser_free_blocks(HCBrowser *b) {
    for (int i=0;i<b->blockCount;i++) { free(b->blocks[i].cells);free(b->blocks[i].nodes);free(b->blocks[i].ends); }
    free(b->blocks); b->blocks=NULL; b->blockCount=0;
    [b->anchorBlocks removeAllObjects];
}
/* No terminal escape parser sees website text. Bound and validate snapshots
   even though the producer runs in our isolated JS world. Table columns retain
   their own runs and hard line breaks; styling and DOM identities travel with
   each cell instead of being reconstructed from a plain-text table. */
static int browser_integer(id value) {
    return [value respondsToSelector:@selector(intValue)] ? [value intValue] : 0;
}
static int browser_reserve(BrowserBlock *block,int *capacity,int needed) {
    if (needed<=*capacity) return 1;
    int next=MAX(*capacity*2,needed+128);
    Cell *cells=realloc(block->cells,(size_t)next*sizeof(Cell));
    if (!cells) return 0;
    block->cells=cells;
    uint16_t *nodes=realloc(block->nodes,(size_t)next*sizeof(uint16_t));
    if (!nodes) return 0;
    block->nodes=nodes; *capacity=next; return 1;
}
static void browser_decode_runs(HCBrowser *b,BrowserBlock *block,NSArray *runs,
    int heading,int *capacity,int *remaining) {
    for (id run in runs) {
        if (*remaining<=0) break;
        if (![run isKindOfClass:NSDictionary.class]) continue;
        NSString *text=browser_string(run[@"text"]);
        int node=browser_integer(run[@"node"]),action=browser_integer(run[@"action"]);
        if (node<0 || node>4000) node=0;
        if (action<0 || action>4000) action=0;
        NSDictionary *asset=[run[@"image"] isKindOfClass:NSDictionary.class] ? run[@"image"] : nil;
        if (asset && b->images.count<32) {
            NSString *src=browser_string(asset[@"src"]);
            if (src.length>3*1024*1024) src=@"";
            HCBrowserImage *image=b->imageCache[src];
            if (!image && src.length && b->imageCache.count<32) {
                image=[[[HCBrowserImage alloc] initWithSource:src owner:b->tabID] autorelease];
                b->imageCache[src]=image;
            }
            if (image) {
                [b->images addObject:image];
                int width=MAX(1,MIN(20000,browser_integer(asset[@"width"])));
                int height=MAX(1,MIN(20000,browser_integer(asset[@"height"])));
                browser_image_size(width,height,cell_w(g_font)/MAX(1,g_scale),cell_h(g_font)/MAX(1,g_scale),&image->cols,&image->rows);
                if (!browser_reserve(block,capacity,block->count+1)) { *remaining=0; break; }
                Cell cell={0}; cell.ch=(uint32_t)b->images.count; cell.wflag=BROWSER_IMAGE;
                cell.fg=(uint32_t)MIN(image->cols,MAX(1,g_cols/4)); cell.bg=(uint32_t)image->rows; cell.link=(uint16_t)action;
                block->nodes[block->count]=(uint16_t)node; block->cells[block->count++]=cell;
                (*remaining)--; continue;
            }
            text=[NSString stringWithFormat:@"[%@]",browser_string(asset[@"alt"])];
        }
        int attr=(browser_integer(run[@"bold"]) || heading ? A_BOLD : 0) |
            (browser_integer(run[@"dim"]) ? A_DIM : 0) | (browser_integer(run[@"italic"]) ? A_ITALIC : 0);
        for (NSUInteger j=0;j<text.length && *remaining>0;j++) {
            uint32_t cp=[text characterAtIndex:j];
            if (cp>=0xD800 && cp<=0xDBFF && j+1<text.length) {
                uint32_t low=[text characterAtIndex:j+1];
                if (low>=0xDC00 && low<=0xDFFF) { cp=0x10000+((cp-0xD800)<<10)+low-0xDC00; j++; }
            }
            if ((cp<32 && cp!='\n' && cp!='\t') || (cp>=0x7F && cp<0xA0) || (cp>=0xD800 && cp<=0xDFFF)) continue;
            if (cp=='\t' || cp==0xA0) cp=' ';
            if (cp=='\n' && !block->literal && !block->columns) cp=' ';
            int width=cp=='\n' ? 1 : cp_width(cp);
            if (width<1) continue; /* combining marks need a future glyph compositor */
            if (!browser_reserve(block,capacity,block->count+width)) { *remaining=0; break; }
            Cell cell={0}; cell.ch=cp; cell.link=(uint16_t)action;
            cell.fg=action ? C_PAL(4) : 0; cell.attr=(uint8_t)attr;
            if (width==2) cell.wflag=W_WIDE;
            block->nodes[block->count]=(uint16_t)node; block->cells[block->count++]=cell;
            if (width==2) { cell.ch=' '; cell.wflag=W_CONT; block->nodes[block->count]=(uint16_t)node; block->cells[block->count++]=cell; }
            (*remaining)--;
        }
    }
}
static void browser_decode(HCBrowser *b, NSDictionary *snapshot) {
    browser_free_blocks(b);
    if (!b->imageCache) b->imageCache=[[NSMutableDictionary alloc] init];
    if (!b->anchorBlocks) b->anchorBlocks=[[NSMutableDictionary alloc] init];
    if (!b->images) b->images=[[NSMutableArray alloc] init];
    [b->images removeAllObjects];
    NSArray *items=snapshot[@"blocks"];
    if (![items isKindOfClass:NSArray.class]) return;
    b->blocks=calloc(2000,sizeof(BrowserBlock));
    if (!b->blocks) return;
    int remaining=100000;
    for (id item in items) {
        if (b->blockCount>=2000 || remaining<=0) break;
        if (![item isKindOfClass:NSDictionary.class]) continue;
        NSArray *columns=[item[@"columns"] isKindOfClass:NSArray.class] ? item[@"columns"] : nil;
        if (columns.count<1 || columns.count>16) columns=nil;
        if (!columns && ![item[@"runs"] isKindOfClass:NSArray.class]) continue;
        BrowserBlock *block=&b->blocks[b->blockCount];
        NSString *kind=browser_string(item[@"kind"]);
        block->indent=MAX(0,MIN(12,browser_integer(item[@"indent"])));
        block->hanging=item[@"hang"] ? MAX(0,MIN(12,browser_integer(item[@"hang"]))) : 2;
        block->literal=[kind isEqualToString:@"code"];
        block->gap=([kind isEqualToString:@"list"] || [kind isEqualToString:@"table"] || [kind isEqualToString:@"caption"]) ? 0 : 1;
        if ([item[@"gap"] respondsToSelector:@selector(intValue)]) block->gap=MAX(0,MIN(2,browser_integer(item[@"gap"])));
        block->rule=!!browser_integer(item[@"rule"]);
        block->table=MAX(0,MIN(2000,browser_integer(item[@"table"])));
        block->header=!!browser_integer(item[@"header"]);
        block->columns=(int)columns.count;
        block->numeric=(unsigned)browser_integer(item[@"numeric"]) & ((1u<<block->columns)-1u);
        block->decoration.attr=A_DIM;
        if (block->columns) {
            block->ends=calloc((size_t)block->columns,sizeof(int));
            if (!block->ends) break;
        }
        int anchor=browser_integer(item[@"anchor"]);
        if (anchor>0 && anchor<=4000) b->anchorBlocks[@(anchor)]=@(b->blockCount);
        b->blockCount++;
        int capacity=0,heading=[kind isEqualToString:@"heading"];
        if (columns) for (int column=0;column<block->columns;column++) {
            if ([columns[column] isKindOfClass:NSArray.class]) browser_decode_runs(b,block,columns[column],heading,&capacity,&remaining);
            block->ends[column]=block->count;
        }
        else browser_decode_runs(b,block,item[@"runs"],heading,&capacity,&remaining);
    }
}

@implementation HCBrowser
- (id)initWithTab:(Tab *)tb {
    if (!(self=[super init])) return nil;
    tabID=tb->id; dirty=1;
    WKWebViewConfiguration *config=[[[WKWebViewConfiguration alloc] init] autorelease];
    WKUserContentController *controller=[[[WKUserContentController alloc] init] autorelease];
    [controller addScriptMessageHandler:self contentWorld:browser_world() name:@"hardcoreBrowser"];
    NSString *source=[NSString stringWithUTF8String:(const char *)browser_script];
    WKUserScript *script=[[[WKUserScript alloc] initWithSource:source injectionTime:WKUserScriptInjectionTimeAtDocumentStart
        forMainFrameOnly:YES inContentWorld:browser_world()] autorelease];
    [controller addUserScript:script]; config.userContentController=controller;
    web=[[WKWebView alloc] initWithFrame:[g_browser_root bounds] configuration:config];
    web.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    web.navigationDelegate=self; web.UIDelegate=self;
    [g_browser_root addSubview:web positioned:NSWindowBelow relativeTo:g_view];
    return self;
}
- (void)dispose {
    for(HCBrowserImage *image in imageCache.allValues)[image cancel];
    [web stopLoading]; web.navigationDelegate=nil; web.UIDelegate=nil;
    [web.configuration.userContentController removeScriptMessageHandlerForName:@"hardcoreBrowser" contentWorld:browser_world()];
    [web removeFromSuperview];
}
- (void)dealloc { browser_free_blocks(self);free(nodeGrid);[imageCache release];[anchorBlocks release];[images release];[editText release];
    [menuItems release];[menuURL release];[inspection release];[inspectLines release];[web release];[super dealloc]; }
- (void)load:(NSString *)url {
    scroll=0; generation=0; browser_free_blocks(self); dirty=1; editKind=0;[editText release];editText=nil;
    Tab *tb=tab_by_id(tabID);
    if (tb) {
        tb->busy=1; mac_browser_metadata(tb,nil,nil,url);
        update_busy(0); plat_invalidate();
    }
    if ([url isEqualToString:@"hardcore:welcome"]) {
        [web loadHTMLString:[NSString stringWithUTF8String:(const char *)browser_welcome] baseURL:nil];
    } else if ([NSURL URLWithString:url].isFileURL) {
        NSURL *file=[NSURL URLWithString:url];
        [web loadFileURL:file allowingReadAccessToURL:[file URLByDeletingLastPathComponent]];
    } else [web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
}
- (void)project {
    if (requesting) { changedAgain=1; return; }
    requesting=1; int documentEpoch=epoch;
    [web evaluateJavaScript:@"globalThis.hardcoreBrowser?.project()" inFrame:nil inContentWorld:browser_world()
        completionHandler:^(id value,NSError *error) {
            requesting=0;
            Tab *tb=tab_by_id(tabID);
            if (tb && tb->browser==self && epoch==documentEpoch && !error && [value isKindOfClass:NSDictionary.class]) [self accept:value];
            if (tb && tb->browser==self && changedAgain) { changedAgain=0; [self project]; }
        }];
}
- (void)accept:(NSDictionary *)snapshot {
    Tab *tb=tab_by_id(tabID); if (!tb || tb->browser!=self) return;
    browser_decode(self,snapshot);
    generation=[snapshot[@"generation"] intValue]; focusedAction=0; dirty=1;

    NSString *rawURL=browser_string(snapshot[@"url"]);
    NSString *url=rawURL.length ? browser_url(rawURL) : nil;
    if (url && ![url isEqualToString:@"about:blank"]) snprintf(tb->browser_url,sizeof(tb->browser_url),"%s",url.UTF8String);
    mac_browser_metadata(tb,browser_string(snapshot[@"title"]),browser_string(snapshot[@"site"]),nil);
    browser_inspect_refresh(self);
    if (tb==active_tab()) { g_sel_on=0; update_title(); }
    else tb->activity=1;
    plat_invalidate();
}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void)controller;
    if (message.webView==web && message.frameInfo.mainFrame && [message.body isKindOfClass:NSDictionary.class] &&
        [message.body[@"type"] isEqual:@"changed"]) [self project];
    if(message.webView==web && message.frameInfo.mainFrame && [message.body isKindOfClass:NSDictionary.class] &&
       [message.body[@"type"] isEqual:@"context"] && tab_by_id(tabID)==active_tab())mac_browser_context_native(message.body);
}
- (void)webView:(WKWebView *)view didStartProvisionalNavigation:(WKNavigation *)navigation {
    (void)view; (void)navigation;
    browser_ui_reset(self);epoch++; generation=0; scroll=0; browser_free_blocks(self); dirty=1;hoverLink=0;
    for(HCBrowserImage *image in imageCache.allValues)[image cancel];[imageCache removeAllObjects];[images removeAllObjects];
    Tab *tb=tab_by_id(tabID); if (tb) tb->busy=1;
    update_busy(0); plat_invalidate();
}
- (void)webView:(WKWebView *)view didCommitNavigation:(WKNavigation *)navigation {
    (void)navigation;
    Tab *tb=tab_by_id(tabID);if(!tb)return;
    NSString *url=browser_url(view.URL.absoluteString);
    // The address supplies an interim label while the new document projects.
    if(url && ![url isEqualToString:@"about:blank"])mac_browser_metadata(tb,nil,nil,url);
    if(tb==active_tab())update_title();
    plat_invalidate();
}
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation {
    (void)navigation;
    Tab *tb=tab_by_id(tabID); if (tb) {tb->busy=0;
        mac_browser_metadata(tb,view.title,[NSString stringWithUTF8String:tb->browser_site],nil);
        if(tb==active_tab())update_title();}
    update_busy(0); [self project]; plat_invalidate();
}
- (void)webView:(WKWebView *)view didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void)view; (void)navigation;
    if (error.code==NSURLErrorCancelled) return;
    Tab *tb=tab_by_id(tabID); if (!tb) return;
    NSString *failedURL=browser_url(browser_string(error.userInfo[NSURLErrorFailingURLStringErrorKey]));
    if (error.userInfo[NSURLErrorFailingURLStringErrorKey] && failedURL)
        snprintf(tb->browser_url,sizeof(tb->browser_url),"%s",failedURL.UTF8String);
    tb->busy=0;
    [self accept:@{@"generation":@0,@"title":@"Page failed to load",@"blocks":@[
        @{@"kind":@"heading",@"runs":@[@{@"text":@"Page failed to load"}]},
        @{@"runs":@[@{@"text":error.localizedDescription ?: @"Unknown error"}]},
        @{@"runs":@[@{@"text":@"Use Command-L to change the address or Command-R to retry."}]}]}];
    update_busy(0);
}
- (void)webView:(WKWebView *)view didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    [self webView:view didFailProvisionalNavigation:navigation withError:error];
}
- (void)webViewWebContentProcessDidTerminate:(WKWebView *)view {
    [self webView:view didFailProvisionalNavigation:nil withError:[NSError errorWithDomain:@"Hardcore" code:1
        userInfo:@{NSLocalizedDescriptionKey:@"The browser process stopped. Command-R reloads this page."}]];
}
- (void)webView:(WKWebView *)view decidePolicyForNavigationAction:(WKNavigationAction *)action
    decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void)view;
    NSString *url=action.request.URL.absoluteString;
    NSString *scheme=action.request.URL.scheme.lowercaseString;
    /* Blob/data URLs may be generated by the page itself. No shell/OS launch
       is allowed from page navigation. Address entry accepts only explicit URLs. */
    BOOL allowed=browser_url(url)!=nil || [scheme isEqualToString:@"blob"] || [scheme isEqualToString:@"data"];
    if (allowed && !action.targetFrame && url.length) {
        Tab *tb=tab_by_id(tabID);
        if (browser_url(url)) mac_browser_new(url,tb ? tb->parent_id : 0,tb ? tb->browser_native : 0);
        else [web loadRequest:action.request];
        handler(WKNavigationActionPolicyCancel); return;
    }
    handler(allowed ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}
/* JS dialogs complete without granting arbitrary host integration. */
- (void)webView:(WKWebView *)view runJavaScriptAlertPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completion {
    (void)view; (void)frame;
    NSAlert *a=[[[NSAlert alloc] init] autorelease]; a.messageText=@"Webpage message"; a.informativeText=message;
    [a runModal]; completion();
}
- (void)webView:(WKWebView *)view runJavaScriptConfirmPanelWithMessage:(NSString *)message
    initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))completion {
    (void)view; (void)frame;
    NSAlert *a=[[[NSAlert alloc] init] autorelease]; a.messageText=@"Webpage confirmation"; a.informativeText=message;
    [a addButtonWithTitle:@"OK"]; [a addButtonWithTitle:@"Cancel"]; completion([a runModal]==NSAlertFirstButtonReturn);
}
@end

static void mac_browser_sync(void) {
    if (!g_view || !g_win) return;
    Tab *active=active_tab(); g_browser_native_drag=0;
    if (active && active->kind==TK_BROWSER && !g_browser_root) {
        g_browser_root=[[NSView alloc] initWithFrame:[g_view bounds]];
        g_view.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
        [g_win setContentView:g_browser_root];
        [g_browser_root addSubview:g_view];
    }
    for (int i=0;i<g_ntabs;i++) {
        Tab *tb=g_tabs[i];
        if (tb==active && tb->kind==TK_BROWSER && !tb->browser) {
            tb->browser=[[HCBrowser alloc] initWithTab:tb];
            [browser_for(tb) load:browser_url([NSString stringWithUTF8String:tb->browser_url]) ?: @"hardcore:welcome"];
        }
        HCBrowser *b=browser_for(tb);
        if (b) { b->web.hidden=tb!=active; if (tb==active) b->dirty=1; }
    }
    BOOL native=active && active->kind==TK_BROWSER && active->browser_native;
    g_layer.opaque=!native;
    HCBrowser *current=browser_for(active);
    [g_win makeFirstResponder:native && current && !current->editKind && !current->menuKind ? (NSView *)current->web : (NSView *)g_view];
    plat_invalidate();
}
static void mac_browser_destroy(Tab *tb) {
    HCBrowser *b=browser_for(tb); if (!b) return;
    tb->browser=NULL; [b dispose]; [b release];
}
static void mac_browser_resize(Tab *tb) {
    HCBrowser *b=browser_for(tb); if (!b) return;
    NSRect rect=[g_browser_root bounds];
    if(b->inspectOpen) {CGFloat scale=[g_win backingScaleFactor];CGFloat pane=(tb->t.rows-mac_browser_page_rows(tb))*CH/scale;rect.origin.y+=pane;rect.size.height=MAX(1,rect.size.height-pane);}
    b->web.frame=rect;b->dirty=1;
}
static void mac_browser_prepare(Tab *tb) {
    /* Every browser already keeps a DOM projection, including Native tabs.
       Hover draws that cache without showing a WKWebView, changing native
       mode, requesting a new document, resizing it, or handing it focus. */
    int preview=chrome_preview_active() && tb==display_tab();
    HCBrowser *b=browser_for(tb); if (!b || (tb->browser_native && !preview)) return;
    tab_layout();
    int cols=tb->t.cols, rows=preview ? tb->t.rows : mac_browser_page_rows(tb), x=sidebar_x()/CW, y=g_chrome.top;
    b->pageRows=rows;
    int height=g_chrome.bottom-y, side=g_tab_underlay==1 ? 1 : 2;
    if (!b->dirty && b->lastCols==cols && b->lastRows==rows && b->lastX==x && b->lastY==y && b->lastHeight==height && b->lastSide==side && b->lastPreview==preview) return;
    Cell *cells=calloc((size_t)cols*rows,sizeof(Cell)); if (!cells) return;
    if(b->nodeCols!=cols || b->nodeRows!=rows){free(b->nodeGrid);b->nodeGrid=calloc((size_t)cols*rows,sizeof(uint16_t));b->nodeCols=cols;b->nodeRows=rows;}
    for(int i=0;i<b->blockCount;i++)for(int j=0;j<b->blocks[i].count;j++)if(b->blocks[i].cells[j].wflag&BROWSER_IMAGE) {
        Cell *c=b->blocks[i].cells+j;int id=c->ch;
        if(id>0 && id<=(int)b->images.count){HCBrowserImage *image=b->images[id-1];
            int w,h;browser_image_size(image->sw ? image->sw : image->cols*8,image->sh ? image->sh : image->rows*16,CW/MAX(1,g_scale),CH/MAX(1,g_scale),&w,&h);
            c->fg=(uint32_t)MIN(w,MAX(1,cols/4));c->bg=(uint32_t)h;}
    }
    int scroll=b->scroll;
    b->total=browser_grid_layout_nodes(b->blocks,b->blockCount,cells,b->nodeGrid,cols,rows,scroll,side,x,y,SB_COLS,height);
    int limit=MAX(0,b->total-rows);
    if (scroll>limit) {
        scroll=limit;
        if(!preview)b->scroll=scroll;
        b->total=browser_grid_layout_nodes(b->blocks,b->blockCount,cells,b->nodeGrid,cols,rows,scroll,side,x,y,SB_COLS,height);
    }
    if (b->focusedAction) for (int i=0;i<cols*rows;i++) if (cells[i].link==b->focusedAction && !(cells[i].wflag&BROWSER_IMAGE)) cells[i].attr|=A_INV;
    for (int row=0;row<rows;row++) {
        memcpy(tb->t.row[0][row],cells+row*cols,(size_t)cols*sizeof(Cell));
        tb->t.rlen[0][row]=cols;
    }
    for(int row=rows;row<tb->t.rows;row++){memset(tb->t.row[0][row],0,(size_t)cols*sizeof(Cell));tb->t.rlen[0][row]=0;}
    tb->t.m_cursor_vis=0; tb->t.view=0;
    free(cells); b->dirty=0; b->lastCols=cols; b->lastRows=rows; b->lastX=x; b->lastY=y; b->lastHeight=height; b->lastSide=side; b->lastPreview=preview;
}
static void mac_browser_scroll(int lines) {
    HCBrowser *b=browser_for(active_tab()); if (!b) return;
    b->scroll=MAX(0,MIN(MAX(0,b->total-mac_browser_page_rows(active_tab())),b->scroll+lines));
    b->dirty=1; g_sel_on=0; plat_invalidate();
}
static Tab *mac_browser_new(NSString *address, int parent, int native) {
    NSString *url=browser_url(address); if (!url || g_ntabs>=MAX_TABS) return NULL;
    Tab *owner=tab_by_id(parent);
    if (!owner) owner=active_tab();
    if (owner && owner->kind==TK_BROWSER) owner=tab_by_id(owner->parent_id);
    Tab *tb=calloc(1,sizeof(Tab));
    if (!tb || !term_init(&tb->t,g_cols,g_rows)) { free(tb); return NULL; }
    tb->id=g_next_id++; tb->fd=-1; tb->kind=TK_BROWSER; tb->browser_native=!!native;
    tb->parent_id=owner ? owner->id : 0; tb->t.m_cursor_vis=0;
    mac_browser_metadata(tb,nil,nil,url);
    int at=g_ntabs;
    if (owner) for (int i=0;i<g_ntabs;i++) if (g_tabs[i]==owner) {
        at=i+1; while (at<g_ntabs && g_tabs[at]->parent_id==owner->id) at++; break;
    }
    memmove(g_tabs+at+1,g_tabs+at,(size_t)(g_ntabs-at)*sizeof(Tab *));
    g_tabs[at]=tb; g_ntabs++; activate(at);
    return tb;
}
static void mac_browser_address(void) { mac_browser_address_grid(); }

static void mac_browser_command(int command) {
    Tab *tb=active_tab(); HCBrowser *b=browser_for(tb);
    if (!b) return;
    if (command==0) { tb->browser_native=!tb->browser_native; mac_browser_sync(); }
    else if (command==1) [b->web goBack];
    else if (command==2) [b->web goForward];
    else if (command==3) {
        if (!strcmp(tb->browser_url,"hardcore:welcome")) [b load:@"hardcore:welcome"];
        else [b->web reload];
    }
    plat_invalidate();
}
/* Grid-only anchor navigation uses the same layout as painting. The menu is
   viewport-relative, so settle the prefix with the intended new scroll before
   exposing the target. This keeps contents links useful without asking WebKit
   to scroll an unrelated native pixel layout. */
static int browser_anchor_row(HCBrowser *b,int anchor,int cols,int side,int x,int y,int width,int height) {
    NSNumber *entry=b->anchorBlocks[@(anchor)];
    if (!entry || cols<4) return -1;
    int block=[entry intValue];
    if (block<0 || block>=b->blockCount) return -1;
    while (block>0 && b->blocks[block].table && b->blocks[block-1].table==b->blocks[block].table) block--;
    Cell *scratch=calloc((size_t)cols,sizeof(Cell));
    if (!scratch) return -1;
    int offset=0;
    for (int attempt=0;attempt<8;attempt++) {
        int next=browser_grid_layout(b->blocks,block,scratch,cols,1,offset,side,x,y,width,height);
        if (next==offset) break;
        offset=next;
    }
    free(scratch); return offset;
}
static int browser_follow_anchor(Tab *tb,HCBrowser *b,int anchor) {
    if (!anchor || tb->browser_native) return 0;
    tab_layout();
    int row=browser_anchor_row(b,anchor,tb->t.cols,g_tab_underlay==1 ? 1 : 2,
        sidebar_x()/CW,g_chrome.top,SB_COLS,g_chrome.bottom-g_chrome.top);
    if (row<0) return 0;
    b->scroll=row; b->focusedAction=0; b->dirty=1; g_sel_on=0;
    plat_invalidate(); return 1;
}
static void mac_browser_action(Tab *tb, int action) {
    HCBrowser *b=browser_for(tb); if (!b || action<1 || action>4000) return;
    int revision=b->generation, identity=tb->id, documentEpoch=b->epoch;
    NSString *js=[NSString stringWithFormat:@"globalThis.hardcoreBrowser?.info(%d,%d)",action,revision];
    [b->web evaluateJavaScript:js inFrame:nil inContentWorld:browser_world() completionHandler:^(id info,NSError *error) {
        Tab *live=tab_by_id(identity);
        if (error || !live || live->browser!=b || b->generation!=revision || b->epoch!=documentEpoch || ![info isKindOfClass:NSDictionary.class]) return;
        if (!browser_follow_anchor(live,b,browser_integer(info[@"target"])))
            mac_browser_begin_control(live,action,revision,documentEpoch,info);
    }];
}

/* Same-user IPC actions. Does not expose arbitrary JS evaluation or shell exec. */
static NSDictionary *mac_browser_request(NSDictionary *request) {
    NSString *op=browser_string(request[@"op"]);
    if ([op isEqualToString:@"list"]) {
        NSMutableArray *tabs=[NSMutableArray array];
        for (int i=0;i<g_ntabs;i++) {
            Tab *tb=g_tabs[i];
            [tabs addObject:@{@"id":@(tb->id),@"parent":@(tb->parent_id),@"active":@(i==g_active),
                @"kind":tb->kind==TK_BROWSER ? @"browser" : @"terminal",@"url":[NSString stringWithUTF8String:tb->browser_url],
                @"title":tb->kind==TK_BROWSER ? browser_page_title(tb) : @"",@"site":[NSString stringWithUTF8String:tb->browser_site] ?: @"",
                @"mode":tb->browser_native ? @"native" : @"hardcore"}];
        }
        return @{@"ok":@YES,@"tabs":tabs};
    }
    int identity=[request[@"id"] respondsToSelector:@selector(intValue)] ? [request[@"id"] intValue] : 0;
    Tab *tb=identity ? tab_by_id(identity) : active_tab();
    if ([op isEqualToString:@"open"]) {
        NSString *url=browser_url(browser_string(request[@"url"]));
        if (!url) return @{@"ok":@NO,@"error":@"Use an http(s) URL, local file path, or hardcore:welcome."};
        int parent=[request[@"parent"] respondsToSelector:@selector(intValue)] ? [request[@"parent"] intValue] : 0;
        if (parent && !tab_by_id(parent)) return @{@"ok":@NO,@"error":@"Parent tab does not exist."};
        tb=mac_browser_new(url,parent,[request[@"mode"] isEqual:@"native"]);
        return tb ? @{@"ok":@YES,@"id":@(tb->id),@"parent":@(tb->parent_id)} : @{@"ok":@NO,@"error":@"Could not create browser tab."};
    }
    if (!tb || tb->kind!=TK_BROWSER) return @{@"ok":@NO,@"error":@"Choose a browser tab ID from browser list."};
    int at=0; while (at<g_ntabs && g_tabs[at]!=tb) at++;
    if ([op isEqualToString:@"mode"]) {
        if (![@[@"hardcore",@"native"] containsObject:request[@"mode"] ?: @""]) return @{@"ok":@NO,@"error":@"Mode must be hardcore or native."};
        tb->browser_native=[request[@"mode"] isEqual:@"native"]; activate(at);
    } else if ([op isEqualToString:@"close"]) {
        /* Defer the close so closing the last tab cannot end the process before
           the command response is sent. */
        int closeID=tb->id;
        dispatch_async(dispatch_get_main_queue(),^{ for (int i=0;i<g_ntabs;i++) if (g_tabs[i]->id==closeID) { close_tab(i); break; } });
    } else if ([@[@"back",@"forward",@"reload"] containsObject:op]) {
        activate(at); mac_browser_command([op isEqual:@"back"] ? 1 : [op isEqual:@"forward"] ? 2 : 3);
    } else return @{@"ok":@NO,@"error":@"Unknown browser command."};
    return @{@"ok":@YES,@"id":@(tb->id)};
}

static void mac_browser_open_welcome(void) {
    mac_browser_new(@"hardcore:welcome",active_tab() ? active_tab()->id : 0,0);
}

#include "hardcore_browser_ui.h"

static int browser_unit(FILE *f) {
    int failures=browser_label_unit(f);
    failures+=![browser_url(@"example.com") isEqual:@"https://example.com"];
    failures+=browser_url(@"javascript:alert(1)")!=nil;
    failures+=![browser_url(@"localhost:3000/test") isEqual:@"http://localhost:3000/test"];
    failures+=![browser_url(@"example.com:8080/test") isEqual:@"https://example.com:8080/test"];
    failures+=browser_url(@"https://example.com\nBAD")!=nil;
    failures+=browser_url(@"file://evil.example/etc/passwd")!=nil;
    failures+=![browser_url(@"/tmp/a b.html") hasPrefix:@"file:///tmp/a%20b.html"];
    failures+=browser_url([@"https://example.com/" stringByAppendingString:[@"中" stringByPaddingToLength:1000 withString:@"中" startingAtIndex:0]])!=nil;
    Tab *saved=g_tabs[0], *gridTab=calloc(1,sizeof(Tab));
    int savedCount=g_ntabs, savedActive=g_active, savedDock=g_tab_underlay;
    gridTab->kind=TK_BROWSER; g_tabs[0]=gridTab; g_ntabs=1; g_active=0;
    for (int side=0;side<3;side++) {
        int cols,rows; g_tab_underlay=side; compute_grid(100*CW,30*CH,&cols,&rows);
        failures+=term_ox()!=0 || cols!=100 || rows!=30;
    }
    mac_browser_metadata(gridTab,@"Ada Lovelace - Wikipedia",@"Wikipedia",@"https://en.wikipedia.org/wiki/Ada_Lovelace");
    snprintf(gridTab->program,sizeof(gridTab->program),"codex");
    snprintf(gridTab->start_dir,sizeof(gridTab->start_dir),"/work/web");
    wchar_t label[256];tab_label_fit(gridTab,label,256,23);
    failures+=!wcsstr(label,L"Ada Lovelace") || wcsstr(label,L"codex")!=NULL;
    mac_browser_metadata(gridTab,@"Tokyo - Wikipedia",@"Wikipedia",nil);tab_label_fit(gridTab,label,256,23);
    failures+=!wcsstr(label,L"Tokyo") || wcsstr(label,L"Ada")!=NULL;
    for(int cols=37;cols<=220;cols++) {
        BrowserNav nav=browser_nav_geometry(cols,13,2);
        failures+=nav.urlWidth<4 || nav.go+4>cols || nav.close+3>cols || nav.mode+8>cols || nav.rows<2;
    }
    NSString *home=[NSString stringWithUTF8String:(const char *)browser_welcome];
    NSRange marker=[home rangeOfString:@"data:image/png;base64,"];
    if(marker.location==NSNotFound)failures++;
    else {
        NSString *suffix=[home substringFromIndex:marker.location];NSRange end=[suffix rangeOfString:@"\""];
        if(end.location==NSNotFound)failures++;
        else {
            HCBrowserImage *image=[[HCBrowserImage alloc] initWithSource:[suffix substringToIndex:end.location] owner:0];
            failures+=!image->rgba || image->sw!=64 || image->sh!=48;
            browser_image_palette(image,64,48);failures+=!image->indexed;
            if(image->indexed)for(int i=0;i<64*48;i++)failures+=image->indexed[i]>=18;
            [image release];
        }
    }
    free(gridTab); g_tabs[0]=saved; g_ntabs=savedCount; g_active=savedActive; g_tab_underlay=savedDock;
    HCBrowser *b=[[HCBrowser alloc] init];
    browser_decode(b,@{@"blocks":@[@{@"kind":@"heading",@"runs":@[
        @{@"text":@"Hello \033world",@"action":@7,@"bold":@YES}]},
        @{@"kind":@"code",@"runs":@[@{@"text":@"x\ny",@"action":@99999}]}]});
    failures+=b->blockCount!=2 || b->blocks[0].count!=11;
    if (b->blockCount==2 && b->blocks[0].count==11) {
        failures+=b->blocks[0].cells[6].ch!='w' || b->blocks[0].cells[6].link!=7;
        failures+=!(b->blocks[0].cells[6].attr&A_BOLD);
        failures+=b->blocks[1].cells[0].link!=0 || b->blocks[1].cells[1].ch!='\n';
        Cell cells[40*12];
        browser_grid_layout(b->blocks,b->blockCount,cells,40,12,0,1,28,0,12,5);
        failures+=cells[6].link!=7 || cells[40*2].ch!='x' || cells[40*3].ch!='y';
    }
    browser_decode(b,@{@"blocks":@[
        @{@"kind":@"heading",@"anchor":@41,@"gap":@0,@"rule":@YES,@"runs":@[@{@"text":@"Facts"}]},
        @{@"kind":@"table",@"table":@3,@"header":@YES,@"numeric":@2,@"columns":@[
            @[@{@"text":@"Measure",@"bold":@YES}],@[@{@"text":@"Value",@"dim":@YES,@"italic":@YES}]]},
        @{@"kind":@"table",@"table":@3,@"numeric":@2,@"columns":@[
            @[@{@"text":@"Length\nspan",@"node":@7}],@[@{@"text":@"12",@"action":@8}]]},
        @{@"kind":@"caption",@"anchor":@42,@"runs":@[@{@"text":@"Measured values",@"dim":@YES}]}]});
    failures+=b->blockCount!=4 || b->anchorBlocks.count!=2;
    if (b->blockCount==4) {
        BrowserBlock *head=b->blocks+1,*data=b->blocks+2;
        failures+=!b->blocks[0].rule || b->blocks[0].gap!=0 || b->blocks[3].gap!=0;
        failures+=head->table!=3 || !head->header || head->columns!=2 || head->numeric!=2;
        failures+=!head->ends || head->ends[0]!=7 || head->ends[1]!=12;
        failures+=(head->cells[7].attr&(A_DIM|A_ITALIC))!=(A_DIM|A_ITALIC);
        failures+=!(head->decoration.attr&A_DIM) || (head->decoration.attr&A_BOLD)!=0;
        failures+=data->cells[6].ch!='\n' || data->cells[0].attr&A_BOLD || data->cells[0].link || data->nodes[0]!=7;
        failures+=data->cells[data->ends[0]].link!=8;
        failures+=browser_anchor_row(b,41,60,1,45,2,15,4)!=0;
        failures+=browser_anchor_row(b,42,60,1,45,2,15,4)<=0;
        failures+=browser_anchor_row(b,9999,60,1,45,2,15,4)!=-1;
    }
    browser_decode(b,@{@"blocks":@[@{@"kind":@"paragraph",@"gap":@99,@"runs":@[@{@"text":@"reset"}]}]});
    failures+=b->anchorBlocks.count!=0 || b->blocks[0].gap!=2;
    [b release];
    fprintf(f,"%s browser site names, toolbar geometry, image decode/palette, semantic tables/anchors, sanitized DOM and action IDs (no GUI)\n",failures ? "FAIL" : "ok  ");
    return failures;
}

static void mac_browser_focus(int direction) {
    Tab *tb=active_tab(); HCBrowser *b=browser_for(tb); if (!b) return;
    mac_browser_prepare(tb);
    int ids[4001], count=0; unsigned char seen[4001]={0};
    for (int y=0;y<tb->t.rows;y++) for (int x=0;x<tb->t.cols;x++) {
        int action=tb->t.row[0][y][x].link;
        if (action>0 && action<=4000 && !seen[action]) { seen[action]=1; ids[count++]=action; }
    }
    if (!count) return;
    int at=direction<0 ? 0 : -1;
    for (int i=0;i<count;i++) if (ids[i]==b->focusedAction) { at=i; break; }
    b->focusedAction=ids[(at+direction+count)%count]; b->dirty=1; plat_invalidate();
}
static void mac_browser_enter(void) {
    HCBrowser *b=browser_for(active_tab()); if (b && b->focusedAction) mac_browser_action(active_tab(),b->focusedAction);
}
