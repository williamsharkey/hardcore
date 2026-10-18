#!/usr/bin/env python3
"""Real GUI Chrome view, input routing, and same-document in-place reload.

Uses an owned app copy, private runtime/config/Chrome profile, and loopback site.
No keyboard/mouse events are posted to the user's desktop or other windows.
"""
import http.server
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zlib

from test_chrome_mac import NativeApp, ROOT, wait_for

PAGE = b'''<!doctype html><meta charset="utf-8"><title>Chrome GUI fixture</title>
<style>body{margin:0;background:#183047;color:#fff;font:20px monospace}
h1{margin:25px 20px}input,button{position:absolute;left:20px;width:240px;height:34px;font:18px monospace}
input{top:100px}button{top:160px}p{position:absolute;top:220px;left:20px}</style>
<h1>Chrome native input fixture</h1><input id="name" aria-label="Name" value="">
<button id="save">Save draft</button><p>Live Chrome documents remain alive across Hardcore reload.</p>
<img src="/protected.png" alt="Authenticated picture" width="64" height="48" style="position:absolute;left:320px;top:100px">
<script>let saves=0;document.getElementById('save').onclick=()=>{document.title='Saved:'+document.getElementById('name').value+':'+(++saves)};</script>'''


def image_fixture():
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value))
    scanlines = b''.join(b'\0' + bytes([197, 15, 31]) * 64 for _ in range(48))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 48, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b'')


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        image = self.path == '/protected.png'
        allowed = not image or 'owned_image=yes' in self.headers.get('Cookie', '')
        if image:
            self.server.image_requests.append(allowed)
        if image and allowed:
            # Projection runs before this image finishes downloading. The
            # renderer must recover when Chrome later exposes the resource.
            time.sleep(2)
        body = image_fixture() if image and allowed else b'Authentication required' if image else PAGE
        self.send_response(200 if allowed else 401)
        self.send_header('Content-Type', 'image/png' if image else 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        if not image:
            self.send_header('Set-Cookie', 'owned_image=yes; Path=/; SameSite=Lax')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


def stop_profile(profile):
    # Never match the user's other Chrome instances or kill by executable name.
    marker = '--user-data-dir=' + str(profile)
    for sig in (signal.SIGTERM, signal.SIGKILL):
        table = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True)
        for line in table.splitlines():
            parts = line.strip().split(None, 1)
            if len(parts) == 2 and marker in parts[1] and 'Chrome' in parts[1]:
                try:
                    os.kill(int(parts[0]), sig)
                except ProcessLookupError:
                    pass
        if sig == signal.SIGTERM:
            time.sleep(.3)


def main():
    if sys.platform != 'darwin':
        print('SKIP: macOS GUI required')
        return
    chrome = Path('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome')
    if not chrome.is_file():
        print('SKIP: installed Google Chrome required')
        return
    with tempfile.TemporaryDirectory(prefix='hc-chrome-gui-') as temporary:
        root = Path(temporary)
        profile = root / 'Chrome'
        runtime = root / 'runtime'
        runtime.mkdir(mode=0o700)
        env = dict(os.environ)
        os.environ.update(HARDCORE_CHROME_PROFILE=str(profile), HARDCORE_CHROME_EXECUTABLE=str(chrome),
                          HARDCORE_RUNTIME_DIR=str(runtime))
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        server.image_requests = []
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with NativeApp(root, 1, root) as app:
                exe = root / 'hardcore.app/Contents/MacOS/hardcore'

                def command(*args):
                    result = subprocess.run([str(exe), 'browser', *map(str, args), '--window', str(app.proc.pid)],
                                            capture_output=True, text=True, timeout=15)
                    assert result.returncode == 0, result.stdout + result.stderr
                    return json.loads(result.stdout)

                url = f'http://127.0.0.1:{server.server_port}/'
                opened = command('open', url, '--mode', 'native')
                identity = opened['id']

                def browser():
                    return next(tab for tab in command('list')['tabs'] if tab['id'] == identity)

                wait_for(lambda: browser()['title'] == 'Chrome GUI fixture', timeout=40, description='Chrome page in GUI')
                before = browser()
                assert before['engine'] == 'chrome' and before['target'], before
                assert before['mode'] == 'native'
                wait_for(lambda: app.state()['first_responder'].endswith(':HCChromeBrowserView'), description='Chrome native responder')
                scale = before['backingScale']

                def click(x, y):
                    app.send(f'cmd pointer down {int(x*scale)},{int(y*scale)}')
                    app.send(f'cmd pointer up {int(x*scale)},{int(y*scale)}')

                click(70, 115)
                for char, keycode in [('a', 0), ('d', 2), ('a', 0)]:
                    app.send(f'key {keycode} 0 {char} {char}')
                click(70, 175)
                wait_for(lambda: browser()['title'] == 'Saved:ada:1', description='Native pointer and keyboard input')
                print('PASS Chrome GUI: real native surface, pointer, keyboard, DOM updates')

                command('mode', identity, 'hardcore')
                assert browser()['mode'] == 'hardcore'
                wait_for(lambda: app.state()['first_responder'].endswith(':HCView'), description='Grid mode responder')
                assert browser()['title'] == 'Saved:ada:1' and browser()['target'] == before['target']
                def rendered_image():
                    bitmap = app.snap('authenticated-chrome-image')
                    pixels = bitmap.data[bitmap.offset:]
                    return sum(pixels[i:i+3] == bytes([31, 15, 197]) for i in range(0, len(pixels), 4)) >= 500
                wait_for(rendered_image, description='Authenticated image decoded into theme-colored grid')
                assert server.image_requests and all(server.image_requests), server.image_requests
                print('PASS Chrome GUI: authenticated image uses Chrome cookies and renders through grid palette')
                command('mode', identity, 'native')
                state_before = app.state()
                result = subprocess.run([str(exe), '--reload'], capture_output=True, text=True, timeout=40)
                assert result.returncode == 0 and '1 window' in result.stdout, result.stdout + result.stderr
                wait_for(lambda: browser()['title'] == 'Saved:ada:1', timeout=40, description='Same document after reload')
                after = browser()
                assert after['target'] == before['target'], (before, after)
                state_after = app.state()
                assert state_after['pid'] == state_before['pid']
                assert [tab['pid'] for tab in state_after['tabs']] == [tab['pid'] for tab in state_before['tabs']]
                click(70, 175)
                wait_for(lambda: browser()['title'] == 'Saved:ada:2', description='Live form and JavaScript heap after reload')
                print('PASS Chrome GUI: mode switches and in-place reload retain target, form, app PID and shell PID')
                command('close', identity)
                wait_for(lambda: all(tab['id'] != identity for tab in command('list')['tabs']), description='Owned browser tab closes')

                # Real application Quit must close this window's Chrome targets,
                # unlike exec/reload, without shutting down the shared profile.
                opened = command('open', url, '--mode', 'native')
                identity = opened['id']
                wait_for(lambda: browser()['title'] == 'Chrome GUI fixture', description='Quit fixture browser ready')
                target = browser()['target']
                endpoint = app.chrome_profile / 'DevToolsActivePort'
                port = int(endpoint.read_text().splitlines()[0])

                def chrome_targets():
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/json/list', timeout=5) as response:
                        return json.load(response)

                assert any(tab['id'] == target for tab in chrome_targets())
                tabs = command('list')['tabs']
                terminal = next(index for index, tab in enumerate(tabs) if tab['kind'] == 'terminal')
                # This is the fixture's own sleep/shell. Close it normally so
                # Cmd-Q has no running-terminal confirmation to dismiss.
                app.send(f'cmd tab {terminal}')
                app.send('cmd closetab')
                wait_for(lambda: all(tab['kind'] == 'browser' for tab in command('list')['tabs']), description='Owned fixture shell closes')
                app.send('key 12 100000 q q')
                assert app.proc.wait(timeout=10) == 0, 'Real GUI quit failed'
                wait_for(lambda: all(tab['id'] != target for tab in chrome_targets()), description='GUI quit closes owned Chrome target')
                # A successful HTTP response after app exit also proves Chrome
                # remains alive; it was not terminated as an app-wide shortcut.
                assert isinstance(chrome_targets(), list)
                print('PASS Chrome GUI: real Quit closes owned page and preserves the Chrome profile process')
        finally:
            server.shutdown()
            server.server_close()
            stop_profile(profile)
            os.environ.clear()
            os.environ.update(env)


if __name__ == '__main__':
    main()
