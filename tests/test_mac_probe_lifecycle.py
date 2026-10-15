#!/usr/bin/env python3
"""Deterministic probe lifecycle checks: no Terminal, GUI, or /dev/tty access."""
import os
import array
from pathlib import Path
import signal
import socket
import struct
import tempfile
import time
import unittest
from unittest.mock import MagicMock, Mock, patch

import probe_mac_zsh_clone_state as state
import probe_mac_zsh_hardcore as native


class LifecycleTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='hc-probe-lifecycle-', dir='/private/tmp')
        self.root = Path(self.directory.name)
        self.addCleanup(self.directory.cleanup)

    def shell(self):
        shell = state.Shell.__new__(state.Shell)
        shell.root = self.root
        shell.master = shell.new_master = shell.new_slave = -1
        shell.pid = 900001
        shell.clone_pid = 900002
        shell.job_pid = 900003
        shell.reaped = False
        shell.cleanup_errors = []
        shell.output_counts = {}
        shell.buffers = {}
        return shell

    def test_unowned_pid_is_never_signalled(self):
        with patch.object(state.os, 'waitpid', side_effect=ChildProcessError), \
                patch.object(state.os, 'kill') as kill:
            self.assertTrue(state.reap_direct_child(900001, grace=0))
            kill.assert_not_called()

    def test_direct_child_escalation_is_bounded(self):
        with patch.object(state.os, 'waitpid', return_value=(0, 0)) as wait, \
                patch.object(state.os, 'kill') as kill:
            self.assertFalse(state.reap_direct_child(900001, grace=0))
            self.assertEqual(wait.call_count, 4)
            self.assertEqual([call.args for call in kill.call_args_list],
                             [(900001, signal.SIGTERM), (900001, signal.SIGKILL)])
            self.assertTrue(all(call.args[1] == os.WNOHANG for call in wait.call_args_list))

    def test_cleanup_closes_fds_and_ignores_observed_clone_and_job(self):
        shell = self.shell()
        shell.master, shell.new_master, shell.new_slave = 20, 21, 22
        with patch.object(state.os, 'close') as close, \
                patch.object(state, 'reap_direct_child', return_value=True) as reap, \
                patch.object(state.os, 'kill') as kill:
            shell.cleanup()
            self.assertEqual([call.args for call in close.call_args_list], [(20,), (21,), (22,)])
            reap.assert_called_once_with(shell.pid)
            kill.assert_not_called()
            self.assertEqual((shell.master, shell.new_master, shell.new_slave), (-1, -1, -1))
            self.assertTrue(shell.reaped)

    def test_cleanup_error_does_not_mask_primary_exception(self):
        shell = self.shell()
        with patch.object(state, 'reap_direct_child', return_value=False):
            with self.assertRaisesRegex(ValueError, 'original failure'):
                try:
                    raise ValueError('original failure')
                finally:
                    shell.cleanup()
        self.assertTrue((self.root / 'cleanup-errors.json').exists())

    def test_cleanup_error_fails_an_otherwise_successful_probe(self):
        with patch.object(state, 'reap_direct_child', return_value=False):
            with self.assertRaisesRegex(RuntimeError, 'cleanup incomplete'):
                self.shell().cleanup()

    def test_constructor_failure_invokes_cleanup(self):
        with patch.object(state.base.Shell, '__init__', side_effect=ValueError('setup failure')), \
                patch.object(state.Shell, 'cleanup') as cleanup:
            with self.assertRaisesRegex(ValueError, 'setup failure'):
                state.Shell(self.root)
            cleanup.assert_called_once()

    def test_output_limit_rejects_before_unbounded_buffer_or_file_growth(self):
        shell = self.shell()
        shell.master = 20
        with patch.object(state, 'MAX_OUTPUT', 16), \
                patch.object(state.select, 'select', return_value=([20], [], [])), \
                patch.object(state.os, 'read', return_value=b'x' * 17):
            with self.assertRaisesRegex(AssertionError, 'output limit'):
                shell.pump()
        self.assertFalse(shell.buffers)
        self.assertFalse((self.root / 'source-received.bin').exists())

    def test_deadline_interrupts_and_restores_signal_handlers(self):
        old_alarm = signal.getsignal(signal.SIGALRM)
        old_term = signal.getsignal(signal.SIGTERM)
        with self.assertRaises(TimeoutError):
            with state.deadline(.01):
                time.sleep(.1)
        self.assertEqual(signal.getsignal(signal.SIGALRM), old_alarm)
        self.assertEqual(signal.getsignal(signal.SIGTERM), old_term)
        self.assertEqual(signal.getitimer(signal.ITIMER_REAL)[0], 0)

    def test_native_packet_size_is_bounded(self):
        for size in (0, 4 * 1024 * 1024):
            with self.subTest(size=size):
                receiver, sender = socket.socketpair()
                try:
                    receiver.settimeout(.1)
                    sender.sendall(struct.pack('!I', size))
                    with self.assertRaisesRegex(ValueError, 'packet size'):
                        native.receive(receiver)
                finally:
                    receiver.close()
                    sender.close()

    def test_native_stalled_packet_has_absolute_deadline(self):
        receiver, sender = socket.socketpair()
        try:
            receiver.settimeout(.01)
            sender.sendall(struct.pack('!I', 20) + b'partial')
            pump = Mock()
            with self.assertRaises(TimeoutError):
                native.receive(receiver, pump=pump)
            self.assertGreater(pump.call_count, 0)
        finally:
            receiver.close()
            sender.close()


class SnapshotTests(unittest.TestCase):
    @staticmethod
    def snapshot(body, row, column):
        footer = ('\x1b[1;1H\x1b(B\x1b7\x1b(B\x1b[?1l\x1b[?25h'
                  '\x1b[?1004l\x1b[?1006l\x1b[?2004l\x1b[2 q\x1b(B'
                  f'\x1b[{row + 1};{column + 1}H')
        return ('\x1b[0m' + body + footer).encode()

    def test_exact_draft_and_wide_character_cursor(self):
        draft = 'abé 🦊 cd tail'
        snapshot = self.snapshot('HC> ' + draft + '\r\n' * 4, 0, 10)
        result = native.verify_fixture_snapshot(snapshot, 12, 6, draft, 5)
        self.assertTrue(result['completeVisibleDraft'])
        self.assertEqual((result['row'], result['column']), (0, 10))

    def test_wide_last_column_prewrap_before_and_after_glyph(self):
        draft = 'abcde🦊 f'
        for cursor, column in ((5, 0), (6, 2)):
            with self.subTest(cursor=cursor):
                snapshot = self.snapshot('HC> ' + draft + '\r\n', 1, column)
                native.verify_fixture_snapshot(snapshot, 10, 3, draft, cursor)

    def test_cursor_at_exact_soft_wrap_boundary(self):
        draft = 'abcdefghi'
        snapshot = self.snapshot('HC> ' + draft + '\r\n', 1, 0)
        native.verify_fixture_snapshot(snapshot, 10, 3, draft, 6)

    def test_hard_row_end_cancels_pending_wrap(self):
        self.assertEqual(native.fixture_rows('0123456789\r\nx', 10),
                         [list('0123456789'), list('x' + ' ' * 9)])

    def test_scrollback_can_precede_verified_visible_grid(self):
        draft = 'abcé 🦊 xyz'
        snapshot = self.snapshot('old history\r\nHC> ' + draft + '\r\n' * 2, 0, 11)
        native.verify_fixture_snapshot(snapshot, 40, 3, draft, 6)

    def test_scrollback_only_draft_is_not_visible_proof(self):
        draft = 'abcé 🦊 xyz'
        snapshot = self.snapshot('HC> ' + draft + '\r\n' * 3, 0, 11)
        with self.assertRaisesRegex(AssertionError, 'visible draft'):
            native.verify_fixture_snapshot(snapshot, 40, 3, draft, 6)

    def test_missing_suffix_and_extra_cell_are_rejected(self):
        draft = 'abcé 🦊 xyz'
        for body in ('HC> ' + draft[:-2] + '\r\n' * 2,
                     'HC> ' + draft + '\r\nleftover\r\n'):
            with self.subTest(body=body), self.assertRaises(AssertionError):
                native.verify_fixture_snapshot(self.snapshot(body, 0, 11), 40, 3, draft, 6)

    def test_correct_text_with_wrong_cursor_is_rejected(self):
        draft = 'abcé 🦊 xyz'
        with self.assertRaisesRegex(AssertionError, 'visible cursor'):
            native.verify_fixture_snapshot(self.snapshot('HC> ' + draft + '\r\n' * 2, 1, 0),
                                           40, 3, draft, 6)

    def test_historical_proof_write_overwrote_suffix(self):
        # Exact visible line from u231f3_d: the old substring check passed,
        # despite the /dev/tty observation overwriting the live draft suffix.
        path = '/private/tmp/hc-zsh-native-u231f3_d/MUST_NOXT_EXECUTE'
        draft = "printf '%s' 'UNSENT café 🦊'; touch " + path
        overwritten = "HC> printf '%s' 'UNSENT café 🦊'; touch " + \
            '/private/tmp/hc-zsh-native-u231f3_d/MUST_NOXHC_CTTY_PROOF_55796'
        snapshot = self.snapshot(overwritten + '\r\n' * 40, 1, 0)
        self.assertIn(b'UNSENT', snapshot)
        self.assertIn('café'.encode(), snapshot)
        with self.assertRaisesRegex(AssertionError, 'visible draft'):
            native.verify_fixture_snapshot(snapshot, 110, 41, draft, 79)

    def test_unknown_or_invisible_modes_fail_closed(self):
        draft = 'abcé 🦊 xyz'
        good = self.snapshot('HC> ' + draft + '\r\n' * 2, 0, 11)
        bad = [good.replace(b'HC>', b'\x1b[0;8mHC>'),
               good.replace(b'HC>', b'\x1b[0;31mHC>'),
               good.replace(b'HC>', b'\x1b[2KHC>'),
               good.replace(b'\x1b[?25h', b'\x1b[?25l'),
               good + b'X',  # Pending-wrap reprint/unknown trailing content.
               good.replace(b'\x1b[2 q', b'\x1b[2 q\x1b[?6h'),
               good.replace(b'\x1b[2 q', b'\x1b[2 q\x1b[?1049h'),
               good.replace(b'\x1b[2 q', b'\x1b[2 q\x1b[2;3r'),
               good.replace(b'\x1b(B', b'\x1b(0', 1)]
        for snapshot in bad:
            with self.subTest(snapshot=snapshot), self.assertRaises(ValueError):
                native.verify_fixture_snapshot(snapshot, 40, 3, draft, 6)
        with self.assertRaises(ValueError):
            native.verify_fixture_snapshot(good, 40, 3, draft, 6, view=1)

    def test_physical_row_expansion_is_bounded(self):
        with self.assertRaisesRegex(ValueError, 'too many physical rows'):
            native.fixture_rows('\r\n' * 4096, 1000, keep=3)
        rows = native.fixture_rows('\r\n' * 100, 1000, keep=3)
        self.assertEqual(len(rows), 3)


class EvidenceTests(unittest.TestCase):
    def record(self, pid):
        return {'state': f'PID={pid} TTY=/dev/owned{pid}\nexact scalar and function\n',
                'history': b'1\0HC_NATIVE_UNFLUSHED_SENTINEL\0'
                           b'2\0print -s -- HC_NATIVE_UNFLUSHED_SENTINEL\0'
                           b'3\0HC_NATIVE_UNFLUSHED_SENTINEL\0'}

    def test_exact_shell_and_history_records_allow_identity_change(self):
        native.verify_fixture_state(self.record(1), self.record(2))

    def test_command_containing_sentinel_is_not_history_event_proof(self):
        received = self.record(2)
        received['history'] = received['history'].replace(
            b'3\0HC_NATIVE_UNFLUSHED_SENTINEL\0', b'')
        with self.assertRaisesRegex(AssertionError, 'exact unflushed'):
            native.verify_fixture_state(self.record(1), received)

    def test_memory_and_history_are_exactly_compared(self):
        for field in ('state', 'history'):
            received = self.record(2)
            if field == 'state':
                received[field] += 'unexpected\n'
            else:
                received[field] = received[field].replace(b'2\0', b'4\0')
            with self.subTest(field=field), self.assertRaises(AssertionError):
                native.verify_fixture_state(self.record(1), received)

    def test_observer_reads_immutable_generation(self):
        with tempfile.TemporaryDirectory(prefix='hc-probe-record-', dir='/private/tmp') as directory:
            root = Path(directory)
            (root / 'observed-123.ready').write_text('4\n')
            record = self.record(123)
            for suffix, value in dict(record, draft='abc', cursor='2', geometry='110 32\n').items():
                path = root / f'observed-123-4.{suffix}'
                path.write_bytes(value if isinstance(value, bytes) else value.encode())
            value = native.observed(root, 123, 'observed')
            self.assertEqual(value['serial'], 4)
            self.assertEqual(value['history'], record['history'])
            self.assertEqual(value['geometry'], (110, 32))
            (root / 'observed-123.ready').write_text('5\n')
            self.assertIsNone(native.observed(root, 123, 'observed'))

    def take(self, packet, flags=0, descriptors=(41,), close_error=None):
        peer = MagicMock()
        peer.__enter__.return_value = peer
        peer.recvmsg.return_value = (b'F', [(socket.SOL_SOCKET, socket.SCM_RIGHTS,
                                           array.array('i', descriptors).tobytes())], flags, None)
        with patch.object(native.socket, 'socket', return_value=peer), \
                patch.object(native, 'send') as send, \
                patch.object(native, 'receive', return_value=packet), \
                patch.object(native, 'read_pty_geometry', return_value={'columns': 110, 'rows': 32,
                                                                       'foregroundPgrp': 123}), \
                patch.object(native.os, 'close', side_effect=close_error) as close:
            try:
                return native.take_snapshot('/owned/fixture.sock', 123)
            finally:
                self.assertEqual([call.args[0] for call in close.call_args_list], list(descriptors))
                send.assert_called_once_with(peer, {'op': 'take'})
                peer.__exit__.assert_called_once()

    def test_snapshot_aborts_take_and_closes_received_fd(self):
        tab = {'pid': 123, 'snapshot': b'fixture'}
        self.assertEqual(self.take({'tabs': [tab]}),
                         dict(tab, probePtyGeometry={'columns': 110, 'rows': 32, 'foregroundPgrp': 123}))

    def test_snapshot_failure_closes_all_received_fds(self):
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            self.take({'tabs': []}, descriptors=(41, 42))
        with self.assertRaisesRegex(ValueError, 'descriptor packet'):
            self.take({'tabs': [{'pid': 123}]}, flags=socket.MSG_CTRUNC)
        with self.assertRaisesRegex(ValueError, 'unexpected shell'):
            self.take({'tabs': [{'pid': 456}]})

    def test_snapshot_cleanup_does_not_mask_primary_failure(self):
        with self.assertRaisesRegex(ValueError, 'unexpected shell'):
            self.take({'tabs': [{'pid': 456}]}, close_error=OSError('close failed'))
        with self.assertRaisesRegex(RuntimeError, 'descriptor cleanup'):
            self.take({'tabs': [{'pid': 123}]}, close_error=OSError('close failed'))


class GeometryTests(unittest.TestCase):
    def test_exact_shell_kernel_and_grid_dimensions(self):
        native.verify_fixture_geometry((105, 43),
            {'columns': 105, 'rows': 43, 'foregroundPgrp': 123}, 105, 43, 123)

    def test_stale_shell_width_is_rejected_despite_correct_cells(self):
        # Saved v2 failure: zsh still used 80 columns while the new native grid
        # had 105. Text could be right while relative cursor moves were wrong.
        with self.assertRaisesRegex(AssertionError, 'shell geometry'):
            native.verify_fixture_geometry((80, 24),
                {'columns': 105, 'rows': 43, 'foregroundPgrp': 123}, 105, 43, 123)

    def test_missed_import_kernel_resize_is_rejected(self):
        with self.assertRaisesRegex(AssertionError, 'kernel PTY geometry'):
            native.verify_fixture_geometry((105, 43),
                {'columns': 110, 'rows': 32, 'foregroundPgrp': 123}, 105, 43, 123)

    def test_zero_size_kernel_defaults_do_not_count_as_geometry_proof(self):
        with self.assertRaisesRegex(AssertionError, 'kernel PTY geometry'):
            native.verify_fixture_geometry((80, 24),
                {'columns': 0, 'rows': 0, 'foregroundPgrp': 123}, 80, 24, 123)

    def test_geometry_requires_owned_shell_in_foreground(self):
        for foreground in (0, 124):
            with self.subTest(foreground=foreground), self.assertRaisesRegex(AssertionError, 'foreground'):
                native.verify_fixture_geometry((105, 43),
                    {'columns': 105, 'rows': 43, 'foregroundPgrp': foreground}, 105, 43, 123)

    def test_owned_fd_geometry_uses_kernel_values(self):
        with patch.object(native.fcntl, 'ioctl', return_value=struct.pack('HHHH', 43, 105, 0, 0)) as ioctl, \
                patch.object(native.os, 'tcgetpgrp', return_value=123) as foreground:
            self.assertEqual(native.read_pty_geometry(41),
                             {'columns': 105, 'rows': 43, 'foregroundPgrp': 123})
            ioctl.assert_called_once_with(41, native.termios.TIOCGWINSZ, b'\0' * 8)
            foreground.assert_called_once_with(41)


if __name__ == '__main__':
    unittest.main()
