#!/usr/bin/env python3
"""The shared Chrome relay: one consented connection for every Hardcore process.

A disposable headless Chrome stands in for the user's own Chrome through
HARDCORE_CHROME_ATTACH; its hidden targets, like the real ones, end with their
connection. Each fixture process is a separate client, as a window and its
in-place reload are. The relay socket lives in a private runtime directory and
cleanup only signals processes naming this test's own paths.
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
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_chrome_browser import OwnedWebsite, chrome_executable  # noqa: E402

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
    [page evaluate:code completion:^(id value,NSError *error){result=[value retain];failure=[error retain];finished=YES;}];
    waitFor(^BOOL{return finished;},15,@"JavaScript result");
    require(!failure,[NSString stringWithFormat:@"JavaScript %@: %@",code,failure]);
    return [result autorelease];
}
static NSError *connectError(void) {
    __block BOOL finished=NO;__block NSError *failure=nil;
    [[HCChromeBroker shared] connect:^(NSError *error){failure=[error retain];finished=YES;}];
    waitFor(^BOOL{return finished;},30,@"relay connect");
    return [failure autorelease];
}
/* Hidden targets are absent from Target.getTargets; ask for this one. */
static BOOL targetExists(NSString *target) {
    __block BOOL finished=NO,exists=NO;
    [[HCChromeBroker shared] send:@"Target.getTargetInfo" params:@{@"targetId":target} session:nil completion:^(NSDictionary *value,NSError *error){
        exists=!error && value[@"targetInfo"];finished=YES;
    }];
    waitFor(^BOOL{return finished;},15,@"target info");
    return exists;
}
static HCChromePage *opened(NSString *mode,NSString *argument,NSString *script) {
    HCChromePage *page=[[HCChromePage alloc] initWithScript:script changed:nil];
    __block BOOL finished=NO;__block NSError *failure=nil;
    void (^done)(NSError *)=^(NSError *error){failure=[error retain];finished=YES;};
    if([mode isEqual:@"create"])[page start:argument completion:done];else [page adoptTarget:argument completion:done];
    waitFor(^BOOL{return finished;},30,@"page ready");
    require(!failure,[NSString stringWithFormat:@"%@ %@: %@",mode,argument,failure]);
    return page;
}
int main(int argc,char **argv) {
    @autoreleasepool {
        if(argc>1 && !strcmp(argv[1],"chrome"))return hc_chrome_setup_main(argc,argv);
        require(argc==3,@"Expected a mode and its argument");
        NSString *mode=@(argv[1]),*argument=@(argv[2]);
        NSString *script=[NSString stringWithUTF8String:(const char *)browser_script];
        if([mode isEqual:@"unavailable"]){NSError *e=connectError();printf("%s\n",e ? e.localizedDescription.UTF8String : "connected");return 0;}
        require(connectError()==nil,@"Relay connects to Chrome");
        if([mode isEqual:@"exists"]){puts(targetExists(argument) ? "yes" : "no");return 0;}
        if([mode isEqual:@"create"]) {
            /* Leave the page open and exit, as an in-place reload does. */
            HCChromePage *page=opened(mode,argument,script);
            waitFor(^BOOL{return [eval(page,@"document.readyState") isEqual:@"complete"];},15,@"fixture load");
            eval(page,@"document.getElementById('name').value='kept across clients'");
            for(int i=0;i<5;i++)require([eval(page,@"document.getElementById('name').value") isEqual:@"kept across clients"],@"Own session replies route back");
            printf("%s\n",page->targetID.UTF8String);return 0;
        }
        if([mode isEqual:@"adopt"]) {
            HCChromePage *page=opened(mode,argument,script);
            printf("%s\n",[eval(page,@"document.getElementById('name').value") UTF8String]);return 0;
        }
        if([mode isEqual:@"quit"]) {
            opened(@"adopt",argument,script);
            hc_chrome_close_pages_main();
            NSUInteger left;@synchronized([HCChromeBroker shared]->quitReplies){left=[HCChromeBroker shared]->quitReplies.count;}
            require(left==0,@"Quit close acknowledged through relay");
            puts("quit");return 0;
        }
        require(NO,@"Unknown mode");
    }
    return 1;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'macOS Foundation Chrome relay')
class ChromeRelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.chrome = chrome_executable()
        if not cls.chrome:
            raise unittest.SkipTest('Chrome/Chromium not installed; set HARDCORE_CHROME_EXECUTABLE to run')
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-relay-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        source = cls.root / 'relay.m'
        source.write_text(HARNESS)
        cls.exe = cls.root / 'relay-fixture'
        result = subprocess.run(['clang', '-fblocks', '-Werror=implicit-function-declaration',
                                 '-framework', 'Foundation', '-framework', 'AppKit', '-I', str(ROOT), str(source),
                                 '-o', str(cls.exe)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.profile = cls.root / 'profile'
        cls.browser = subprocess.Popen([cls.chrome, '--headless=new', '--remote-debugging-address=127.0.0.1',
                                        '--remote-debugging-port=0', f'--user-data-dir={cls.profile}',
                                        '--no-first-run', '--no-default-browser-check', 'about:blank'],
                                       stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.addClassCleanup(cls.stop_processes)
        deadline = time.monotonic() + 30
        while not (cls.profile / 'DevToolsActivePort').exists() and time.monotonic() < deadline:
            time.sleep(.05)
        assert (cls.profile / 'DevToolsActivePort').exists(), 'headless Chrome endpoint'
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), OwnedWebsite)
        cls.addClassCleanup(cls.server.server_close)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()
        cls.addClassCleanup(cls.server.shutdown)
        cls.url = f'http://127.0.0.1:{cls.server.server_port}/'

    @classmethod
    def stop_processes(cls):
        markers = [str(cls.exe), '--user-data-dir=' + str(cls.profile)]
        for sig in (signal.SIGTERM, signal.SIGKILL):
            output = subprocess.run(['/bin/ps', '-axo', 'pid=,command='], capture_output=True, text=True, timeout=5).stdout
            for line in output.splitlines():
                fields = line.strip().split(None, 1)
                if len(fields) == 2 and any(marker in fields[1].split() for marker in markers):
                    try:
                        os.kill(int(fields[0]), sig)
                    except ProcessLookupError:
                        pass
            if sig == signal.SIGTERM:
                time.sleep(.5)

    def setUp(self):
        self.runtime = Path(tempfile.mkdtemp(prefix='run-', dir=self.root))
        self.runtime.chmod(0o700)
        self.addCleanup(self.stop_relay)

    def stop_relay(self):
        for pid in self.relay_pids():
            try:
                os.kill(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def relay_pids(self):
        output = subprocess.run(['/bin/ps', '-axo', 'pid=,command='], capture_output=True, text=True, timeout=5).stdout
        return [int(line.split(None, 1)[0]) for line in output.splitlines()
                if f'{self.exe} chrome relay' in line and 'relay-launch' not in line]

    def env(self, **extra):
        env = {key: value for key, value in os.environ.items() if not key.startswith('HARDCORE_')}
        env.update(HARDCORE_CHROME_ATTACH=str(self.profile), HARDCORE_RUNTIME_DIR=str(self.runtime),
                   HARDCORE_CHROME_RELAY_GRACE='30', HARDCORE_CHROME_RELAY_IDLE='600')
        env.update(extra)
        return env

    def fixture(self, mode, argument, **extra):
        result = subprocess.run([str(self.exe), mode, argument], cwd=self.root, env=self.env(**extra),
                                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def test_pages_survive_client_exit_and_are_adopted(self):
        target = self.fixture('create', self.url)
        self.assertEqual(len(self.relay_pids()), 1, 'one background relay outlives its first client')
        self.assertEqual(self.fixture('exists', target), 'yes')
        self.assertEqual(self.fixture('adopt', target), 'kept across clients',
                         'a later client re-attaches the same live document')
        self.assertEqual(len(self.relay_pids()), 1, 'later clients reuse the relay')

    def test_concurrent_clients_route_their_own_sessions(self):
        procs = [subprocess.Popen([str(self.exe), 'create', self.url], cwd=self.root, env=self.env(),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) for _ in range(3)]
        outputs = [proc.communicate(timeout=90) for proc in procs]
        for proc, (out, err) in zip(procs, outputs):
            self.assertEqual(proc.returncode, 0, out + err)
        self.assertEqual(len({out.strip() for out, _ in outputs}), 3)
        self.assertEqual(len(self.relay_pids()), 1, 'simultaneous first clients start one relay')

    def test_unadopted_pages_close_after_grace(self):
        target = self.fixture('create', self.url, HARDCORE_CHROME_RELAY_GRACE='2')
        deadline = time.monotonic() + 15
        self.assertEqual(self.fixture('exists', target, HARDCORE_CHROME_RELAY_GRACE='2'), 'yes', 'kept during grace')
        while self.fixture('exists', target, HARDCORE_CHROME_RELAY_GRACE='2') == 'yes':
            self.assertLess(time.monotonic(), deadline, 'orphaned page closed after the grace period')
            time.sleep(.5)

    def test_quit_closes_adopted_page_immediately(self):
        target = self.fixture('create', self.url)
        self.assertEqual(self.fixture('quit', target), 'quit')
        self.assertEqual(self.fixture('exists', target), 'no')

    def test_idle_relay_exits_and_removes_socket(self):
        target = self.fixture('create', self.url, HARDCORE_CHROME_RELAY_GRACE='1', HARDCORE_CHROME_RELAY_IDLE='2')
        self.assertTrue(target)
        deadline = time.monotonic() + 20
        while self.relay_pids() or (self.runtime / 'chrome-relay.sock').exists():
            self.assertLess(time.monotonic(), deadline, 'idle relay exits and removes its socket')
            time.sleep(.25)

    def test_unavailable_chrome_reports_the_reason(self):
        message = self.fixture('unavailable', '-', HARDCORE_CHROME_ATTACH=str(self.root / 'missing'))
        self.assertIn('Chrome attach is unavailable', message)
        deadline = time.monotonic() + 5
        while self.relay_pids():
            self.assertLess(time.monotonic(), deadline, 'a relay with no Chrome exits')
            time.sleep(.1)


if __name__ == '__main__':
    unittest.main()
