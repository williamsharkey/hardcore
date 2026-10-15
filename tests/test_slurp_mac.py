#!/usr/bin/env python3
"""Offline automatic slurp orchestration: no real CLI/settings/session changes."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import time
import unittest
from unittest import mock
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('slurp_mac', ROOT/'tools/slurp_mac.py')
slurp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(slurp)
SID = '11111111-1111-4111-8111-111111111111'


class SlurpMacTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hc-slurp-coordinator-')
        self.addCleanup(self.temp.cleanup)
        self.project = Path(self.temp.name)/'project café;$(false)'
        self.project.mkdir()
        self.source = {'tty': '/dev/ttys012', 'window': 44, 'host': 123, 'pid': 1001,
                       'program': 'claude', 'shell': '', 'cwd': str(self.project), 'managed': False}
        self.snapshot = {'terminal': [self.source], 'hardcore': [], 'tmux': []}
        self.helper = mock.Mock()
        self.helper.process_identity.side_effect = lambda pid: 'instance-'+str(pid)
        self.helper.jobs.return_value = [self.job()]
        self.helper.valid_session_id.side_effect = lambda value: isinstance(value, str) and str(uuid.UUID(value)) == value
        self.helper.progress.return_value = {'ok': True, 'state': 'attaching', 'sourceSessionId': SID}
        self.adapter = mock.Mock()
        self.adapter.handoff_helper.return_value = self.helper
        self.adapter.bundled_marketplace.return_value = Path('/bundled plugins')
        self.result = {'ok': True, 'state': 'queued', 'intentId': 'ab'*16, 'sourceSessionId': SID}
        def move(*args, **kwargs):
            kwargs['validate_source']()
            kwargs['validate_source']()
            return dict(self.result)
        self.adapter.move.side_effect = move
        self.native = subprocess.CompletedProcess([], 0, 'native transfer report\n', '')

    def job(self, **updates):
        return {'sessionId': SID, 'pid': 1001, 'kind': 'interactive', 'cwd': str(self.project), **updates}

    def coordinate(self, *, discovery=None, native=None, terminal=None, acknowledged=False, adapter=None, bootstrap=None):
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(slurp, 'discover', side_effect=discovery) if callable(discovery) or isinstance(discovery, list) else \
             mock.patch.object(slurp, 'discover', return_value=discovery or self.snapshot), \
             mock.patch.object(slurp, 'run_native', return_value=native or self.native) as runner, \
             mock.patch.object(slurp.shutil, 'which', return_value='/mock path/claude'), \
             mock.patch.object(slurp, 'adapter_acknowledged', return_value=acknowledged):
            status = slurp.coordinate(Path('/exact app/hardcore'), terminal, adapter=adapter or self.adapter,
                                      bootstrap=bootstrap, stdout=stdout, stderr=stderr)
        return status, stdout.getvalue(), stderr.getvalue(), runner

    def test_matches_live_pid_in_shared_project_without_user_session_id(self):
        self.helper.jobs.return_value += [self.job(pid=2002, sessionId='22222222-2222-4222-8222-222222222222')]
        status, out, err, native = self.coordinate()
        self.assertEqual(status, 0, err)
        args, kwargs = self.adapter.move.call_args
        self.assertEqual(args, ('/mock path/claude', self.project.resolve(), Path('/bundled plugins'), SID))
        self.assertEqual(kwargs['expected_source_pid'], 1001)
        self.assertEqual(kwargs['expected_source_identity'], 'instance-1001')
        self.assertIn('1 Claude queued (pending)', out)
        self.assertIn('/reload-plugins', out)
        self.assertIn('native transfer report', out)
        self.assertIn('parent shell and Terminal state are not transferred', out)
        native.assert_called_once_with(Path('/exact app/hardcore'), ['--slurp-native'], timeout=120)

    def test_no_unique_exact_interactive_match_means_no_install_or_queue(self):
        for rows in ([], [self.job(pid=2002)], [self.job(kind='background')], [self.job(cwd='/different')],
                     [self.job(), self.job()], [self.job(sessionId='not-a-uuid')]):
            with self.subTest(rows=rows):
                self.adapter.move.reset_mock()
                self.helper.jobs.return_value = rows
                status, out, err, native = self.coordinate()
                self.assertEqual(status, 1)
                self.adapter.move.assert_not_called()
                native.assert_called_once()
                self.assertIn('source retained', err)

    def test_kernel_identity_changes_prevent_adapter_mutation(self):
        for change in ({'pid': 2002}, {'cwd': str(self.project.parent)}, {'host': 999}, {'window': 88},
                       {'program': 'zsh'}, {'managed': True}):
            with self.subTest(change=change):
                self.adapter.move.reset_mock()
                fresh = {**self.snapshot, 'terminal': [{**self.source, **change}]}
                status, out, err, native = self.coordinate(discovery=[self.snapshot, fresh])
                self.assertEqual(status, 1)
                self.adapter.move.assert_not_called()
                self.assertIn('source retained', err)
                native.assert_called_once()

    def test_process_identity_reuse_prevents_move(self):
        self.helper.process_identity.side_effect = ['old instance', 'new instance']
        status, _, err, native = self.coordinate()
        self.assertEqual(status, 1)
        self.adapter.move.assert_not_called()
        self.assertIn('changed', err)
        native.assert_called_once()

    def test_source_change_during_adapter_callback_stops_queue(self):
        fresh = {**self.snapshot, 'terminal': [{**self.source, 'pid': 2002}]}
        status, _, err, native = self.coordinate(discovery=[self.snapshot, self.snapshot, fresh])
        self.assertEqual(status, 1)
        self.assertIn('changed', err)
        self.adapter.move.assert_called_once()
        native.assert_called_once()

    def test_managed_sources_are_left_to_native_path(self):
        status, out, err, native = self.coordinate(discovery={**self.snapshot, 'terminal': [{**self.source, 'managed': True}]})
        self.assertEqual(status, 0, err)
        self.adapter.move.assert_not_called()
        self.helper.jobs.assert_not_called()
        native.assert_called_once()
        self.assertIn('0 Claude queued', out)

    def test_native_gathering_finishes_before_any_claude_receiver_can_be_created(self):
        events=[]
        def native(*args,**kwargs):events.append('native');return self.native
        def move(*args,**kwargs):events.append('claude');return dict(self.result)
        self.adapter.move.side_effect=move
        with mock.patch.object(slurp,'discover',return_value=self.snapshot), \
             mock.patch.object(slurp,'run_native',side_effect=native), \
             mock.patch.object(slurp.shutil,'which',return_value='/mock/claude'), \
             mock.patch.object(slurp,'adapter_acknowledged',return_value=True):
            status=slurp.coordinate('/exact/hardcore',adapter=self.adapter,stdout=io.StringIO(),stderr=io.StringIO())
        self.assertEqual(status,0)
        self.assertEqual(events,['native','claude'])

    def test_unsupported_sources_are_reported_and_never_bridged(self):
        for program in ('codex', 'grok', 'agy', ''):
            with self.subTest(program=program):
                status, _, err, native = self.coordinate(discovery={**self.snapshot, 'terminal': [{**self.source, 'program': program, 'shell': 'zsh'}]})
                self.assertEqual(status, 1)
                self.assertIn('direct transfer is unavailable', err)
                self.adapter.move.assert_not_called()
                self.assertNotIn('--bridge', str(native.call_args))

    def test_per_source_failure_does_not_prevent_other_claude_or_native(self):
        second_sid = '22222222-2222-4222-8222-222222222222'
        second = {**self.source, 'tty': '/dev/ttys013', 'pid': 2002}
        self.helper.jobs.return_value.append(self.job(pid=2002, sessionId=second_sid))
        self.adapter.move.side_effect = [RuntimeError('plugin policy refused'), dict(self.result)]
        snapshot = {**self.snapshot, 'terminal': [self.source, second]}
        status, out, err, native = self.coordinate(discovery=snapshot)
        self.assertEqual(status, 1)
        self.assertEqual(self.adapter.move.call_count, 2)
        self.assertIn('1 Claude queued (pending)', out)
        self.assertIn('1 failed', out)
        self.assertIn('plugin policy refused', err)
        native.assert_called_once()

    def test_requested_terminal_limits_both_claude_and_native(self):
        status, _, err, native = self.coordinate(terminal=self.source['tty'])
        self.assertEqual(status, 0, err)
        native.assert_called_once_with(Path('/exact app/hardcore'), ['--slurp-native', '--terminal', '/dev/ttys012'], timeout=120)
        self.adapter.move.reset_mock()
        status, _, err, native = self.coordinate(terminal='/dev/ttys999')
        self.assertEqual(status, 1)
        self.adapter.move.assert_not_called()
        self.assertIn('not found', err)

    def test_discovery_failure_does_not_prevent_native_transfer(self):
        for discovery in ([slurp.SlurpError('cannot list')],
                          {'terminal': [], 'hardcore': [], 'tmux': [], 'terminalDiscoveryFailed': True}):
            with self.subTest(discovery=discovery):
                status, _, err, native = self.coordinate(discovery=discovery)
                self.assertEqual(status, 1)
                native.assert_called_once()
                self.adapter.move.assert_not_called()

    def test_duplicate_tty_is_ambiguous_and_never_moved(self):
        status, _, err, native = self.coordinate(discovery={**self.snapshot, 'terminal': [self.source, dict(self.source)]})
        self.assertEqual(status, 1)
        self.assertIn('duplicate TTY', err)
        self.adapter.move.assert_not_called()
        native.assert_called_once()

    def test_acknowledged_adapter_waits_without_telling_user_to_reload(self):
        status, out, err, _ = self.coordinate(acknowledged=True)
        self.assertEqual(status, 0, err)
        self.assertIn('next safe completed turn', out)
        self.assertNotIn('/reload-plugins', out)
        self.assertIn('1 Claude queued (pending)', out)

    def test_prepared_or_attached_state_alone_is_not_verified(self):
        self.result.update(state='prepared', requestId='cd'*16)
        status, out, err, _ = self.coordinate()
        self.assertEqual(status, 0, err)
        self.assertIn('0 Claude verified', out)
        self.helper.progress.assert_called_once_with({'requestId': 'cd'*16})

    def test_verified_handoff_requires_matching_source_and_all_receipts(self):
        self.result.update(state='prepared', requestId='cd'*16)
        progress = {'ok': True, 'state': 'attached', 'sourceSessionId': SID, 'draftVerified': True,
                    'historyProjectionVerified': True, 'clientAttachedVerified': True}
        for change, verified in (({}, True), ({'draftVerified': False}, False), ({'clientAttachedVerified': False}, False),
                                 ({'sourceSessionId': 'another'}, False), ({'state': 'attaching'}, False)):
            with self.subTest(change=change):
                self.helper.progress.return_value = {**progress, **change}
                status, out, err, _ = self.coordinate()
                self.assertEqual(status, 0, err)
                self.assertIn(f'{int(verified)} Claude verified', out)

    def test_bootstrap_seam_cannot_claim_transfer_completion(self):
        bootstrap = mock.Mock(return_value={'runtimeLoaded': True, 'transferred': True})
        status, out, err, _ = self.coordinate(bootstrap=bootstrap)
        self.assertEqual(status, 0, err)
        self.assertIn('0 Claude verified, 1 Claude queued', out)
        self.assertNotIn('/reload-plugins', out)
        bootstrap.assert_called_once_with(self.source, self.project.resolve(), self.result)

    def test_native_failure_is_reported_and_output_controls_are_neutralized(self):
        native = subprocess.CompletedProcess([], 3, 'native\x1b[31m report\n', 'error\x1b]0;title\x07\n')
        status, out, err, _ = self.coordinate(native=native)
        self.assertEqual(status, 1)
        self.assertNotIn('\x1b', out+err)
        self.assertNotIn('\x07', out+err)
        self.assertIn('exit 3', err)

    def test_labels_and_errors_cannot_inject_terminal_or_bidi_controls(self):
        source = {**self.source, 'program': 'bad\x1b[2J\n\u202eprogram'}
        _, out, err, _ = self.coordinate(discovery={**self.snapshot, 'terminal': [source]})
        self.assertNotIn('\x1b', out+err)
        self.assertNotIn('\u202e', out+err)
        self.assertIn('\\u001b', err)

    def test_cli_uses_literal_argv_no_shell_and_noninteractive_stdin(self):
        result = subprocess.CompletedProcess([], 0, '{}', '')
        with mock.patch.object(slurp.subprocess, 'run', return_value=result) as run:
            self.assertIs(slurp.run_native(Path('/app name;$(false)'), ['--slurp-native'], timeout=9), result)
        args, kwargs = run.call_args
        self.assertEqual(args[0], ['/app name;$(false)', '--slurp-native'])
        self.assertFalse(kwargs.get('shell', False))
        self.assertEqual(kwargs['stdin'], subprocess.DEVNULL)

    def test_invalid_json_schema_and_oversized_cli_output_are_rejected(self):
        for text in ('[]', '{}', 'not json', json.dumps({'terminal': [False], 'hardcore': [], 'tmux': []})):
            with self.subTest(text=text), mock.patch.object(slurp, 'run_native', return_value=subprocess.CompletedProcess([], 0, text, '')):
                with self.assertRaises(slurp.SlurpError): slurp.discover('/app')
        with mock.patch.object(slurp.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, 'x'*(slurp.MAX_OUTPUT+1), '')):
            with self.assertRaises(slurp.SlurpError): slurp.run_native('/app', [], timeout=1)

    def test_intent_receipt_requires_current_identity_and_recent_observation(self):
        record = {'intentId': 'ab'*16, 'sourceSessionId': SID, 'sourceIdentity': 'instance-1001', 'observedAt': time.time()}
        for change, expected in (({}, True), ({'sourceIdentity': 'other'}, False), ({'intentId': 'other'}, False),
                                 ({'sourceSessionId': 'other'}, False), ({'observedAt': time.time()-60}, False),
                                 ({'observedAt': True}, False), ({'observedAt': time.time()+60}, False)):
            with self.subTest(change=change):
                self.helper.read_json.return_value = {**record, **change}
                self.assertEqual(slurp.adapter_acknowledged(self.result, SID, 'instance-1001', self.helper, timeout=0), expected)

    def test_preparation_racing_queue_return_adopts_request_and_checks_receipts(self):
        record={'intentId':'ab'*16,'sourceSessionId':SID,'sourceIdentity':'instance-1001',
                'state':'prepared','requestId':'cd'*16}
        self.helper.read_json.return_value=record
        result=dict(self.result)
        self.assertTrue(slurp.adapter_acknowledged(result,SID,'instance-1001',self.helper,timeout=0))
        self.assertEqual(result['requestId'],'cd'*16)
        self.assertEqual(result['state'],'prepared')
        self.helper.progress.return_value={'ok':True,'state':'attached','sourceSessionId':SID,
                                          'draftVerified':True,'historyProjectionVerified':True,'clientAttachedVerified':True}
        with mock.patch.object(slurp,'discover',return_value=self.snapshot), \
             mock.patch.object(slurp,'run_native',return_value=self.native), \
             mock.patch.object(slurp.shutil,'which',return_value='/mock/claude'):
            out=io.StringIO()
            status=slurp.coordinate('/exact/hardcore',adapter=self.adapter,stdout=out,stderr=io.StringIO())
        self.assertEqual(status,0)
        self.assertIn('1 Claude verified, 0 Claude queued',out.getvalue())
        self.assertNotIn('/reload-plugins',out.getvalue())

    def test_prepared_receipt_cannot_replace_an_already_bound_request(self):
        self.helper.read_json.return_value={'intentId':'ab'*16,'sourceSessionId':SID,'sourceIdentity':'instance-1001',
                                           'state':'prepared','requestId':'cd'*16}
        result={**self.result,'requestId':'ef'*16}
        self.assertFalse(slurp.adapter_acknowledged(result,SID,'instance-1001',self.helper,timeout=0))
        self.assertEqual(result['requestId'],'ef'*16)

    def test_cli_rejects_injected_terminal_selector_and_relative_executable(self):
        for argv in (['--executable', '/app', '--terminal', '/dev/ttys1; touch bad'], ['--executable', 'relative']):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit): slurp.main(argv)


if __name__ == '__main__':
    unittest.main()
