#!/usr/bin/env python3
"""Offline checks that Mac slurp distinguishes transfers from bridge views.

Uses production reporting functions with owned tab metadata. No app, Terminal,
PTY, existing process, or user preference is opened or changed.
"""
from pathlib import Path
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct { char run[600]; } Tab;
static Tab fixtures[8], *g_tabs[8];
static int g_ntabs;

/* FUNCTIONS */

static void require(BOOL condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static NSString *report(NSDictionary *result) {
    FILE *file = tmpfile(); require(file != NULL, "create owned output stream");
    mac_slurp_report(result, file); rewind(file);
    char bytes[8192] = {0};
    size_t size = fread(bytes, 1, sizeof(bytes)-1, file);
    require(feof(file), "report exceeded bounded fixture stream");
    fclose(file);
    return [[[NSString alloc] initWithBytes:bytes length:size encoding:NSUTF8StringEncoding] autorelease];
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc == 2, "case required");
        const char *which = argv[1];
        if (!strcmp(which, "counts")) {
            const char *runs[] = {"", "terminal:/dev/ttys003", "$1", "claude-wait:/private/tmp/request", "terminal:/dev/ttys008", "$2", ""};
            g_ntabs = 7;
            for (int i = 0; i < g_ntabs; i++) { g_tabs[i] = fixtures+i; strcpy(fixtures[i].run, runs[i]); }
            NSDictionary *value = mac_slurp_result();
            require([value[@"ok"] boolValue] && [value[@"pid"] intValue] == getpid(), "receiver identity changed");
            require([value[@"nativeTabs"] intValue] == 2, "native PTYs miscounted");
            require([value[@"terminalBridges"] intValue] == 2, "Terminal bridges miscounted");
            require([value[@"tmuxClients"] intValue] == 2, "tmux clients miscounted");
            require([value[@"claudeClients"] intValue] == 1, "Claude attachment miscounted");
        } else if (!strcmp(which, "bridge-report")) {
            NSString *text = report(@{@"pid":@123, @"nativeTabs":@0, @"terminalBridges":@2, @"tmuxClients":@0, @"claudeClients":@0});
            require([text containsString:@"2 Terminal bridge views opened"], "bridge view count absent");
            require([text containsString:@"source processes remain in Terminal"], "bridge described as process transfer");
            require([text containsString:@"Keep the original Terminal tabs open"], "source lifetime missing");
            require([text containsString:@"plain text"] && [text containsString:@"colors and exact cursor position are not preserved"], "bridge fidelity omitted");
            require([text containsString:@"parent shell and Terminal history still remain"], "Claude handoff falsely suggests complete move");
            require(![text containsString:@"transferred with live PTYs"], "bridge falsely reports native transfer");
            require([text hasSuffix:@"hardcore: opened receiving window 123\n"], "final PID compatibility changed");
        } else if (!strcmp(which, "native-report")) {
            NSString *text = report(@{@"pid":@123, @"nativeTabs":@1, @"terminalBridges":@0, @"tmuxClients":@2, @"claudeClients":@1});
            require([text containsString:@"1 native Hardcore tab transferred with live PTYs and terminal state"], "native transfer omitted");
            require([text containsString:@"2 tmux clients opened; sessions remain in their tmux server"], "tmux lifetime omitted");
            require([text containsString:@"1 Claude attachment client opened"], "cooperative attachment omitted");
            require(![text containsString:@"Keep the original Terminal tabs open"], "nonbridge transfer received misleading bridge warning");
            require([text hasSuffix:@"hardcore: opened receiving window 123\n"], "final PID compatibility changed");
        } else if (!strcmp(which, "legacy-report")) {
            NSString *text = report(@{@"ok":@YES, @"pid":@123});
            require([text isEqual:@"hardcore: opened receiving window 123\n"], "legacy result guessed transfer success or counts");
        } else if (!strcmp(which, "labels")) {
            NSDictionary *source = @{@"cwd":@"/Users/wm/Desktop/repo/ruffian", @"program":@"claude", @"shell":@"", @"title":@"unrelated decorative title"};
            require([mac_slurp_terminal_label(source) isEqual:@"ruffian:claude"], "source cwd/program omitted or title trusted");
            require([mac_slurp_terminal_label(@{@"cwd":@"/work/café e\u0301 🚀", @"program":@"", @"shell":@"zsh"}) isEqual:@"café e\u0301 🚀:zsh"], "Unicode or idle shell label damaged");
            require([mac_slurp_terminal_label(@{}) isEqual:@"unknown folder:shell"], "missing cwd silently presented as home folder");
            NSString *clean = mac_slurp_terminal_label(@{@"cwd":@"/work/ru\nffian", @"program":@"cl\033aude"});
            require([clean isEqual:@"ru ffian:claude"], "CLI labels retain terminal controls");
        } else require(NO, "unknown case");
        printf("passed %s\n", which);
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation required')
class SlurpReportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-slurp-report-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        functions = '\n\n'.join([
            production_function(ROOT/'hardcore_mac_dock.m', 'static NSString *hc_dock_label(id value, NSString *fallback)'),
            production_function(ROOT/'hardcore_mac_transfer.m', 'static NSDictionary *mac_slurp_result(void)'),
            production_function(ROOT/'hardcore_mac_transfer.m', 'static NSString *mac_slurp_terminal_label(NSDictionary *source)'),
            production_function(ROOT/'hardcore_mac_transfer.m', 'static void mac_slurp_report(NSDictionary *reply, FILE *out)'),
        ])
        source = directory/'report.m'
        source.write_text(HARNESS.replace('/* FUNCTIONS */', functions))
        cls.binary = directory/'report'
        result = bounded_run(['clang', '-O0', '-Wall', '-Wextra', '-Werror', '-framework', 'Foundation',
                              str(source), '-o', str(cls.binary)], timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr)

    def case(self, name):
        result = bounded_run([str(self.binary), name], timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertEqual(result.stdout.strip(), 'passed '+name)

    def test_actual_receiver_tab_kinds_are_counted(self): self.case('counts')
    def test_bridge_report_identifies_source_lifetime_and_fidelity(self): self.case('bridge-report')
    def test_native_and_tmux_routes_keep_their_distinct_semantics(self): self.case('native-report')
    def test_legacy_receiver_does_not_imply_verified_transfer(self): self.case('legacy-report')
    def test_source_labels_use_metadata_and_escape_terminal_controls(self): self.case('labels')


if __name__ == '__main__':
    unittest.main()
