#!/usr/bin/env python3
"""Runner checks; subprocess cases use only disposable Python children."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verify_mac_slurp', ROOT / 'tools/verify_mac_slurp.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hc-runner-unit-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        output = contextlib.redirect_stdout(io.StringIO())
        output.__enter__()
        self.addCleanup(output.__exit__, None, None, None)

    def runner(self):
        return verify.Verification(self.root, 'offline')

    def test_private_atomic_reports_and_honest_scope(self):
        runner = self.runner()
        runner.record('offline', 'passed', 'Synthetic check passed.')
        runner.record('native', 'skipped', 'Excluded by --offline.', selected=False)
        self.assertEqual(runner.finish(), 0)
        report = json.loads((self.root / 'report.json').read_text())
        self.assertEqual(report['status'], 'passed')
        self.assertEqual(report['scope'], 'offline')
        for key in verify.FALSE_SCOPE:
            self.assertFalse(report[key], key)
        self.assertEqual(report['stages'][1]['status'], 'skipped')
        for filename in ('report.json', 'SUMMARY.txt'):
            self.assertEqual(stat.S_IMODE((self.root / filename).stat().st_mode), 0o600)
        self.assertEqual(list(self.root.glob('*.tmp')), [])
        self.assertIn('does not establish full Terminal state', (self.root / 'SUMMARY.txt').read_text())

    def test_outcome_keeps_failures_incomplete_and_excluded_scopes_distinct(self):
        passed = {'status': 'passed', 'selected': True}
        skipped = {'status': 'skipped', 'selected': False}
        self.assertEqual(verify.outcome([passed, skipped]), ('passed', 0))
        self.assertEqual(verify.outcome([passed, {'status': 'incomplete', 'selected': True}]), ('incomplete', 2))
        self.assertEqual(verify.outcome([passed, {'status': 'timed-out', 'selected': True}]), ('failed', 1))
        self.assertEqual(verify.outcome([skipped]), ('incomplete', 2))

    def test_child_environment_isolated_and_does_not_record_credentials(self):
        with mock.patch.dict(os.environ, {'HOME': '/real/home', 'ANTHROPIC_API_KEY': 'private-token',
                                         'HARDCORE_CTL': '/real/control', 'TMUX': 'real-tmux', 'CLAUDECODE': '1'}):
            env = verify.child_environment(self.root)
        self.assertEqual(env['HOME'], str(self.root / 'home'))
        self.assertNotIn('ANTHROPIC_API_KEY', env)
        self.assertNotIn('HARDCORE_CTL', env)
        self.assertNotIn('TMUX', env)
        self.assertNotIn('CLAUDECODE', env)

    def test_real_owned_child_has_devnull_stdin_private_logs_and_incremental_report(self):
        runner = self.runner()
        code = 'import sys; assert sys.stdin.read()==""; print("owned stdout"); print("owned stderr",file=sys.stderr)'
        original = verify.subprocess.Popen
        with mock.patch.object(verify.subprocess, 'Popen', wraps=original) as popen:
            stage = runner.run_stage('owned-python', [sys.executable, '-c', code], timeout=5)
        self.assertEqual(stage['status'], 'passed')
        self.assertEqual(popen.call_args.kwargs['stdin'], subprocess.DEVNULL)
        self.assertIs(popen.call_args.kwargs['start_new_session'], True)
        self.assertIs(popen.call_args.kwargs['close_fds'], True)
        self.assertEqual(Path(stage['stdoutLog']).read_text(), 'owned stdout\n')
        self.assertEqual(Path(stage['stderrLog']).read_text(), 'owned stderr\n')
        for key in ('stdoutLog', 'stderrLog'):
            self.assertEqual(stat.S_IMODE(Path(stage[key]).stat().st_mode), 0o600)
        self.assertEqual(json.loads((self.root / 'report.json').read_text())['stages'][0]['status'], 'passed')

    def test_real_owned_timeout_only_signals_the_created_process_group_and_next_stage_runs(self):
        runner = self.runner()
        original = verify.os.killpg
        with mock.patch.object(verify.os, 'killpg', wraps=original) as signals:
            stage = runner.run_stage('owned-timeout', [sys.executable, '-u', '-c',
                'import time; print("ready before timeout",flush=True); time.sleep(30)'], timeout=.15)
        self.assertEqual(stage['status'], 'timed-out')
        self.assertLess(stage['elapsedSeconds'], 5)
        self.assertIn('ready before timeout', Path(stage['stdoutLog']).read_text())
        self.assertTrue(signals.called)
        self.assertTrue(all(call.args[0] == stage['pid'] for call in signals.call_args_list))
        self.assertNotEqual(stage['pid'], os.getpgrp())
        self.assertIn('SIGTERM-owned-process-group', stage['termination'])
        self.assertEqual(runner.run_stage('after-timeout', [sys.executable, '-c', 'print("next check")'])['status'], 'passed')
        self.assertEqual(runner.finish(), 1)

    def test_stream_storage_is_bounded_while_large_child_output_is_drained(self):
        runner = self.runner()
        original = verify.collect
        def collect(process, streams, timeout):
            return original(process, streams, timeout, limit=4096)
        with mock.patch.object(verify, 'collect', side_effect=collect):
            stage = runner.run_stage('bounded-output', [sys.executable, '-c',
                'import sys; sys.stdout.write("x"*100000); sys.stderr.write("y"*100000)'], timeout=5)
        self.assertEqual(stage['status'], 'passed')
        for name, key in (('stdout', 'stdoutLog'), ('stderr', 'stderrLog')):
            self.assertEqual(Path(stage[key]).stat().st_size, 4096)
            self.assertEqual(stage['logStreams'][name]['bytesObserved'], 100000)
            self.assertEqual(stage['logStreams'][name]['bytesStored'], 4096)
            self.assertTrue(stage['logStreams'][name]['truncated'])

    def test_unexpected_collect_failure_stops_owned_child_and_records_terminal_status(self):
        runner = self.runner()
        launched = []
        original = subprocess.Popen
        def spawn(*args, **kwargs):
            process = original(*args, **kwargs)
            launched.append(process)
            return process
        with mock.patch.object(verify.subprocess, 'Popen', side_effect=spawn), \
             mock.patch.object(verify, 'collect', side_effect=OSError('synthetic collector failure')):
            stage = runner.run_stage('collector-error', [sys.executable, '-c', 'import time; time.sleep(30)'])
        self.assertEqual(stage['status'], 'failed')
        self.assertIn('collector failure', stage['reason'])
        self.assertIsNotNone(launched[0].poll())
        self.assertIn('SIGTERM-owned-process-group', stage['termination'])
        self.assertEqual(json.loads((self.root / 'report.json').read_text())['stages'][0]['status'], 'failed')
        self.assertEqual(runner.run_stage('after-collector-error', [sys.executable, '-c', 'pass'])['status'], 'passed')

    def test_exception_in_timeout_cleanup_is_retried_before_next_stage(self):
        runner = self.runner()
        original = verify.stop_owned
        attempted = []
        def stop(process, grace=3):
            attempted.append(process)
            if len(attempted) == 1:
                raise subprocess.TimeoutExpired('synthetic cleanup deadline', grace)
            return original(process, grace)
        with mock.patch.object(verify, 'stop_owned', side_effect=stop):
            stage = runner.run_stage('cleanup-error', [sys.executable, '-c', 'import time; time.sleep(30)'], timeout=.05)
        self.assertEqual(stage['status'], 'failed')
        self.assertEqual(len(attempted), 2)
        self.assertIs(attempted[0], attempted[1])
        self.assertIsNotNone(attempted[0].poll())
        self.assertNotEqual(json.loads((self.root / 'report.json').read_text())['stages'][0]['status'], 'running')

    def test_owned_direct_child_fallback_when_group_cleanup_raises(self):
        runner = self.runner()
        with mock.patch.object(verify, 'collect', side_effect=OSError('collector failed')), \
             mock.patch.object(verify, 'stop_owned', side_effect=OSError('group cleanup failed')):
            stage = runner.run_stage('fallback', [sys.executable, '-c', 'import time; time.sleep(30)'])
        self.assertEqual(stage['status'], 'failed')
        self.assertEqual(stage['exitCode'], -signal.SIGKILL)
        self.assertIn('SIGKILL-owned-direct-child-fallback', stage['termination'])
        self.assertIn('cleanup failed', stage['cleanupError'])

    def test_second_log_open_failure_closes_first_descriptor_before_returning(self):
        runner = self.runner()
        original = verify.os.open
        stdout_descriptors = []
        def open_file(path, *args, **kwargs):
            if Path(path).name == 'stderr.log':
                raise PermissionError('synthetic stderr open failure')
            fd = original(path, *args, **kwargs)
            if Path(path).name == 'stdout.log':
                stdout_descriptors.append(fd)
            return fd
        with mock.patch.object(verify.os, 'open', side_effect=open_file), \
             mock.patch.object(verify.subprocess, 'Popen') as spawn:
            stage = runner.run_stage('log-open-failure', [sys.executable, '-c', 'pass'])
        self.assertEqual(stage['status'], 'incomplete')
        spawn.assert_not_called()
        self.assertEqual(len(stdout_descriptors), 1)
        with self.assertRaises(OSError):
            os.fstat(stdout_descriptors[0])

    def test_original_interrupt_survives_failure_during_its_initial_cleanup(self):
        runner = self.runner()
        interrupted = KeyboardInterrupt('original user interrupt')
        original = verify.stop_owned
        stopped = []
        def stop(process, grace=3):
            stopped.append(process)
            if len(stopped) == 1:
                raise OSError('synthetic first interrupt-cleanup failure')
            return original(process, grace)
        with mock.patch.object(verify, 'collect', side_effect=interrupted), \
             mock.patch.object(verify, 'stop_owned', side_effect=stop):
            with self.assertRaises(KeyboardInterrupt) as raised:
                runner.run_stage('interrupted-cleanup', [sys.executable, '-c', 'import time; time.sleep(30)'])
        self.assertIs(raised.exception, interrupted)
        self.assertEqual(len(stopped), 2)
        self.assertIs(stopped[0], stopped[1])
        self.assertIsNotNone(stopped[0].poll())
        stage = json.loads((self.root / 'report.json').read_text())['stages'][0]
        self.assertNotEqual(stage['status'], 'running')
        self.assertIn('first interrupt-cleanup failure', stage['reason'])

    def test_missing_executable_records_incomplete_without_stopping_next_stage(self):
        runner = self.runner()
        stage = runner.run_stage('missing', [str(self.root / 'missing-command')])
        self.assertEqual(stage['status'], 'incomplete')
        self.assertEqual(runner.run_stage('available', [sys.executable, '-c', 'pass'])['status'], 'passed')
        self.assertEqual(runner.finish(), 2)

    def test_success_exit_without_required_probe_evidence_fails(self):
        runner = self.runner()
        stage = runner.run_stage('missing-proof', [sys.executable, '-c', 'pass'],
                                 evidence_report=self.root / 'missing.json')
        self.assertEqual(stage['status'], 'failed')
        self.assertIn('Missing or invalid probe evidence', stage['reason'])

    def test_discovery_only_looks_at_generated_originals_and_skips_newest_invalid(self):
        valid = self.root / 'hc-terminal-archive-valid/capture'
        invalid = self.root / 'hc-terminal-archive-invalid/capture'
        unrelated = self.root / 'other-data/capture'
        for path in (valid, invalid, unrelated):
            path.mkdir(parents=True)
        os.utime(valid, (100, 100))
        os.utime(invalid, (200, 200))
        visited = []
        def validate(path):
            visited.append(path)
            if path == invalid:
                raise ValueError('not a saved fixture')
            return ('validated', path)
        found, rejected = verify.discover_capture(validate, self.root)
        self.assertEqual(found, ('validated', valid))
        self.assertEqual(rejected, 1)
        self.assertEqual(visited, [invalid, valid])

    def test_snapshot_preserves_original_and_keeps_raw_data_private(self):
        original = self.root / 'original'
        original.mkdir()
        source = original / 'capture'
        source.mkdir()
        before = b'raw original bytes'
        (source / 'original.rtf').write_bytes(before)
        copy, metadata = verify.copy_capture((original, 'a' * 32, 'expected fixture', source, before, b'plain'),
                                            self.root / 'snapshot')
        self.assertEqual((source / 'original.rtf').read_bytes(), before)
        self.assertEqual((copy / 'original.rtf').read_bytes(), before)
        self.assertEqual(stat.S_IMODE(copy.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((copy / 'original.rtf').stat().st_mode), 0o600)
        self.assertTrue(metadata['historicalReplayOnly'])
        self.assertEqual(metadata['sourceCapture'], str(source))

    def test_offline_main_excludes_pty_native_and_continues_after_independent_failure(self):
        calls = []
        def check(runner, name, script, *args, **kwargs):
            calls.append(script)
            return runner.record(name, 'failed' if name == 'archive-tests' else 'passed', 'mocked offline check')
        with mock.patch.object(verify.sys, 'platform', 'darwin'), \
             mock.patch.object(verify, 'python_check', side_effect=check), \
             mock.patch.object(verify, 'discover_capture', return_value=(None, 0)):
            result = verify.main(['--offline', '--output-dir', str(self.root / 'verification')])
        self.assertEqual(result, 1)
        self.assertIn('tests/probe_mac_archive_robustness.py', calls)
        self.assertFalse(any('zsh' in value for value in calls))
        report = json.loads((self.root / 'verification/report.json').read_text())
        self.assertEqual(report['scope'], 'offline')
        skipped = [stage for stage in report['stages'] if 'owned-zsh' in stage['name'] or 'owned-native' in stage['name']]
        self.assertEqual(len(skipped), 2)
        self.assertTrue(all(stage['status'] == 'skipped' and not stage['selected'] for stage in skipped))

    def test_full_main_missing_binary_is_incomplete_and_never_builds_application(self):
        calls = []
        def check(runner, name, script, *args, **kwargs):
            calls.append(script)
            return runner.record(name, 'passed', 'mocked owned check')
        with mock.patch.object(verify.sys, 'platform', 'darwin'), \
             mock.patch.object(verify, 'python_check', side_effect=check), \
             mock.patch.object(verify, 'discover_capture', return_value=(None, 0)), \
             mock.patch.object(verify.subprocess, 'Popen', side_effect=AssertionError('unplanned build/process')):
            result = verify.main(['--executable', str(self.root / 'missing-binary'),
                                  '--output-dir', str(self.root / 'verification')])
        self.assertEqual(result, 2)
        report = json.loads((self.root / 'verification/report.json').read_text())
        native = next(stage for stage in report['stages'] if stage['name'] == 'owned-native-receiver')
        self.assertEqual(native['status'], 'incomplete')
        self.assertIn('make -C', native['reason'])
        self.assertNotIn('tests/probe_mac_zsh_hardcore.py', calls)

    def test_repeat_bounds_reject_before_creating_results(self):
        for value in ('0', '11'):
            with self.subTest(value=value), contextlib.redirect_stderr(io.StringIO()), \
                 mock.patch.object(verify.tempfile, 'mkdtemp') as create:
                with self.assertRaises(SystemExit):
                    verify.main(['--repeat', value])
            create.assert_not_called()


if __name__ == '__main__':
    unittest.main()
