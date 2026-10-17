#!/usr/bin/env python3
"""Text web parsing/network limits and a real owned terminal lifecycle."""
import http.server
import fcntl
import importlib.util
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import here_web as web


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == '/slow':
            time.sleep(2)
            return
        if self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/page')
            self.end_headers()
            return
        if self.path == '/bad-redirect':
            self.send_response(302)
            self.send_header('Location', 'file:///etc/passwd')
            self.end_headers()
            return
        if self.path == '/loop':
            self.send_response(302)
            self.send_header('Location', '/loop')
            self.end_headers()
            return
        self.send_response(200)
        self.send_header('Content-Type', 'image/png' if self.path == '/image' else 'text/html; charset=utf-8')
        if self.path == '/huge':
            self.send_header('Content-Length', str(web.MAX_RESPONSE + 1))
        elif self.path == '/gzip':
            self.send_header('Content-Encoding', 'gzip')
        self.end_headers()
        self.wfile.write(b'<title>Owned web fixture</title><h1>HELLO_WEB</h1><p>First page <a href="/next">next</a>.</p>')


class WebTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f'http://127.0.0.1:{cls.server.server_port}'

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def test_url_validation_relative_unicode_and_port(self):
        self.assertEqual(web.normalize_url('example.test/a b'), 'https://example.test/a%20b')
        self.assertEqual(web.normalize_url('../b', 'https://example.test/a/c'), 'https://example.test/b')
        self.assertEqual(web.normalize_url('http://localhost:9000/'), 'http://localhost:9000/')
        self.assertIn('xn--', web.normalize_url('https://münich.test/'))
        for url in ('file:///tmp/a', 'javascript:alert(1)', 'data:text/html,Hi',
                    'https://user:password@example.test/', 'https://example.test:bad/',
                    'https://example.test/\x1b[2J', 'http://', 'x'*5000):
            with self.assertRaises(ValueError, msg=url):
                web.normalize_url(url)

    def test_html_visible_text_links_title_and_preformatted_data(self):
        parser = web.TextHTML('https://example.test/base/page')
        parser.feed('''<html><head><title>Nice &amp; readable</title><style>NO_STYLE</style>
            <base href="/new/"></head><body><h1>Hello</h1><p>A <a href="next">next page</a>
            and <a href="javascript:bad()">invalid target</a>.</p><div hidden>HIDDEN</div>
            <span style="display: none">NO_DISPLAY</span><script>NO_SCRIPT</script>
            <p><a href="next">same target</a><img alt="red ball" src="huge.png"></p>
            <pre>  indented\n    next</pre></body></html>''')
        page = parser.page()
        self.assertEqual(page.title, 'Nice & readable')
        self.assertIn('Hello', page.text)
        self.assertIn('next page [1]', page.text)
        self.assertIn('same target [1]', page.text)
        self.assertEqual(page.links, ['https://example.test/new/next'])
        self.assertIn('[Image: red ball]', page.text)
        self.assertIn('  indented\n    next', page.text)
        for hidden in ('HIDDEN', 'NO_SCRIPT', 'NO_STYLE', 'NO_DISPLAY'):
            self.assertNotIn(hidden, page.text)

    def test_terminal_controls_are_removed_and_bounds_enforced(self):
        parser = web.TextHTML(self.base)
        parser.feed('<p>Hello&#27;[2J\x07\u202eWorld</p>' + '<p>x</p>'*100 + 'a'*(web.MAX_TEXT+30))
        page = parser.page()
        for forbidden in ('\x1b', '\x07', '\u202e'):
            self.assertNotIn(forbidden, page.text)
        self.assertIn('truncated', page.text)
        self.assertLess(len(page.text), web.MAX_TEXT+100)
        parser = web.TextHTML(self.base)
        parser.feed(''.join(f'<a href="/{i}">link</a>' for i in range(web.MAX_LINKS+10)))
        self.assertEqual(len(parser.page().links), web.MAX_LINKS)
        with self.assertRaises(ValueError):
            web.TextHTML(self.base).feed('<div>'*300)

    def test_redirect_page_and_network_rejections(self):
        page = web.fetch_page(self.base+'/redirect')
        self.assertEqual(page.url, self.base+'/page')
        self.assertIn('HELLO_WEB', page.text)
        self.assertEqual(page.links, [self.base+'/next'])
        for path in ('/bad-redirect', '/loop', '/image', '/huge', '/gzip'):
            with self.assertRaises((ValueError, web.HTTPError), msg=path):
                web.fetch_page(self.base+path)

    def test_wrap_preserves_identifiers_unicode_and_blank_lines(self):
        lines = web.wrap_text('abcdefghijklm\n\n界界界\nhello world', 6)
        self.assertEqual(lines[:3], ['abcdef', 'ghijkl', 'm'])
        self.assertEqual(lines[3], '')
        self.assertEqual(lines[4], '界界界')
        self.assertEqual(lines[-2:], ['hello', 'world'])

    def test_keyboard_navigation_and_paste_never_execute_commands(self):
        reader = web.WebReader(self.base, host=object())
        reader.page = web.Page(self.base, 'fixture', '\n'.join(str(i) for i in range(50)), [self.base+'/next'])
        reader.rewrap()
        reader.input_bytes(b'j\x1b[B')
        self.assertEqual(reader.offset, 2)
        reader.input_bytes(b'\x1b[200~quoooo\x1b[201~')
        self.assertFalse(reader.stop)
        self.assertIsNone(reader.prompt)
        reader.key('u')
        for data in (b'\x1b[20', b'0~https://example.test/', b'\x1b[201~'):
            reader.input_bytes(data)
        self.assertEqual(reader.input, 'https://example.test/')
        reader.key('\x1b')
        with patch.object(reader, 'navigate') as navigate:
            reader.input_bytes(b'1\r')
            navigate.assert_called_once_with(self.base+'/next')
        with patch.object(web.webbrowser, 'open', return_value=True) as external:
            reader.input_bytes(b'o')
            external.assert_called_once_with(reader.url, new=2)

    def test_whole_fetch_deadline_terminates_owned_network_worker(self):
        class Host:
            def write(self, value):
                pass
        reader = web.WebReader(self.base, host=Host())
        try:
            with patch.object(web, 'FETCH_SECONDS', .2):
                reader.navigate(self.base+'/slow')
            worker = reader.worker
            deadline = time.monotonic()+2
            while reader.worker is not None and time.monotonic()<deadline:
                reader.poll()
                time.sleep(.02)
            self.assertIsNone(reader.worker)
            self.assertIn('timed out', reader.message)
        finally:
            reader.cancel_fetch()

    def test_real_owned_pty_load_quit_and_restore(self):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        original = termios.tcgetattr(slave)
        process = subprocess.Popen([sys.executable, '-B', str(ROOT/'tools/here_web.py'), self.base+'/page'],
                                   stdin=slave, stdout=slave, stderr=slave, start_new_session=True)
        output = bytearray()
        try:
            deadline = time.monotonic()+8
            while time.monotonic()<deadline and b'HELLO_WEB' not in output:
                if select.select([master], [], [], .1)[0]:
                    output.extend(os.read(master, 65536))
                if process.poll() is not None:
                    break
            self.assertIn(b'HELLO_WEB', output, output.decode(errors='replace'))
            os.write(master, b'q')
            deadline = time.monotonic()+4
            while process.poll() is None and time.monotonic()<deadline:
                if select.select([master], [], [], .05)[0]:
                    output.extend(os.read(master, 65536))
            self.assertEqual(process.wait(timeout=1), 0)
            restored = termios.tcgetattr(slave)
            # Darwin marks pending canonical input for retyping after raw mode;
            # that kernel-managed transient bit is not an application setting.
            restored[3] &= ~getattr(termios, 'PENDIN', 0)
            original[3] &= ~getattr(termios, 'PENDIN', 0)
            self.assertEqual(restored, original)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


if __name__ == '__main__':
    unittest.main()
