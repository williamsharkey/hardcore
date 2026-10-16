#!/usr/bin/env python3
"""Headless macOS import regression: owned PTYs, no shells or terminal UI."""
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def production_function(path, signature):
    """Extract a top-level production function, retaining its actual body."""
    source = path.read_text()
    match = re.search(r'^' + re.escape(signature) + r' \{[^\n]*\}$', source, re.MULTILINE)
    if match is not None:
        return match.group(0)
    match = re.search(r'^' + re.escape(signature) + r' \{\n.*?^\}',
                      source, re.MULTILINE | re.DOTALL)
    if match is None:
        raise AssertionError(f'production function not found: {signature}')
    return match.group(0)


def bounded_run(command, timeout):
    """No terminal stdin; timeout cleanup is confined to our child group."""
    child = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, start_new_session=True)
    try:
        stdout, stderr = child.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        child.communicate(timeout=5)
        raise
    if len(stdout) + len(stderr) > 1024 * 1024:
        raise AssertionError('unexpected compiler/harness output volume')
    return subprocess.CompletedProcess(command, child.returncode, stdout.decode(errors='replace'),
                                       stderr.decode(errors='replace'))


HARNESS = r'''
#import <Foundation/Foundation.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <util.h>

#define CW 9
#define CH 16
#define MAX_FIXTURE_TABS 4
typedef struct { int cols, rows; } Term;
enum { TK_TERMINAL, TK_BROWSER };
typedef struct { Term t; int kind, fd, exited, index; void *wq; } Tab;
static Tab fixtures[MAX_FIXTURE_TABS], *g_tabs[MAX_FIXTURE_TABS];
static NSMutableArray *g_mac_imports;
static int g_handoff_paint, g_handoff_painted, paint_ok = 1, paint_calls;
static int donor[MAX_FIXTURE_TABS], slave_fd[MAX_FIXTURE_TABS], started[MAX_FIXTURE_TABS];
static int tab_group[MAX_FIXTURE_TABS], fixture_count, group_count, send_calls, recv_calls;
static struct { int socket, peer, ready_ok, go_ok, go_seen; } groups[MAX_FIXTURE_TABS];

static void require(int condition, const char *message) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static void mac_browser_resize(Tab *tab) {
    (void)tab; require(0, "terminal PTY resize routed to browser");
}

static void require_size(int index, int received) {
    struct winsize ws = {0};
    require(ioctl(donor[index], TIOCGWINSZ, &ws) == 0, "read owned PTY dimensions");
    int cols = received ? fixtures[index].t.cols : 80 + index;
    int rows = received ? fixtures[index].t.rows : 20 + index;
    require(ws.ws_col == cols && ws.ws_row == rows,
            received ? "committed PTY differs from current Term dimensions" :
                       "donor PTY changed before ownership committed");
    require(ws.ws_xpixel == (received ? cols * CW : 0) &&
            ws.ws_ypixel == (received ? rows * CH : 0), "PTY pixel dimensions differ");
}

static void require_unchanged_uncommitted(void) {
    for (int i = 0; i < fixture_count; i++) if (!started[i]) require_size(i, 0);
}

static void paint_view_now(void) {
    paint_calls++;
    require(g_handoff_paint == 1, "initial paint must be a handoff paint");
    require_unchanged_uncommitted();
    g_handoff_painted = paint_ok;
}

static int group_for_socket(int fd) {
    for (int i = 0; i < group_count; i++) if (groups[i].socket == fd) return i;
    require(0, "unexpected handoff socket");
    return -1;
}

static int mac_send(int fd, NSDictionary *message) {
    send_calls++;
    require([message[@"ready"] boolValue], "READY must precede commit");
    require_unchanged_uncommitted();
    return groups[group_for_socket(fd)].ready_ok;
}

static NSDictionary *mac_recv(int fd) {
    recv_calls++;
    require_unchanged_uncommitted();
    int group = group_for_socket(fd);
    groups[group].go_seen = groups[group].go_ok;
    return @{@"go": @(groups[group].go_ok)};
}

static void tab_start_io(Tab *tab) {
    require(groups[tab_group[tab->index]].go_seen, "I/O started without GO");
    require_size(tab->index, 0);
    require(!started[tab->index] && tab->wq == NULL, "I/O started twice");
    started[tab->index] = 1;
    tab->wq = tab;  /* Only this queue-presence guard is used by tab_set_size. */
}

static void term_message(Term *term, const char *message) {
    (void)term;
    require(strstr(message, "Transfer cancelled") != NULL, "missing cancellation notice");
}

/* PRODUCTION_FUNCTIONS */

static void add_group(int first, int count, int ready_ok, int go_ok) {
    int pair[2];
    require(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "create owned handoff pair");
    int group = group_count++;
    groups[group].socket = pair[0]; groups[group].peer = pair[1];
    groups[group].ready_ok = ready_ok; groups[group].go_ok = go_ok;
    for (int i = first; i < first + count; i++) tab_group[i] = group;
    [g_mac_imports addObject:@{@"socket": @(pair[0]), @"first": @(first), @"count": @(count)}];
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc == 2, "one fixture case required");
        const char *which = argv[1];
        fixture_count = !strcmp(which, "mixed") ? 4 : !strcmp(which, "multiple") ? 3 : 1;
        g_mac_imports = [[NSMutableArray alloc] init];
        for (int i = 0; i < fixture_count; i++) {
            struct winsize ws = {0};
            ws.ws_col = 80 + i; ws.ws_row = 20 + i;
            require(openpty(&donor[i], &slave_fd[i], NULL, NULL, &ws) == 0, "create owned PTY");
            fixtures[i].fd = dup(donor[i]);
            require(fixtures[i].fd >= 0, "duplicate receiver master");
            fixtures[i].index = i;
            fixtures[i].t.cols = 105 + i; fixtures[i].t.rows = 43 + i;
            g_tabs[i] = &fixtures[i];
            /* Window layout runs while the staged receiver has no queue. */
            tab_set_size(g_tabs[i], fixtures[i].t.cols, fixtures[i].t.rows);
            require_size(i, 0);
            /* Commit must use the current grid, not the first staged grid. */
            fixtures[i].t.cols += 2; fixtures[i].t.rows += 3;
        }
        int expected_commit = 1;
        if (!strcmp(which, "staged")) {
            expected_commit = -1;
        } else if (!strcmp(which, "paint-failure")) {
            paint_ok = 0; expected_commit = 0; add_group(0, 1, 1, 1);
        } else if (!strcmp(which, "ready-failure")) {
            expected_commit = 0; add_group(0, 1, 0, 1);
        } else if (!strcmp(which, "go-failure")) {
            expected_commit = 0; add_group(0, 1, 1, 0);
        } else if (!strcmp(which, "multiple")) {
            add_group(0, 2, 1, 1); add_group(2, 1, 1, 1);
        } else if (!strcmp(which, "mixed")) {
            add_group(0, 2, 1, 0); add_group(2, 1, 1, 1); add_group(3, 1, 0, 1);
        } else {
            require(!strcmp(which, "success"), "unknown fixture case");
            add_group(0, 1, 1, 1);
        }
        if (expected_commit >= 0) {
            require(mac_commit_imports() == expected_commit, "unexpected commit result");
            require(g_handoff_paint == 0 && paint_calls == 1, "paint flag not restored");
            require(send_calls == (paint_ok ? group_count : 0), "unexpected READY attempts");
            int expected_recv = 0;
            for (int group = 0; group < group_count; group++) expected_recv += groups[group].ready_ok;
            require(recv_calls == (paint_ok ? expected_recv : 0), "unexpected GO reads");
        }
        for (int i = 0; i < fixture_count; i++) {
            int accepted = expected_commit >= 0 && paint_ok &&
                groups[tab_group[i]].ready_ok && groups[tab_group[i]].go_ok;
            require(started[i] == accepted, "unexpected I/O ownership");
            require_size(i, accepted);
            if (expected_commit >= 0 && paint_ok && !accepted) {
                require(fixtures[i].fd == -1 && fixtures[i].exited,
                        "cancelled receiver duplicate not retired");
            }
            if (fixtures[i].fd >= 0) close(fixtures[i].fd);
            close(donor[i]); close(slave_fd[i]);
        }
        for (int group = 0; group < group_count; group++) {
            /* Commit closes these; a paint failure retains staged imports. */
            if (!paint_ok) close(groups[group].socket);
            close(groups[group].peer);
        }
        [g_mac_imports release]; g_mac_imports = nil;
        printf("passed %s\n", which);
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation and macOS owned PTYs required')
class ImportResizeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-import-resize-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        os.chmod(cls.root, 0o700)
        resize = production_function(ROOT / 'hardcore_mac.m', 'static void tab_set_size(Tab *tb, int cols, int rows)')
        commit = production_function(ROOT / 'hardcore_mac_transfer.m', 'static int mac_commit_imports(void)')
        resize_call = 'tab_set_size(tb, tb->t.cols, tb->t.rows);'
        if commit.count(resize_call) != 1:
            raise AssertionError('expected exactly one post-commit PTY resize call')
        cls.binaries = {}
        for name, body in (('production', commit), ('missing-resize', commit.replace(resize_call, '(void)tb;'))):
            source, binary = cls.root / (name + '.m'), cls.root / name
            source.write_text(HARNESS.replace('/* PRODUCTION_FUNCTIONS */', resize + '\n\n' + body))
            os.chmod(source, 0o600)
            result = bounded_run(['clang', '-O0', '-Wall', '-Wextra', '-Werror', '-framework',
                                  'Foundation', str(source), '-o', str(binary)], timeout=60)
            if result.returncode:
                raise AssertionError('headless harness compile failed:\n' + result.stderr[:12000])
            cls.binaries[name] = binary

    def assert_case(self, case):
        result = bounded_run([str(self.binaries['production']), case], timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'passed ' + case)

    def test_staged_layout_does_not_resize_donor(self):
        self.assert_case('staged')

    def test_paint_failure_preserves_donor_size(self):
        self.assert_case('paint-failure')

    def test_ready_failure_preserves_donor_size(self):
        self.assert_case('ready-failure')

    def test_go_failure_preserves_donor_size(self):
        self.assert_case('go-failure')

    def test_go_syncs_current_grid_and_pixels(self):
        self.assert_case('success')

    def test_multiple_tabs_and_groups_each_sync_their_grid(self):
        self.assert_case('multiple')

    def test_mixed_groups_resize_only_committed_tabs(self):
        self.assert_case('mixed')

    def test_negative_control_without_resize_fails_real_kernel_check(self):
        result = bounded_run([str(self.binaries['missing-resize']), 'success'], timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('committed PTY differs from current Term dimensions', result.stderr)


if __name__ == '__main__':
    unittest.main()
