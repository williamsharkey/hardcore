#!/usr/bin/env python3
"""Source-session label routing without windows, Terminal automation, or PTYs.

Compiles the production refresh/apply/label functions with deterministic source
metadata providers. Real kernel metadata is tested by test_mac_terminal.py.
"""
from pathlib import Path
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#import <Foundation/Foundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { TK_TERMINAL, TK_BROWSER };
typedef struct { char cwd[1024]; int dirty, side, had_activity, nmarks, hcount, cy; long long marks[512]; } Term;
typedef struct {
    Term t;
    char run[1024], hist[1024], start_dir[1024], program[64];
    int kind, fd, exited;
} Tab;
static Tab fixtures[8], *g_tabs[8];
static int g_ntabs, terminal_calls, tmux_calls, local_calls, title_updates, invalidations;
static NSDictionary *terminal_metadata, *tmux_metadata;
static NSString *queried_tty, *queried_session, *queried_socket;
static NSDictionary *mac_terminal_metadata(NSString *tty) {
    terminal_calls++; queried_tty = tty; return terminal_metadata;
}
static NSDictionary *mac_tmux_metadata(NSString *session, NSString *socket) {
    tmux_calls++; queried_session = session; queried_socket = socket; return tmux_metadata;
}
static void mac_foreground_name(Tab *tab, char *out, int capacity) {
    local_calls++;
    snprintf(out, capacity, "%s", tab->exited ? "" : "local-helper");
}
static void mac_browser_tab_label(Tab *tab, wchar_t *out, int capacity, int columns) {
    (void)tab; (void)columns;
    wcsncpy(out, L"Page title", capacity); out[capacity - 1] = 0;
}
static void update_title(void) { title_updates++; }
static void plat_invalidate(void) { invalidations++; }
static long long term_abs_line(const Term *term, int y) { (void)term; return y; }

/* FUNCTIONS */

static void require(BOOL value, const char *message) {
    if (!value) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void equal(id value, id expected, const char *message) {
    if (![value isEqual:expected]) fprintf(stderr, "actual=%s expected=%s\n", [[value description] UTF8String], [[expected description] UTF8String]);
    require([value isEqual:expected], message);
}
static NSString *label(Tab *tab) {
    wchar_t text[256]; tab_label(tab, text, 256);
    return [[[NSString alloc] initWithBytes:text length:wcslen(text) * sizeof(wchar_t)
                                  encoding:NSUTF32LittleEndianStringEncoding] autorelease];
}
static Tab *add_tab(const char *run) {
    Tab *tab = fixtures + g_ntabs;
    snprintf(tab->run, sizeof(tab->run), "%s", run);
    snprintf(tab->start_dir, sizeof(tab->start_dir), "/Users/receiver");
    snprintf(tab->program, sizeof(tab->program), "hardcore");
    tab->fd = 42;
    g_tabs[g_ntabs++] = tab;
    return tab;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc == 2, "case required");
        const char *which = argv[1];
        if (!strcmp(which, "new-and-used")) {
            Tab *tab = add_tab(""); tab->program[0] = 0;
            equal(label(tab), @"receiver:<new>", "fresh shell is not marked new");
            add_mark(&tab->t);
            equal(label(tab), @"receiver:<new>", "first prompt should stay new");
            add_mark(&tab->t);
            require(!tab->t.had_activity, "redrawing the first prompt counts as command activity");
            tab->t.cy = 3; add_mark(&tab->t);
            equal(label(tab), @"receiver:term", "next prompt did not mark used terminal");
            tab->t.nmarks = 0;
            equal(label(tab), @"receiver:term", "clearing prompt marks forgot terminal activity");
            tab->t.had_activity = 0; tab->t.hcount = 2;
            equal(label(tab), @"receiver:term", "restored scrollback was labeled new");
        } else if (!strcmp(which, "terminal-source")) {
            Tab *tab = add_tab("terminal:/dev/ttys123");
            terminal_metadata = @{@"cwd":@"/projects/ruffian", @"program":@"claude", @"title":@"misleading title"};
            mac_refresh_programs();
            equal(queried_tty, @"/dev/ttys123", "queried helper tty instead of source tty");
            equal(@(tab->t.cwd), @"/projects/ruffian", "source cwd ignored");
            equal(@(tab->program), @"claude", "source foreground program ignored");
            equal(label(tab), @"ruffian:claude [bridge]", "visible source label differs");
            require(terminal_calls == 1 && !tmux_calls && !local_calls, "bridge used local helper process metadata");
            require(title_updates == 1 && invalidations == 1 && tab->t.side, "source change not presented");
        } else if (!strcmp(which, "quiet-refresh")) {
            Tab *tab = add_tab("terminal:/dev/ttys123");
            terminal_metadata = @{@"cwd":@"/projects/ruffian", @"program":@"claude"};
            mac_refresh_programs();
            for (int i = 0; i < 100; i++) mac_refresh_programs();
            require(title_updates == 1 && invalidations == 1, "unchanged metadata redraws the app");
            terminal_metadata = @{@"cwd":@"/projects/caf\u00e9 \U0001f680", @"program":@"grok"};
            mac_refresh_programs();
            equal(label(tab), @"caf\u00e9 \U0001f680:grok [bridge]", "quiet cwd/program update or Unicode label lost");
            require(title_updates == 2 && invalidations == 2 && !local_calls, "quiet source update not presented exactly once");
        } else if (!strcmp(which, "idle-shell")) {
            Tab *tab = add_tab("terminal:/dev/ttys123");
            terminal_metadata = @{@"cwd":@"/projects/ruffian", @"program":@"claude"};
            mac_refresh_programs();
            terminal_metadata = @{@"cwd":@"/projects/ruffian", @"program":@"", @"shell":@"zsh"};
            mac_refresh_programs();
            equal(@(tab->program), @"", "finished foreground job retained stale name");
            equal(label(tab), @"ruffian:term [bridge]", "idle source shell shows helper or stale app");
            require(title_updates == 2 && !local_calls, "idle transition was ignored or consulted helper");
        } else if (!strcmp(which, "unavailable-source")) {
            Tab *tab = add_tab("terminal:/dev/ttys123");
            terminal_metadata = @{@"cwd":@"/projects/ruffian", @"program":@"claude"};
            mac_refresh_programs();
            terminal_metadata = @{}; mac_refresh_programs();
            terminal_metadata = nil; mac_refresh_programs();
            equal(label(tab), @"ruffian:claude [bridge]", "temporary source lookup failure replaced known label");
            require(title_updates == 1 && !local_calls, "lookup failure substituted receiver HOME/helper");
            require(!mac_apply_session_metadata(tab, @{@"cwd":@"relative/path", @"program":@42}), "invalid metadata changed source label");
            NSMutableString *longPath = [NSMutableString stringWithString:@"/"];
            for (int i = 0; i < 1100; i++) [longPath appendString:@"x"];
            require(!mac_apply_session_metadata(tab, @{@"cwd":longPath}), "oversized source path was truncated into label");
        } else if (!strcmp(which, "source-routing")) {
            Tab *terminal = add_tab("terminal:/dev/ttys456");
            Tab *tmux = add_tab("$7"); strcpy(tmux->hist, "/private/tmp/owned-tmux.sock");
            Tab *claude = add_tab("claude-wait:/private/tmp/request.json");
            Tab *native = add_tab("");
            Tab *browser = add_tab(""); browser->kind = TK_BROWSER;
            terminal_metadata = @{@"cwd":@"/terminal/project", @"program":@"claude"};
            tmux_metadata = @{@"cwd":@"/tmux/workspace", @"program":@"codex"};
            mac_refresh_programs();
            require(terminal_calls == 1 && tmux_calls == 1 && local_calls == 1, "source-specific metadata dispatch differs");
            equal(queried_session, @"$7", "wrong tmux source session");
            equal(queried_socket, @"/private/tmp/owned-tmux.sock", "wrong tmux source server");
            equal(label(terminal), @"project:claude [bridge]", "Terminal source label differs");
            equal(label(tmux), @"workspace:codex", "tmux source label differs");
            equal(@(claude->program), @"claude", "Claude attachment classified as local helper");
            equal(@(native->program), @"local-helper", "native tab skipped actual foreground lookup");
            equal(label(browser), @"Page title", "browser label used terminal metadata");
            equal(@(browser->program), @"hardcore", "browser polled foreground process metadata");
        } else if (!strcmp(which, "exited-source")) {
            Tab *tab = add_tab("terminal:/dev/ttys789"); tab->exited = 1;
            terminal_metadata = @{@"cwd":@"/reused/source", @"program":@"unrelated"};
            mac_refresh_programs();
            require(!terminal_calls && !tmux_calls && local_calls == 1, "exited bridge queried a potentially reused source tty");
            equal(@(tab->program), @"", "exited tab retained helper process name");
        } else require(NO, "unknown test case");
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation required')
class SourceSessionLabelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-session-labels-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        functions = '\n\n'.join([
            production_function(ROOT/'hardcore.c', 'static void utf8_to_wide(const char *s, wchar_t *out, int cap)'),
            production_function(ROOT/'hardcore.c', 'static void set_cwd(Term *t, const char *p)'),
            production_function(ROOT/'hardcore.c', 'static void tab_label_fit(Tab *tb, wchar_t *out, int cap, int cols)'),
            production_function(ROOT/'hardcore.c', 'static void tab_label(Tab *tb, wchar_t *out, int cap)'),
            production_function(ROOT/'hardcore_mac.m', 'static int mac_apply_session_metadata(Tab *tb, NSDictionary *metadata)'),
            production_function(ROOT/'hardcore_mac.m', 'static void mac_refresh_programs(void)'),
            production_function(ROOT/'hardcore.c', 'static void add_mark(Term *t)'),
        ])
        harness = directory/'labels.m'
        harness.write_text(HARNESS.replace('/* FUNCTIONS */', functions))
        cls.binary = directory/'labels'
        result = bounded_run(['clang', '-Wall', '-Wextra', '-Werror', '-mmacosx-version-min=11.0',
                              str(harness), '-o', str(cls.binary), '-framework', 'Foundation'], 30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, case):
        result = bounded_run([str(self.binary), case], 10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_terminal_uses_original_source_metadata(self):
        self.run_case('terminal-source')

    def test_fresh_and_used_terminal_labels_survive_prompt_redraw_and_clear(self):
        self.run_case('new-and-used')

    def test_quiet_changes_update_without_redundant_redraws(self):
        self.run_case('quiet-refresh')

    def test_idle_shell_clears_finished_program(self):
        self.run_case('idle-shell')

    def test_temporary_lookup_failure_keeps_known_source(self):
        self.run_case('unavailable-source')

    def test_terminal_tmux_claude_and_native_routes(self):
        self.run_case('source-routing')

    def test_exited_bridge_does_not_query_reused_source(self):
        self.run_case('exited-source')


if __name__ == '__main__':
    unittest.main()
