#!/usr/bin/env python3
"""Command transport against a local mock socket; never launches AppKit."""
import json
import os
from pathlib import Path
import socket
import subprocess
import threading

binary = str(Path(__file__).resolve().parents[1] / 'build/hardcore')
env = {k: v for k, v in os.environ.items() if k not in ('HARDCORE_WINDOW_PID', 'HARDCORE_TAB_ID')}
runtime = Path('/tmp') / f'hardcore-{os.getuid()}'
runtime.mkdir(mode=0o700, exist_ok=True)
pid = 90_000_000 + os.getpid()
path = runtime / f'{pid}.sock'
server = socket.socket(socket.AF_UNIX)
try:
    server.bind(str(path))
except PermissionError:
    server.close()
    for args in (['open','javascript:alert(1)'], ['mode','123','bad'], ['close','0'], ['open','x','extra']):
        result = subprocess.run([binary,'browser'] + args, env=env, capture_output=True, text=True, timeout=5)
        assert result.returncode == 2, (args,result.returncode,result.stderr)
    print('PASS browser CLI malformed arguments; SKIP socket transport: sandbox denies Unix socket creation')
    raise SystemExit(0)
server.listen(1)
errors = []

def exchange(arguments, expected, use_environment=False):
    received = []
    def receive():
        try:
            server.settimeout(5)
            with server.accept()[0] as client:
                data = b''
                while not data.endswith(b'\n'):
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                assert data.startswith(b'browser '), data
                received.append(json.loads(data[8:]))
                client.sendall(b'{"ok":true,"id":123}')
        except Exception as error:
            errors.append(error)
    worker = threading.Thread(target=receive)
    worker.start()
    child_env = dict(env)
    if use_environment:
        child_env.update(HARDCORE_WINDOW_PID=str(pid), HARDCORE_TAB_ID='321')
    else:
        arguments = arguments + ['--window', str(pid)]
    result = subprocess.run([binary, 'browser'] + arguments, env=child_env, capture_output=True, text=True, timeout=8)
    worker.join(6)
    assert not errors, errors
    assert result.returncode == 0, result.stderr
    assert received == [expected], received
    assert json.loads(result.stdout)['id'] == 123

try:
    exchange(['open', 'https://example.com/a?b=x&c=y', '--parent', '321'],
             {'op':'open', 'url':'https://example.com/a?b=x&c=y', 'parent':321}, True)
    exchange(['open', 'example.com', '--parent', '321'], {'op':'open','url':'https://example.com','parent':321})
    exchange(['mode', '123', 'native'], {'op':'mode','id':123,'mode':'native'})
    exchange(['list'], {'op':'list'})
    exchange([], {'op':'open','url':'hardcore:welcome'})
    exchange(['open', '/tmp/a b.html', '--mode', 'native'],
             {'op':'open','url':'file:///tmp/a%20b.html','mode':'native'})
    for args in (['open','javascript:alert(1)'], ['mode','123','bad'], ['close','0'], ['open','x','extra']):
        result = subprocess.run([binary,'browser'] + args, env=env, capture_output=True, text=True, timeout=5)
        assert result.returncode == 2, (args,result.returncode,result.stderr)
    print('PASS browser CLI: window/tab targeting, URL escaping, mode/list/default commands and malformed argument rejection (no GUI)')
finally:
    server.close()
    path.unlink(missing_ok=True)
