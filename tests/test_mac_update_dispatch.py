#!/usr/bin/env python3
"""Exercise reload CLI routing against owned fake sockets; no GUI or update runs."""
import json
import os
from pathlib import Path
import plistlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = (ROOT/'build/hardcore').resolve()


def exact(peer, length):
    data = b''
    while len(data) < length:
        chunk = peer.recv(length-len(data))
        if not chunk:
            raise EOFError('peer disconnected')
        data += chunk
    return data


class OwnedServer:
    def __init__(self, path, mode, executable):
        self.mode, self.executable = mode, str(executable)
        self.requests, self.errors = [], []
        self.stop = threading.Event()
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.bind(str(path))
        self.socket.listen(4)
        self.socket.settimeout(.05)
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stop.is_set():
            try:
                peer, _ = self.socket.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                with peer:
                    peer.settimeout(2)
                    first = exact(peer, 4)
                    if self.mode == 'exec':
                        command = first
                        while not command.endswith(b'\n') and len(command) < 32:
                            command += exact(peer, 1)
                        self.requests.append(command.decode())
                        if command == b'info\n':
                            peer.sendall(json.dumps({'exe': self.executable, 'pid': 1234}).encode())
                        elif command == b'reload\n':
                            peer.sendall(b'ok\n')
                        else:
                            raise AssertionError('unexpected exec command: '+repr(command))
                    else:
                        size = struct.unpack('!I', first)[0]
                        if size > 256*1024*1024:
                            # Early plist-only windows reject text info at framing.
                            continue
                        request = plistlib.loads(exact(peer, size))
                        self.requests.append(request)
                        if request['op'] == 'list':
                            reply = {'protocol': 1, 'exe': self.executable, 'tabs': [{'pid': 5678}]}
                        elif request['op'] == 'reload':
                            if request['exe'] != self.executable:
                                raise AssertionError('wrong replacement executable')
                            reply = {'ok': True}
                        else:
                            raise AssertionError('unexpected plist request')
                        data = plistlib.dumps(reply, fmt=plistlib.FMT_BINARY)
                        peer.sendall(struct.pack('!I', len(data))+data)
            except BaseException as error:
                self.errors.append(repr(error))

    def close(self):
        self.stop.set()
        self.socket.close()
        self.thread.join(timeout=3)
        if self.thread.is_alive():
            raise AssertionError('owned fake-server thread did not stop')


@unittest.skipUnless(sys.platform == 'darwin' and EXE.is_file(), 'built macOS executable required')
class UpdateDispatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hc-update-routing-', dir='/private/tmp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root/'run'
        self.runtime.mkdir(mode=0o700)
        self.home = self.root/'home'
        self.home.mkdir(mode=0o700)
        self.env = dict(os.environ, HOME=str(self.home), HARDCORE_RUNTIME_DIR=str(self.runtime))

    def run_cli(self, *args):
        result = subprocess.run([str(EXE), *args], env=self.env, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        return result

    def server(self, mode='exec', executable=EXE):
        server = OwnedServer(self.runtime/'1234.sock', mode, executable)
        self.addCleanup(server.close)
        return server

    def test_all_local_reload_spellings_use_exec_protocol(self):
        server = self.server()
        for args in [('reload',), ('--reload',), ('update', '--local')]:
            with self.subTest(args=args):
                result = self.run_cli(*args)
                self.assertIn('1 window reloaded in place', result.stdout)
        self.assertEqual(server.requests, ['info\n', 'reload\n']*3)
        self.assertEqual(server.errors, [])

    def test_early_plist_only_window_uses_compatible_reload_once(self):
        server = self.server('plist')
        result = self.run_cli('--reload')
        self.assertIn('1 window reloaded in place', result.stdout)
        self.assertEqual([r['op'] for r in server.requests], ['list', 'reload'])
        self.assertEqual(server.errors, [])

    def test_unrelated_installation_never_receives_reload(self):
        server = self.server(executable='/private/tmp/unrelated-hardcore')
        result = self.run_cli('update', '--local')
        self.assertIn('0 windows reloaded in place', result.stdout)
        self.assertEqual(server.requests, ['info\n'])
        self.assertEqual(server.errors, [])

    def test_empty_private_runtime_does_not_discover_user_windows(self):
        result = self.run_cli('--reload')
        self.assertIn('0 windows reloaded in place', result.stdout)
        self.assertEqual(list(self.runtime.iterdir()), [])


if __name__ == '__main__':
    unittest.main()
