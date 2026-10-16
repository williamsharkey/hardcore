#!/usr/bin/env python3
"""Owned macOS caffeinate lifecycle, exec adoption and cached power probes.

Only disposable fixture children are signaled. No user's caf process, shell,
terminal window, configuration or sleep settings are read or changed. Brief
system sleep assertions belong to the fixture and expire with its owner.
"""
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <errno.h>
#include <spawn.h>
#include <string.h>
static int spawn_failure, hung_power;
static pid_t probe_child;
static int test_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *files,
                      const posix_spawnattr_t *attributes, char *const argv[], char *const env[]) {
    if (spawn_failure && !strcmp(path, "/usr/bin/caffeinate")) return EACCES;
    if (hung_power && !strcmp(path, "/usr/bin/pmset")) {
        char *sleep[] = {"/bin/sleep", "20", NULL};
        int result = posix_spawn(pid, sleep[0], files, attributes, sleep, env);
        if (!result) probe_child = *pid;
        return result;
    }
    return posix_spawn(pid, path, files, attributes, argv, env);
}
#define posix_spawn test_spawn
static int changes;
#define HARDCORE_CAF_CHANGED() (++changes)
#include "hardcore_mac_caf.m"

static void report(int result) {
    int active = mac_caf_active();
    printf("{\"owner\":%ld,\"child\":%ld,\"active\":%d,\"result\":%d,\"elapsed\":%.6f,"
           "\"descriptor\":\"%s\",\"power\":\"%s\",\"changes\":%d}\n",
           (long)getpid(), (long)g_mac_caf.pid, active, result, mac_caf_elapsed(),
           getenv(MAC_CAF_ENV) ?: "", mac_caf_power(), changes);
    fflush(stdout);
}

static int reject_child(const char *mode) {
    char owner[32]; snprintf(owner, sizeof(owner), "%ld", (long)getpid());
    char *coffee[] = {"/usr/bin/caffeinate", "-di", "-w", owner, NULL};
    char *sleep[] = {"/bin/sleep", "20", NULL};
    char **args = !strcmp(mode, "wrong-executable") ? sleep : coffee;
    pid_t child;
    if (mac_caf_spawn(&child, args[0], args, -1)) return 90;
    struct proc_bsdinfo info;
    if (proc_pidinfo(child, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) return 91;
    char descriptor[160];
    snprintf(descriptor, sizeof(descriptor), "%ld:%ld:%llu:%llu:%llu",
        (long)getpid() + !strcmp(mode, "wrong-owner"), (long)child,
        (unsigned long long)info.pbi_start_tvsec + !strcmp(mode, "wrong-start"),
        (unsigned long long)info.pbi_start_tvusec, (unsigned long long)mac_caf_now());
    setenv(MAC_CAF_ENV, descriptor, 1);
    int active = mac_caf_active();
    mac_caf_stop();
    int alive = !kill(child, 0);
    printf("{\"active\":%d,\"alive\":%d,\"descriptor\":%d}\n", active, alive, getenv(MAC_CAF_ENV) != NULL);
    kill(child, SIGTERM);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "power-hang")) {
        hung_power = 1;
        uint64_t before = mac_caf_now();
        char result[96]; mac_caf_query_power(result);
        printf("{\"seconds\":%.3f,\"reaped\":%d,\"text\":\"%s\"}\n",
            (mac_caf_now()-before)/1e9, waitpid(probe_child, NULL, WNOHANG) < 0 && errno == ECHILD, result);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "parse")) {
        char result[96]; mac_caf_parse_power(argv[2], result, sizeof(result));
        puts(result); return 0;
    }
    if (argc == 2 && !strncmp(argv[1], "wrong-", 6)) return reject_child(argv[1]);
    report(0);
    if (argc == 2 && !strcmp(argv[1], "once")) return 0;
    char command[64];
    while (fgets(command, sizeof(command), stdin)) {
        command[strcspn(command, "\r\n")] = 0;
        int result = 0;
        if (!strcmp(command, "toggle")) result = mac_caf_toggle();
        else if (!strcmp(command, "spawn-failure")) {
            spawn_failure = 1; result = mac_caf_toggle(); spawn_failure = 0;
        }
        else if (!strcmp(command, "stop")) mac_caf_stop();
        else if (!strcmp(command, "kill-child")) {
            if (g_mac_caf.pid) {
                kill(g_mac_caf.pid, SIGKILL);
                usleep(20000);
            }
        } else if (!strcmp(command, "fork-exit")) {
            pid_t child = fork();
            if (!child) exit(0); /* Exercises the inherited atexit handler. */
            while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        } else if (!strcmp(command, "exec")) {
            execl(argv[0], argv[0], (char *)NULL); return 92;
        } else if (!strcmp(command, "failed-exec")) {
            execl("/hardcore-caf-fixture-does-not-exist", "missing", (char *)NULL);
        } else if (!strcmp(command, "exit")) return 0;
        else if (!strcmp(command, "die")) _exit(0);
        else if (strcmp(command, "status")) return 93;
        report(result);
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == "darwin", "macOS caffeinate integration")
class MacCafTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="hc-caf-test-", dir="/private/tmp")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name)/"caf-fixture"
        source = cls.binary.with_suffix(".m")
        source.write_text(HARNESS)
        result = subprocess.run(["clang", "-fblocks", "-Wall", "-Wextra", "-Werror",
            "-mmacosx-version-min=11.0", "-arch", "arm64", "-arch", "x86_64",
            "-I", str(ROOT), str(source), "-o", str(cls.binary)],
            text=True, capture_output=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def env(self, **extra):
        return {"PATH": "/usr/bin:/bin", **extra}

    def fixture(self):
        p = subprocess.Popen([str(self.binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, env=self.env())
        self.addCleanup(self.finish, p)
        return p, self.read(p)

    def read(self, process):
        ready, _, _ = select.select([process.stdout], [], [], 5)
        self.assertTrue(ready, "owned caf fixture did not respond")
        line = process.stdout.readline()
        self.assertTrue(line, f"fixture exited unexpectedly: {process.poll()}")
        return json.loads(line)

    def ask(self, process, command):
        process.stdin.write(command + "\n")
        process.stdin.flush()
        return self.read(process)

    def finish(self, process):
        if process.poll() is None:
            try:
                process.stdin.write("exit\n")
                process.stdin.flush()
                process.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                process.kill()
                process.wait(timeout=5)
        for stream in (process.stdin, process.stdout, process.stderr):
            stream.close()

    def gone(self, pid):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            state = subprocess.run(["/bin/ps", "-p", str(pid), "-o", "stat="],
                                   text=True, capture_output=True, timeout=2).stdout.strip()
            if not state or state.startswith("Z"):
                return
            time.sleep(.02)
        self.fail(f"owned assertion child {pid} outlived its fixture")

    def test_toggle_exact_flags_elapsed_and_stop(self):
        p, initial = self.fixture()
        self.assertFalse(initial["active"])
        enabled = self.ask(p, "toggle")
        self.assertEqual(enabled["result"], 1)
        self.assertTrue(enabled["active"])
        self.assertEqual(enabled["owner"], p.pid)
        self.assertTrue(enabled["descriptor"].startswith(f"{p.pid}:{enabled['child']}:"))
        argv = subprocess.run(["/bin/ps", "-p", str(enabled["child"]), "-o", "command="],
                              text=True, capture_output=True, check=True, timeout=2).stdout.strip()
        self.assertEqual(argv, f"/usr/bin/caffeinate -di -w {p.pid}")
        time.sleep(.06)
        later = self.ask(p, "status")
        self.assertGreater(later["elapsed"], enabled["elapsed"] + .04)
        disabled = self.ask(p, "toggle")
        self.assertEqual(disabled["result"], 0)
        self.assertFalse(disabled["active"])
        self.assertEqual(disabled["elapsed"], 0)
        self.assertEqual(disabled["descriptor"], "")
        self.gone(enabled["child"])
        self.assertFalse(self.ask(p, "stop")["active"])

    def test_exec_preserves_exact_child_and_elapsed(self):
        p, _ = self.fixture()
        first = self.ask(p, "toggle")
        time.sleep(.05)
        adopted = self.ask(p, "exec")
        self.assertTrue(adopted["active"])
        self.assertEqual(adopted["child"], first["child"])
        self.assertEqual(adopted["owner"], first["owner"])
        self.assertEqual(adopted["descriptor"], first["descriptor"])
        self.assertGreater(adopted["elapsed"], first["elapsed"])
        failed = self.ask(p, "failed-exec")
        self.assertTrue(failed["active"])
        self.assertEqual(failed["child"], first["child"])
        self.ask(p, "stop")
        self.gone(first["child"])

    def test_unexpected_child_exit_is_not_reported_active(self):
        p, _ = self.fixture()
        enabled = self.ask(p, "toggle")
        result = self.ask(p, "kill-child")
        self.assertFalse(result["active"])
        self.assertEqual(result["descriptor"], "")
        restarted = self.ask(p, "toggle")
        self.assertTrue(restarted["active"])
        self.assertNotEqual(restarted["child"], enabled["child"])

    def test_independent_window_assertions_do_not_stop_each_other(self):
        first, _ = self.fixture()
        second, _ = self.fixture()
        a = self.ask(first, "toggle")
        b = self.ask(second, "toggle")
        self.assertNotEqual(a["child"], b["child"])
        self.ask(first, "stop")
        self.gone(a["child"])
        other = self.ask(second, "status")
        self.assertTrue(other["active"])
        self.assertEqual(other["child"], b["child"])

    def test_normal_exit_cleans_up_and_owner_death_releases_assertion(self):
        for how in ("exit", "die", "kill"):
            with self.subTest(how=how):
                p, _ = self.fixture()
                enabled = self.ask(p, "toggle")
                if how == "kill":
                    p.kill()
                else:
                    p.stdin.write(how + "\n")
                    p.stdin.flush()
                p.wait(timeout=5)
                self.gone(enabled["child"])

    def test_forked_child_exit_cannot_stop_parent(self):
        p, _ = self.fixture()
        enabled = self.ask(p, "toggle")
        after = self.ask(p, "fork-exit")
        self.assertTrue(after["active"])
        self.assertEqual(after["child"], enabled["child"])

    def test_wrong_identity_is_rejected_without_signaling(self):
        for mode in ("wrong-executable", "wrong-owner", "wrong-start"):
            with self.subTest(mode=mode):
                result = subprocess.run([str(self.binary), mode], env=self.env(),
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), {"active": 0, "alive": 1, "descriptor": 0})

    def test_spawn_failure_leaves_no_false_enabled_state(self):
        p, _ = self.fixture()
        failed = self.ask(p, "spawn-failure")
        self.assertEqual(failed["result"], -1)
        self.assertFalse(failed["active"])
        self.assertEqual(failed["descriptor"], "")
        self.assertTrue(self.ask(p, "toggle")["active"])

    def test_malformed_descriptors_are_cleared(self):
        for value in ("", "1", "-1:2:3:4:5", "1:2:3:4:5junk", "9"*200,
                      "1:2:3:4:5:6", "1:2:3:1000000:5", "1:2:3:4:18446744073709551616"):
            with self.subTest(value=value):
                result = subprocess.run([str(self.binary), "once"],
                    env=self.env(HARDCORE_CAF_CHILD=value), capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads(result.stdout)
                self.assertFalse(data["active"])
                self.assertEqual(data["descriptor"], "")

    def test_power_status_parsing(self):
        cases = {
            "Now drawing from 'AC Power'\n -InternalBattery-0 98%; charging; 0:12 remaining":
                "Plugged in · 98% · charging",
            "Now drawing from 'Battery Power'\n -InternalBattery-0 19%; discharging; 1:05 remaining":
                "On battery · 19%",
            "Now drawing from 'AC Power'\n": "Plugged in",
            "Now drawing from 'AC Power'\n 100%; charged;": "Plugged in · 100%",
            "Now drawing from 'Battery Power'\n 300%; discharging;": "On battery",
            "truncated garbage": "Power status unavailable",
        }
        for output, expected in cases.items():
            with self.subTest(expected=expected):
                result = subprocess.run([str(self.binary), "parse", output], env=self.env(),
                                        capture_output=True, text=True, check=True, timeout=5)
                self.assertEqual(result.stdout.strip(), expected)

    def test_power_probe_does_not_block_caf_control(self):
        p, _ = self.fixture()
        start = time.monotonic()
        enabled = self.ask(p, "toggle")
        self.assertLess(time.monotonic() - start, .5)
        self.assertTrue(enabled["active"])
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline:
            state = self.ask(p, "status")
            if state["power"] != "Power status unavailable":
                break
            time.sleep(.02)
        self.assertRegex(state["power"], r"^(Plugged in|On battery)( · \d+%( · charging)?)?$")

    def test_hung_power_probe_is_bounded_and_reaped(self):
        result = subprocess.run([str(self.binary), "power-hang"], env=self.env(),
                                capture_output=True, text=True, check=True, timeout=5)
        data = json.loads(result.stdout)
        self.assertGreaterEqual(data["seconds"], 1.9)
        self.assertLess(data["seconds"], 2.5)
        self.assertTrue(data["reaped"])
        self.assertEqual(data["text"], "Power status unavailable")


if __name__ == "__main__":
    unittest.main()
