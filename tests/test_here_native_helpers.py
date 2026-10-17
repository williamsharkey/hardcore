#!/usr/bin/env python3
"""Native helper acceptance tests; Python is only the test harness.

The fixture links the shipped header with system frameworks. Actual file and
web views run in disposable PTYs with no Python in PATH or app resources.
"""
from __future__ import annotations

import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <ctype.h>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
/* Replace only the system script executor in this test translation unit.
   Production helper code has no test flags and no Apple event reaches a tab. */
static NSArray *fontReplies;
static NSMutableArray *fontScripts;
@interface HHHMockScript : NSObject { NSString *source; }
- (id)initWithSource:(NSString*)value;
- (NSAppleEventDescriptor*)executeAndReturnError:(NSDictionary**)error;
@end
@implementation HHHMockScript
- (id)initWithSource:(NSString*)value {if((self=[super init]))source=[value copy];return self;}
- (void)dealloc {[source release];[super dealloc];}
- (NSAppleEventDescriptor*)executeAndReturnError:(NSDictionary**)error {
    NSUInteger index=[fontScripts count];[fontScripts addObject:source];
    if(index>=[fontReplies count]){if(error)*error=@{NSAppleScriptErrorMessage:@"Unexpected extra font operation"};return nil;}
    NSDictionary *reply=fontReplies[index];
    if(reply[@"error"]){if(error)*error=@{NSAppleScriptErrorMessage:reply[@"error"]};return nil;}
    NSAppleEventDescriptor *list=[NSAppleEventDescriptor listDescriptor];
    [list insertDescriptor:[NSAppleEventDescriptor descriptorWithInt32:72] atIndex:1];
    [list insertDescriptor:[NSAppleEventDescriptor descriptorWithString:reply[@"name"]] atIndex:2];
    [list insertDescriptor:[NSAppleEventDescriptor descriptorWithString:[reply[@"size"] stringValue]] atIndex:3];
    return list;
}
@end
#define NSAppleScript HHHMockScript
static int cp_width(uint32_t cp) {
    if (cp >= 0x300 && cp <= 0x36f) return 0;
    return cp >= 0x1100 ? 2 : 1;
}
static int utf8_encode(uint32_t c, char *o) {
    if(c<128){o[0]=c;return 1;}
    if(c<2048){o[0]=0xc0|(c>>6);o[1]=0x80|(c&63);return 2;}
    if(c<65536){o[0]=0xe0|(c>>12);o[1]=0x80|((c>>6)&63);o[2]=0x80|(c&63);return 3;}
    o[0]=0xf0|(c>>18);o[1]=0x80|((c>>12)&63);o[2]=0x80|((c>>6)&63);o[3]=0x80|(c&63);return 4;
}
#include "hardcore_here_helpers.h"
int main(int argc, char **argv) {
    if(argc>1 && !strcmp(argv[1],"files"))return hc_here_files_main(argc-1,argv+1);
    if(argc>1 && !strcmp(argv[1],"web"))return hc_here_web_main(argc-1,argv+1);
    @autoreleasepool {
        if(argc>2 && !strcmp(argv[1],"url")) {
            NSString *url=hhh_url([NSString stringWithUTF8String:argv[2]],argc>3?[NSString stringWithUTF8String:argv[3]]:nil);
            if(!url)return 2;puts([url UTF8String]);return 0;
        }
        if(argc>1 && !strcmp(argv[1],"parse")) {
            NSMutableData *input=[NSMutableData data];char buf[8192];size_t n;
            while((n=fread(buf,1,sizeof(buf),stdin)) && [input length]<HHH_BODY_MAX)[input appendBytes:buf length:n];
            NSString *html=[[[NSString alloc]initWithData:input encoding:NSUTF8StringEncoding]autorelease];
            NSDictionary *page=hhh_html(html?:@"",@"https://example.com/base/",0);
            NSData *out=[NSJSONSerialization dataWithJSONObject:page options:0 error:NULL];
            fwrite(out.bytes,1,out.length,stdout);return 0;
        }
        if(argc>1 && !strcmp(argv[1],"settings")) {
            HereSettings s;here_settings_load(&s);
            if(argc>2){s.theme='n';s.font_size=17;s.allow_host_font=1;snprintf(s.font_name,sizeof(s.font_name),"Monaco");if(!here_settings_save(&s))return 2;}
            printf("%c %d %d %s\n",s.theme,s.font_size,s.allow_host_font,s.font_name);return 0;
        }
        if(argc>1 && !strcmp(argv[1],"font-denied")) {
            HCHereHostFont font;hc_here_host_font_init(&font);
            if(hc_here_host_font_apply(&font,"Monaco",15,0)||font.active)return 2;
            if(!hc_here_host_font_restore(&font))return 3;puts(font.error);return 0;
        }
        if(argc>2 && !strcmp(argv[1],"font-transaction")) {
            HCHereHostFont font={0};font.available=1;snprintf(font.tty,sizeof(font.tty),"/dev/ttysowned");
            fontScripts=[NSMutableArray array];
            NSDictionary *before=@{@"name":@"Menlo-Regular",@"size":@11.5};
            NSDictionary *after=@{@"name":@"Monaco",@"size":@15};
            NSDictionary *failure=@{@"error":@"Owned fixture denied request"};
            if(!strcmp(argv[2],"normal")) {
                fontReplies=@[before,after,before];
                if(!hc_here_host_font_apply(&font,"Monaco",15,1)||!font.active||font.original_size!=11.5)return 10;
                if(!hc_here_host_font_restore(&font)||font.active)return 11;
            }else if(!strcmp(argv[2],"apply-failed")) {
                fontReplies=@[before,failure,before];
                if(hc_here_host_font_apply(&font,"Monaco",15,1)||font.active)return 12;
            }else if(!strcmp(argv[2],"restore-retry")) {
                fontReplies=@[before,after,failure,before];
                if(!hc_here_host_font_apply(&font,"Monaco",15,1))return 13;
                if(hc_here_host_font_restore(&font)||!font.active||strcmp(font.original_name,"Menlo-Regular")||font.original_size!=11.5)return 14;
                if(!hc_here_host_font_restore(&font)||font.active)return 15;
            }else if(!strcmp(argv[2],"mismatch")) {
                fontReplies=@[before,@{@"name":@"Unexpected",@"size":@15},before];
                if(hc_here_host_font_apply(&font,"Monaco",15,1)||font.active)return 16;
            }else if(!strcmp(argv[2],"read-failed")) {
                fontReplies=@[failure];
                if(hc_here_host_font_apply(&font,"Monaco",15,1)||font.active)return 17;
            }else if(!strcmp(argv[2],"not-allowed")) {
                fontReplies=@[];
                if(hc_here_host_font_apply(&font,"Monaco",15,0)||font.active)return 18;
            }else return 19;
            NSData *out=[NSJSONSerialization dataWithJSONObject:fontScripts options:0 error:NULL];
            fwrite(out.bytes,1,out.length,stdout);return 0;
        }
    }
    return 2;
}
'''


class OwnedPTY:
    def __init__(self, command, env):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 100, 0, 0))
        self.before = termios.tcgetattr(self.slave)
        self.process = subprocess.Popen(command, stdin=self.slave, stdout=self.slave,
                                        stderr=self.slave, env=env, start_new_session=True)
        self.output = bytearray()

    def read_until(self, text, timeout=6):
        wanted = text.encode() if isinstance(text, str) else text
        limit = time.monotonic() + timeout
        while time.monotonic() < limit:
            if wanted in self.output:
                return
            if select.select([self.master], [], [], .05)[0]:
                try:
                    self.output.extend(os.read(self.master, 65536))
                except OSError:
                    break
        raise AssertionError(f'{wanted!r} missing from {bytes(self.output)[-3000:]!r}')

    def send(self, data):
        os.write(self.master, data)

    def close(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        deadline = time.monotonic() + 3
        while self.process.poll() is None and time.monotonic() < deadline:
            if select.select([self.master], [], [], .05)[0]:
                try:
                    self.output.extend(os.read(self.master, 65536))
                except OSError:
                    pass
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait()
        after = termios.tcgetattr(self.slave)
        # Darwin toggles PENDIN on a raw -> canonical transition itself.
        mask = ~getattr(termios, 'PENDIN', 0)
        self.before[3] &= mask
        after[3] &= mask
        os.close(self.master)
        os.close(self.slave)
        if self.before != after:
            raise AssertionError('host termios was not restored')


@unittest.skipUnless(sys.platform == 'darwin', 'native system-framework helpers use macOS')
class NativeHelpers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-native-helpers-')
        cls.root = Path(cls.temp.name)
        source = cls.root / 'fixture.m'
        source.write_text(SOURCE)
        cls.binary = cls.root / 'fixture'
        subprocess.run(['clang', '-Wall', '-Wextra', '-Wno-unused-function',
                        '-Wno-unused-parameter', '-fblocks', '-framework', 'Foundation',
                        '-framework', 'AppKit', '-I', str(ROOT), str(source), '-o', str(cls.binary)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(dir=self.root)
        self.path = Path(self.case.name)
        self.env = {**os.environ, 'PATH': '/usr/bin:/bin',
                    'XDG_CONFIG_HOME': str(self.path / 'config'), 'TERM_PROGRAM': 'test-owned'}
        self.addCleanup(self.case.cleanup)

    def run_helper(self, *args, input=None):
        return subprocess.run([str(self.binary), *args], input=input, capture_output=True,
                              env=self.env, timeout=5)

    def parse(self, html):
        proc = self.run_helper('parse', input=html.encode())
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return json.loads(proc.stdout)

    def owned(self, *args):
        p = OwnedPTY([str(self.binary), *args], self.env)
        self.addCleanup(p.close)
        return p

    def test_html_text_links_entities_and_hidden_content(self):
        page = self.parse('<head><title>Small &amp; useful</title><script>bad()</script></head>'
                          '<h1>Heading</h1><p>café 🚀 &lt;literal&gt; &#27;[31m</p>'
                          '<span hidden>SECRET</span><span style="display: none">HIDDEN</span>'
                          '<a href="../next?a=1&amp;b=2">Next</a>'
                          '<a href="javascript:alert(1)">unsafe</a>'
                          '<img alt="diagram"><pre>a  b\nc</pre>')
        self.assertEqual(page['title'], 'Small & useful')
        self.assertIn('café 🚀 <literal>', page['text'])
        self.assertIn('Next [1]', page['text'])
        self.assertIn('[Image: diagram]', page['text'])
        self.assertIn('a  b\nc', page['text'])
        self.assertNotIn('SECRET', page['text'])
        self.assertNotIn('HIDDEN', page['text'])
        self.assertNotIn('bad()', page['text'])
        self.assertNotIn('\x1b', page['text'])
        self.assertEqual(page['links'], ['https://example.com/next?a=1&b=2'])

    def test_response_text_and_link_bounds(self):
        page = self.parse('<p>' + 'a' * 190000 + '</p>')
        self.assertLessEqual(len(page['text']), 160100)
        self.assertIn('truncated', page['text'])
        page = self.parse(''.join(f'<a href="/{i}">link</a>' for i in range(400)))
        self.assertEqual(len(page['links']), 300)

    def test_url_validation_and_relative_resolution(self):
        self.assertEqual(self.run_helper('url', '/hello', 'https://example.com/a/').stdout.strip(),
                         b'https://example.com/hello')
        for url in ('file:///etc/passwd', 'javascript:alert(1)', 'https://user:pass@example.com',
                    'https://example.com/\nBAD', 'x' * 4100):
            self.assertNotEqual(self.run_helper('url', url).returncode, 0, url)

    def test_settings_roundtrip_and_private_mode(self):
        self.assertEqual(self.run_helper('settings').stdout.strip(), b'g 14 0 Menlo-Regular')
        self.assertEqual(self.run_helper('settings', 'save').returncode, 0)
        self.assertEqual(self.run_helper('settings').stdout.strip(), b'n 17 1 Monaco')
        settings = self.path / 'config/hardcore/here.json'
        self.assertEqual(settings.stat().st_mode & 0o777, 0o600)
        settings.write_text('{"theme": [], "font_size": true, "allow_host_font": "yes", "font_name": "bad\\u001b"}')
        self.assertEqual(self.run_helper('settings').stdout.strip(), b'g 14 0 Menlo-Regular')

    def test_settings_refuse_symlink_read(self):
        directory = self.path / 'config/hardcore'
        directory.mkdir(parents=True)
        source = self.path / 'other'
        source.write_text('{"theme":"n"}')
        (directory / 'here.json').symlink_to(source)
        self.assertEqual(self.run_helper('settings').stdout.strip(), b'g 14 0 Menlo-Regular')

    def test_font_changes_are_opt_in_without_apple_events(self):
        result = self.run_helper('font-denied')
        self.assertEqual(result.returncode, 0)
        self.assertIn(b'Enable Allow host font changes first', result.stdout)

    def test_font_transaction_snapshot_apply_restore_order(self):
        result = self.run_helper('font-transaction', 'normal')
        self.assertEqual(result.returncode, 0, result.stderr)
        scripts = json.loads(result.stdout)
        self.assertEqual(len(scripts), 3)
        self.assertNotIn('set font name', scripts[0])
        self.assertIn('set font name of pickedTab to "Monaco"', scripts[1])
        self.assertIn('set font name of pickedTab to "Menlo-Regular"', scripts[2])
        self.assertIn('set font size of pickedTab to 11.5', scripts[2])
        self.assertIn('pickedWindow is not 72', scripts[1])
        self.assertNotIn('current settings', ''.join(scripts))

    def test_font_failed_write_and_verification_both_roll_back(self):
        for scenario in ('apply-failed', 'mismatch'):
            result = self.run_helper('font-transaction', scenario)
            self.assertEqual(result.returncode, 0, (scenario, result.stderr))
            scripts = json.loads(result.stdout)
            self.assertEqual(len(scripts), 3)
            self.assertIn('set font size of pickedTab to 11.5', scripts[-1])

    def test_font_failed_restore_retains_snapshot_for_retry(self):
        result = self.run_helper('font-transaction', 'restore-retry')
        self.assertEqual(result.returncode, 0, result.stderr)
        scripts = json.loads(result.stdout)
        self.assertEqual(len(scripts), 4)
        self.assertEqual(scripts[2], scripts[3])

    def test_font_read_failure_and_missing_opt_in_never_write(self):
        result = self.run_helper('font-transaction', 'read-failed')
        self.assertEqual(result.returncode, 0)
        scripts = json.loads(result.stdout)
        self.assertEqual(len(scripts), 1)
        self.assertNotIn('set font name', scripts[0])
        result = self.run_helper('font-transaction', 'not-allowed')
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), [])

    def test_files_native_preview_and_restore(self):
        (self.path / 'sample.txt').write_text('NATIVE FILE CONTENT café\n')
        p = self.owned('files', str(self.path))
        p.read_until('sample.txt')
        p.send(b'j\r')  # .. is the first directory entry, then sample.txt
        p.read_until('NATIVE FILE CONTENT')
        p.send(b'q')
        p.read_until('Hardcore files')

    def test_filename_controls_are_visible_text_not_terminal_commands(self):
        (self.path / '\x1b]2;REMOTE TITLE\x07.txt').write_text('safe')
        p = self.owned('files', str(self.path))
        p.read_until('REMOTE TITLE')
        self.assertNotIn(b'\x1b]2;REMOTE TITLE', p.output)

    def test_bracketed_paste_cannot_execute_helper_shortcuts(self):
        p = self.owned('files', str(self.path))
        p.read_until('Hardcore files')
        p.send(b'\x1b[200~qoq\x03\x1b[201~')
        time.sleep(.1)
        self.assertIsNone(p.process.poll())
        p.send(b'\x1b]not-a-command')
        time.sleep(.1)
        p.send(b'q\x03\x1b\\')
        time.sleep(.1)
        self.assertIsNone(p.process.poll())

    def test_native_web_fetch_links_back_and_cancel(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                payload = (b'<h1>FIRST PAGE</h1><a href="/next">Next</a>' if self.path == '/'
                           else b'<h1>SECOND PAGE</h1>')
                self.send_response(200)
                self.send_header('Content-Type', 'text/html; charset=utf-8')
                self.send_header('Content-Length', str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        p = self.owned('web', f'http://127.0.0.1:{server.server_port}/')
        p.read_until('FIRST PAGE')
        p.send(b'1\r')
        p.read_until('SECOND PAGE')
        p.output.clear()
        p.send(b'b')
        p.read_until('FIRST PAGE')
        p.send(b'q')
        deadline = time.monotonic() + 3
        while p.process.poll() is None and time.monotonic() < deadline:
            if select.select([p.master], [], [], .05)[0]:
                p.output.extend(os.read(p.master, 65536))
        self.assertEqual(p.process.poll(), 0)

    def test_native_web_download_redirect_bounds_and_live_cancellation(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path == '/large':
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/html')
                    self.send_header('Content-Length', str(2 * 1024 * 1024 + 1))
                    self.end_headers()
                elif self.path == '/loop':
                    self.send_response(302)
                    self.send_header('Location', '/loop')
                    self.end_headers()
                else:
                    time.sleep(.7)
                    try:
                        self.send_response(200)
                        self.send_header('Content-Type', 'text/html')
                        self.end_headers()
                        self.wfile.write(b'LATE RESPONSE')
                    except (BrokenPipeError, ConnectionResetError):
                        pass
            def log_message(self, *args):
                pass
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        base = f'http://127.0.0.1:{server.server_port}'
        large = self.owned('web', base + '/large')
        large.read_until('2 MiB limit')
        large.send(b'q')
        redirect = self.owned('web', base + '/loop')
        redirect.read_until('excessive redirects')
        redirect.send(b'q')
        pending = self.owned('web', base + '/pending')
        pending.read_until('Loading page')
        started = time.monotonic()
        pending.send(b'q')
        while pending.process.poll() is None and time.monotonic() - started < .5:
            if select.select([pending.master], [], [], .02)[0]:
                pending.output.extend(os.read(pending.master, 65536))
        self.assertEqual(pending.process.poll(), 0, 'quit must not wait for a network response')


if __name__ == '__main__':
    unittest.main()
