#!/usr/bin/env python3
"""Owned real-PTY regression checks for the detachable here session owner."""
import base64
import json
import os
import re
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import here_broker as broker


class BrokerTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='hc-here-broker-')
        self.root = Path(self.directory.name) / 'private'
        self.cwd = Path(self.directory.name) / 'work'
        self.cwd.mkdir()
        self.clients = []
        self.names = []

    def tearDown(self):
        for client in self.clients:
            client.close()
        for name in self.names:
            try:
                broker.stop_session(name, self.root)
            except (OSError, broker.BrokerError):
                pass
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline and self.root.exists() and list(self.root.glob('*.sock')):
            time.sleep(.02)
        self.directory.cleanup()

    def start(self, name='owned', command=None):
        name = broker.start_session(name, shell='/bin/sh', cwd=str(self.cwd), command=command,
                                    cols=80, rows=12, root=self.root)
        self.names.append(name)
        return name

    def attach(self, name='owned', takeover=False):
        client = broker.BrokerClient(name, root=self.root, takeover=takeover)
        self.clients.append(client)
        return client

    def wait(self, predicate, client=None, timeout=4):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if client:
                client.poll(.02)
            else:
                time.sleep(.02)
            if predicate():
                return
        self.fail('Timed out waiting for owned broker state')

    def screen(self, client):
        return ''.join(''.join(c.text for c in row).rstrip() for term in client.terminals.values()
                         for row in list(term.history) + term.screen.grid)

    def request(self, client, op, **arguments):
        identifier = client.send(op, **arguments)
        events = []
        self.wait(lambda: any(e.get('request') == identifier for e in events),
                  _EventClient(client, events))
        return next(e for e in events if e.get('request') == identifier)

    def test_detach_keeps_shell_state_jobs_partial_input_and_screen(self):
        self.start()
        client = self.attach()
        pid = client.state['tabs'][0]['pid']
        client.input(b"set +H\nHC_VALUE=kept; cd ..; sleep 60 & HC_JOB=$!; printf 'READY:%s:%s\\n' \"$HC_VALUE\" \"$HC_JOB\"\n")
        self.wait(lambda: 'READY:kept:' in self.screen(client), client)
        job = int(re.search(r'READY:kept:(\d+)', self.screen(client))[1])
        self.assertEqual(os.getsid(job), pid)
        # Leave an incomplete command line in the kernel/shell input buffer.
        client.input(b"printf 'REJOIN:%s:%s:%s\\n' \"$HC_VALUE\" \"$PWD\" \"$HC_JOB\"")
        self.wait(lambda: 'REJOIN:' in self.screen(client), client)
        self.assertTrue(self.request(client, 'detach')['ok'])
        client.close()
        self.wait(lambda: not broker.status_session('owned', self.root)['attached'])
        again = self.attach()
        self.assertEqual(again.state['tabs'][0]['pid'], pid)
        self.wait(lambda: 'READY:kept:' in self.screen(again), again)
        again.input(b'\n')
        expected = 'REJOIN:kept:' + str(self.cwd.parent.resolve()) + ':'
        self.wait(lambda: expected in self.screen(again), again)
        self.assertEqual(os.getsid(pid), pid)
        self.assertEqual(os.getsid(job), pid)
        os.kill(job, 0)
        # Losing the client without an orderly detach also preserves the shell.
        again.close()
        self.wait(lambda: not broker.status_session('owned', self.root)['attached'])
        third = self.attach()
        self.assertEqual(third.state['tabs'][0]['pid'], pid)
        self.assertTrue(self.request(third, 'stop')['ok'])
        self.wait(lambda: not broker.socket_path('owned', self.root).exists())
        with self.assertRaises(ProcessLookupError):
            os.kill(pid, 0)

    def test_exclusive_attachment_and_explicit_takeover(self):
        self.start()
        first = self.attach()
        with self.assertRaisesRegex(broker.BrokerError, 'already attached'):
            self.attach()
        second = self.attach(takeover=True)
        seen = []
        self.wait(lambda: any(e.get('event') == 'detached' for e in seen), _EventClient(first, seen))
        self.assertEqual(second.state['tabs'][0]['pid'], first.state['tabs'][0]['pid'])
        self.assertTrue(self.request(second, 'resize', cols=40, rows=8)['ok'])
        self.wait(lambda: second.state['cols'] == 40, second)
        self.assertEqual(second.state['rows'], 8)

    def test_bounded_protocol_errors_do_not_kill_shell(self):
        self.start()
        client = self.attach()
        pid = client.state['tabs'][0]['pid']
        for op, args in [('input', {'data': 'bad!'}), ('resize', {'cols': 'x', 'rows': 10}),
                         ('choose', {'tab': 12345}), ('new', {'cwd': '/does-not-exist'}),
                         ('watch', {'tabs': ['wrong']})]:
            reply = self.request(client, op, **args)
            self.assertFalse(reply['ok'], (op, reply))
        self.assertEqual(broker.status_session('owned', self.root)['tabs'][0]['pid'], pid)
        client.close()
        self.wait(lambda: not broker.status_session('owned', self.root)['attached'])
        with broker._safe_connect(broker.socket_path('owned', self.root)) as raw:
            raw.sendall(b'x' * (broker.MAX_REQUEST + 1))
            raw.settimeout(1)
            try:
                self.assertEqual(raw.recv(1), b'')
            except ConnectionResetError:
                pass
        self.assertEqual(broker.status_session('owned', self.root)['tabs'][0]['pid'], pid)

    def test_new_tab_snapshot_watch_and_close(self):
        self.start()
        client = self.attach()
        first = client.state['tabs'][0]['id']
        reply = self.request(client, 'new', command=['/bin/sh', '-c', "printf SECOND; sleep 60"])
        second = reply['tab']
        self.assertNotEqual(first, second)
        self.wait(lambda: len(client.state['tabs']) == 2 and 'SECOND' in self.screen(client), client)
        self.assertTrue(self.request(client, 'choose', tab=first)['ok'])
        self.assertTrue(self.request(client, 'watch', tabs=[first, second])['ok'])
        self.wait(lambda: set(client.terminals) == {first, second}, client)
        self.assertTrue(self.request(client, 'close', tab=second)['ok'])
        self.wait(lambda: len(client.state['tabs']) == 1, client)
        self.assertEqual(client.state['tabs'][0]['id'], first)
        self.assertEqual(len(broker.list_sessions(self.root)), 1)

    def test_incremental_history_and_resize_restore(self):
        self.start(command=['/bin/sh', '-c', "printf 'first\\n'; while IFS= read -r x; do printf '%s\\n' \"$x\"; done"])
        client = self.attach()
        self.wait(lambda: bool(client.terminals), client)
        events = []
        client.input(b'SMALL\n')
        self.wait(lambda: 'SMALL' in self.screen(client), _EventClient(client, events))
        deltas = [event for event in events if event.get('event') == 'terminal_delta']
        self.assertTrue(deltas)
        self.assertTrue(all('history' not in event['snapshot'] for event in deltas))
        self.assertTrue(all('cells' not in event['snapshot']['normal'] for event in deltas))
        events.clear()
        client.input(b''.join(f'ROW-{i:03}\n'.encode() for i in range(50)))
        self.wait(lambda: 'ROW-049' in self.screen(client), _EventClient(client, events))
        self.assertTrue(any(event.get('history', {}).get('append') for event in events if event.get('history')))
        identifier = client.state['tabs'][0]['id']
        before = [cell.text for row in client.terminals[identifier].history for cell in row]
        self.assertTrue(before)
        self.assertTrue(self.request(client, 'resize', cols=40, rows=10)['ok'])
        self.wait(lambda: client.terminals[identifier].cols == 40, client)
        self.assertIn('ROW-049', self.screen(client))
        client.close()
        self.wait(lambda: not broker.status_session('owned', self.root)['attached'])
        again = self.attach()
        self.wait(lambda: bool(again.terminals), again)
        self.assertIn('ROW-049', self.screen(again))
        self.assertEqual(again.terminals[identifier].cols, 40)

    def test_background_preview_omits_history_until_selected(self):
        self.start(command=['/bin/sh', '-c', 'printf FIRST; sleep 60'])
        client = self.attach()
        first = client.state['tabs'][0]['id']
        command = ['/'.join(('', 'bin', 'sh')), '-c',
                   'i=0; while [ "$i" -lt 100 ]; do echo SEQ-$i; i=$((i+1)); done; sleep 60']
        second = self.request(client, 'new', command=command)['tab']
        self.wait(lambda: 'SEQ-99' in self.screen(client), client)
        self.assertTrue(self.request(client, 'choose', tab=first)['ok'])
        client.close()
        self.wait(lambda: not broker.status_session('owned', self.root)['attached'])
        again = self.attach()
        self.request(again, 'watch', tabs=[first, second])
        self.wait(lambda: second in again.terminals, again)
        self.assertEqual(len(again.terminals[second].history), 0)
        self.assertTrue(self.request(again, 'choose', tab=second)['ok'])
        self.wait(lambda: bool(again.terminals[second].history), again)
        self.assertIn('SEQ-0', self.screen(again))

    def test_new_tab_inherits_owned_shell_current_directory(self):
        self.start()
        client = self.attach()
        client.input(b'cd ..; printf CWD_READY\n')
        self.wait(lambda: 'CWD_READY' in self.screen(client), client)
        reply = self.request(client, 'new', command=['/bin/sh', '-c', 'pwd; sleep 60'])
        self.assertTrue(reply['ok'])
        self.wait(lambda: len(client.state['tabs']) == 2, client)
        second = client.state['tabs'][1]
        self.assertEqual(Path(second['cwd']), self.cwd.parent.resolve())

    def test_stale_socket_is_recovered_and_duplicate_start_refused(self):
        path = broker.socket_path('owned', self.root)
        raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        raw.bind(str(path));raw.close();path.chmod(0o700)
        self.start()
        with self.assertRaisesRegex(broker.BrokerError, 'already exists'):
            self.start()
        self.assertEqual(len(broker.list_sessions(self.root)), 1)

    def test_deeply_malformed_peer_cannot_stop_owned_session(self):
        self.start()
        pid = broker.status_session('owned', self.root)['tabs'][0]['pid']
        for payload in (b'[' * 2000 + b']' * 2000 + b'\n', b'42\n', b'{bad json}\n'):
            with broker._safe_connect(broker.socket_path('owned', self.root)) as raw:
                raw.sendall(payload)
                raw.settimeout(1)
                try:raw.recv(65536)
                except ConnectionResetError:pass
            self.assertEqual(broker.status_session('owned', self.root)['tabs'][0]['pid'], pid)
        client = self.attach()
        client.input(b'printf ALIVE\n')
        self.wait(lambda: 'ALIVE' in self.screen(client), client)

    def test_theme_applies_to_existing_and_new_tabs(self):
        self.start()
        client = self.attach()
        palette = [[i, i+1, i+2] for i in range(16)]
        fg, bg = [101,102,103], [1,2,3]
        self.assertTrue(self.request(client, 'theme', palette=palette, foreground=fg, background=bg)['ok'])
        self.wait(lambda: bool(client.terminals) and next(iter(client.terminals.values())).foreground == tuple(fg), client)
        reply = self.request(client, 'new', command=['/bin/sh', '-c', 'printf COLORED; sleep 60'])
        self.wait(lambda: reply['tab'] in client.terminals, client)
        self.assertEqual(client.terminals[reply['tab']].palette, tuple(tuple(c) for c in palette))
        self.assertEqual(client.terminals[reply['tab']].background, tuple(bg))

    def test_paths_are_private_and_symlinks_refused(self):
        with self.assertRaises(broker.BrokerError):
            broker.socket_path('../escape', self.root)
        unsafe = Path(self.directory.name) / 'unsafe'
        unsafe.mkdir(mode=0o755)
        with self.assertRaises(broker.BrokerError):
            broker.runtime_directory(unsafe)
        linked = Path(self.directory.name) / 'linked'
        linked.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(broker.BrokerError):
            broker.runtime_directory(linked)
        self.start()
        self.assertEqual(broker.socket_path('owned', self.root).stat().st_mode & 0o777, 0o700)


class _EventClient:
    def __init__(self, client, events):
        self.client, self.events = client, events

    def poll(self, timeout=0):
        self.events.extend(self.client.poll(timeout))


if __name__ == '__main__':
    unittest.main()
