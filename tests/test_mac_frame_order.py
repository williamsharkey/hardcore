#!/usr/bin/env python3
"""Headless native merge regressions: frame ordering, UTF-8 paths, owned-child reaping.

Uses production functions with a fake renderer and fake tab spawn; no GUI,
clipboard, shell, or pre-existing process is accessed.
"""
import os
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
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct { int nmarks; } Term;
typedef struct { Term t; int fd, id; char run[600], hist[400], start_dir[1024]; } Tab;
typedef struct Frame {
    int snap, handoff, painted, trace_tab;
    uint64_t serial;
    double lat_key, lat_out, lat_down, lat_written;
} Frame;
static pthread_mutex_t g_draw_mx = PTHREAD_MUTEX_INITIALIZER, g_fmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;
static uint64_t g_frame_serial, g_last_draw_serial;
static Frame *g_build, *g_pending;
static int g_handoff_paint, g_handoff_painted;
static int g_lat, g_trace_commit, g_ntabs;
static double g_lat_key, g_lat_out, g_lat_down, g_lat_written;
static int g_cols = 80, g_rows = 24, g_next_id;
static int rendered_count, spawn_count, render_ok = 1, active_draws, peak_draws;
static uint64_t rendered[32];
static Tab dummy_tab;

static void require(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void r_flush(void) {}
static Tab *active_tab(void) { return &dummy_tab; }
static void trace(const char *s) { (void)s; }
static void frame_free(Frame *frame) { free(frame); }
static int term_init(Term *t, int cols, int rows) { (void)t; (void)cols; (void)rows; return 1; }
static void term_free(Term *t) { (void)t; }
static int tab_spawn(Tab *tab) { (void)tab; spawn_count++; return 1; }

static void draw_frame_contents(Frame *f) {
    int active = __atomic_add_fetch(&active_draws, 1, __ATOMIC_SEQ_CST);
    int peak = __atomic_load_n(&peak_draws, __ATOMIC_SEQ_CST);
    while (active > peak && !__atomic_compare_exchange_n(&peak_draws, &peak, active, 0,
           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {}
    usleep(1000);
    rendered[rendered_count++] = f->serial;
    if (f->handoff) f->painted = render_ok;
    /* Simulate the stale global completion left by a different rendered frame.
       A failed receipt must take its result from the requested frame itself. */
    if (!render_ok) g_handoff_painted = 1;
    __atomic_sub_fetch(&active_draws, 1, __ATOMIC_SEQ_CST);
}

/* PRODUCTION_FUNCTIONS */

static void *worker(void *arg) { draw_frame(arg); return NULL; }

int main(int argc, const char **argv) {
    @autoreleasepool {
        require(argc == 2, "case required");
        const char *which = argv[1];
        if (!strcmp(which, "stale-frame")) {
            Frame old = {.serial=1}, receipt = {.serial=2, .handoff=1};
            draw_frame(&receipt); draw_frame(&old);
            require(receipt.painted, "receipt was not rendered");
            require(rendered_count == 1 && rendered[0] == 2, "stale worker frame rendered after receipt");
        } else if (!strcmp(which, "failed-receipt")) {
            render_ok = 0; g_handoff_paint = 1;
            g_build = calloc(1, sizeof(Frame));
            r_present();
            require(!g_handoff_painted, "unrelated frame falsely certified failed receipt");
        } else if (!strcmp(which, "successful-receipt")) {
            g_handoff_paint = 1; g_build = calloc(1, sizeof(Frame));
            r_present();
            require(g_handoff_painted && rendered_count == 1, "successful receipt not acknowledged");
        } else if (!strcmp(which, "worker-order")) {
            pthread_t threads[8]; Frame frames[8] = {0};
            for (int i=0; i<8; i++) { frames[i].serial = i+1; require(!pthread_create(&threads[i], NULL, worker, &frames[i]), "create worker"); }
            for (int i=0; i<8; i++) pthread_join(threads[i], NULL);
            require(peak_draws == 1, "draw encoders overlapped");
            for (int i=1; i<rendered_count; i++) require(rendered[i] > rendered[i-1], "frames presented out of submission order");
        } else if (!strcmp(which, "unicode-paths")) {
            NSString *over = [@"" stringByPaddingToLength:200 withString:@"界" startingAtIndex:0];
            require(!mac_tmux_tab(over, nil) && !spawn_count, "600-byte Unicode request was truncated and spawned");
            NSString *limit = [[@"" stringByPaddingToLength:199 withString:@"界" startingAtIndex:0] stringByAppendingString:@"ab"];
            Tab *tab = mac_tmux_tab(limit, nil);
            require(tab && !strcmp(tab->run, limit.UTF8String) && strlen(tab->run) == 599, "valid Unicode request did not survive exactly");
            free(tab);
            NSString *socket = [@"" stringByPaddingToLength:134 withString:@"界" startingAtIndex:0];
            require(!mac_tmux_tab(@"$1", socket) && spawn_count == 1, "overlong Unicode helper/socket path spawned");
        } else if (!strcmp(which, "reap-exited") || !strcmp(which, "reap-later")) {
            pid_t child = fork(); require(child >= 0, "fork owned fixture");
            if (!child) { if (!strcmp(which, "reap-later")) usleep(20000); _exit(0); }
            if (!strcmp(which, "reap-exited")) usleep(30000);
            mac_reap_previous_spare(child);
            int gone = 0;
            for (int i=0; i<2000; i++) {
                if (kill(child, 0) < 0 && errno == ESRCH) { gone = 1; break; }
                usleep(1000);
            }
            /* The fixture exits on its own after 20 ms. A timeout never
               signals a PID which the asynchronous watcher may have reaped. */
            if (!gone) while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
            require(gone, "previous spare child was not reaped");
            require(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD, "owned child remains waitable");
            mac_reap_previous_spare(child); /* Already reaped; never affect another process. */
        } else require(0, "unknown case");
        frame_free(g_pending);
        printf("passed %s\n", which);
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Foundation/libdispatch required')
class NativeFrameTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-native-merge-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.root = Path(cls.temp.name)
        os.chmod(cls.root, 0o700)
        functions = '\n\n'.join([
            production_function(ROOT/'hardcore_mac.m', 'static void draw_frame(Frame *f)'),
            production_function(ROOT/'hardcore_mac.m', 'static void r_present(void)'),
            production_function(ROOT/'hardcore_mac_transfer.m', 'static Tab *mac_tmux_tab(NSString *session, NSString *socket)'),
            production_function(ROOT/'hardcore_mac_update.h', 'static void mac_reap_previous_spare(pid_t pid)'),
        ])
        cls.binaries = {}
        variants = {
            'production': functions,
            'no-order': functions.replace('if (f->serial > g_last_draw_serial)', 'if (1)'),
            'global-receipt': functions.replace('if (f->handoff) g_handoff_painted = f->painted;', '(void)f;'),
        }
        for name, body in variants.items():
            source, binary = cls.root/(name+'.m'), cls.root/name
            source.write_text(HARNESS.replace('/* PRODUCTION_FUNCTIONS */', body))
            result = bounded_run(['clang', '-O0', '-Wall', '-Wextra', '-Werror', '-framework',
                                  'Foundation', str(source), '-o', str(binary)], timeout=60)
            if result.returncode:
                raise AssertionError('harness compile failed:\n'+result.stderr)
            cls.binaries[name] = binary

    def assert_case(self, case):
        result = bounded_run([str(self.binaries['production']), case], timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'passed '+case)

    def test_stale_worker_cannot_overwrite_receipt(self): self.assert_case('stale-frame')
    def test_failed_receipt_cannot_borrow_other_frame_success(self): self.assert_case('failed-receipt')
    def test_successful_receipt_acknowledged(self): self.assert_case('successful-receipt')
    def test_worker_draws_are_serialized_and_ordered(self): self.assert_case('worker-order')
    def test_unicode_paths_validate_byte_capacity_before_spawn(self): self.assert_case('unicode-paths')
    def test_already_exited_spare_is_reaped(self): self.assert_case('reap-exited')
    def test_later_spare_exit_is_reaped(self): self.assert_case('reap-later')

    def test_negative_control_without_frame_order_fails(self):
        result = bounded_run([str(self.binaries['no-order']), 'stale-frame'], timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn('stale worker frame rendered after receipt', result.stderr)

    def test_negative_control_global_receipt_fails(self):
        result = bounded_run([str(self.binaries['global-receipt']), 'failed-receipt'], timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout+result.stderr)
        self.assertIn('unrelated frame falsely certified failed receipt', result.stderr)


if __name__ == '__main__':
    unittest.main()
