#!/usr/bin/env python3
"""Exercise the embedded Chrome transport against an owned local website.

The compiled fixture contains the production adapter bytes and needs no Python
or JavaScript resource files at runtime. Chrome gets a disposable user profile;
cleanup is restricted to processes whose command line names that exact profile.
No personal browser/profile, network website, or Hardcore window is used.
"""
import http.server
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

PAGE = b'''<!doctype html><meta charset="utf-8"><title>Owned Chrome Fixture</title>
<style>body{background:#17324d;color:white;font:18px monospace}input,button{font:inherit}</style>
<h1>Live browser fixture</h1><p id="status">Awaiting input</p>
<label>Name <input id="name" name="name" aria-label="Name" value="Ada"></label>
<button id="update">Update greeting</button><a id="next" href="/second">Second page</a>
<img alt="Embedded pixel" width="24" height="24"
src="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/lL8AAAAASUVORK5CYII=">
<script>
// A site-defined name must never replace Hardcore's isolated adapter API.
window.hardcoreBrowser={project:()=>({title:'page-world poison'})};
nameInput=document.getElementById('name');
nameInput.addEventListener('input',()=>document.body.dataset.input='yes');
nameInput.addEventListener('change',()=>document.body.dataset.change='yes');
document.getElementById('update').onclick=()=>{
 document.getElementById('status').textContent='Hello '+nameInput.value;
 document.title='Greeting updated';
};
</script>'''

HARNESS = r'''
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hardcore_chrome_browser.h"
#include "build/browser.inc"

static void require(BOOL value, NSString *message) {
    if (!value) { fprintf(stderr,"FAIL: %s\n",message.UTF8String); fflush(stderr); exit(1); }
}
static void waitFor(BOOL (^ready)(void), NSTimeInterval seconds, NSString *what) {
    NSDate *end=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(!ready() && end.timeIntervalSinceNow>0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:.02]];
    require(ready(),[@"Timed out: " stringByAppendingString:what]);
}
static id eval(HCChromePage *page, NSString *code) {
    __block BOOL finished=NO;__block id result=nil;__block NSError *failure=nil;
    [page evaluate:code completion:^(id value,NSError *error){
        require([NSThread isMainThread],@"JavaScript callback must run on main thread");
        result=[value retain];failure=[error retain];finished=YES;
    }];
    waitFor(^BOOL{return finished;},15,@"JavaScript result");
    require(!failure,[NSString stringWithFormat:@"JavaScript %@: %@",code,failure]);
    [failure release];return [result autorelease];
}
static NSDictionary *command(HCChromePage *page, NSString *method, NSDictionary *params) {
    __block BOOL finished=NO;__block NSDictionary *result=nil;__block NSError *failure=nil;
    [page command:method params:params completion:^(NSDictionary *value,NSError *error){
        require([NSThread isMainThread],@"CDP callback must run on main thread");
        result=[value retain];failure=[error retain];finished=YES;
    }];
    waitFor(^BOOL{return finished;},15,method);
    require(!failure,[NSString stringWithFormat:@"CDP %@: %@",method,failure]);
    [failure release];return [result autorelease];
}
static NSDictionary *browserCommand(NSString *method,NSDictionary *params) {
    __block BOOL finished=NO;__block NSDictionary *result=nil;__block NSError *failure=nil;
    [[HCChromeBroker shared] send:method params:params session:nil completion:^(NSDictionary *value,NSError *error){
        result=[value retain];failure=[error retain];finished=YES;
    }];
    waitFor(^BOOL{return finished;},20,method);
    require(!failure,[NSString stringWithFormat:@"Browser CDP %@: %@",method,failure]);
    [failure release];return [result autorelease];
}
static void start(HCChromePage *page,NSString *address) {
    __block BOOL finished=NO;__block NSError *failure=nil;
    [page start:address completion:^(NSError *error){
        require([NSThread isMainThread],@"Startup callback must run on main thread");
        failure=[error retain];finished=YES;
    }];
    waitFor(^BOOL{return finished;},30,@"Chrome startup");
    require(!failure,[NSString stringWithFormat:@"Chrome startup: %@",failure]);[failure release];
}
static void closePage(HCChromePage *page) {
    __block BOOL finished=NO;__block NSError *failure=nil;
    [page closeWithCompletion:^(NSError *error){failure=[error retain];finished=YES;}];
    waitFor(^BOOL{return finished;},10,@"Chrome target close acknowledgement");
    require(!failure,[NSString stringWithFormat:@"Chrome target close: %@",failure]);[failure release];
}
static NSDictionary *projection(HCChromePage *page,NSString *title) {
    __block NSDictionary *result=nil;
    waitFor(^BOOL{
        __block BOOL finished=NO;__block id value=nil;
        [page evaluate:@"globalThis.hardcoreBrowser?.project()" completion:^(id data,NSError *error){
            if(!error)value=[data retain];finished=YES;
        }];
        waitFor(^BOOL{return finished;},10,@"Document projection");
        if([value isKindOfClass:NSDictionary.class] && [value[@"title"] isEqual:title])result=value;
        else [value release];
        return result!=nil;
    },20,[@"Document title " stringByAppendingString:title]);
    return [result autorelease];
}
static NSString *words(NSDictionary *document) {
    NSMutableString *out=[NSMutableString string];
    for(NSDictionary *block in document[@"blocks"])
        for(NSDictionary *run in block[@"runs"])if([run[@"text"] isKindOfClass:NSString.class])[out appendString:run[@"text"]];
    return out;
}
int main(int argc,char **argv) {
    @autoreleasepool {
        if(argc>1 && !strcmp(argv[1],"chrome"))return hc_chrome_setup_main(argc,argv);
        require(argc>=2 && argc<=4,@"Expected local fixture URL and optional test mode");
        if(!strcmp(argv[1],"shutdown-unused")) {
            require(hc_chrome_shared_broker==nil,@"No browser before terminal-only quit");
            hc_chrome_close_pages_main();
            require(hc_chrome_shared_broker==nil,@"Terminal-only quit never instantiates Chrome");
            puts("PASS terminal-only quit does not start Chrome");return 0;
        }
        __block int changes=0;__block NSString *popupID=nil;
        NSString *script=[NSString stringWithUTF8String:(const char *)browser_script];
        HCChromePage *page=[[HCChromePage alloc] initWithScript:script changed:^(NSDictionary *event){
            require([NSThread isMainThread],@"Browser events must run on main thread");
            if([event[@"type"] isEqual:@"changed"])changes++;
            if([event[@"type"] isEqual:@"popup"]){[popupID release];popupID=[event[@"targetID"] copy];}
        }];
        if(!strcmp(argv[1],"missing")) {
            __block BOOL finished=NO;__block NSError *failure=nil;
            [page start:@"about:blank" completion:^(NSError *error){failure=[error retain];finished=YES;}];
            waitFor(^BOOL{return finished;},15,@"Missing Chrome error");
            require(failure!=nil,@"Missing Chrome must return a useful error");
            require(failure.localizedDescription.length>0,@"Missing Chrome error must have a description");
            [page close];[page release];[failure release];puts("PASS missing Chrome error");return 0;
        }
        NSString *base=[NSString stringWithUTF8String:argv[1]];
        start(page,base);
        NSDictionary *doc=projection(page,@"Owned Chrome Fixture");
        if(argc==3 && !strcmp(argv[2],"shutdown")) {
            NSMutableSet *expected=[NSMutableSet set];
            for(NSDictionary *info in browserCommand(@"Target.getTargets",@{})[@"targetInfos"])
                if([info[@"type"] isEqual:@"page"] && ![info[@"targetId"] isEqual:page->targetID])[expected addObject:info[@"targetId"]];
            NSString *sentinel=browserCommand(@"Target.createTarget",@{@"url":base,@"background":@YES})[@"targetId"];
            [expected addObject:sentinel];
            HCChromePage *quick=[[HCChromePage alloc] initWithScript:script changed:nil];
            start(quick,base);[quick close]; // Last-tab path: close already queued, no registry entry.
            NSString *attachingID=browserCommand(@"Target.createTarget",@{@"url":base,@"background":@YES})[@"targetId"];
            HCChromePage *attaching=[[HCChromePage alloc] initWithScript:script changed:nil];
            [attaching adoptTarget:attachingID completion:nil]; // Target exists; attachment reply is pending.
            HCChromePage *creating=[[HCChromePage alloc] initWithScript:script changed:nil];
            [creating start:base completion:nil]; // No target ID has arrived yet.
            hc_chrome_close_pages_main();
            NSUInteger unacknowledged;@synchronized([HCChromeBroker shared]->quitReplies){unacknowledged=[HCChromeBroker shared]->quitReplies.count;}
            require([HCChromeBroker shared]->openingCount==0 && unacknowledged==0,@"Quit waits for owned target creation/attachment/close acknowledgements");
            // Chrome acknowledges close before its renderer target disappears.
            waitFor(^BOOL {
                NSMutableSet *remaining=[NSMutableSet set];
                for(NSDictionary *info in browserCommand(@"Target.getTargets",@{})[@"targetInfos"])
                    if([info[@"type"] isEqual:@"page"])[remaining addObject:info[@"targetId"]];
                return [remaining isEqualToSet:expected];
            },5,@"Quit removes owned targets while retaining unrelated sentinel");
            browserCommand(@"Target.closeTarget",@{@"targetId":sentinel});
            [page release];[quick release];[attaching release];[creating release];
            puts("PASS real Chrome quit cleanup: owned, attaching, creating and queued-close targets removed; unrelated target retained");return 0;
        }
        if(argc==4 && !strcmp(argv[2],"extension")) {
            NSString *path=[NSString stringWithUTF8String:argv[3]];
            NSDictionary *loaded=browserCommand(@"Extensions.loadUnpacked",@{@"path":path});
            require([loaded[@"id"] isKindOfClass:NSString.class] && [loaded[@"id"] length]>0,@"Chrome loaded the owned Manifest V3 extension");
            [page navigate:base];projection(page,@"Owned Chrome Fixture");
            waitFor(^BOOL{return [eval(page,@"document.querySelector('#owned-extension')?.textContent") isEqual:@"Extension worker and storage active"];},15,@"Extension content script and service worker");
            require([words(projection(page,@"Owned Chrome Fixture")) containsString:@"Extension worker and storage active"],@"Extension-added DOM appears in the Hardcore adapter document");
            require([eval(page,@"document.querySelector('#owned-extension').dataset.extension") isEqual:loaded[@"id"]],@"The content script communicates with its own real Chrome extension worker");
            closePage(page);[page release];puts("PASS real MV3 extension: content script, service worker, runtime messaging, persistent storage and Hardcore DOM projection");return 0;
        }
        if(argc==3 && !strcmp(argv[2],"reconnect")) {
            require([eval(page,@"document.cookie") containsString:@"hc_owned=persisted"],@"A new Hardcore process reconnects with the same auth cookie");
            closePage(page);[page release];puts("PASS independent Hardcore process reuses the Chrome profile and auth cookie");return 0;
        }
        require([words(doc) containsString:@"Live browser fixture"],@"Real embedded adapter projects the live DOM");
        require([words(doc) containsString:@"Name: Ada"],[NSString stringWithFormat:@"Live input projected with its current value: %@",words(doc)]);
        require([doc[@"url"] isEqual:base],@"Projection preserves current page URL");
        require(page->ready && page->targetID.length,@"Browser owns a ready Chrome target");
        BOOL image=NO;
        for(NSDictionary *block in doc[@"blocks"])for(NSDictionary *run in block[@"runs"])if(run[@"image"])image=YES;
        require(image,@"Live DOM projection includes inline images");
        NSDictionary *realm=command(page,@"Runtime.evaluate",@{@"expression":@"globalThis.hardcoreBrowser.project().title",@"returnByValue":@YES});
        require([realm[@"result"][@"value"] isEqual:@"page-world poison"],@"Adapter is isolated from the site's JavaScript realm");
        int previousChanges=changes;
        NSDictionary *interactive=eval(page,@"(()=>{const d=hardcoreBrowser.project(),r=d.blocks.flatMap(b=>b.runs||[]),input=r.find(x=>x.text.includes('Name: Ada')),button=r.find(x=>x.text.includes('Update greeting'));return {input:hardcoreBrowser.activate(input.action,d.generation,'Grace'),button:hardcoreBrowser.activate(button.action,d.generation),status:document.querySelector('#status').textContent,inputEvent:document.body.dataset.input,changeEvent:document.body.dataset.change}})()");
        require([interactive[@"input"] boolValue] && [interactive[@"button"] boolValue],@"TUI activates live controls");
        require([interactive[@"status"] isEqual:@"Hello Grace"],@"Site click handler executes");
        require([interactive[@"inputEvent"] isEqual:@"yes"] && [interactive[@"changeEvent"] isEqual:@"yes"],@"Real input/change events reach the site");
        projection(page,@"Greeting updated");
        waitFor(^BOOL{return changes>previousChanges;},5,@"DOM change notification");
        [page setWidth:640 height:400];
        waitFor(^BOOL{return [eval(page,@"innerWidth") intValue]==640 && [eval(page,@"innerHeight") intValue]==400;},10,@"Viewport resize");
        NSDictionary *point=eval(page,@"(()=>{const e=document.querySelector('#name'),r=e.getBoundingClientRect();e.value='';return {x:r.x+r.width/2,y:r.y+r.height/2}})()");
        command(page,@"Input.dispatchMouseEvent",@{@"type":@"mousePressed",@"x":point[@"x"],@"y":point[@"y"],@"button":@"left",@"clickCount":@1});
        command(page,@"Input.dispatchMouseEvent",@{@"type":@"mouseReleased",@"x":point[@"x"],@"y":point[@"y"],@"button":@"left",@"clickCount":@1});
        command(page,@"Input.insertText",@{@"text":@"Native input"});
        require([eval(page,@"document.querySelector('#name').value") isEqual:@"Native input"],@"Normal browser view receives native mouse/keyboard input");
        __block BOOL captured=NO;__block NSData *png=nil;__block NSError *captureError=nil;
        [page capture:^(NSData *bytes,NSError *error){
            require([NSThread isMainThread],@"Screenshot callback must run on main thread");
            png=[bytes retain];captureError=[error retain];captured=YES;
        }];
        waitFor(^BOOL{return captured;},15,@"Native browser screenshot");
        static const unsigned char signature[]={137,80,78,71,13,10,26,10};
        require(!captureError && png.length>100 && !memcmp(png.bytes,signature,sizeof(signature)),@"Normal browser view produces PNG pixels");
        const unsigned char *pixels=png.bytes;
        uint32_t pixelWidth=(pixels[16]<<24)|(pixels[17]<<16)|(pixels[18]<<8)|pixels[19];
        uint32_t pixelHeight=(pixels[20]<<24)|(pixels[21]<<16)|(pixels[22]<<8)|pixels[23];
        require(pixelWidth==640 && pixelHeight==400,@"Normal browser pixels match the requested content viewport");
        [png release];[captureError release];
        eval(page,@"window.open('/second','owned-popup');true");
        waitFor(^BOOL{return popupID.length>0;},10,@"Live page popup target");
        HCChromePage *popup=[[HCChromePage alloc] initWithScript:script changed:^(NSDictionary *event){(void)event;}];
        __block BOOL adopted=NO;__block NSError *adoptError=nil;
        [popup adoptTarget:popupID completion:^(NSError *error){adoptError=[error retain];adopted=YES;}];
        waitFor(^BOOL{return adopted;},10,@"Adopt popup target");
        require(!adoptError,[NSString stringWithFormat:@"Popup adoption: %@",adoptError]);[adoptError release];
        projection(popup,@"Second owned page");
        require([popup->targetID isEqual:popupID],@"Popup adoption preserves the original Chrome target");
        require([eval(popup,@"window.opener && window.opener.document.title") isEqual:@"Greeting updated"],@"Adopted popup preserves its opener for login flows");
        closePage(popup);[popup release];[popupID release];popupID=nil;
        eval(page,@"document.cookie='hc_owned=persisted; Max-Age=3600; SameSite=Lax; path=/'");
        eval(page,@"document.querySelector('#next').click();true");
        projection(page,@"Second owned page");
        waitFor(^BOOL{return page->canGoBack;},5,@"Back navigation availability");
        require([page->url hasSuffix:@"/second"],@"Live navigation updates tab URL");
        [page go:-1];projection(page,@"Owned Chrome Fixture");
        waitFor(^BOOL{return page->canGoForward;},5,@"Forward navigation availability");
        [page go:1];projection(page,@"Second owned page");
        NSDictionary *history=command(page,@"Page.getNavigationHistory",@{});
        require([history[@"entries"] count]>=2,@"Chrome retains navigation history");
        [page navigate:base];projection(page,@"Owned Chrome Fixture");
        [page reload];projection(page,@"Owned Chrome Fixture");
        HCChromePage *other=[[HCChromePage alloc] initWithScript:script changed:^(NSDictionary *event){(void)event;}];
        start(other,[base stringByAppendingString:@"second"]);projection(other,@"Second owned page");
        require(![other->targetID isEqual:page->targetID],@"Hardcore tabs use independent Chrome targets");
        NSString *oldTarget=[page->targetID copy];closePage(page);[page release];
        require([eval(other,@"document.title") isEqual:@"Second owned page"],@"Closing one tab preserves its sibling");
        page=[[HCChromePage alloc] initWithScript:script changed:^(NSDictionary *event){(void)event;}];
        start(page,base);projection(page,@"Owned Chrome Fixture");
        require(![page->targetID isEqual:oldTarget],@"Reopened tab gets a fresh target");[oldTarget release];
        require([eval(page,@"document.cookie") containsString:@"hc_owned=persisted"],@"Auth cookie survives tab close/reopen in the persistent Chrome profile");
        closePage(page);closePage(other);[page release];[other release];
        puts("PASS real Chrome: isolated embedded DOM adapter, live controls, mutation events, native input, screenshot, navigation/history, popup adoption, independent tabs and persistent cookies");
    }
    return 0;
}
'''


def chrome_executable():
    override = os.environ.get('HARDCORE_CHROME_EXECUTABLE')
    candidates = [override] if override else [
        '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
        str(Path.home() / 'Applications/Google Chrome.app/Contents/MacOS/Google Chrome'),
        '/Applications/Chromium.app/Contents/MacOS/Chromium',
    ]
    return next((str(Path(path).resolve()) for path in candidates
                 if path and Path(path).is_file() and os.access(path, os.X_OK)), None)


class OwnedWebsite(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        data = (b'<!doctype html><title>Second owned page</title><h1>Second owned page</h1>'
                if self.path.startswith('/second') else PAGE)
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_args):
        pass


@unittest.skipUnless(sys.platform == 'darwin', 'macOS Foundation Chrome transport')
class ChromeIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chrome = chrome_executable()
        if not cls.chrome:
            raise unittest.SkipTest('Chrome/Chromium not installed; set HARDCORE_CHROME_EXECUTABLE to run')
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-owned-chrome-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        cls.profile = cls.root / 'profile'
        cls.profiles = [cls.profile]
        cls.addClassCleanup(cls.cleanup_chrome)
        source = cls.root / 'chrome.m'
        source.write_text(HARNESS)
        cls.exe = cls.root / 'chrome-fixture'
        result = subprocess.run(['clang', '-fblocks', '-Werror=implicit-function-declaration',
                                 '-framework', 'Foundation', '-framework', 'AppKit', '-I', str(ROOT), str(source),
                                 '-o', str(cls.exe)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), OwnedWebsite)
        cls.addClassCleanup(cls.server.server_close)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.addClassCleanup(cls.server.shutdown)
        cls.url = f'http://127.0.0.1:{cls.server.server_port}/'

    @classmethod
    def cleanup_chrome(cls):
        # Match our random absolute user-data-dir token, never a browser name.
        markers = ['--user-data-dir=' + str(profile) for profile in cls.profiles]
        for sig in (signal.SIGTERM, signal.SIGKILL):
            output = subprocess.run(['/bin/ps', '-axo', 'pid=,command='],
                                    capture_output=True, text=True, timeout=5).stdout
            for line in output.splitlines():
                fields = line.strip().split(None, 1)
                if len(fields) == 2 and any(marker in fields[1].split() for marker in markers):
                    try:
                        os.kill(int(fields[0]), sig)
                    except ProcessLookupError:
                        pass
            if sig == signal.SIGTERM:
                time.sleep(.5)

    def run_fixture(self, address, executable=None, mode=None, extra=(), profile=None):
        env = {key: value for key, value in os.environ.items() if not key.startswith('HARDCORE_')}
        env.update(HARDCORE_CHROME_EXECUTABLE=executable or self.chrome,
                   HARDCORE_CHROME_PROFILE=str(profile or self.profile))
        args = [str(self.exe), address] + ([mode] if mode else []) + list(extra)
        result = subprocess.run(args, cwd=self.root,
                                env=env, capture_output=True, text=True, timeout=150)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        print(result.stdout.strip())

    def test_real_chrome_embedded_adapter_and_native_view(self):
        self.run_fixture(self.url)
        self.run_fixture(self.url, mode='reconnect')

    def test_real_extension_content_script_worker_and_storage(self):
        profile = self.root / 'extension-profile'
        self.profiles.append(profile)
        extension = self.root / 'owned-extension'
        extension.mkdir()
        (extension / 'manifest.json').write_text(json.dumps({
            'manifest_version': 3, 'name': 'Hardcore owned integration fixture', 'version': '1.0',
            'permissions': ['storage'], 'background': {'service_worker': 'worker.js'},
            'content_scripts': [{'matches': ['http://127.0.0.1/*'], 'js': ['content.js']}],
        }))
        (extension / 'worker.js').write_text('''
chrome.runtime.onMessage.addListener((message,sender,reply)=>{
  if(message.ownedTest){chrome.storage.local.get('owned',value=>reply({
    value:value.owned,extension:chrome.runtime.id
  }));return true;}
});
''')
        (extension / 'content.js').write_text('''
chrome.storage.local.set({owned:'Extension worker and storage active'},()=>{
  chrome.runtime.sendMessage({ownedTest:true},reply=>{
    const node=document.createElement('aside');node.id='owned-extension';
    node.textContent=reply.value;node.dataset.extension=reply.extension;
    document.body.append(node);
  });
});
''')
        # This permission exists only in this disposable test Chrome. Production
        # launch deliberately does not enable unsafe extension debugging.
        child = subprocess.Popen([self.chrome, '--headless=new', '--remote-debugging-port=0',
                                  '--remote-debugging-address=127.0.0.1',
                                  '--enable-unsafe-extension-debugging',
                                  '--user-data-dir=' + str(profile), '--no-first-run',
                                  '--no-default-browser-check', 'about:blank'],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True)
        def stop_owned_chrome():
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
        self.addCleanup(stop_owned_chrome)
        deadline = time.monotonic() + 20
        while not (profile / 'DevToolsActivePort').is_file() and time.monotonic() < deadline:
            self.assertIsNone(child.poll(), 'Owned extension-test Chrome exited during startup')
            time.sleep(.03)
        self.assertTrue((profile / 'DevToolsActivePort').is_file(), 'Owned extension-test Chrome did not start')
        self.run_fixture(self.url, mode='extension', extra=[str(extension)], profile=profile)

    def test_missing_chrome_fails_without_fallback_or_window(self):
        self.run_fixture('missing', str(self.root / 'no-such-chrome'))

    def test_quit_cleans_only_owned_and_inflight_targets(self):
        self.run_fixture(self.url, mode='shutdown')

    def test_terminal_only_quit_does_not_start_chrome(self):
        self.run_fixture('shutdown-unused', str(self.root / 'no-such-chrome'))

    def test_setup_rejects_regular_browser_profiles_before_launch(self):
        # Strings only: do not create/read any personal profile or start Chrome.
        roots = ['Google/Chrome', 'Google/Chrome Beta', 'Google/Chrome Dev',
                 'Google/Chrome Canary', 'Microsoft Edge', 'Microsoft Edge Beta',
                 'Microsoft Edge Dev', 'Microsoft Edge Canary', 'Chromium']
        for suffix in roots:
            for child in ('', 'Default'):
                with self.subTest(profile=suffix, child=child):
                    profile = Path.home() / 'Library/Application Support' / suffix
                    if child:
                        profile /= child
                    env = {key: value for key, value in os.environ.items()
                           if not key.startswith('HARDCORE_')}
                    env.update(HARDCORE_CHROME_EXECUTABLE='/usr/bin/true',
                               HARDCORE_CHROME_PROFILE=str(profile))
                    result = subprocess.run([str(self.exe), 'chrome', 'setup'], env=env,
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                    self.assertIn('regular Chrome profile is never launched', result.stderr)
                    self.assertNotIn('Opened', result.stdout)

    def test_setup_detects_child_exit_before_publishing_endpoint(self):
        profile = self.root / 'failed-setup-profile'
        env = {key: value for key, value in os.environ.items()
               if not key.startswith('HARDCORE_')}
        # A successful exec followed by exit(0) must not report a working browser.
        env.update(HARDCORE_CHROME_EXECUTABLE='/usr/bin/true',
                   HARDCORE_CHROME_PROFILE=str(profile))
        result = subprocess.run([str(self.exe), 'chrome', 'setup'], env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('setup did not open its profile', result.stderr)
        self.assertNotIn('Opened', result.stdout)
        self.assertFalse((profile / 'DevToolsActivePort').exists())


if __name__ == '__main__':
    unittest.main()
