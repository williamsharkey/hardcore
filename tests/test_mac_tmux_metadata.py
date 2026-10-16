#!/usr/bin/env python3
"""Headless tmux title metadata tests, using owned children/private servers only.

No Terminal windows, pane output capture, key injection, or existing tmux
servers are used. Fake clients cover timeouts, cache behavior and malformed
responses; an optional private tmux server checks actual active-pane metadata.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#import <Foundation/Foundation.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static void require(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static NSString *fixture_directory(void) { return @(getenv("HC_META_FIXTURE")); }
static NSDictionary *fixture_state(void) {
    NSData *data = [NSData dataWithContentsOfFile:[fixture_directory() stringByAppendingPathComponent:@"state.json"]];
    return data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : @{};
}
static void save_state(NSDictionary *state) {
    NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:NULL];
    require([data writeToFile:[fixture_directory() stringByAppendingPathComponent:@"state.json"] atomically:YES], "write owned fixture");
}
/* NATIVE_METADATA */
/* TMUX_METADATA */

static NSArray *requests(void) {
    NSString *text = [NSString stringWithContentsOfFile:[fixture_directory() stringByAppendingPathComponent:@"requests.jsonl"] encoding:NSUTF8StringEncoding error:NULL];
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) if (line.length)
        [out addObject:[NSJSONSerialization JSONObjectWithData:[line dataUsingEncoding:NSUTF8StringEncoding] options:0 error:NULL]];
    return out;
}
static void pump(double milliseconds) {
    double end = mac_tmux_metadata_time() + milliseconds;
    do {
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
    } while (mac_tmux_metadata_time() < end);
}
static NSDictionary *complete(NSString *session, NSString *socket) {
    mac_tmux_metadata(session, socket);
    NSArray *key = @[session, socket ?: @""];
    double deadline = mac_tmux_metadata_time() + 2500;
    while ([g_mac_tmux_metadata_cache[key][@"busy"] boolValue]) {
        require(mac_tmux_metadata_time() < deadline, "background query did not complete");
        pump(5);
    }
    return mac_tmux_metadata(session, socket);
}
static void equal(id actual, id expected, const char *message) {
    require([actual isEqual:expected], message);
}
int main(int argc, const char **argv) {
  @autoreleasepool {
    require(argc >= 2, "case required"); const char *which = argv[1];
    if (!strcmp(which, "cache")) {
        double start = mac_tmux_metadata_time();
        require(!mac_tmux_metadata(@"$0", nil), "first lookup should return immediately without a snapshot");
        require(mac_tmux_metadata_time() - start < 60, "initial lookup blocked main thread");
        for (int i = 0; i < 10000; i++) mac_tmux_metadata(@"$0", nil);
        require(mac_tmux_metadata_time() - start < 180, "duplicate lookups blocked on a slow client");
        NSDictionary *value = complete(@"$0", nil);
        equal(value[@"cwd"], @"/native/project", "kernel cwd did not supersede pane shell cwd");
        equal(value[@"program"], @"claude", "kernel foreground program did not supersede tmux wrapper");
        equal(value[@"pid"], @404, "foreground PID was replaced by shell PID");
        equal(value[@"sessionId"], @"$0", "session identity missing");
        equal(value[@"paneId"], @"%0", "pane identity missing");
        require(requests().count == 1, "duplicate tabs launched duplicate queries");
        for (int i = 0; i < 10000; i++) equal(mac_tmux_metadata(@"$0", @""), value, "cached snapshot changed before TTL");
        require(requests().count == 1, "cached lookups launched processes before TTL");
    } else if (!strcmp(which, "sockets")) {
        mac_tmux_metadata(@"$0", @"first"); mac_tmux_metadata(@"$0", @"second");
        NSDictionary *first = complete(@"$0", @"first"), *second = complete(@"$0", @"second");
        equal(first[@"cwd"], @"/first", "first socket snapshot wrong");
        equal(second[@"cwd"], @"/second", "second socket aliased first server's $0");
        require(requests().count == 2, "socket queries did not remain separate");
    } else if (!strcmp(which, "refresh")) {
        equal(complete(@"$0", nil)[@"paneId"], @"%0", "initial pane missing");
        NSMutableDictionary *state = [[fixture_state() mutableCopy] autorelease];
        state[@"fields"] = @[@"$0", @"fixture", @"%1", @"/dev/ttys9002", @"/other/pane", @"grok", @"333"];
        save_state(state);
        equal(mac_tmux_metadata(@"$0", nil)[@"paneId"], @"%0", "unexpired cache did not return prior snapshot");
        require(requests().count == 1, "unexpired cache refreshed early");
        pump(1050);
        NSDictionary *value = complete(@"$0", nil);
        equal(value[@"paneId"], @"%1", "active pane change did not refresh");
        equal(value[@"cwd"], @"/other/pane", "active pane cwd did not refresh");
        equal(value[@"program"], @"grok", "active pane program did not refresh");
        require(requests().count == 2, "TTL did not permit exactly one refresh");
    } else if (!strcmp(which, "failed-refresh")) {
        require(complete(@"$0", nil) != nil, "initial snapshot missing");
        NSMutableDictionary *state = [[fixture_state() mutableCopy] autorelease]; state[@"exit"] = @1; save_state(state);
        pump(1050);
        require(!complete(@"$0", nil), "failed refresh retained stale pane metadata");
        for (int i = 0; i < 1000; i++) require(!mac_tmux_metadata(@"$0", nil), "unavailable cache revived stale metadata");
        require(requests().count == 2, "failed queries retried without backoff");
    } else if (!strcmp(which, "unavailable")) {
        double start = mac_tmux_metadata_time();
        require(!complete(@"$0", nil), "invalid/failed response accepted");
        require(mac_tmux_metadata_time() - start < 1500, "metadata timeout exceeded bounded deadline");
        int status; errno = 0;
        require(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD, "timed-out owned query was not reaped");
        require(requests().count == 1, "failed query retried immediately");
    } else if (!strcmp(which, "arguments")) {
        NSString *session = @"name $(touch forbidden) ; *", *socket = @"server path $(touch forbidden)";
        setenv("TMUX", "inherited-socket", 1); setenv("TMUX_PANE", "%999", 1);
        NSDictionary *value = complete(session, socket);
        equal(value[@"sessionName"], session, "literal unusual session name changed");
        NSDictionary *request = requests()[0]; NSArray *args = request[@"args"];
        require([args containsObject:@"-N"] && [args containsObject:@"display-message"] && [args containsObject:@"-p"], "query is not explicitly read-only and server-start-free");
        NSUInteger target = [args indexOfObject:@"-t"], path = [args indexOfObject:@"-S"];
        equal(args[target+1], [NSString stringWithFormat:@"=%@:", session], "session uses wildcard/prefix target instead of exact literal match");
        equal(args[path+1], socket, "socket escaped or shell-expanded");
        require(![request[@"tmux"] boolValue] && ![request[@"pane"] boolValue], "inherited client changed default server resolution");
        require(![[NSFileManager defaultManager] fileExistsAtPath:[fixture_directory() stringByAppendingPathComponent:@"forbidden"]], "metadata argument executed shell code");
    } else if (!strcmp(which, "shell")) {
        NSDictionary *value = complete(@"$0", nil);
        equal(value[@"program"], @"", "idle shell incorrectly appears as foreground program");
        equal(value[@"shell"], @"zsh", "idle shell basename lost");
    } else if (!strcmp(which, "path")) {
        equal(complete(@"$0", nil)[@"cwd"], @"/repo/café\twork\nother", "tabs/newlines/Unicode in paths corrupted record framing");
    } else if (!strcmp(which, "cache-bound")) {
        require(!mac_tmux_metadata(nil, nil) && !mac_tmux_metadata(@"", nil), "invalid session allocated query");
        require(!g_mac_tmux_metadata_cache, "invalid lookup allocated cache");
        g_mac_tmux_metadata_cache = [[NSMutableDictionary alloc] init];
        for (int i = 0; i < 64; i++) g_mac_tmux_metadata_cache[@[[NSString stringWithFormat:@"$%d", i], @""]] =
            [NSMutableDictionary dictionaryWithDictionary:@{@"used":@(i), @"next":@(DBL_MAX)}];
        mac_tmux_metadata(@"$100", nil);
        require(g_mac_tmux_metadata_cache.count == 64 && !g_mac_tmux_metadata_cache[@[@"$0", @""]], "cache did not evict oldest idle session");
        complete(@"$100", nil);
    } else if (!strcmp(which, "real-query")) {
        require(argc == 5, "private tmux session/socket/cwd required");
        NSDictionary *value = complete(@(argv[2]), @(argv[3]));
        require(value != nil, "private tmux active pane unavailable");
        equal(value[@"cwd"], @(argv[4]), "private tmux active cwd differs");
        equal(value[@"program"], @"sleep", "private tmux foreground process differs");
        require([value[@"pid"] intValue] > 1 && [value[@"tty"] hasPrefix:@"/dev/ttys"], "private pane process/tty missing");
    } else if (!strcmp(which, "real-unavailable")) {
        require(argc == 4 && !complete(@(argv[2]), @(argv[3])), "exact session target unexpectedly matched prefix");
    } else require(0, "unknown case");
    printf("passed %s\n", which);
  }
  return 0;
}
'''

STUB_NATIVE = r'''
static NSDictionary *mac_terminal_metadata(NSString *tty) {
    require(![NSThread isMainThread], "kernel metadata query ran on main thread");
    return fixture_state()[@"native"][tty] ?: @{};
}
'''

FAKE_CLIENT = r'''
import json, os, pathlib, sys, time
root = pathlib.Path(os.environ['HC_META_FIXTURE'])
state = json.loads((root / 'state.json').read_text())
args = sys.argv[1:]
with (root / 'requests.jsonl').open('a') as log:
    log.write(json.dumps({'args': args, 'tmux': bool(os.environ.get('TMUX')), 'pane': bool(os.environ.get('TMUX_PANE'))}) + '\n')
if '-S' in args:
    state = state.get('responses', {}).get(args[args.index('-S') + 1], state)
time.sleep(state.get('delay', 0))
if state.get('mode') == 'oversize': sys.stdout.write('X' * 20000)
elif state.get('mode') == 'invalid-utf8': sys.stdout.buffer.write(b'\xff\n')
elif state.get('mode') == 'unterminated': sys.stdout.write('no newline')
else: sys.stdout.write('\x1e'.join(state.get('fields', [])) + '\n')
sys.exit(state.get('exit', 0))
'''


@unittest.skipUnless(sys.platform == 'darwin', 'macOS Foundation and spawn extensions required')
class MacTmuxMetadataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-tmux-metadata-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        cls.fakebin = cls.directory/'bin'
        cls.fakebin.mkdir()
        fake = cls.fakebin/'tmux'
        fake.write_text('#!'+sys.executable+'\n'+FAKE_CLIENT)
        fake.chmod(0o700)
        module = (ROOT/'hardcore_mac_tmux_metadata.m').read_text()
        for variant, native in [('fake', STUB_NATIVE), ('real', cls.native_source())]:
            source = cls.directory/(variant+'.m')
            source.write_text(HARNESS.replace('/* NATIVE_METADATA */', native).replace('/* TMUX_METADATA */', module))
            result = bounded_run(['clang', '-O0', '-Wall', '-Wextra', '-Werror', '-fblocks',
                                  '-mmacosx-version-min=11.0', '-framework', 'Cocoa', str(source), '-o', str(cls.directory/variant)], 60)
            if result.returncode:
                raise AssertionError('metadata harness compile failed:\n'+result.stderr)

    @staticmethod
    def native_source():
        source = (ROOT/'hardcore_mac_terminal.m').read_text()
        return source[source.index('#include <libproc.h>'):source.index('static NSAppleEventDescriptor *mac_terminal_script')]

    def state(self, **overrides):
        return {'fields': ['$0', 'fixture', '%0', '/dev/ttys9001', '/pane/project', 'node', '222'], **overrides}

    def run_case(self, name, state):
        with tempfile.TemporaryDirectory(prefix=name+'-', dir=self.directory) as directory:
            directory = Path(directory)
            (directory/'state.json').write_text(json.dumps(state))
            command = ['/usr/bin/env', 'PATH='+str(self.fakebin)+':/usr/bin:/bin',
                       'HC_META_FIXTURE='+str(directory), str(self.directory/'fake'), name]
            result = bounded_run(command, 10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertEqual(result.stdout.strip(), 'passed '+name)

    def test_main_thread_is_nonblocking_and_duplicate_tabs_share_ttl_cache(self):
        self.run_case('cache', self.state(delay=.25, native={'/dev/ttys9001': {
            'cwd': '/native/project', 'program': 'claude', 'pid': 404, 'shell': 'zsh'}}))

    def test_identical_session_ids_on_different_sockets_do_not_alias(self):
        self.run_case('sockets', self.state(responses={
            key: self.state(fields=['$0', 'fixture', '%0', '/dev/ttys9001', '/'+key, 'codex', '222'])
            for key in ('first', 'second')}))

    def test_active_pane_and_cwd_refresh_after_ttl(self): self.run_case('refresh', self.state())
    def test_failed_refresh_clears_stale_snapshot_and_backs_off(self): self.run_case('failed-refresh', self.state())

    def test_failed_oversize_hung_and_malformed_clients_are_bounded_and_reaped(self):
        for invalid in ({'exit': 1}, {'delay': 30}, {'mode': 'oversize'}, {'mode': 'invalid-utf8'},
                        {'mode': 'unterminated'}, {'fields': ['$1', 'fixture', '%0', 'tty', 'cwd', 'cmd', '2']},
                        {'fields': ['$0', 'fixture', 'bad-pane', 'tty', 'cwd', 'cmd', '2']},
                        {'fields': ['$0', 'fixture', '%0', 'tty', 'ambiguous\x1epath', 'cmd', '2']}):
            with self.subTest(invalid=invalid): self.run_case('unavailable', self.state(**invalid))

    def test_session_and_socket_arguments_are_exact_literal_and_read_only(self):
        self.run_case('arguments', self.state(fields=['$3', 'name $(touch forbidden) ; *', '%4', '/dev/ttys9001', '/repo', 'codex', '222']))

    def test_idle_kernel_shell_and_tmux_shell_fallback_have_no_program(self):
        self.run_case('shell', self.state(native={'/dev/ttys9001': {'cwd': '/native', 'program': '', 'pid': 404, 'shell': 'zsh'}}))
        self.run_case('shell', self.state(fields=['$0', 'fixture', '%0', '/dev/ttys9001', '/pane', 'zsh', '222']))

    def test_unicode_tabs_and_newlines_in_cwd_do_not_break_framing(self):
        self.run_case('path', self.state(fields=['$0', 'fixture', '%0', '/dev/ttys9001', '/repo/café\twork\nother', 'codex', '222']))

    def test_cache_has_bounded_idle_lru_and_invalid_input_does_no_work(self):
        self.run_case('cache-bound', self.state(fields=['$100', 'fixture', '%0', '/dev/ttys9001', '/pane', 'codex', '222']))

    @unittest.skipUnless(shutil.which('tmux'), 'tmux required for owned private-server integration')
    def test_owned_tmux_server_real_foreground_and_active_pane(self):
        with tempfile.TemporaryDirectory(prefix='server-', dir=self.directory) as directory:
            directory = Path(directory)
            first, second = directory/'first project', directory/'second café'
            first.mkdir(); second.mkdir()
            socket = directory/'private.sock'
            tmux = shutil.which('tmux')
            command = [tmux, '-S', str(socket), '-f', '/dev/null']
            env = {**os.environ, 'PATH': str(Path(tmux).parent)+':/usr/bin:/bin'}
            env.pop('TMUX', None); env.pop('TMUX_PANE', None)
            def run(*args):
                return subprocess.run(command+list(args), stdin=subprocess.DEVNULL, capture_output=True,
                                      text=True, timeout=5, env=env)
            started = run('new-session', '-d', '-s', 'metadata fixture', '-c', str(first), '/bin/sleep', '30')
            if started.returncode and 'Operation not permitted' in started.stderr:
                self.skipTest('sandbox does not allow a private tmux server: '+started.stderr.strip())
            self.assertEqual(started.returncode, 0, started.stderr)
            try:
                def query(name, *args):
                    result = bounded_run(['/usr/bin/env', 'PATH='+env['PATH'], str(self.directory/'real'), name, *map(str, args)], 10)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                query('real-query', 'metadata fixture', socket, first)
                split = run('split-window', '-d', '-P', '-F', '#{pane_id}', '-t', '=metadata fixture:', '-c', str(second), '/bin/sleep', '30')
                self.assertEqual(split.returncode, 0, split.stderr)
                selected = run('select-pane', '-t', split.stdout.strip())
                self.assertEqual(selected.returncode, 0, selected.stderr)
                query('real-query', '$0', socket, second)
                query('real-unavailable', 'metadata', socket)
            finally:
                stopped = run('kill-server')
                self.assertEqual(stopped.returncode, 0, stopped.stderr)


if __name__ == '__main__':
    unittest.main()
