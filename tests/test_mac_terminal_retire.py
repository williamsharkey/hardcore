#!/usr/bin/env python3
"""Retirement policy checks. No Terminal automation or window close occurs."""
import copy
import importlib.util
from pathlib import Path
import unittest


spec = importlib.util.spec_from_file_location('mac_terminal_retire',
    Path(__file__).resolve().parents[1] / 'tools/mac_terminal_retire.py')
retire = importlib.util.module_from_spec(spec)
spec.loader.exec_module(retire)


class RetirementTests(unittest.TestCase):
    def setUp(self):
        self.birth = 'Thu Oct 15 11:00:00 2026'
        self.source = {'terminalPid': 100, 'terminalBirth': self.birth,
                       'windowId': 97, 'tty': '/dev/ttys001',
                       'claudePid': 101, 'claudeBirth': self.birth,
                       'sessionId': '11111111-1111-4111-8111-111111111111'}
        self.captured = retire.snapshot('ab' * 16, self.source, 'older\nnewest\n',
                                       {'text': 'draft 😀', 'cursor': 8})
        self.receipt = {'protocol': 1, 'requestId': 'ab' * 16,
                        'sourceSha256': self.captured['sha256'],
                        'sourceSessionId': self.source['sessionId'],
                        'destinationSessionId': '22222222-2222-4222-8222-222222222222',
                        'state': 'attached', 'attachedAt': 1000.0,
                        'payloads': copy.deepcopy(self.captured['payloads']),
                        'destination': {'claudePid': 200, 'claudeBirth': self.birth,
                                        'clientPid': 201, 'clientBirth': self.birth,
                                        'hardcorePid': 202, 'hardcoreBirth': self.birth}}
        self.terminal = {key: self.source[key] for key in
                         ('terminalPid', 'terminalBirth', 'windowId', 'tty')}
        self.terminal.update(tabTtys=['/dev/ttys001'], scrollback='older\nnewest\n')
        self.ps = '\n'.join(f'{pid} 1 {tty} {self.birth}' for pid, tty in
                            [(1, '??'), (100, '??'), (200, '??'),
                             (201, 'ttys002'), (202, '??')])
        self.inventory = retire.parse_process_table(self.ps, 1001.0)
        self.handles = retire.parse_tty_handles('', 1, '/dev/ttys001', 1001.0)

    def decide(self, **kwargs):
        return retire.retirement_decision(self.captured, self.receipt, self.terminal,
            self.inventory, handles=self.handles,
            scope=kwargs.pop('scope', 'claude-and-text'), now=1001.5, **kwargs)

    def test_explicit_reduced_scope_needs_complete_positive_evidence(self):
        self.assertEqual(self.decide(), {'ok': True, 'reasons': []})

    def test_default_never_claims_full_shell_from_text(self):
        decision = retire.retirement_decision(self.captured, self.receipt,
            self.terminal, self.inventory, now=1001.5)
        self.assertFalse(decision['ok'])
        self.assertIn('live shell', ' '.join(decision['reasons']))

    def test_payload_presence_and_exact_draft_cursor_are_required(self):
        valid = copy.deepcopy(self.receipt)
        for value in (None, {}, {'scrollback': 'older\nnewest\n'},
                      {'scrollback': 'older\nnewest\n',
                       'draft': {'text': 'draft 😀', 'cursor': 7}}):
            self.receipt = copy.deepcopy(valid)
            self.receipt['payloads'] = value
            with self.subTest(value=value):
                self.assertFalse(self.decide()['ok'])
        self.receipt = copy.deepcopy(valid)
        self.receipt['historyCopied'] = True
        del self.receipt['payloads']
        self.assertFalse(self.decide()['ok'], 'a success boolean is not payload evidence')

    def test_no_startup_or_exec_attempt_is_an_attachment_receipt(self):
        for state in ('ready', 'attaching', None, 'failed'):
            self.receipt['state'] = state
            with self.subTest(state=state):
                self.assertFalse(self.decide()['ok'])

    def test_wrong_snapshot_or_session_is_rejected(self):
        valid = copy.deepcopy(self.receipt)
        for key, value in [('requestId', 'cd' * 16), ('sourceSha256', '0' * 64),
                           ('sourceSessionId', self.receipt['destinationSessionId']),
                           ('destinationSessionId', self.source['sessionId'])]:
            self.receipt = {**valid, key: value}
            with self.subTest(key=key):
                self.assertFalse(self.decide()['ok'])

    def test_source_snapshot_mutation_is_rejected(self):
        self.captured['payloads']['scrollback'] += 'unacknowledged text'
        self.assertFalse(self.decide()['ok'])

    def test_terminal_replacement_and_sibling_tabs_are_rejected(self):
        valid = dict(self.terminal)
        for key, value in [('terminalPid', 102), ('terminalBirth', 'another birth'),
                           ('windowId', 98), ('tty', '/dev/ttys003'),
                           ('tabTtys', ['/dev/ttys001', '/dev/ttys003']),
                           ('tabTtys', []), ('scrollback', 'new output')]:
            self.terminal = {**valid, key: value}
            with self.subTest(key=key, value=value):
                self.assertFalse(self.decide()['ok'])

    def test_idle_shell_or_background_job_still_blocks_close(self):
        for pid in (101, 102, 999):
            self.inventory = retire.parse_process_table(
                self.ps + f'\n{pid} 1 ttys001 {self.birth}', 1001.0)
            with self.subTest(pid=pid):
                decision = self.decide()
                self.assertFalse(decision['ok'])
                self.assertIn('source TTY', ' '.join(decision['reasons']))

    def test_original_frontend_alive_elsewhere_still_blocks(self):
        self.inventory = retire.parse_process_table(
            self.ps + f'\n101 1 ?? {self.birth}', 1001.0)
        self.assertIn('original Claude', ' '.join(self.decide()['reasons']))

    def test_destination_pid_reuse_and_pty_dependence_are_rejected(self):
        valid = copy.deepcopy(self.receipt)
        for label in ('claude', 'client', 'hardcore'):
            self.receipt = copy.deepcopy(valid)
            self.receipt['destination'][label + 'Birth'] = 'a different process'
            with self.subTest(label=label):
                self.assertFalse(self.decide()['ok'])
        self.receipt = valid
        self.inventory = retire.parse_process_table(self.ps.replace('ttys002', 'ttys001'), 1001.0)
        self.assertFalse(self.decide()['ok'])

    def test_missing_or_stale_process_inventory_is_not_evidence(self):
        valid = self.inventory
        for inventory in (None, [], retire.ProcessTable(997, valid.rows),
                          retire.ProcessTable(1002, valid.rows)):
            self.inventory = inventory
            with self.subTest(inventory=inventory):
                self.assertFalse(self.decide()['ok'])

    def test_inventory_must_follow_actual_attach_receipt(self):
        for timestamp in (None, 1002, 800, float('nan')):
            self.receipt['attachedAt'] = timestamp
            with self.subTest(timestamp=timestamp):
                self.assertFalse(self.decide()['ok'])

    def test_filtered_duplicate_and_malformed_ps_output_fail(self):
        for output in ('', f'100 1 ?? {self.birth}', self.ps + '\ntruncated',
                       self.ps + f'\n100 1 ?? {self.birth}'):
            with self.subTest(output=output), self.assertRaises(ValueError):
                retire.parse_process_table(output, 1001)

    def test_draft_images_and_unknown_fields_fail_closed(self):
        for draft in ({'text': '', 'cursor': 0, 'attachments': ['image']},
                      {'text': '', 'cursor': True}, {'text': 'x', 'cursor': 2},
                      {'text': '', 'cursor': 0, 'imageCount': 0}):
            with self.subTest(draft=draft), self.assertRaises(ValueError):
                retire.snapshot('ab' * 16, self.source, '', draft)

    def test_detached_process_with_no_controlling_tty_still_blocks(self):
        self.handles = retire.parse_tty_handles(
            'p999\nf1\nn/dev/ttys001\nf2\nn/dev/ttys001\n',
            0, '/dev/ttys001', 1001.0)
        self.assertEqual(self.handles.pids, (999,))
        decision = self.decide()
        self.assertFalse(decision['ok'])
        self.assertIn('Detached jobs', ' '.join(decision['reasons']))

    def test_missing_wrong_and_stale_open_handle_inventory_block(self):
        for handles in (None, [], retire.TtyHandles(997, '/dev/ttys001', ()),
                        retire.TtyHandles(1002, '/dev/ttys001', ()),
                        retire.TtyHandles(1001, '/dev/ttys003', ())):
            self.handles = handles
            with self.subTest(handles=handles):
                self.assertFalse(self.decide()['ok'])

    def test_lsof_partial_or_wrong_device_output_is_not_evidence(self):
        for output, status in [('', 0), ('', 2), ('p999\n', 0),
                               ('p999\nf1\n', 0),
                               ('p999\nf1\nn/dev/ttys002\n', 0),
                               ('p999\nf1\nn/dev/ttys001\np1000\n', 0)]:
            with self.subTest(output=output), self.assertRaises(ValueError):
                retire.parse_tty_handles(output, status, '/dev/ttys001', 1001)


if __name__ == '__main__':
    unittest.main()
