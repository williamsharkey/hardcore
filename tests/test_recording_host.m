/* Native bridge/lifecycle fixture: synthetic files and stub views only.
   No NSApplication, NSWindow, WKWebView, capture devices or user files. */
#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>
#include <math.h>
static NSString *fixtureRoot,*fixtureResources,*lastStatus;
static NSString *record_root(void){return fixtureRoot;}
static NSString *record_resource(NSString *name){return [fixtureResources stringByAppendingPathComponent:name];}
static void record_status(NSString *message){lastStatus=message;}
static void record_jobs_changed(void){}
#include "../hardcore_recording_jobs.m"

typedef struct Tab {struct {int rows;}t;int id,kind,busy,browser_native,browser_recording;void *browser;char browser_url[4096];} Tab;
#define TK_BROWSER 1
static Tab *g_tabs[8];static int g_ntabs,g_tab_underlay;
static struct {unsigned bg,bar,hover,fg,dim,accent;} g_look;
static NSWindow *g_win;static NSView *g_browser_root;static void *g_font;
#define UI_FONT
#define UI_DONE
#define CH 20
static int mac_title_height(void){return 40;}
static int sidebar_w(void){return 280;}
static int cell_w(void *font){(void)font;return 10;}
static int mac_browser_page_rows(Tab *tb){return tb->t.rows;}
static void mac_browser_resize(Tab *tb){(void)tb;}
static Tab *tab_by_id(int identity){for(int i=0;i<g_ntabs;i++)if(g_tabs[i]->id==identity)return g_tabs[i];return NULL;}
static int busyUpdates,opened,libraryActions,fileActions;
static NSString *openedURL,*filePath;
static int mac_record_active(void){return 0;}
static void activate(int i){(void)i;}
static void update_busy(int mode){(void)mode;busyUpdates++;}
static void plat_invalidate(void){}
static void mac_browser_metadata(Tab *tb,NSString *title,NSString *site,NSString *url){(void)tb;(void)title;(void)site;(void)url;}

@interface FixtureWeb:NSObject
@property(nonatomic,retain) NSURL *URL;
@property(nonatomic,assign,getter=isLoading) BOOL loading;
@property(nonatomic,copy) NSString *lastScript;
@property(nonatomic,readonly) WKWebViewConfiguration *configuration;
@end
@implementation FixtureWeb
@synthesize URL,loading,lastScript;
- (WKWebViewConfiguration *)configuration{return nil;}
- (void)evaluateJavaScript:(NSString *)script completionHandler:(void (^)(id,NSError *))handler{self.lastScript=script;if(handler)handler(nil,nil);}
- (void)dealloc{[URL release];[lastScript release];[super dealloc];}
@end
@interface HCBrowser:NSObject <WKScriptMessageHandler> {
@public int tabID,inspectOpen;WKWebView *web;
}
- (id)initWithTab:(Tab *)tb;
- (WKWebViewConfiguration *)configuration;
- (void)load:(NSString *)url;
- (void)dispose;
- (void)accept:(NSDictionary *)snapshot;
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation;
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message;
@end
@implementation HCBrowser
- (id)initWithTab:(Tab *)tb{if((self=[super init])){tabID=tb->id;web=(WKWebView *)[FixtureWeb new];((FixtureWeb *)web).URL=[NSURL URLWithString:@(tb->browser_url)];}return self;}
- (WKWebViewConfiguration *)configuration{return nil;}
- (void)load:(NSString *)url{(void)url;}
- (void)dispose{}
- (void)accept:(NSDictionary *)snapshot{(void)snapshot;}
- (void)webView:(WKWebView *)view didFinishNavigation:(WKNavigation *)navigation{(void)view;(void)navigation;}
- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message{(void)controller;(void)message;}
- (void)dealloc{[web release];[super dealloc];}
@end
static HCBrowser *browser_for(Tab *tb){return tb && tb->kind==TK_BROWSER?(HCBrowser *)tb->browser:nil;}
static Tab *mac_browser_new_with_class(NSString *url,int parent,int native,Class cls){(void)parent;(void)native;(void)cls;opened++;openedURL=url;return g_tabs[0];}
static void mac_record_library(void){libraryActions++;}
static void mac_record_file_viewer(NSString *path){fileActions++;filePath=path;}
#include "../hardcore_recording_library.m"

@interface FixtureFrame:NSObject
@property(nonatomic,assign,getter=isMainFrame) BOOL mainFrame;
@property(nonatomic,retain) NSURLRequest *request;
@end
@implementation FixtureFrame
@synthesize mainFrame,request;
- (void)dealloc{[request release];[super dealloc];}
@end
@interface FixtureMessage:NSObject
@property(nonatomic,retain) id body;
@property(nonatomic,copy) NSString *name;
@property(nonatomic,retain) FixtureFrame *frameInfo;
@property(nonatomic,assign) WKWebView *webView;
@end
@implementation FixtureMessage
@synthesize body,name,frameInfo,webView;
- (void)dealloc{[body release];[name release];[frameInfo release];[super dealloc];}
@end
static void post(HCRecordingBrowser *browser,FixtureMessage *message,id body){message.body=body;[browser userContentController:nil didReceiveScriptMessage:(WKScriptMessage *)message];}
#define CHECK(condition,number) do{if(!(condition)){fprintf(stderr,"FAIL native Clips host %d\n",number);return number;}}while(0)
int main(void){@autoreleasepool{
    fixtureResources=NSFileManager.defaultManager.currentDirectoryPath;fixtureRoot=record_library_temporary();
    NSString *bundle=[fixtureRoot stringByAppendingPathComponent:@"fixture.hcrec"];
    [NSFileManager.defaultManager createDirectoryAtPath:bundle withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *index=[bundle stringByAppendingPathComponent:@"index.html"];
    [@"old player" writeToFile:index atomically:YES encoding:NSUTF8StringEncoding error:nil];
    [@"{\"durationMs\":1000}" writeToFile:[bundle stringByAppendingPathComponent:@"manifest.json"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    NSString *payload=[bundle stringByAppendingPathComponent:@"recording.js"];
    [@"sentinel recording data" writeToFile:payload atomically:YES encoding:NSUTF8StringEncoding error:nil];
    Tab tab={.id=1,.kind=TK_BROWSER};g_tabs[g_ntabs++]=&tab;
    snprintf(tab.browser_url,sizeof(tab.browser_url),"%s",[NSURL fileURLWithPath:index].absoluteString.UTF8String);
    HCRecordingBrowser *browser=[[HCRecordingBrowser alloc] initWithTab:&tab];tab.browser=browser;browser->recordReady=YES;
    CHECK(tab.browser_recording==1,1);
    FixtureWeb *web=(FixtureWeb *)browser->web;
    FixtureMessage *message=[FixtureMessage new];message.name=@"hardcoreRecording";message.webView=browser->web;
    message.frameInfo=[[[FixtureFrame alloc] init] autorelease];message.frameInfo.mainFrame=YES;message.frameInfo.request=[NSURLRequest requestWithURL:web.URL];
    NSDictionary *edits=@{@"action":@"saveEdits",@"startMs":@100,@"endMs":@900,@"captionItems":@[@{@"start":@100,@"end":@500,@"text":@"safe caption"}]};
    post(browser,message,edits);
    NSString *editsPath=[bundle stringByAppendingPathComponent:@"edits.json"];
    NSData *saved=[NSData dataWithContentsOfFile:editsPath];CHECK(saved.length && [web.lastScript containsString:@"Saved edits"],2);
    CHECK(record_library_refresh_assets(bundle),3);
    CHECK([[NSData dataWithContentsOfFile:editsPath] isEqual:saved],4);
    CHECK([[NSString stringWithContentsOfFile:payload encoding:NSUTF8StringEncoding error:nil] isEqual:@"sentinel recording data"],5);
    CHECK([[NSData dataWithContentsOfFile:index] isEqual:[NSData dataWithContentsOfFile:record_resource(@"recording/index.html")]],6);
    for(int invalid=0;invalid<4;invalid++) {
        message.frameInfo.mainFrame=invalid!=0;message.webView=invalid==1?nil:browser->web;
        message.frameInfo.request=[NSURLRequest requestWithURL:invalid==2?[NSURL URLWithString:@"https://example.invalid/index.html"]:[NSURL fileURLWithPath:index]];
        web.URL=invalid==3?[NSURL fileURLWithPath:[fixtureRoot stringByAppendingPathComponent:@"other.html"]]:[NSURL fileURLWithPath:index];
        post(browser,message,@{@"action":@"saveEdits",@"startMs":@0,@"endMs":@800});
        CHECK([[NSData dataWithContentsOfFile:editsPath] isEqual:saved],7+invalid);
    }
    web.URL=[NSURL fileURLWithPath:index];message.frameInfo.request=[NSURLRequest requestWithURL:web.URL];message.frameInfo.mainFrame=YES;message.webView=browser->web;
    post(browser,message,@{@"action":@"saveEdits",@"startMs":@0,@"endMs":@2000});CHECK([[NSData dataWithContentsOfFile:editsPath] isEqual:saved],11);
    post(browser,message,@{@"action":@"library"});post(browser,message,@{@"action":@"files"});CHECK(libraryActions==1 && fileActions==1 && [filePath isEqual:bundle],12);
    NSString *movie=[fixtureRoot stringByAppendingPathComponent:@"output.mp4"];[@"synthetic output" writeToFile:movie atomically:YES encoding:NSUTF8StringEncoding error:nil];
    g_record_jobs=[[NSMutableArray alloc] initWithArray:@[@{@"id":@"known",@"bundle":bundle,@"kind":@"mp4",@"state":@"running",@"progress":@0.5,@"message":@"Exporting",@"output":movie}]];
    [browser pollJobs:nil];CHECK(tab.busy && busyUpdates && [web.lastScript containsString:@"hostTheme"],13);
    web.loading=YES;[g_record_jobs replaceObjectAtIndex:0 withObject:@{@"id":@"known",@"bundle":bundle,@"kind":@"mp4",@"state":@"done",@"progress":@1,@"message":@"Done",@"output":movie}];
    [g_record_job_snapshots release];g_record_job_snapshots=nil;[browser pollJobs:nil];CHECK(tab.busy,14);
    web.loading=NO;[browser pollJobs:nil];CHECK(!tab.busy,15);
    post(browser,message,@{@"action":@"output",@"id":@"wrong",@"output":movie});CHECK(!opened,16);
    post(browser,message,@{@"action":@"output",@"id":@"known",@"output":@"/invalid.mp4"});CHECK(opened==1 && [openedURL isEqual:[NSURL fileURLWithPath:movie].absoluteString],17);
    browser->recordTimer=[[NSTimer timerWithTimeInterval:1 target:browser selector:@selector(pollJobs:) userInfo:nil repeats:YES] retain];
    [browser dispose];CHECK(browser->recordClosed && !browser->recordTimer && g_record_jobs.count==1,18);
    post(browser,message,@{@"action":@"library"});CHECK(libraryActions==1,19);
    [message release];tab.browser=NULL;[browser release];
    [g_record_jobs release];g_record_jobs=nil;[g_record_job_snapshots release];g_record_job_snapshots=nil;
    [NSFileManager.defaultManager removeItemAtPath:fixtureRoot error:nil];
    puts("PASS native Clips host: strict origin/frame, duration, asset refresh preserves data, job-id output, busy/load state, disposal without windows or devices");return 0;
}}
