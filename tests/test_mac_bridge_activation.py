#!/usr/bin/env python3
"""Verify a bundled Terminal bridge stays out of the Dock using real macOS policy.

Only disposable fixture processes run. They evaluate a constant AppleScript;
no Terminal app, existing session, Accessibility input, or user files are used.
The regular-app control briefly registers its own private fixture in the Dock.
"""
import json
import os
from pathlib import Path
import plistlib
import socket
import subprocess
import sys
import tempfile
import time
import unittest

from test_mac_import_resize import production_function


ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int force_failure;
static OSStatus transform_for_test(const ProcessSerialNumber *self,
                                   ProcessApplicationTransformState state) {
    return force_failure ? paramErr : TransformProcessType(self, state);
}
#define TransformProcessType transform_for_test
/* BACKGROUND_ROLE */
#undef TransformProcessType
/* READINESS_FD */
/* READINESS_COMMIT */

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 90;
        signal(SIGPIPE, SIG_IGN);
        if (!strcmp(argv[1], "ready")) {
            int fd = mac_terminal_bridge_ready_fd();
            if (getenv("HARDCORE_BRIDGE_READY_FD")) return 97;
            if (fd < 0) return fd == -2 ? 2 : 3;
            if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) return 98;
            int committed = mac_terminal_bridge_ready(fd);
            if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) return 99;
            puts(committed ? "committed" : "refused");
            return committed ? 0 : 1;
        }
        BOOL background = !strcmp(argv[1], "background");
        if (!strcmp(argv[1], "failure")) {
            force_failure = 1;
            return mac_terminal_bridge_background() ? 91 : 0;
        }
        if (!background && strcmp(argv[1], "regular")) return 92;
        if (background && !mac_terminal_bridge_background()) return 93;
        if (!background) {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        }

        /* The bridge uses these frameworks before ever creating NSApplication.
           A harmless local script exercises that registration path without
           asking for Automation consent or contacting another application. */
        [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.example.hardcore.absent-test-app"];
        NSAppleScript *script = [[[NSAppleScript alloc] initWithSource:@"return 42"] autorelease];
        NSDictionary *error = nil;
        NSAppleEventDescriptor *result = [script executeAndReturnError:&error];
        if (!result || result.int32Value != 42) return 95;
        CGEventRef event = CGEventCreateKeyboardEvent(NULL, 0, true);
        if (!event) return 96;
        CFRelease(event); /* Never post it. */
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        NSRunningApplication *app = [NSRunningApplication currentApplication];
        NSDictionary *report = @{@"policy":@(app.activationPolicy),
            @"bundle":app.bundleIdentifier ?: @"",
            @"applicationCreated":@(NSApp != nil),
            @"windows":@(NSApp.windows.count), @"script":@(result.int32Value)};
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
        fwrite(json.bytes, 1, json.length, stdout);
        putchar('\n');
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'macOS activation policy')
class BridgeActivationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-bridge-activation-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        cls.bundle_id = 'com.example.hardcore.bridge-policy.' + root.name
        contents = root/'Bridge Policy Fixture.app'/'Contents'
        (contents/'MacOS').mkdir(parents=True)
        (contents/'Info.plist').write_bytes(plistlib.dumps({
            'CFBundleName': 'Hardcore Bridge Policy Test',
            'CFBundleIdentifier': cls.bundle_id,
            'CFBundleExecutable': 'fixture',
            'CFBundlePackageType': 'APPL',
            'CFBundleVersion': '1',
            'LSMinimumSystemVersion': '11.0',
        }))
        source = root/'fixture.m'
        code = HARNESS
        for marker, signature in (
            ('BACKGROUND_ROLE', 'static int mac_terminal_bridge_background(void)'),
            ('READINESS_FD', 'static int mac_terminal_bridge_ready_fd(void)'),
            ('READINESS_COMMIT', 'static int mac_terminal_bridge_ready(int fd)'),
        ):
            code = code.replace('/* ' + marker + ' */', production_function(ROOT/'hardcore_mac_terminal.m', signature))
        source.write_text(code)
        cls.binary = contents/'MacOS'/'fixture'
        result = subprocess.run([
            'clang', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations',
            '-mmacosx-version-min=11.0', '-arch', 'arm64', '-arch', 'x86_64',
            str(source), '-framework', 'Cocoa', '-framework', 'ApplicationServices',
            '-o', str(cls.binary),
        ], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def fixture_environment(self):
        # Keep the real login-session identity so Launch Services is exercised,
        # but do not pass arbitrary launcher/agent settings to the fixture.
        env = {key: os.environ[key] for key in ('HOME', 'USER', 'LOGNAME', 'TMPDIR') if key in os.environ}
        env['PATH'] = '/usr/bin:/bin'
        return env

    def run_fixture(self, mode):
        env = self.fixture_environment()
        result = subprocess.run([str(self.binary), mode], env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_background_bridge_remains_prohibited_after_framework_use(self):
        report = json.loads(self.run_fixture('background').stdout)
        self.assertEqual(report['bundle'], self.bundle_id)
        self.assertEqual(report['policy'], 2)  # NSApplicationActivationPolicyProhibited
        self.assertFalse(report['applicationCreated'])
        self.assertEqual(report['windows'], 0)
        self.assertEqual(report['script'], 42)

    def test_regular_window_process_keeps_normal_dock_role(self):
        report = json.loads(self.run_fixture('regular').stdout)
        self.assertEqual(report['bundle'], self.bundle_id)
        self.assertEqual(report['policy'], 0)  # NSApplicationActivationPolicyRegular
        self.assertTrue(report['applicationCreated'])

    def test_failed_background_transition_is_reported_and_refused(self):
        result = self.run_fixture('failure')
        self.assertEqual(result.stdout, '')
        self.assertIn('cannot start Terminal bridge in the background', result.stderr)
        self.assertIn('macOS error -50', result.stderr)

    def test_bridge_enters_background_before_discovery_or_input(self):
        body = production_function(ROOT/'hardcore_mac_terminal.m',
                                   'static int mac_terminal_bridge(const char *device, const char *window_arg)')
        self.assertLess(body.index('mac_terminal_bridge_background()'), body.index('NSString *tty'))
        self.assertLess(body.index('if (!screen)'), body.index('mac_terminal_bridge_ready(ready_fd)'))
        self.assertLess(body.index('mac_terminal_bridge_ready(ready_fd)'), body.index('mac_terminal_input('))

    def run_handshake(self, reply, timeout=5):
        parent, child = socket.socketpair()
        with parent, child:
            parent.settimeout(5)
            env = self.fixture_environment()
            env['HARDCORE_BRIDGE_READY_FD'] = str(child.fileno())
            process = subprocess.Popen([str(self.binary), 'ready'], env=env,
                                       pass_fds=(child.fileno(),), stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            try:
                child.close()
                self.assertEqual(parent.recv(1), b'R')
                self.assertIsNone(process.poll(), 'candidate bypassed parent commit')
                if reply == 'close':
                    parent.close()
                elif reply is not None:
                    parent.sendall(reply)
                output, error = process.communicate(timeout=timeout)
                return process.returncode, output, error
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)

    def test_replacement_waits_for_exact_commit_and_closes_socket(self):
        code, output, error = self.run_handshake(b'G')
        self.assertEqual((code, output, error), (0, 'committed\n', ''))

    def test_replacement_rejects_cancelled_or_disconnected_controller(self):
        for reply in (b'X', 'close'):
            with self.subTest(reply=reply):
                code, output, error = self.run_handshake(reply)
                self.assertEqual((code, output), (1, 'refused\n'))
                self.assertIn('replacement was not committed', error)

    def test_replacement_commit_wait_is_bounded(self):
        start = time.monotonic()
        code, output, error = self.run_handshake(None, timeout=12)
        self.assertEqual((code, output), (1, 'refused\n'))
        self.assertIn('replacement was not committed', error)
        self.assertGreaterEqual(time.monotonic() - start, 9)
        self.assertLess(time.monotonic() - start, 13)

    def test_invalid_readiness_descriptor_never_becomes_a_bridge(self):
        for value in ('', '0', '2', '-1', '999999', 'junk', '99999999999999999999999'):
            with self.subTest(value=value):
                env = self.fixture_environment()
                env['HARDCORE_BRIDGE_READY_FD'] = value
                result = subprocess.run([str(self.binary), 'ready'], env=env,
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('invalid Terminal bridge readiness socket', result.stderr)

    def test_readiness_requires_socket_and_is_optional(self):
        with tempfile.TemporaryFile(dir=self.temp.name) as ordinary_file:
            env = self.fixture_environment()
            env['HARDCORE_BRIDGE_READY_FD'] = str(ordinary_file.fileno())
            result = subprocess.run([str(self.binary), 'ready'], env=env,
                                    pass_fds=(ordinary_file.fileno(),),
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn('invalid Terminal bridge readiness socket', result.stderr)
        result = subprocess.run([str(self.binary), 'ready'], env=self.fixture_environment(),
                                capture_output=True, text=True, timeout=5)
        self.assertEqual((result.returncode, result.stdout, result.stderr), (3, '', ''))


if __name__ == '__main__':
    unittest.main()
