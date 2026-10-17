#!/usr/bin/env python3
"""Own-screen themes/config and mocked, reversible Terminal font transactions."""
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('hc_here_settings', ROOT/'tools/here_settings.py')
settings = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = settings
SPEC.loader.exec_module(settings)


class FakeTerminal:
    def __init__(self):
        self.state = {'tty': '/dev/ttys999', 'window_id': 77, 'name': 'Monaco', 'size': 13}
        self.calls = []
        self.fail_set = False
        self.invalid_read = False
        self.partial_failure = False

    def __call__(self, argv, **kwargs):
        self.calls.append((argv, kwargs))
        request = json.loads(argv[-1])
        if request['action'] == 'read':
            state = {'name': 'bad'} if self.invalid_read else self.state
            return SimpleNamespace(returncode=0, stdout=json.dumps(state), stderr='')
        if self.fail_set:
            raise subprocess.TimeoutExpired(argv, 3)
        if request['window_id'] != self.state['window_id']:
            return SimpleNamespace(returncode=1, stdout='', stderr='Originating Terminal window changed')
        self.state['name'] = request['name']
        if self.partial_failure:
            self.partial_failure = False
            return SimpleNamespace(returncode=1, stdout='', stderr='Font size change failed')
        self.state['size'] = request['size']
        return SimpleNamespace(returncode=0, stdout=json.dumps(self.state), stderr='')


def font_session(fake, **kwargs):
    return settings.HostFontSession(platform='darwin', env={'TERM_PROGRAM': 'Apple_Terminal'},
                                    tty_name='/dev/ttys999', runner=fake, **kwargs)


class ThemeTests(unittest.TestCase):
    def test_every_theme_exactly_matches_native_palette(self):
        pattern = r"\{'([^']+)', \"([^\"]+)\", ([01]), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), (0x[0-9A-F]+), \{([^}]+)\}\}"
        native = re.findall(pattern, (ROOT/'hardcore.c').read_text())
        self.assertEqual(len(native), len(settings.THEMES))
        self.assertEqual(len(native), 16)
        for row, theme in zip(native, settings.THEMES):
            key, name, dark, bg, fg, cursor, selection, palette = row
            self.assertEqual((theme.key, theme.name, theme.dark), (key, name, bool(int(dark))))
            self.assertEqual((theme.bg, theme.fg, theme.cursor, theme.selection),
                             tuple(settings.rgb(int(v, 16)) for v in (bg, fg, cursor, selection)))
            self.assertEqual(theme.palette, tuple(settings.rgb(int(v.strip(), 16)) for v in palette.split(',')))
        self.assertEqual(settings.theme_for('unknown').key, 'g')

    def test_settings_are_private_atomic_and_do_not_touch_native_config(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'hardcore/here.json'
            path.parent.mkdir()
            native = path.with_name('config.ini')
            native.write_text('theme=m\n')
            saved = settings.save_settings({'theme': 'd', 'font_size': 16, 'allow_host_font': True}, path)
            self.assertEqual(settings.load_settings(path), saved)
            self.assertEqual(saved['theme'], 'd')
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(native.read_text(), 'theme=m\n')
            self.assertEqual(sorted(p.name for p in path.parent.iterdir()), ['config.ini', 'here.json'])

    def test_missing_corrupt_oversized_and_invalid_settings_fall_back(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'here.json'
            self.assertEqual(settings.load_settings(path), settings.DEFAULT_SETTINGS)
            for data in ('{bad', 'null', '[]', '"foo"', 'x'*17000):
                path.write_text(data)
                self.assertEqual(settings.load_settings(path), settings.DEFAULT_SETTINGS)
            bad = {'theme': ['m'], 'allow_host_font': 'yes', 'font_name': '\x1b]50;bad', 'font_size': True}
            path.write_text(json.dumps(bad))
            self.assertEqual(settings.load_settings(path), settings.DEFAULT_SETTINGS)
            settings.save_settings({'font_size': 1000}, path)
            self.assertEqual(settings.load_settings(path)['font_size'], 14)

    def test_failed_atomic_replace_preserves_old_settings(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/'here.json'
            settings.save_settings({'theme': 'm'}, path)
            with patch.object(settings.os, 'replace', side_effect=OSError('disk full')):
                with self.assertRaises(OSError):
                    settings.save_settings({'theme': 'd'}, path)
            self.assertEqual(settings.load_settings(path)['theme'], 'm')
            self.assertEqual([p.name for p in path.parent.iterdir()], ['here.json'])


class HostFontTests(unittest.TestCase):
    def test_default_and_denied_requests_have_no_host_calls(self):
        fake = FakeTerminal()
        host = font_session(fake)
        self.assertTrue(host.available)
        self.assertFalse(host.active)
        self.assertEqual(fake.calls, [])
        with self.assertRaises(settings.HostFontError):
            host.apply('Menlo', 16)
        self.assertEqual(fake.calls, [])
        self.assertFalse(host.restore())

    def test_unsupported_nested_and_remote_hosts_never_probe(self):
        fake = FakeTerminal()
        envs = ({}, {'TERM_PROGRAM': 'iTerm.app'}, {'TERM_PROGRAM': 'Apple_Terminal', 'TMUX': 'a'},
                {'TERM_PROGRAM': 'Apple_Terminal', 'SSH_CONNECTION': 'host'},
                {'TERM_PROGRAM': 'Apple_Terminal', 'STY': 'screen'})
        for env in envs:
            host = settings.HostFontSession(platform='darwin', env=env, tty_name='/dev/ttys999', runner=fake)
            self.assertFalse(host.available)
            with self.assertRaises(settings.HostFontError):
                host.apply('Menlo', 16, allowed=True)
        host = settings.HostFontSession(platform='linux', env={'TERM_PROGRAM': 'Apple_Terminal'},
                                        tty_name='/dev/pts/7', runner=fake)
        self.assertFalse(host.available)
        self.assertEqual(fake.calls, [])

    def test_apply_preserves_exact_first_snapshot_and_restores(self):
        fake = FakeTerminal()
        original = dict(fake.state)
        host = font_session(fake)
        host.apply('Menlo', 16, allowed=True)
        host.apply('Courier', 18, allowed=True)
        self.assertTrue(host.active)
        self.assertEqual(host.original, original)
        self.assertEqual(fake.state['name'], 'Courier')
        self.assertTrue(host.restore())
        self.assertEqual(fake.state, original)
        self.assertFalse(host.active)
        self.assertFalse(host.restore())
        requests = [json.loads(call[0][-1]) for call in fake.calls]
        self.assertEqual([r['action'] for r in requests], ['read', 'set', 'set', 'set'])
        self.assertTrue(all(r['tty'] == '/dev/ttys999' for r in requests))
        self.assertTrue(all(r.get('window_id') == 77 for r in requests[1:]))

    def test_suspend_restores_and_resume_honors_user_change_while_suspended(self):
        fake = FakeTerminal()
        host = font_session(fake)
        host.apply('Menlo', 16, allowed=True)
        self.assertTrue(host.suspend())
        self.assertEqual(fake.state['name'], 'Monaco')
        fake.state.update(name='Courier', size=12)
        self.assertTrue(host.resume())
        self.assertEqual((fake.state['name'], fake.state['size']), ('Menlo', 16))
        host.restore()
        self.assertEqual((fake.state['name'], fake.state['size']), ('Courier', 12))

    def test_reenable_takes_new_restoration_snapshot(self):
        fake = FakeTerminal()
        host = font_session(fake)
        host.apply('Menlo', 16, allowed=True)
        host.restore()
        fake.state.update(name='Courier', size=12)
        host.apply('Monaco', 18, allowed=True)
        host.restore()
        self.assertEqual((fake.state['name'], fake.state['size']), ('Courier', 12))

    def test_invalid_snapshot_prevents_every_write(self):
        fake = FakeTerminal()
        fake.invalid_read = True
        host = font_session(fake)
        with self.assertRaises(settings.HostFontError):
            host.apply('Menlo', 16, allowed=True)
        self.assertFalse(host.active)
        self.assertEqual(len(fake.calls), 1)

    def test_partial_write_rolls_back_both_original_properties(self):
        fake = FakeTerminal()
        host = font_session(fake)
        original = dict(fake.state)
        fake.partial_failure = True
        with self.assertRaises(settings.HostFontError):
            host.apply('Menlo', 16, allowed=True)
        self.assertEqual(fake.state, original)
        self.assertFalse(host.active)

    def test_timeout_retains_snapshot_until_restore_succeeds(self):
        fake = FakeTerminal()
        host = font_session(fake)
        fake.fail_set = True
        with self.assertRaisesRegex(settings.HostFontError, 'restoration is still pending'):
            host.apply('Menlo', 16, allowed=True)
        self.assertTrue(host.active)
        self.assertIsNotNone(host.original)
        fake.fail_set = False
        self.assertTrue(host.restore())
        self.assertEqual(fake.state['name'], 'Monaco')

    def test_does_not_retarget_reused_tty_in_another_window(self):
        fake = FakeTerminal()
        host = font_session(fake)
        host.apply('Menlo', 16, allowed=True)
        fake.state.update(window_id=88, name='Courier', size=12)
        with self.assertRaises(settings.HostFontError):
            host.restore()
        self.assertEqual((fake.state['name'], fake.state['size']), ('Courier', 12))

    def test_no_source_interpolation_or_shell_commands(self):
        fake = FakeTerminal()
        host = font_session(fake)
        name = 'Font "quoted" $(do not run)'
        host.apply(name, 16, allowed=True)
        argv, options = fake.calls[-1]
        self.assertEqual(argv[0], '/usr/bin/osascript')
        self.assertNotIn(name, argv[4])
        self.assertEqual(json.loads(argv[-1])['name'], name)
        self.assertEqual(options['timeout'], 3)
        self.assertNotIn('shell', options)

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS compiler required')
    def test_adapter_compiles_without_running_apple_events(self):
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp)/'font.js'
            source.write_text(settings._TERMINAL_FONT_SCRIPT)
            result = subprocess.run(['/usr/bin/osacompile', '-l', 'JavaScript', '-o',
                                     str(Path(temp)/'font.scpt'), str(source)], capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr.decode())


if __name__ == '__main__':
    unittest.main()
