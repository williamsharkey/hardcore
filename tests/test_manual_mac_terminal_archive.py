#!/usr/bin/env python3
"""Manual archive workflow checks with every capture/compiler call mocked."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('manual_archive', ROOT / 'tools/manual_mac_terminal_archive.py')
manual = importlib.util.module_from_spec(spec)
spec.loader.exec_module(manual)
TOKEN = '0123456789abcdef0123456789abcdef'


def comparison(exact=True, whole=False):
    return {'format': 'hardcore-terminal-text-comparison-v1', 'fixtureToken': TOKEN,
            'scope': 'fixture', 'fullTerminalState': False,
            'wholeSelection': {'exact': whole, 'plainUTF16Length': 130, 'rtfUTF16Length': 131,
                               'firstMismatchUTF16': None if whole else 5},
            'fixture': {'exact': exact, 'plainUTF16Length': 100, 'rtfUTF16Length': 100 if exact else 101,
                        'firstMismatchUTF16': None if exact else 50,
                        'plainCodeUnits': ['SECRET-NOT-FOR-SUMMARY'],
                        'diagnosticEquivalence': {'lineEndingsOnly': not exact,
                            'unicodeNormalizationOnly': False, 'lineEndingsAndUnicodeNormalization': not exact}}}


def style_mapping(method='literal', count=0):
    return {'method': method, 'verified': True, 'plainTextPreserved': True, 'remappedClusters': count}


class ManualArchiveTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='hc-manual-archive-test-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.run = self.root / 'saved-run'
        self.run.mkdir(mode=0o700)
        self.plain = manual.fixture(TOKEN)[1]
        manual.private_write(self.run / 'fixture.txt', self.plain)
        manual.private_write(self.run / 'manifest.json', json.dumps({'fixtureToken': TOKEN, 'fullTerminalState': False}))

    def test_saved_fixture_loads_without_mutation_or_processes(self):
        paths = [self.run / 'fixture.txt', self.run / 'manifest.json']
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths}
        with mock.patch.object(manual.subprocess, 'run', side_effect=AssertionError('process launched')):
            self.assertEqual(manual.saved_fixture(self.run), (self.run, TOKEN, self.plain))
        self.assertEqual(before, {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in paths})

    def test_saved_fixture_refuses_directory_symlink_or_public_directory(self):
        link = self.root / 'link'
        link.symlink_to(self.run, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symlink'):
            manual.saved_fixture(link)
        self.run.chmod(0o755)
        with self.assertRaisesRegex(ValueError, 'private directory'):
            manual.saved_fixture(self.run)

    def test_saved_fixture_refuses_symlink_public_or_oversized_inputs(self):
        manifest = self.run / 'manifest.json'
        original = manifest.read_bytes()
        manifest.rename(self.run / 'original-manifest.json')
        manifest.symlink_to(self.run / 'original-manifest.json')
        with self.assertRaises(OSError):
            manual.saved_fixture(self.run)
        manifest.unlink()
        manifest.write_bytes(original)
        manifest.chmod(0o644)
        with self.assertRaisesRegex(ValueError, 'private regular file'):
            manual.saved_fixture(self.run)
        manifest.chmod(0o600)
        manifest.write_bytes(b'x' * (64 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, 'size limit'):
            manual.saved_fixture(self.run)

    def test_saved_fixture_refuses_fifo_without_blocking(self):
        path = self.run / 'fixture.txt'
        path.unlink()
        os.mkfifo(path, 0o600)
        with self.assertRaisesRegex(ValueError, 'private regular file'):
            manual.saved_fixture(self.run)

    def test_saved_fixture_rejects_invalid_identity_and_modified_text(self):
        for manifest in ([], {}, {'fixtureToken': '../invalid', 'fullTerminalState': False},
                         {'fixtureToken': TOKEN.upper(), 'fullTerminalState': False},
                         {'fixtureToken': TOKEN, 'fullTerminalState': True}):
            with self.subTest(manifest=manifest):
                (self.run / 'manifest.json').write_text(json.dumps(manifest))
                with self.assertRaises(ValueError):
                    manual.saved_fixture(self.run)
        (self.run / 'manifest.json').write_text(json.dumps({'fixtureToken': TOKEN, 'fullTerminalState': False}))
        (self.run / 'fixture.txt').write_text(self.plain[:-1])
        with self.assertRaisesRegex(ValueError, 'does not match'):
            manual.saved_fixture(self.run)

    def test_comparison_summary_is_metadata_only_and_does_not_accept_equivalence(self):
        text = '\n'.join(manual.comparison_summary(comparison(exact=False)))
        self.assertIn('Fixture only: RTF/plain text differ', text)
        self.assertIn('First difference at UTF-16 unit 50', text)
        self.assertIn('line endings', text)
        self.assertIn('does not count as an exact match', text)
        self.assertNotIn('SECRET', text)
        for value in (None, [], {'fixture': 'raw secret'}, {'wholeSelection': {'plainUTF16Length': 'secret'}}):
            self.assertNotIn('secret', '\n'.join(manual.comparison_summary(value)))

    def test_native_bool_accepts_only_json_booleans_and_integer_zero_one(self):
        for value, expected in ((True, True), (False, False), (0, False), (1, True),
                                (None, None), ('true', None), ('1', None), (2, None), (1.0, None), ([], None)):
            with self.subTest(value=value):
                self.assertIs(manual.native_bool(value), expected)

    def test_numeric_native_normalization_flags_produce_diagnostic_hint(self):
        metadata = comparison(exact=False)
        metadata['fixture']['exact'] = 0
        metadata['wholeSelection']['exact'] = 0
        metadata['fixture']['diagnosticEquivalence'] = {'lineEndingsOnly': 0,
            'unicodeNormalizationOnly': 1, 'lineEndingsAndUnicodeNormalization': 1}
        text = '\n'.join(manual.comparison_summary(metadata))
        self.assertIn('Fixture only: RTF/plain text differ', text)
        self.assertIn('Diagnostic clue: Unicode normalization', text)
        self.assertNotIn('Diagnostic clue: line endings', text)
        self.assertIn('does not count as an exact match', text)

    def test_report_separates_whole_selection_fixture_representations_and_expected_fixture(self):
        report = manual.evaluate({'text': self.plain, 'comparison': comparison()}, self.plain, TOKEN)
        self.assertFalse(report['wholeSelectionTextExact'])
        self.assertTrue(report['fixtureRepresentationsExact'])
        self.assertTrue(report['fixtureTextExact'])
        self.assertFalse(report['fullTerminalState'])
        modified = self.plain.replace('ANSI red', 'ANSI blue')
        report = manual.evaluate({'text': modified, 'comparison': comparison()}, self.plain, TOKEN)
        self.assertTrue(report['fixtureRepresentationsExact'])
        self.assertFalse(report['fixtureTextExact'])

    def fake_capture(self, target, returncode=0, text=None):
        target.mkdir(mode=0o700)
        metadata = comparison(exact=not returncode)
        manual.private_write(target / 'comparison.json', json.dumps(metadata))
        if returncode:
            for name in ('original.rtf', 'original.txt', 'decoded-rtf.txt'):
                manual.private_write(target / name, 'private captured material')
        else:
            manual.private_write(target / 'archive.json', json.dumps({'text': self.plain if text is None else text,
                'comparison': metadata, 'styleMapping': style_mapping()}))

    def test_style_mapping_requires_explicit_known_method_and_verified_plaintext(self):
        self.assertTrue(manual.mapping_verified(style_mapping()))
        self.assertTrue(manual.mapping_verified(style_mapping('canonical-grapheme', 1)))
        for mapping in (None, {}, {**style_mapping(), 'verified': False},
                        {**style_mapping(), 'plainTextPreserved': False},
                        {**style_mapping(), 'verified': 'true'},
                        style_mapping('unknown', 0), style_mapping('literal', 1),
                        style_mapping('canonical-grapheme', 0), style_mapping('canonical-grapheme', -1),
                        style_mapping('canonical-grapheme', True), style_mapping('canonical-grapheme', 1024 * 1024 + 1)):
            with self.subTest(mapping=mapping):
                self.assertFalse(manual.mapping_verified(mapping))

    def test_canonical_mapping_accepts_exact_plaintext_without_claiming_raw_exactness(self):
        target = self.run / 'capture'
        target.mkdir(mode=0o700)
        metadata = comparison(exact=False)
        manual.private_write(target / 'comparison.json', json.dumps(metadata))
        mapping = style_mapping('canonical-grapheme', 1)
        manual.private_write(target / 'archive.json', json.dumps({'text': self.plain,
            'comparison': metadata, 'styleMapping': mapping}))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            code = manual.finish(subprocess.CompletedProcess([], 0), TOKEN, self.plain, self.run, self.run)
        self.assertEqual(code, 0)
        report = json.loads((self.run / 'coverage.json').read_text())
        self.assertTrue(report['fixtureTextExact'])
        self.assertFalse(report['fixtureRepresentationsExact'])
        self.assertTrue(report['styleMappingVerified'])
        self.assertEqual(report['styleMapping'], mapping)
        self.assertIn('raw RTF/plain strings still differ', output.getvalue())

    def test_normalization_hint_alone_does_not_authorize_mapping(self):
        target = self.run / 'capture'
        target.mkdir(mode=0o700)
        manual.private_write(target / 'archive.json', json.dumps({'text': self.plain,
            'comparison': comparison(exact=False)}))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(manual.finish(subprocess.CompletedProcess([], 0), TOKEN, self.plain, self.run, self.run), 1)

    def test_mapping_cannot_hide_lost_plaintext_or_inconsistent_raw_comparison(self):
        cases = ((self.plain[:-1], comparison(exact=False), style_mapping('canonical-grapheme', 1)),
                 (self.plain, comparison(exact=False), style_mapping()),
                 (self.plain, comparison(exact=True), style_mapping('canonical-grapheme', 1)),
                 (self.plain, comparison(exact=True), 'not-a-mapping'))
        for number, (text, metadata, mapping) in enumerate(cases):
            with self.subTest(number=number):
                attempt = self.root / f'inconsistent-{number}'
                target = attempt / 'capture'
                target.mkdir(parents=True, mode=0o700)
                manual.private_write(target / 'archive.json', json.dumps({'text': text,
                    'comparison': metadata, 'styleMapping': mapping}))
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(manual.finish(subprocess.CompletedProcess([], 0), TOKEN, self.plain,
                                                   attempt, self.run), 1)

    def test_capture_requires_enter_and_retains_nonzero_diagnostics_without_raw_text(self):
        events = []
        def call(arguments, **kwargs):
            self.assertEqual(events, ['confirmed'])
            self.assertEqual(arguments[1:3], ['--capture', TOKEN])
            self.assertEqual(kwargs['stdin'], subprocess.DEVNULL)
            self.assertEqual(kwargs['timeout'], 30)
            self.fake_capture(Path(arguments[-1]), returncode=7)
            return subprocess.CompletedProcess(arguments, 7)
        output = io.StringIO()
        with mock.patch('builtins.input', side_effect=lambda prompt: events.append('confirmed') or ''), \
             mock.patch.object(manual.subprocess, 'run', side_effect=call) as command, \
             contextlib.redirect_stdout(output):
            code = manual.capture(self.run / 'helper', TOKEN, self.plain, self.run, self.run)
        self.assertEqual(code, 7)
        command.assert_called_once()
        self.assertFalse((self.run / 'capture/archive.json').exists())
        self.assertFalse((self.run / 'coverage.json').exists())
        self.assertIn('comparison.json', output.getvalue())
        self.assertIn('--recapture', output.getvalue())
        self.assertNotIn('private captured material', output.getvalue())
        self.assertNotIn('SECRET', output.getvalue())

    def test_cancel_before_capture_does_not_read_clipboard(self):
        with mock.patch('builtins.input', side_effect=KeyboardInterrupt), \
             mock.patch.object(manual.subprocess, 'run') as command:
            with self.assertRaises(KeyboardInterrupt):
                manual.capture(self.run / 'helper', TOKEN, self.plain, self.run, self.run)
        command.assert_not_called()
        self.assertFalse((self.run / 'capture').exists())

    def test_matching_representations_but_wrong_expected_text_is_not_success(self):
        def call(arguments, **kwargs):
            self.fake_capture(Path(arguments[-1]), text=self.plain[:-1])
            return subprocess.CompletedProcess(arguments, 0)
        with mock.patch('builtins.input', return_value=''), \
             mock.patch.object(manual.subprocess, 'run', side_effect=call), \
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(manual.capture(self.run / 'helper', TOKEN, self.plain, self.run, self.run), 1)
        self.assertFalse(json.loads((self.run / 'coverage.json').read_text())['fixtureTextExact'])

    def test_native_timeout_is_failure_with_private_diagnostics_and_no_accepted_result(self):
        with mock.patch('builtins.input', return_value=''), \
             mock.patch.object(manual.subprocess, 'run', side_effect=subprocess.TimeoutExpired(['helper'], 30)), \
             contextlib.redirect_stdout(io.StringIO()):
            result = manual.capture(self.run / 'helper', TOKEN, self.plain, self.run, self.run)
        self.assertEqual(result, 124)
        metadata = self.run / 'helper-timeout.json'
        self.assertEqual(json.loads(metadata.read_text())['status'], 'timed_out')
        self.assertEqual(stat.S_IMODE(metadata.stat().st_mode), 0o600)
        self.assertFalse((self.run / 'coverage.json').exists())

    def main_context(self, args, stdin_tty=True):
        stack = contextlib.ExitStack()
        stack.enter_context(mock.patch.object(manual.sys, 'argv', ['manual-archive', *args]))
        stack.enter_context(mock.patch.object(manual.sys, 'platform', 'darwin'))
        stack.enter_context(mock.patch.object(manual.sys.stdin, 'isatty', return_value=stdin_tty))
        stack.enter_context(mock.patch.dict(manual.os.environ, {'TERM_PROGRAM': 'Apple_Terminal'}))
        stack.enter_context(mock.patch.object(manual.os, 'umask'))
        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
        stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
        return stack

    def test_recapture_reuses_fixture_and_compiles_current_helper_in_unique_attempts(self):
        (self.run / 'capture').mkdir(mode=0o700)
        manual.private_write(self.run / 'capture/original.txt', 'prior capture must remain unchanged')
        before = {name: (self.run / name).read_bytes() for name in ('fixture.txt', 'manifest.json')}
        captures, compiles = [], []
        def call(arguments, **kwargs):
            if arguments[0] == 'clang':
                compiles.append(arguments)
            elif '--selftest' in arguments:
                pass
            elif '--capture' in arguments:
                target = Path(arguments[-1])
                self.assertFalse(target.exists())
                self.fake_capture(target)
                captures.append(target)
            else:
                self.fail(f'Unexpected process invocation: {arguments}')
            return subprocess.CompletedProcess(arguments, 0)
        for _ in range(2):
            with self.main_context(['--recapture', str(self.run)]), \
                 mock.patch('builtins.input', return_value='') as confirmation, \
                 mock.patch.object(manual.subprocess, 'run', side_effect=call):
                self.assertEqual(manual.main(), 0)
            confirmation.assert_called_once()
        self.assertEqual(len(captures), 2)
        self.assertNotEqual(captures[0], captures[1])
        for target, compile_args in zip(captures, compiles):
            self.assertEqual(target.parent.parent, self.run)
            self.assertIn(str(ROOT / 'tools/mac_terminal_archive.m'), compile_args)
            self.assertEqual(compile_args[-1], str(target.parent / 'archive-helper'))
            self.assertEqual(stat.S_IMODE(target.parent.stat().st_mode), 0o700)
            self.assertTrue((target.parent / 'coverage.json').exists())
        self.assertEqual(before, {name: (self.run / name).read_bytes() for name in before})
        self.assertEqual((self.run / 'capture/original.txt').read_text(), 'prior capture must remain unchanged')

    def test_recapture_requires_manual_terminal_and_does_not_compile_or_capture_when_redirected(self):
        with self.main_context(['--recapture', str(self.run)], stdin_tty=False), \
             mock.patch.object(manual.subprocess, 'run') as command:
            with self.assertRaises(SystemExit) as raised:
                manual.main()
        self.assertEqual(raised.exception.code, 2)
        command.assert_not_called()
        self.assertEqual(list(self.run.glob('attempt-*')), [])

    def test_prepare_only_compiles_selftests_without_capture_or_input(self):
        prepared = self.root / 'prepared'
        prepared.mkdir(mode=0o700)
        with self.main_context(['--prepare-only'], stdin_tty=False), \
             mock.patch.object(manual.tempfile, 'mkdtemp', return_value=str(prepared)), \
             mock.patch('builtins.input', side_effect=AssertionError('unexpected user input')), \
             mock.patch.object(manual.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0)) as command:
            self.assertEqual(manual.main(), 0)
        self.assertEqual(command.call_count, 2)
        self.assertEqual(command.call_args_list[0].args[0][0], 'clang')
        self.assertEqual(command.call_args_list[1].args[0][-1], '--selftest')
        self.assertNotIn('--capture', repr(command.call_args_list))
        self.assertFalse((prepared / 'capture').exists())

    def original_capture(self):
        target = self.run / 'capture'
        target.mkdir(mode=0o700)
        manual.private_write(target / 'original.rtf', r'{\rtf1 synthetic retained RTF}')
        manual.private_write(target / 'original.txt', self.plain)
        return target

    def test_offline_decode_snapshots_saved_raw_data_without_terminal_input_or_capture(self):
        original = self.original_capture()
        before = {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in original.iterdir()}
        calls, attempts = [], []
        def command(arguments, **kwargs):
            calls.append(arguments)
            if '--decode' in arguments:
                self.assertEqual(arguments[4], TOKEN)
                self.assertEqual(Path(arguments[2]).read_bytes(), before['original.rtf'][0])
                self.assertEqual(Path(arguments[3]).read_bytes(), before['original.txt'][0])
                self.assertNotEqual(Path(arguments[2]).parent, original)
                target = Path(arguments[-1])
                attempts.append(target.parent)
                self.fake_capture(target)
            elif arguments[0] != 'clang' and arguments[-1] != '--selftest':
                self.fail(f'Unexpected command: {arguments}')
            return subprocess.CompletedProcess(arguments, 0)
        for _ in range(2):
            with self.main_context(['--decode-capture', str(original)], stdin_tty=False), \
                 mock.patch.dict(manual.os.environ, {'TERM_PROGRAM': 'not-terminal'}), \
                 mock.patch('builtins.input', side_effect=AssertionError('offline path requested input')), \
                 mock.patch.object(manual.subprocess, 'run', side_effect=command):
                self.assertEqual(manual.main(), 0)
        self.assertNotIn('--capture', repr(calls))
        self.assertEqual(len(attempts), 2)
        self.assertNotEqual(attempts[0], attempts[1])
        for attempt in attempts:
            self.assertEqual(attempt.parent, self.run)
            self.assertTrue((attempt / 'coverage.json').exists())
            self.assertEqual(stat.S_IMODE((attempt / 'input.rtf').stat().st_mode), 0o600)
        self.assertEqual(before, {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in original.iterdir()})

    def test_saved_capture_accepts_recorded_recapture_and_offline_attempts(self):
        for name, origin_key in (('attempt-recapture', 'recaptureOf'), ('attempt-decoded', 'sourceRun')):
            parent = self.run / name
            parent.mkdir(mode=0o700)
            target = parent / 'capture'
            target.mkdir(mode=0o700)
            manual.private_write(parent / 'attempt.json', json.dumps({'fixtureToken': TOKEN,
                origin_key: str(self.run), 'fullTerminalState': False}))
            manual.private_write(target / 'original.rtf', 'bytes')
            manual.private_write(target / 'original.txt', self.plain)
            run, token, expected, found, rtf, plain = manual.saved_capture(target)
            self.assertEqual((run, token, expected, found, rtf, plain),
                             (self.run, TOKEN, self.plain, target, b'bytes', self.plain.encode()))

    def test_saved_capture_rejects_mismatched_attempt_parent_and_raw_symlinks(self):
        parent = self.run / 'attempt-invalid'
        parent.mkdir(mode=0o700)
        target = parent / 'capture'
        target.mkdir(mode=0o700)
        manual.private_write(parent / 'attempt.json', json.dumps({'fixtureToken': 'a' * 32,
            'sourceRun': str(self.run), 'fullTerminalState': False}))
        with self.assertRaisesRegex(ValueError, 'does not belong'):
            manual.saved_capture(target)
        original = self.original_capture()
        (original / 'original.rtf').rename(original / 'elsewhere.rtf')
        (original / 'original.rtf').symlink_to(original / 'elsewhere.rtf')
        with self.assertRaises(OSError):
            manual.saved_capture(original)

    def test_oversized_saved_capture_is_refused_before_compile_or_decode(self):
        original = self.original_capture()
        with (original / 'original.rtf').open('wb') as stream:
            stream.truncate(8 * 1024 * 1024 + 1)
        with self.main_context(['--decode-capture', str(original)], stdin_tty=False), \
             mock.patch.object(manual.subprocess, 'run') as command:
            with self.assertRaises(SystemExit) as raised:
                manual.main()
        self.assertEqual(raised.exception.code, 2)
        command.assert_not_called()
        self.assertEqual(list(self.run.glob('attempt-*')), [])


if __name__ == '__main__':
    unittest.main()
