#!/usr/bin/env python3
"""Real image/character UI in disposable PTYs. Never starts a physical camera."""
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import time
import unittest
import zlib

from test_native_here import NativeHere
from test_here_dispatch import OwnedTerminal, tty_settings


def png(path):
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
    rows = bytearray()
    for y in range(48):
        rows.append(0)
        for x in range(64):
            color = (255, 0, 0, 255) if x < 32 else (0, 0, 255, 255)
            if 16 < x < 48 and abs(y - x * .65) < 3:
                color = (255, 255, 255, 255)
            rows.extend(color)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!2I5B', 64, 48, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


@unittest.skipUnless(sys.platform == 'darwin', 'native ImageIO source')
class HereMedia(unittest.TestCase):
    setUp = NativeHere.setUp
    cli = NativeHere.cli
    stop = NativeHere.stop
    host = NativeHere.host
    status = NativeHere.status

    def image(self):
        path = self.root / "photo 'with spaces'.png"
        png(path)
        return path

    def media(self, path):
        self.env['TERM_PROGRAM'] = 'owned-test'
        t = self.host('--session', self.name, '--image', str(path))
        t.wait(lambda: b'Braille' in t.output and b'38;2;255;0;0' in t.output, 'native image frame')
        return t

    def test_bare_executable_renders_image_modes_and_resize(self):
        t = self.media(self.image())
        t.wait(lambda: b'0;0;255' in t.output, 'both image edges')
        self.assertTrue(any(0x2800 <= ord(c) <= 0x28ff for c in t.output.decode('utf8', 'replace')))
        state = self.status()
        self.assertTrue(state['tabs'][0]['media'])
        pid = state['tabs'][0]['pid']
        for name in (b'sextants', b'quadrants', b'half blocks', b'Braille'):
            start = len(t.output)
            t.send(b'm')
            t.wait(lambda: name in t.output[start:], 'rendering mode toggle')
        t.size(19, 91)
        os.kill(t.process.pid, signal.SIGWINCH)
        t.wait(lambda: self.status()['rows'] == 18, 'resized media terminal')
        self.assertEqual(pid, self.status()['tabs'][0]['pid'])
        t.send(b'\x02D')
        self.assertEqual(t.exited(), 0)
        self.assertEqual(tty_settings(t.slave), t.original)

    def test_image_view_and_tv_survive_detach_without_interpreter(self):
        t = self.media(self.image())
        t.send(b'\x02v')
        before = self.status()['tabs'][0]['pid']
        t.send(b'\x02D')
        self.assertEqual(t.exited(), 0)
        second = self.host('--attach', self.name)
        second.wait(lambda: b'Braille' in second.output, 'restored media view')
        self.assertEqual(before, self.status()['tabs'][0]['pid'])
        second.send(b'q')
        second.wait(lambda: self.status()['tabs'][0]['exited'], 'image helper closes')

    def test_files_opens_image_and_returns_to_listing(self):
        gallery = self.root / 'gallery'
        gallery.mkdir()
        path = gallery / "photo 'with spaces'.png"
        png(path)
        t = self.host('--session', self.name, '--', str(self.exe), '--here-files', str(gallery))
        t.wait(lambda: b'Hardcore files' in t.output, 'native Files')
        t.send(b'j\r')  # ../ is first, then the only image.
        t.wait(lambda: b'Braille' in t.output, 'image opened from Files')
        start = len(t.output)
        t.send(b'q')
        t.wait(lambda: b'Hardcore files' in t.output[start:], 'return to Files')
        self.assertEqual(len(self.status()['tabs']), 1)
        # Direct image entry is also reachable without shell interpretation.
        t.send(b'\x02I' + str(path).encode() + b'\r')
        t.wait(lambda: b'Braille' in t.output, 'image path prompt')
        self.assertEqual(len(self.status()['tabs']), 2)

    def test_missing_image_reports_error_without_losing_terminal(self):
        t = self.host('--session', self.name, '--image', str(self.root / 'missing.png'))
        t.wait(lambda: b'Choose a regular image file' in t.output, 'image error')
        t.send(b'q')
        t.wait(lambda: self.status()['tabs'][0]['exited'], 'close failed image')

    def test_hidden_camera_never_opens_and_control_eof_exits(self):
        parent, child = socket.socketpair()
        self.addCleanup(parent.close)
        self.addCleanup(child.close)
        env = dict(self.env, HC_HERE_MEDIA_FD=str(child.fileno()))
        t = OwnedTerminal([str(self.exe), '--here-media', '--camera'], env, self.root, (child.fileno(),))
        self.addCleanup(t.close)
        child.close()
        parent.sendall(b'0')
        t.wait(lambda: b'Camera off while hidden' in t.output, 'hidden camera is off')
        self.assertNotIn(b'Waiting for camera permission', t.output)
        parent.close()
        self.assertEqual(t.exited(), 0)
        self.assertEqual(tty_settings(t.slave), t.original)

    def test_files_image_handles_batched_return_and_restores_wrap(self):
        gallery = self.root / 'gallery'
        gallery.mkdir()
        png(gallery / 'photo.png')
        t = OwnedTerminal([str(self.exe), '--here-files', str(gallery)], self.env, self.root)
        self.addCleanup(t.close)
        t.wait(lambda: b'Hardcore files' in t.output, 'Files ready')
        start = len(t.output)
        t.send(b'j\rq')
        def returned():
            data = t.output[start:]
            image_at = data.find(b'Braille')
            return image_at >= 0 and b'Hardcore files' in data[image_at:]
        t.wait(returned, 'batched enter and return')
        self.assertIsNone(t.process.poll(), 'return key must not close Files')
        self.assertIn(b'\x1b[?7h', t.output[start:])
        t.send(b'q')
        self.assertEqual(t.exited(), 0)
        self.assertEqual(tty_settings(t.slave), t.original)


del NativeHere
if __name__ == '__main__':
    unittest.main(verbosity=2)
