#!/usr/bin/env python3
"""Exercise production tab environment setup in owned real PTYs, without a GUI.

An automation parent often suppresses color in its own captured output. A new
interactive terminal must advertise its capabilities independently, while still
letting the user's shell startup files choose a different color policy.
"""
import errno
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time
import unittest

from test_mac_import_resize import production_function


ROOT = Path(__file__).resolve().parents[1]
COLOR_KEYS = ('NO_COLOR', 'FORCE_COLOR', 'CLICOLOR', 'CLICOLOR_FORCE')

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_mac_bin_dir = "/owned/hardcore/bin";
#define HARDCORE_VERSION "owned-env-test"

/* PRODUCTION_FUNCTION */

int main(int argc, const char **argv) {
    if (!isatty(0) || !isatty(1) || !isatty(2)) return 90;
    child_env();
    if (argc == 3 && !strcmp(argv[1], "--shell")) {
        execl("/bin/bash", "bash", "--noprofile", "--rcfile", argv[2], "-i", "-c",
              "printf 'HCENV NO_COLOR=%s\\nHCENV FORCE_COLOR=%s\\nHCENV COLORTERM=%s\\n' "
              "\"$NO_COLOR\" \"$FORCE_COLOR\" \"$COLORTERM\"", (char *)NULL);
        return 91;
    }
    const char *keys[] = {"TERM", "COLORTERM", "TERM_PROGRAM", "TERM_PROGRAM_VERSION",
                          "NO_COLOR", "FORCE_COLOR", "CLICOLOR", "CLICOLOR_FORCE",
                          "PATH", "HARDCORE_BIN_DIR", "HARDCORE_WINDOW_PID", "LANG",
                          "HC_USER_VALUE", "CI", "CLAUDECODE", "HARDCORE_CTL"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const char *value = getenv(keys[i]);
        printf("HCENV %s=%s\n", keys[i], value ? value : "<unset>");
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'macOS child environment')
class ChildEnvironmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-child-env-', dir='/private/tmp')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        harness = directory/'env.c'
        harness.write_text(HARNESS.replace('/* PRODUCTION_FUNCTION */', production_function(
            ROOT/'hardcore_mac.m', 'static void child_env(void)')))
        cls.binary = directory/'env'
        built = subprocess.run(['clang', '-Wall', '-Wextra', '-Werror', str(harness),
                                '-o', str(cls.binary)], capture_output=True, text=True, timeout=30)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def run_child(self, extra=None, args=()):
        # Deliberately isolated: never expose this test runner's credentials or
        # shell startup files to the fixture or its output.
        env = {'PATH': '/usr/bin:/bin', 'HOME': self.temp.name,
               'TERM': 'dumb', 'COLORTERM': 'launcher-color', 'TERM_PROGRAM': 'parent-agent',
               'HC_USER_VALUE': 'preserve-me', 'CI': '1',
               'CLAUDECODE': '1', 'HARDCORE_CTL': '/owned/parent-control'}
        env.update(extra or {})
        parent_environment = dict(os.environ)
        pid, master = os.forkpty()
        if pid == 0:
            try:
                os.execve(str(self.binary), [str(self.binary), *args], env)
            except BaseException:
                os._exit(92)
        output = bytearray()
        deadline = time.monotonic() + 5
        status = None
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    self.fail('owned child environment probe timed out')
                ready, _, _ = select.select([master], [], [], remaining)
                if not ready:
                    self.fail('owned child environment probe timed out')
                try:
                    chunk = os.read(master, 4096)
                except OSError as exc:
                    if exc.errno != errno.EIO:
                        raise
                    chunk = b''
                if not chunk:
                    break
                output.extend(chunk)
                self.assertLess(len(output), 65536, 'unexpected fixture output volume')
            while time.monotonic() < deadline:
                waited, status = os.waitpid(pid, os.WNOHANG)
                if waited:
                    break
                time.sleep(0.01)
            else:
                status = None
                self.fail('owned child did not exit after closing PTY')
        finally:
            os.close(master)
            if status is None:
                try:
                    os.killpg(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                os.waitpid(pid, 0)
        decoded = output.decode('utf-8', errors='replace')
        self.assertTrue(os.WIFEXITED(status), decoded)
        self.assertEqual(os.WEXITSTATUS(status), 0, decoded)
        self.assertEqual(dict(os.environ), parent_environment, 'child setup changed parent environment')
        return dict(line.removeprefix('HCENV ').split('=', 1)
                    for line in decoded.splitlines() if line.startswith('HCENV '))

    def assert_terminal_capabilities(self, values):
        self.assertEqual(values['TERM'], 'xterm-256color')
        self.assertEqual(values['COLORTERM'], 'truecolor')
        self.assertEqual(values['TERM_PROGRAM'], 'hardcore')
        self.assertEqual(values['TERM_PROGRAM_VERSION'], 'owned-env-test')
        self.assertEqual(values['HARDCORE_WINDOW_PID'], str(os.getpid()))

    def test_inherited_color_suppression_does_not_disable_new_terminal(self):
        values = self.run_child({'NO_COLOR': '1', 'FORCE_COLOR': '0',
                                 'CLICOLOR': '0', 'CLICOLOR_FORCE': '0'})
        self.assert_terminal_capabilities(values)
        for key in COLOR_KEYS:
            self.assertEqual(values[key], '<unset>', key)

    def test_inherited_forcing_does_not_cap_or_override_new_terminal(self):
        for forced in ('', '1', '2', '3'):
            with self.subTest(force_color=forced):
                values = self.run_child({'FORCE_COLOR': forced, 'CLICOLOR_FORCE': '1'})
                self.assert_terminal_capabilities(values)
                for key in COLOR_KEYS:
                    self.assertEqual(values[key], '<unset>', key)

    def test_clean_environment_keeps_natural_tty_color_detection(self):
        values = self.run_child()
        self.assert_terminal_capabilities(values)
        for key in COLOR_KEYS:
            self.assertEqual(values[key], '<unset>', key)

    def test_unrelated_user_configuration_and_existing_isolation_survive(self):
        values = self.run_child({'LANG': 'fr_FR.UTF-8'})
        self.assertEqual(values['PATH'], '/owned/hardcore/bin:/usr/bin:/bin')
        self.assertEqual(values['HARDCORE_BIN_DIR'], '/owned/hardcore/bin')
        self.assertEqual(values['LANG'], 'fr_FR.UTF-8')
        self.assertEqual(values['HC_USER_VALUE'], 'preserve-me')
        self.assertEqual(values['CI'], '1')
        self.assertEqual(values['CLAUDECODE'], '<unset>')
        self.assertEqual(values['HARDCORE_CTL'], '<unset>')

    def test_shell_startup_can_still_choose_its_own_color_policy(self):
        rc = Path(self.temp.name)/'owned.bashrc'
        rc.write_text('export NO_COLOR=1 FORCE_COLOR=0\n')
        values = self.run_child({'NO_COLOR': 'launcher-policy', 'FORCE_COLOR': '3'},
                                ('--shell', str(rc)))
        self.assertEqual(values['NO_COLOR'], '1')
        self.assertEqual(values['FORCE_COLOR'], '0')
        self.assertEqual(values['COLORTERM'], 'truecolor')


if __name__ == '__main__':
    unittest.main()
