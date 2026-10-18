#!/usr/bin/env python3
"""Refresh owned bridge helpers on real PTYs; never access Terminal or user tabs."""
from pathlib import Path
import sys
import tempfile
import unittest

from test_mac_import_resize import bounded_run, production_function

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <util.h>
#include <os/lock.h>
#define CW 12
#define CH 26
typedef unsigned DWORD;
enum { TK_TERMINAL, TK_BROWSER };
typedef struct { int cols, rows, marker, nmarks, had_activity; } Term;
typedef struct {
    Term t; int id, kind, parent_id, fd, exited, born; pid_t pid;
    void *rsrc, *psrc, *wq, *pin; int *wpending;
    char *in; int in_n, in_cap;
    unsigned long long write_seq;
    char run[600], hist[400];
} Tab;
static Tab *g_tabs[4], *g_spare;
static int g_ntabs, g_next_id = 10, unwanted_exit;
static int g_lat;
static double g_lat_written;
static char g_mac_exe_path[4096];
static DWORD GetTickCount(void) { return 12; }
static double uptime_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static Tab *tab_by_id(int id) {
    for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->id == id) return g_tabs[i];
    return NULL;
}
static int mac_input_held(Tab *tb) { (void)tb; return 0; }
static void child_env(void) {}
static void update_title(void) {}
static void plat_invalidate(void) {}
static void schedule_output(void) {}
static void read_some(Tab *tb) {
    char buffer[4096];
    while (read(tb->fd, buffer, sizeof(buffer)) > 0) {}
}
static int tab_drain(Tab *tb) { (void)tb; return 0; }
static void on_tab_exited(int id, int code) {
    (void)code;
    if (tab_by_id(id)) unwanted_exit++;
}
/* IO */
/* REFRESH */
static void require(int value, const char *message) {
    if (!value) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static Tab *owned_tab(int bridge) {
    Tab *tb = calloc(1, sizeof(Tab));
    tb->id = g_next_id++; tb->t.cols = 80; tb->t.rows = 24; tb->t.marker = 12345;
    snprintf(tb->run, sizeof(tb->run), "%s", bridge ? "terminal:/dev/owned-source" : "");
    snprintf(tb->hist, sizeof(tb->hist), "good");
    tb->pid = forkpty(&tb->fd, NULL, NULL, NULL);
    require(tb->pid >= 0, "fixture forkpty");
    if (!tb->pid) { while (1) pause(); }
    g_tabs[g_ntabs++] = tb;
    tab_start_io(tb);
    return tb;
}
static Tab *owned_browser(int parent) {
    Tab *tb = calloc(1, sizeof(Tab));
    tb->id = g_next_id++; tb->kind = TK_BROWSER; tb->parent_id = parent;
    tb->fd = -1; tb->t.marker = 45678;
    g_tabs[g_ntabs++] = tb;
    return tb;
}
static void pump(void) {
    for (int i = 0; i < 30; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, false);
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 4 && !strcmp(argv[1], "--terminal-bridge")) {
        int ready = atoi(getenv("HARDCORE_BRIDGE_READY_FD"));
        if (!strcmp(argv[3], "fail")) return 7;
        if (!strcmp(argv[3], "timeout")) { sleep(20); return 8; }
        require(write(ready, "R", 1) == 1, "fixture ready");
        char byte = 0;
        require(read(ready, &byte, 1) == 1 && byte == 'G', "fixture GO");
        close(ready);
        while (1) pause();
    }
    @autoreleasepool {
        require(argc == 2, "case required");
        realpath(argv[0], g_mac_exe_path);
        Tab *tb = owned_tab(1), *native = owned_tab(0);
        pid_t original = tb->pid, native_pid = native->pid;
        int original_id = tb->id, original_fd = tb->fd, native_fd = native->fd;
        Tab *browser = owned_browser(original_id), *unrelated = owned_browser(native->id);
        int browser_id = browser->id, unrelated_id = unrelated->id, committed = 0;
        MacBridgeRefresh r;
        if (!strcmp(argv[1], "native")) {
            require(!mac_bridge_refresh_stage(native_pid, &r), "native shell was restartable");
        } else if (!strcmp(argv[1], "browser")) {
            require(!mac_bridge_refresh_stage(browser->pid, &r), "browser tab was restartable");
        } else if (!strcmp(argv[1], "pending")) {
            *tb->wpending = 1;
            require(!mac_bridge_refresh_stage(original, &r), "pending input was discarded");
            *tb->wpending = 0;
        } else if (!strcmp(argv[1], "wrong-pid")) {
            require(!mac_bridge_refresh_stage(getpid(), &r), "unrelated PID was restartable");
        } else {
            if (!strcmp(argv[1], "failed")) snprintf(tb->hist, sizeof(tb->hist), "fail");
            if (!strcmp(argv[1], "timeout")) snprintf(tb->hist, sizeof(tb->hist), "timeout");
            require(mac_bridge_refresh_stage(original, &r), "owned bridge stage failed");
            pid_t candidate = r.pid;
            int ready = mac_bridge_refresh_wait(&r);
            if (!strcmp(argv[1], "failed") || !strcmp(argv[1], "timeout")) {
                require(!ready, "bad candidate became ready");
                mac_bridge_refresh_discard(&r);
                require(kill(candidate, 0) < 0 && errno == ESRCH, "failed candidate remains alive");
                require(tb->id == original_id && tb->pid == original && tb->fd == original_fd,
                        "failed candidate changed original transport");
                require(!kill(original, 0), "original helper died on failure");
            } else {
                require(ready, "candidate failed readiness");
                if (!strcmp(argv[1], "changed") || !strcmp(argv[1], "new-input") || !strcmp(argv[1], "resized")) {
                    if (!strcmp(argv[1], "changed")) tb->id = g_next_id++;
                    else if (!strcmp(argv[1], "resized")) tb->t.cols++;
                    else {
                        tab_write(tb, "typed", 5);
                        require(!*tb->wpending, "fixture should exercise immediately written input");
                    }
                    require(!mac_bridge_refresh_commit(&r), "changed source was committed");
                    mac_bridge_refresh_discard(&r);
                    require(tb->pid == original && tb->fd == original_fd && !kill(original, 0),
                            "changed source was retired");
                } else {
                    require(mac_bridge_refresh_commit(&r), "ready bridge commit failed");
                    committed = 1;
                    mac_bridge_refresh_discard(&r);
                    pump();
                    require(tb->pid == candidate && tb->id != original_id && tb->fd != original_fd,
                            "new transport was not adopted");
                    require(!kill(candidate, 0), "new helper died");
                    require(kill(original, 0) < 0 && errno == ESRCH, "old helper remains alive");
                    require(waitpid(original, NULL, WNOHANG) < 0 && errno == ECHILD, "old helper not reaped");
                    require(!unwanted_exit && !tb->exited, "stale exit callback closed new tab");
                    require(tb->t.marker == 12345 && g_tabs[0] == tb && g_ntabs == 4,
                            "cells, tab position, or tab count changed");
                }
            }
        }
        require(native->pid == native_pid && native->fd == native_fd && !kill(native_pid, 0),
                "native process/PTY changed");
        require(browser->parent_id == (committed ? tb->id : original_id),
                "browser child lost its owner or changed owner before commit");
        require(unrelated->parent_id == native->id, "unrelated browser parent changed");
        require(browser->id == browser_id && unrelated->id == unrelated_id &&
                browser->fd == -1 && unrelated->fd == -1 && !browser->pid && !unrelated->pid &&
                browser->t.marker == 45678 && unrelated->t.marker == 45678 &&
                g_tabs[2] == browser && g_tabs[3] == unrelated && g_ntabs == 4,
                "browser identity, contents, transport or tab position changed");
        for (int i = 0; i < g_ntabs; i++) if (g_tabs[i]->pid > 0) {
            kill(g_tabs[i]->pid, SIGKILL);
            waitpid(g_tabs[i]->pid, NULL, 0);
        }
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'requires macOS PTYs and dispatch')
class BridgeRefreshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix='hc-bridge-refresh-')
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        mac = (ROOT/'hardcore_mac.m').read_text()
        reader = mac[mac.index('enum { PIN_FULL'):mac.index('/* Stops reading; the fd closes')]   # PtyIn and its helpers
        code = HARNESS.replace('/* IO */', '\n'.join([
            reader,
            production_function(ROOT/'hardcore_mac.m', 'static void stop_reading(Tab *tb)'),
            production_function(ROOT/'hardcore_mac.m', 'static void tab_start_io(Tab *tb)'),
            production_function(ROOT/'hardcore_mac.m', 'static void tab_write(Tab *tb, const void *s, int n)'),
        ])).replace('/* REFRESH */', (ROOT/'hardcore_mac_bridge_refresh.m').read_text())
        source = directory/'refresh.m'
        source.write_text(code)
        cls.binary = directory/'refresh'
        result = bounded_run(['clang', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                              '-mmacosx-version-min=11.0', str(source), '-o', str(cls.binary),
                              '-framework', 'Foundation', '-lutil'], 30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def case(self, name):
        result = bounded_run([str(self.binary), name], 12)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_ready_replacement_preserves_cells_native_pty_and_stale_exit_safety(self): self.case('success')
    def test_failed_replacement_keeps_original(self): self.case('failed')
    def test_timeout_replacement_keeps_original(self): self.case('timeout')
    def test_changed_tab_cannot_be_committed(self): self.case('changed')
    def test_input_written_during_staging_keeps_original(self): self.case('new-input')
    def test_resize_during_staging_keeps_original(self): self.case('resized')
    def test_native_shell_refused(self): self.case('native')
    def test_browser_tab_refused(self): self.case('browser')
    def test_pending_input_refused(self): self.case('pending')
    def test_wrong_pid_refused(self): self.case('wrong-pid')


if __name__ == '__main__':
    unittest.main()
