#!/usr/bin/env python3
"""The production shared IPC server, without AppKit, real tabs, or user sockets."""
import json
import os
from pathlib import Path
import plistlib
import select
import socket
import struct
import subprocess
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#define MAC_PACKET_MAX (256u * 1024u * 1024u)
#define MAC_HANDOFF_VERSION 1
#define MAX_TABS 4
#define TK_BROWSER 1
static char g_mac_endpoint[104], g_mac_exe_path[64] = "/fixture/hardcore";
static NSString *g_mac_install_exe;
typedef struct { int kind, fd, exited; int *wpending; } Tab;
static Tab terminal = {0, 42, 0, NULL}, browser = {TK_BROWSER, -1, 0, NULL};
static Tab *g_tabs[MAX_TABS];
static int g_ntabs = 2, g_active, touched, spawned;
@interface FixtureWindow : NSObject
- (NSRect)frame;
@end
@implementation FixtureWindow
- (NSRect)frame { return NSMakeRect(0, 0, 80, 24); }
@end
static FixtureWindow *g_win;
static int mac_input_held(Tab *tab) { (void)tab; return 0; }
static void read_some(Tab *tab) { (void)tab; touched++; }
static void tab_drain(Tab *tab) { (void)tab; touched++; }
static NSDictionary *mac_tab_snapshot(Tab *tab) { (void)tab; touched++; return @{}; }
static int mac_send_fd(int socket, int fd) { (void)socket; (void)fd; touched++; return 0; }
static int mac_spawn(NSString *exe, NSArray *args, int fd, int detached, pid_t *pid) {
    (void)exe; (void)args; (void)fd; (void)detached; (void)pid; spawned++; return 0;
}
static NSDictionary *mac_info(void) { return @{@"protocol": @1, @"tabs": @[]}; }
static NSDictionary *mac_claude_receipt(NSDictionary *request) { return @{@"fixture": request[@"op"]}; }
static NSDictionary *mac_browser_request(NSDictionary *request) {
    if ([request[@"op"] isEqual:@"throw"]) [NSException raise:@"fixture" format:@"malformed"];
    return @{@"ok": @YES, @"request": request, @"touched": @(touched), @"spawned": @(spawned)};
}
static int mac_reload(int ack, char *why, size_t capacity) {
    (void)why; (void)capacity; dprintf(ack, "ok\n"); return 0;
}
static void mac_retire(void) { _exit(99); }
/* FUNCTIONS */
int main(int argc, char **argv) {
    @autoreleasepool {
        signal(SIGPIPE, SIG_IGN);
        g_win = [FixtureWindow new];
        int first = argc > 1 && !strcmp(argv[1], "browser-first");
        g_tabs[first ? 1 : 0] = &terminal; g_tabs[first ? 0 : 1] = &browser;
        if (mac_transfer_reload(@"/fixture/replacement") || spawned) return 2;
        mac_server();
        if (!g_mac_endpoint[0] || access(g_mac_endpoint, F_OK)) {
            fprintf(stderr, "fixture server did not bind: %s\n", strerror(errno)); return 3;
        }
        puts(g_mac_endpoint); fflush(stdout);
        dispatch_main();
    }
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'macOS Foundation required')
class BrowserSharedServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-browser-ipc-', dir='/private/tmp')
        cls.root = Path(cls.temp.name)
        source = ROOT / 'hardcore_mac_transfer.m'
        signatures = [
            'static int mac_io(int fd, void *data, size_t n, int sending)',
            'static void mac_socket_setup(int fd)',
            'static int mac_peer_ok(int fd)',
            'static int mac_send(int fd, NSDictionary *value)',
            'static NSDictionary *mac_recv(int fd)',
            'static NSString *mac_runtime(void)',
            'static NSString *mac_transfer_block_reason(void)',
            'static int mac_export(int sock)',
            'static int mac_transfer_reload(NSString *exe)',
            'static void mac_server(void)',
        ]
        functions = '\n\n'.join(production_function(source, signature) for signature in signatures)
        harness = cls.root / 'server.m'
        harness.write_text(HARNESS.replace('/* FUNCTIONS */', functions))
        cls.exe = cls.root / 'server'
        result = bounded_run(['clang', '-Wall', '-Wextra', '-Wno-unused-parameter',
                              '-Wno-incompatible-pointer-types', '-fblocks',
                              '-framework', 'Foundation', str(harness), '-o', str(cls.exe)], 30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def start_server(self, order='browser-last'):
        # sockaddr_un.sun_path is only 104 bytes on macOS.
        runtime = Path(tempfile.mkdtemp(prefix='run-', dir=self.root))
        child = subprocess.Popen([str(self.exe), order], env=dict(os.environ, HARDCORE_RUNTIME_DIR=str(runtime)),
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, start_new_session=True)
        def cleanup():
            child.terminate()
            child.communicate(timeout=5)
        self.addCleanup(cleanup)
        # A missing sandbox socket permission is an explicit failure, not a pass.
        ready, _, _ = select.select([child.stdout], [], [], 5)
        self.assertTrue(ready, 'server did not announce its socket within five seconds')
        path = child.stdout.readline().strip()
        if not path:
            _, stderr = child.communicate(timeout=5)
            self.fail(stderr or 'server did not start')
        return path

    def exchange(self, path, data):
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(3)
            client.connect(path)
            client.sendall(data)
            client.shutdown(socket.SHUT_WR)
            received = bytearray()
            while chunk := client.recv(65536):
                received.extend(chunk)
            return bytes(received)

    def plist(self, path, value):
        data = plistlib.dumps(value, fmt=plistlib.FMT_BINARY)
        reply = self.exchange(path, struct.pack('!I', len(data)) + data)
        self.assertGreaterEqual(len(reply), 4)
        length, = struct.unpack('!I', reply[:4])
        self.assertEqual(length, len(reply) - 4)
        return plistlib.loads(reply[4:])

    def test_browser_and_legacy_protocols_share_one_server(self):
        path = self.start_server()
        info = json.loads(self.exchange(path, b'info\n'))
        self.assertEqual(info['browser_protocol'], 1)
        self.assertEqual(info['exe'], '/fixture/hardcore')
        for request in ({'op': 'list'}, {'op': 'open', 'url': 'https://example.com/a?b=x&c=y', 'parent': 3},
                        {'op': 'mode', 'id': 7, 'mode': 'native'}):
            reply = json.loads(self.exchange(path, b'browser ' + json.dumps(request).encode() + b'\n'))
            self.assertTrue(reply['ok'])
            self.assertEqual(reply['request'], request)
        self.assertEqual(self.plist(path, {'op': 'list'})['protocol'], 1)
        self.assertEqual(self.plist(path, {'op': 'claude-guard'})['fixture'], 'claude-guard')

    def test_malformed_browser_requests_do_not_kill_server(self):
        path = self.start_server()
        for data in (b'browser []\n', b'browser garbage\n', b'browser {"op":"throw"}\n'):
            self.assertFalse(json.loads(self.exchange(path, data))['ok'])
        for data in (b'browser {"op":"list"}', b'browser ' + b'x' * 32760 + b'\n'):
            self.assertEqual(self.exchange(path, data), b'')
        self.assertIn('browser_protocol', json.loads(self.exchange(path, b'info\n')))

    def assert_transfer_retained(self, path):
        for request in ({'op': 'take'}, {'op': 'reload', 'exe': '/fixture/replacement'}):
            self.assertIn('browser tabs', self.plist(path, request)['error'])
        reply = json.loads(self.exchange(path, b'browser {"op":"list"}\n'))
        self.assertTrue(reply['ok'])
        self.assertEqual(reply['touched'], 0, 'donor PTYs were accessed before refusing transfer')
        self.assertEqual(reply['spawned'], 0, 'replacement was launched for an unsupported transfer')

    def test_browser_last_preserves_entire_donor(self):
        self.assert_transfer_retained(self.start_server())

    def test_browser_first_preserves_entire_donor(self):
        self.assert_transfer_retained(self.start_server('browser-first'))


if __name__ == '__main__':
    unittest.main()
