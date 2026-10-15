#!/usr/bin/env python3
"""Manual fixture safety checks; no Terminal, Claude, model, or GUI is started."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import types
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'manual_terminal_handoff', Path(__file__).with_name('manual_mac_terminal_handoff.py'))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

SID = '11111111-1111-4111-8111-111111111111'
DEST = '22222222-2222-4222-8222-222222222222'
REQUEST = '0123456789abcdef' * 2
SOURCE_PID = 12345
SOURCE_IDENTITY = 'owned source instance'
NOW_MS = 1_000_000


def ready_state(session=SID):
    return {'sessionId': session, 'prompt': {'text': fixture.DRAFT, 'cursor': fixture.CURSOR},
            'observedAt': NOW_MS}


def ready_turn():
    return {'sessionId': SID, 'reason': 'answer', 'answer': 'HC_TERMINAL_READY',
            'isAborted': False, 'turnId': 'fixture-turn', 'observedAt': NOW_MS - 100}


def verified_record():
    return {'sourceSessionId': SID, 'sourcePid': SOURCE_PID, 'sourceIdentity': SOURCE_IDENTITY,
            'requestId': REQUEST, 'destinationSession': DEST, 'state': 'attached',
            'windowPid': 34567, 'receiverPid': 45678, 'receiverTty': '/dev/ttys999',
            'snapshotVerifiedAt': NOW_MS / 1000 - .1, 'inputReleasedAt': NOW_MS / 1000,
            'nativeReceipt': {'ok': True, 'rendered': True, 'requestId': REQUEST,
                              'windowPid': 34567, 'receiverPid': 45678, 'receiverTty': '/dev/ttys999'},
            'snapshot': {'draft': {'text': fixture.DRAFT, 'cursor': fixture.CURSOR},
                         'history': {'format': 'api'}}}


class ManualFixtureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='hc-manual-fixture-test-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()

    def test_launch_inherits_real_standard_descriptors_and_process_group(self):
        sentinel = object()
        with mock.patch.object(fixture.subprocess, 'Popen', return_value=sentinel) as launch:
            result = fixture.launch_source('/mock/claude', self.root, {'TERM': 'xterm-256color'})
        self.assertIs(result, sentinel)
        arguments, options = launch.call_args
        self.assertEqual(arguments[0][0], '/mock/claude')
        self.assertEqual(options['cwd'], self.root)
        self.assertEqual({name: options[name] for name in ('stdin', 'stdout', 'stderr')},
                         {'stdin': None, 'stdout': None, 'stderr': None})
        for unsafe in ('preexec_fn', 'start_new_session', 'process_group', 'shell', 'pass_fds'):
            self.assertNotIn(unsafe, options)
        self.assertNotIn('--plugin-dir', arguments[0], 'Adapter must be installed after source starts')
        self.assertNotIn('host', arguments[0])
        self.assertEqual(arguments[0][-2:], ['--tools', ''])

    def test_private_environment_drops_parent_agent_and_host_configuration(self):
        original = {'PATH': '/safe/bin', 'TERM': 'xterm-256color', 'TERM_PROGRAM': 'Apple_Terminal',
                    'HOME': '/real/home', 'CLAUDE_CONFIG_DIR': '/real/claude', 'CLAUDECODE': '1',
                    'ANTHROPIC_API_KEY': 'secret', 'ANTHROPIC_BASE_URL': 'https://example.invalid',
                    'HARDCORE_WINDOW_PID': '99', 'HARDCORE_CTL': '/real/control',
                    'TMUX': '/real/socket', 'TMUX_PANE': '%1'}
        before = original.copy()
        env = fixture.private_environment(self.root, original)
        self.assertEqual(original, before)
        for unwanted in ('CLAUDECODE', 'ANTHROPIC_API_KEY', 'ANTHROPIC_BASE_URL',
                         'HARDCORE_WINDOW_PID', 'HARDCORE_CTL', 'TMUX', 'TMUX_PANE'):
            self.assertNotIn(unwanted, env)
        self.assertEqual(env['HOME'], str(self.root / 'home'))
        self.assertEqual(env['CLAUDE_CONFIG_DIR'], str(self.root / 'config'))
        self.assertEqual(env['HARDCORE_RUNTIME_DIR'], str(self.root / 'run'))
        self.assertEqual(env['PATH'], '/safe/bin')
        self.assertEqual(env['TERM_PROGRAM'], 'Apple_Terminal')
        self.assertNotIn('secret', json.dumps(env))

    def test_copy_login_is_private_and_copies_only_onboarding_fields(self):
        home, config = self.root / 'original', self.root / 'config'
        (home / '.claude').mkdir(parents=True)
        config.mkdir()
        secret = b'{"accessToken":"fixture-secret-never-print"}'
        (home / '.claude/.credentials.json').write_bytes(secret)
        (home / '.claude.json').write_text(json.dumps({'theme': 'dark', 'hasCompletedOnboarding': True,
            'oauthAccount': {'id': 'fixture-account'}, 'projects': {'private-path': {}},
            'apiKey': 'unrelated-secret', 'enabledPlugins': {'unrelated': True}}))
        output = io.StringIO()
        with mock.patch.object(fixture.subprocess, 'run') as command, \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            fixture.copy_login(config, home)
        command.assert_not_called()
        self.assertEqual(output.getvalue(), '')
        self.assertEqual((config / '.credentials.json').read_bytes(), secret)
        for name in ('.claude.json', '.credentials.json'):
            self.assertEqual(stat.S_IMODE((config / name).stat().st_mode), 0o600)
        saved = json.loads((config / '.claude.json').read_text())
        self.assertEqual(saved, {'theme': 'dark', 'hasCompletedOnboarding': True,
                                 'oauthAccount': {'id': 'fixture-account'}})

    def test_keychain_authentication_is_captured_without_printing(self):
        config = self.root / 'config'
        config.mkdir()
        output = io.StringIO()
        result = subprocess.CompletedProcess([], 0, stdout=b'{"token":"fixture-secret"}\n', stderr=b'')
        with mock.patch.object(fixture.subprocess, 'run', return_value=result) as command, \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            fixture.copy_login(config, self.root / 'absent-original-home')
        self.assertEqual(output.getvalue(), '')
        self.assertTrue(command.call_args.kwargs['capture_output'])
        self.assertEqual((config / '.credentials.json').read_bytes(), b'{"token":"fixture-secret"}')
        self.assertEqual(stat.S_IMODE((config / '.credentials.json').stat().st_mode), 0o600)

    def test_copy_login_refuses_to_overwrite_existing_credentials(self):
        home, config = self.root / 'original', self.root / 'config'
        (home / '.claude').mkdir(parents=True)
        config.mkdir()
        (home / '.claude/.credentials.json').write_bytes(b'new')
        (config / '.credentials.json').write_bytes(b'keep')
        with self.assertRaises(FileExistsError):
            fixture.copy_login(config, home)
        self.assertEqual((config / '.credentials.json').read_bytes(), b'keep')

    def test_ready_requires_exact_unicode_text_and_middle_utf16_cursor(self):
        self.assertEqual(fixture.CURSOR, len(fixture.DRAFT.encode('utf-16-le')) // 2 - 3)
        self.assertTrue(fixture.draft_ready(ready_state(), ready_turn(), SID, NOW_MS + 100))
        for prompt in ({'text': fixture.DRAFT + '\n', 'cursor': fixture.CURSOR},
                       {'text': fixture.DRAFT.replace('é', 'e'), 'cursor': fixture.CURSOR},
                       {'text': fixture.DRAFT, 'cursor': fixture.CURSOR + 1},
                       {'text': fixture.DRAFT, 'cursor': len(fixture.DRAFT)},
                       {'text': '', 'cursor': 0}):
            with self.subTest(prompt=prompt):
                self.assertFalse(fixture.draft_ready({**ready_state(), 'prompt': prompt}, ready_turn(), SID, NOW_MS))

    def test_ready_rejects_wrong_session_stale_future_or_incomplete_observations(self):
        for state in (None, [], {}, {**ready_state(), 'sessionId': DEST},
                      {**ready_state(), 'observedAt': NOW_MS - 3000},
                      {**ready_state(), 'observedAt': NOW_MS + 1},
                      {**ready_state(), 'observedAt': True},
                      {**ready_state(), 'observedAt': float('nan')}):
            with self.subTest(state=state):
                self.assertFalse(fixture.draft_ready(state, ready_turn(), SID, NOW_MS))
        for changes in ({'sessionId': DEST}, {'reason': 'tool'}, {'isAborted': True},
                        {'isAborted': None}, {'answer': 'Something else'}, {'answer': None}):
            with self.subTest(changes=changes):
                self.assertFalse(fixture.draft_ready(ready_state(), {**ready_turn(), **changes}, SID, NOW_MS))

    def test_ready_requires_fresh_finite_completion_observed_before_draft(self):
        for timestamp in (None, True, float('nan'), float('inf'),
                          NOW_MS - 20 * 60 * 1000, NOW_MS + 1):
            with self.subTest(timestamp=timestamp):
                turn = {**ready_turn(), 'observedAt': timestamp}
                self.assertFalse(fixture.draft_ready(ready_state(), turn, SID, NOW_MS))
        state = {**ready_state(), 'observedAt': NOW_MS - 200}
        self.assertFalse(fixture.draft_ready(state, ready_turn(), SID, NOW_MS))

    def test_verification_requires_exact_source_rendered_receipt_and_input_release(self):
        self.assertTrue(fixture.record_verified(verified_record(), SID, SOURCE_PID, SOURCE_IDENTITY))
        for changes in ({'sourceSessionId': DEST}, {'sourcePid': SOURCE_PID + 1},
                        {'sourceIdentity': 'reused PID'}, {'state': 'attaching'},
                        {'snapshotVerifiedAt': None}, {'inputReleasedAt': None},
                        {'nativeReceipt': None}, {'nativeReceipt': {'rendered': False}}):
            with self.subTest(changes=changes):
                record = {**verified_record(), **changes}
                self.assertFalse(fixture.record_verified(record, SID, SOURCE_PID, SOURCE_IDENTITY))

    def test_verification_rejects_draft_cursor_or_history_format_loss(self):
        for section, replacement in (
            ('draft', {'text': fixture.DRAFT, 'cursor': fixture.CURSOR + 1}),
            ('draft', {'text': 'changed', 'cursor': fixture.CURSOR}),
            ('history', {'format': 'text'}), ('history', None)):
            with self.subTest(section=section, replacement=replacement):
                record = verified_record()
                record['snapshot'][section] = replacement
                self.assertFalse(fixture.record_verified(record, SID, SOURCE_PID, SOURCE_IDENTITY))

    def test_verification_requires_receipt_bound_to_exact_request_window_client_and_tty(self):
        for field, mismatch in (('requestId', 'f' * 32), ('windowPid', 34568),
                                ('receiverPid', 45679), ('receiverTty', '/dev/ttys998'), ('ok', False)):
            with self.subTest(field=field):
                record = verified_record()
                record['nativeReceipt'][field] = mismatch
                self.assertFalse(fixture.record_verified(record, SID, SOURCE_PID, SOURCE_IDENTITY))
        record = verified_record()
        del record['receiverTty']
        del record['nativeReceipt']['receiverTty']
        self.assertFalse(fixture.record_verified(record, SID, SOURCE_PID, SOURCE_IDENTITY))

    def test_verification_handles_incomplete_or_malformed_records(self):
        for record in (None, [], {}, {**verified_record(), 'snapshot': ['bad']},
                       {**verified_record(), 'nativeReceipt': 'bad'},
                       {**verified_record(), 'snapshot': {'history': 'bad'}}):
            with self.subTest(record=record):
                self.assertFalse(fixture.record_verified(record, SID, SOURCE_PID, SOURCE_IDENTITY))

    def test_partial_and_oversized_observer_files_are_retryable(self):
        path = self.root / 'observation.json'
        self.assertIsNone(fixture.read_json(path))
        for data in ('{', '[]', 'null', 'x' * (4 * 1024 * 1024 + 1)):
            path.write_text(data)
            self.assertIsNone(fixture.read_json(path))
        path.write_text('{"ok":true}')
        self.assertEqual(fixture.read_json(path), {'ok': True})

    def monitor_run(self, inspection=None, source_identity=SOURCE_IDENTITY, source_exit=0):
        run = fixture.Run(self.root, '/mock/claude', '/mock/hardcore')
        now = [100.0]
        exited = [False]
        run.process = types.SimpleNamespace(pid=SOURCE_PID, returncode=source_exit,
            poll=lambda: source_exit if exited[0] else None)
        run.save = lambda **changes: run.data.update(changes)
        def command(arguments, log, executable=None):
            if arguments[:2] == ['claude', 'move']:
                exited[0] = True
                return json.dumps({'intentId': REQUEST})
            if arguments[:2] == ['claude', 'inspect']:
                return json.dumps(inspection or {'destinationAvailable': True, 'clientAvailable': True})
            raise AssertionError(arguments)
        run.cli = mock.Mock(side_effect=command)
        fixture.write_json(run.project / f'terminal-probe-state-{SID}.json', ready_state())
        fixture.write_json(run.project / f'terminal-probe-turn-{SID}.json', ready_turn())
        fixture.write_json(run.project / f'terminal-probe-state-{DEST}.json', ready_state(DEST))
        requests = run.root / 'home/Library/Caches/hardcore/claude-handoff'
        requests.mkdir(parents=True)
        fixture.write_json(requests / (REQUEST + '.json'), verified_record())
        for patcher in (
            mock.patch.object(fixture, 'identity', return_value=source_identity),
            mock.patch.object(fixture.time, 'time', return_value=NOW_MS / 1000),
            mock.patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]),
            mock.patch.object(fixture.time, 'sleep', side_effect=lambda seconds: now.__setitem__(0, now[0] + seconds))):
            patcher.start()
            self.addCleanup(patcher.stop)
        return run

    def test_monitor_passes_only_after_verified_live_destination_and_normal_source_exit(self):
        run = self.monitor_run()
        run.monitor(SID, SOURCE_IDENTITY)
        self.assertEqual(run.data['status'], 'passed')
        self.assertEqual(run.data['sourceExitCode'], 0)
        self.assertTrue(run.data['draftVerified'])
        self.assertFalse(run.data['terminalClosed'])
        self.assertFalse(run.data['shellStateTransferred'])
        self.assertEqual([call.args[0][:2] for call in run.cli.call_args_list],
                         [['claude', 'move'], ['claude', 'inspect']])

    def test_monitor_does_not_queue_after_source_identity_changes(self):
        run = self.monitor_run(source_identity='different process')
        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
            run.monitor(SID, SOURCE_IDENTITY)
        run.cli.assert_not_called()
        self.assertNotEqual(run.data['status'], 'passed')

    def test_monitor_rejects_saved_success_when_receiver_is_no_longer_live(self):
        run = self.monitor_run(inspection={'destinationAvailable': True, 'clientAvailable': False})
        with self.assertRaisesRegex(RuntimeError, 'no longer available'):
            run.monitor(SID, SOURCE_IDENTITY)
        self.assertNotEqual(run.data['status'], 'passed')

    def test_monitor_rejects_abnormal_source_exit(self):
        run = self.monitor_run(source_exit=7)
        with self.assertRaisesRegex(RuntimeError, 'unexpected status 7'):
            run.monitor(SID, SOURCE_IDENTITY)
        self.assertNotEqual(run.data['status'], 'passed')

    def test_monitor_rejects_stale_destination_readback_even_with_saved_receipt(self):
        run = self.monitor_run()
        fixture.write_json(run.project / f'terminal-probe-state-{DEST}.json',
                           {**ready_state(DEST), 'observedAt': NOW_MS - 4000})
        with self.assertRaisesRegex(RuntimeError, 'did not finish verification'):
            run.monitor(SID, SOURCE_IDENTITY)
        self.assertNotEqual(run.data['status'], 'passed')
        self.assertEqual(run.cli.call_count, 1, 'Stale prompt readback must not reach live-success inspection')

    def test_monitor_requires_destination_readback_after_snapshot_verification(self):
        run = self.monitor_run()
        fixture.write_json(run.project / f'terminal-probe-state-{DEST}.json',
                           {**ready_state(DEST), 'observedAt': NOW_MS - 200})
        with self.assertRaisesRegex(RuntimeError, 'did not finish verification'):
            run.monitor(SID, SOURCE_IDENTITY)
        self.assertNotEqual(run.data['status'], 'passed')

    def test_progress_distinguishes_reload_reply_draft_cursor_and_ready(self):
        cases = (
            (None, None, 'waiting-for-reload'),
            ({**ready_state(), 'sessionId': DEST}, ready_turn(), 'waiting-for-reload'),
            ({**ready_state(), 'observedAt': NOW_MS - 4000}, ready_turn(), 'waiting-for-observer'),
            (ready_state(), None, 'waiting-for-test-reply'),
            (ready_state(), {**ready_turn(), 'isAborted': True}, 'waiting-for-test-reply'),
            ({**ready_state(), 'prompt': {'text': '', 'cursor': 0}}, ready_turn(), 'waiting-for-draft'),
            ({**ready_state(), 'prompt': {'text': 'almost correct', 'cursor': 2}}, ready_turn(), 'waiting-for-draft'),
            ({**ready_state(), 'prompt': {'text': fixture.DRAFT, 'cursor': fixture.CURSOR + 3}},
                ready_turn(), 'waiting-for-cursor'),
            (ready_state(), ready_turn(), 'draft-ready'),
        )
        for state, turn, expected in cases:
            with self.subTest(expected=expected, state=state):
                self.assertEqual(fixture.observation_phase(state, turn, SID, NOW_MS), expected)

    def test_temporary_missing_observation_does_not_forget_completed_reload(self):
        self.assertEqual(fixture.observation_phase(None, None, SID, NOW_MS, observer_loaded=True),
                         'waiting-for-observer')

    def test_phase_transitions_persist_without_copying_arbitrary_drafts_to_status(self):
        run = fixture.Run(self.root, '/mock/claude', '/mock/hardcore')
        run.save(status='waiting-for-reload', sourceSessionId=SID)
        run.save(status='waiting-for-draft', sourceDraft={'prompt': {'text': 'private arbitrary draft'}})
        run.save(status='waiting-for-draft', anotherObservation=True)
        saved = fixture.read_json(self.root / 'result.json')
        self.assertEqual([row['phase'] for row in saved['phaseHistory']],
                         ['preparing', 'waiting-for-reload', 'waiting-for-draft'])
        self.assertTrue(all(isinstance(row['at'], (int, float)) for row in saved['phaseHistory']))
        status = (self.root / 'STATUS.txt').read_text()
        self.assertIn('Phase: waiting-for-draft', status)
        self.assertIn('without Return', status)
        self.assertNotIn('private arbitrary draft', status)
        self.assertEqual(stat.S_IMODE((self.root / 'STATUS.txt').stat().st_mode), 0o600)

    def test_status_reads_latest_observations_without_contacting_or_changing_anything(self):
        run = fixture.Run(self.root, '/mock/claude', '/mock/hardcore')
        run.save(status='waiting-for-reload', sourceSessionId=SID)
        fixture.write_json(run.project / f'terminal-probe-state-{SID}.json', ready_state())
        fixture.write_json(run.project / f'terminal-probe-turn-{SID}.json', ready_turn())
        tracked = [path for path in self.root.rglob('*') if path.is_file()]
        before = {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in tracked}
        output = io.StringIO()
        with mock.patch.object(fixture.time, 'time', return_value=NOW_MS / 1000), \
             mock.patch.object(fixture, 'write_json', side_effect=AssertionError('status wrote data')), \
             mock.patch.object(fixture.subprocess, 'run', side_effect=AssertionError('status launched a command')), \
             mock.patch.object(fixture.subprocess, 'Popen', side_effect=AssertionError('status launched a process')), \
             mock.patch.object(fixture.os, 'kill', side_effect=AssertionError('status signalled a process')), \
             contextlib.redirect_stdout(output):
            fixture.show_status(self.root)
        self.assertIn('Phase: draft-ready', output.getvalue())
        self.assertIn('Watcher last saved phase: waiting-for-reload', output.getvalue())
        self.assertIn('Stop typing', output.getvalue())
        self.assertEqual(before, {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in tracked})

    def test_status_does_not_print_arbitrary_composer_text(self):
        run = fixture.Run(self.root, '/mock/claude', '/mock/hardcore')
        run.save(status='waiting-for-draft', sourceSessionId=SID)
        fixture.write_json(run.project / f'terminal-probe-state-{SID}.json',
            {**ready_state(), 'prompt': {'text': 'sensitive text must remain private', 'cursor': 3}})
        fixture.write_json(run.project / f'terminal-probe-turn-{SID}.json', ready_turn())
        with mock.patch.object(fixture.time, 'time', return_value=NOW_MS / 1000):
            text = fixture.status_text(self.root, run.data, observe=True)
        self.assertIn('Phase: waiting-for-draft', text)
        self.assertNotIn('sensitive text', text)

    def test_status_cli_works_without_tty_claude_or_macos_and_does_not_write(self):
        run = fixture.Run(self.root, '/missing/claude', '/missing/hardcore')
        run.save(status='passed')
        output = io.StringIO()
        with mock.patch.object(fixture.sys, 'argv', ['manual-fixture', '--status', str(self.root)]), \
             mock.patch.object(fixture.sys, 'platform', 'linux'), \
             mock.patch.object(fixture.shutil, 'which', side_effect=AssertionError('dependency lookup')), \
             mock.patch.object(fixture.os, 'isatty', side_effect=AssertionError('TTY access')), \
             mock.patch.object(fixture, 'write_json', side_effect=AssertionError('status mutation')), \
             contextlib.redirect_stdout(output):
            self.assertEqual(fixture.main(), 0)
        self.assertIn('Phase: passed', output.getvalue())
        self.assertIn('saved outcome', output.getvalue())
        self.assertIn('does not check whether processes are still running', output.getvalue())

    def test_status_rejects_an_unrecognized_run(self):
        fixture.write_json(self.root / 'result.json', {'format': 'other', 'root': str(self.root)})
        with self.assertRaisesRegex(RuntimeError, 'Not an owned private'):
            fixture.show_status(self.root)

    def test_monitor_saves_draft_readiness_before_requesting_move(self):
        run = self.monitor_run()
        phases = []
        def save(**changes):
            run.data.update(changes)
            if 'status' in changes:
                phases.append(changes['status'])
        run.save = save
        run.monitor(SID, SOURCE_IDENTITY)
        self.assertEqual(phases, ['draft-ready', 'requesting-move', 'waiting-for-verification', 'passed'])
        self.assertEqual(run.data['reloadObservedAt'], NOW_MS / 1000)

    def test_cleanup_refuses_unowned_or_non_fixture_directory_without_process_changes(self):
        fixture.write_json(self.root / 'result.json', {'format': 'other', 'root': str(self.root)})
        with mock.patch.object(fixture.os, 'kill') as kill, mock.patch.object(fixture.subprocess, 'run') as command:
            with self.assertRaisesRegex(RuntimeError, 'Not an owned private'):
                fixture.cleanup(self.root)
        kill.assert_not_called()
        command.assert_not_called()

    def test_failed_background_stop_retains_login_and_does_not_signal_other_processes(self):
        (self.root / 'config').mkdir()
        secret = self.root / 'config/.credentials.json'
        secret.write_text('keep credential until stop succeeds')
        fixture.write_json(self.root / 'result.json', {
            'format': fixture.FORMAT, 'root': str(self.root), 'claude': '/mock/claude',
            'project': str(self.root), 'handoff': {'destinationPid': 23456,
                'destinationIdentity': 'owned destination', 'jobId': 'disposable-job'}})
        with mock.patch.object(fixture, 'identity', side_effect=lambda pid: 'owned destination' if pid == 23456 else None), \
             mock.patch.object(fixture.os, 'kill') as kill, \
             mock.patch.object(fixture.subprocess, 'run', side_effect=[
                 subprocess.CompletedProcess([], 0, stdout='[]'), subprocess.CompletedProcess([], 1)]):
            with self.assertRaisesRegex(RuntimeError, 'credentials retained'):
                fixture.cleanup(self.root)
        kill.assert_not_called()
        self.assertTrue(secret.exists())

    def cleanup_fixture(self, record=None):
        (self.root / 'config').mkdir()
        (self.root / 'config/backups').mkdir()
        for name in ('.credentials.json', '.claude.json', 'backups/auth.json'):
            (self.root / 'config' / name).write_text('fixture authentication')
        data = {'format': fixture.FORMAT, 'root': str(self.root), 'claude': '/mock/claude',
                'project': str(self.root), 'sourcePid': SOURCE_PID, 'sourceIdentity': SOURCE_IDENTITY,
                'handoff': record or {'destinationPid': 23456, 'destinationIdentity': 'destination',
                    'windowPid': 34567, 'windowIdentity': 'window', 'receiverPid': 45678,
                    'receiverIdentity': 'receiver', 'jobId': 'fixture-job'}}
        fixture.write_json(self.root / 'result.json', data)
        return data

    def test_cleanup_rejects_reused_source_pid_before_any_stop_or_signal(self):
        self.cleanup_fixture()
        with mock.patch.object(fixture, 'identity', side_effect=lambda pid: 'reused source PID' if pid == SOURCE_PID else None), \
             mock.patch.object(fixture.os, 'kill') as kill, mock.patch.object(fixture.subprocess, 'run') as command:
            with self.assertRaisesRegex(RuntimeError, 'identity changed'):
                fixture.cleanup(self.root)
        kill.assert_not_called()
        command.assert_not_called()
        self.assertTrue((self.root / 'config/.credentials.json').exists())

    def test_cleanup_retains_auth_for_live_receiver_without_bound_identity(self):
        self.cleanup_fixture(record={'receiverPid': 45678})
        with mock.patch.object(fixture, 'identity', side_effect=lambda pid: 'unknown receiver' if pid == 45678 else None), \
             mock.patch.object(fixture.os, 'kill') as kill, mock.patch.object(fixture.subprocess, 'run') as command:
            with self.assertRaisesRegex(RuntimeError, 'identity changed'):
                fixture.cleanup(self.root)
        kill.assert_not_called()
        command.assert_not_called()
        self.assertTrue((self.root / 'config/.credentials.json').exists())

    def test_cleanup_retains_auth_for_unrecorded_project_process(self):
        self.cleanup_fixture()
        with mock.patch.object(fixture, 'identity', side_effect=lambda pid: 'unexpected process' if pid == 56789 else None), \
             mock.patch.object(fixture.os, 'kill') as kill, \
             mock.patch.object(fixture.subprocess, 'run', return_value=subprocess.CompletedProcess(
                 [], 0, stdout='[{"pid":56789}]')) as command:
            with self.assertRaisesRegex(RuntimeError, 'unrecorded process'):
                fixture.cleanup(self.root)
        kill.assert_not_called()
        self.assertEqual(command.call_count, 1)
        self.assertTrue((self.root / 'config/.credentials.json').exists())

    def test_cleanup_deletes_auth_only_after_exact_owned_processes_exit(self):
        self.cleanup_fixture()
        alive = {SOURCE_PID: SOURCE_IDENTITY, 23456: 'destination', 34567: 'window', 45678: 'receiver'}
        def command(arguments, **kwargs):
            self.assertTrue((self.root / 'config/.credentials.json').exists())
            if arguments[1] == 'agents':
                return subprocess.CompletedProcess([], 0, stdout='[{"pid":23456}]')
            self.assertEqual(arguments, ['/mock/claude', 'stop', 'fixture-job'])
            alive.pop(23456)
            return subprocess.CompletedProcess([], 0)
        def stop(pid, signum):
            self.assertTrue((self.root / 'config/.credentials.json').exists())
            self.assertEqual(signum, fixture.signal.SIGTERM)
            alive.pop(pid)
        with mock.patch.object(fixture, 'identity', side_effect=alive.get), \
             mock.patch.object(fixture.os, 'kill', side_effect=stop) as kill, \
             mock.patch.object(fixture.subprocess, 'run', side_effect=command), \
             contextlib.redirect_stdout(io.StringIO()):
            fixture.cleanup(self.root)
        self.assertEqual({call.args[0] for call in kill.call_args_list}, {SOURCE_PID, 34567, 45678})
        self.assertFalse(alive)
        self.assertFalse((self.root / 'config/.credentials.json').exists())
        self.assertFalse((self.root / 'config/.claude.json').exists())
        self.assertFalse((self.root / 'config/backups').exists())
        self.assertTrue(fixture.read_json(self.root / 'result.json')['copiedLoginRemoved'])
        self.assertIn('Phase: cleaned', (self.root / 'STATUS.txt').read_text())

    def test_cleanup_timeout_keeps_auth_while_owned_process_still_exits(self):
        self.cleanup_fixture(record={'state': 'not-started'})
        now = [100.0]
        with mock.patch.object(fixture, 'identity', side_effect=lambda pid: SOURCE_IDENTITY if pid == SOURCE_PID else None), \
             mock.patch.object(fixture.os, 'kill'), \
             mock.patch.object(fixture.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, stdout='[]')), \
             mock.patch.object(fixture.time, 'monotonic', side_effect=lambda: now[0]), \
             mock.patch.object(fixture.time, 'sleep', side_effect=lambda seconds: now.__setitem__(0, now[0] + seconds)):
            with self.assertRaisesRegex(RuntimeError, 'still exiting'):
                fixture.cleanup(self.root)
        self.assertTrue((self.root / 'config/.credentials.json').exists())


if __name__ == '__main__':
    unittest.main()
