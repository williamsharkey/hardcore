#!/usr/bin/env python3
"""Real TUI controls and muted local playback in an isolated Hardcore window."""
import os
from pathlib import Path
import socket
import tempfile
import time
import unittest
import wave
from unittest.mock import patch

from test_chrome_mac import ROOT, NativeApp, wait_for


def fixtures(root):
    media=root/'quiet.wav'
    with wave.open(str(media),'wb') as wav:
        wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(8000)
        wav.writeframes(b'\0\0'*8000*30)
    catalog=root/'catalog.xml'
    items=''.join(f'<item><title>Episode {n}: Quiet fixture {n}</title>'
                  f'<link>https://musicforprogramming.net/{n}</link>'
                  f'<enclosure url="https://datashat.net/fixture-{n}.mp3" type="audio/mpeg"/>'
                  '<itunes:duration>00:30</itunes:duration></item>' for n in range(30,0,-1))
    catalog.write_text('<rss xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd"><channel>'+items+'</channel></rss>')
    return catalog,media


class MusicNativeTests(unittest.TestCase):
    def test_transport_tv_and_reload(self):
        with tempfile.TemporaryDirectory(prefix='hc-music-native-',dir='/private/tmp') as name:
            root=Path(name);catalog,media=fixtures(root)
            with patch.dict(os.environ,HARDCORE_TEST_MUSIC_CATALOG=str(catalog),HARDCORE_TEST_MUSIC_MEDIA=str(media)):
                with NativeApp(root/'app',0,ROOT/'build/music-native') as app:
                    original=app.state()['tabs'][0]['pid']
                    app.header_click('music')
                    chrome=wait_for(lambda:(c if (c:=app.chrome())['music']['placed'] and c['music']['player']['count']==30 else None),description='Music TV with local catalog')
                    self.assertFalse(chrome['music']['player']['playing'],'opening the player must not autoplay')
                    self.assertEqual(chrome['music']['player']['position'],0)
                    app.snap('music-open')

                    def point(col,row):
                        c=app.chrome();r=c['music']['rect'];cell=c['cell']
                        return ((r['x']+col)*cell['width']+cell['width']//2,(r['y']+row)*cell['height']+cell['height']//2)
                    def click(col,row):
                        x,y=point(col,row)
                        app.send(f'cmd pointer monitor-down {x},{y}');app.send(f'cmd pointer monitor-up {x},{y}')
                    # Title text, not just the move glyph, is a drag handle.
                    r=chrome['music']['rect'];x,y=point(14,0);cell=chrome['cell']
                    app.send(f'cmd pointer monitor-down {x},{y}')
                    app.send(f'cmd pointer monitor-drag {x+3*cell["width"]},{y+cell["height"]}')
                    app.send(f'cmd pointer monitor-up {x+3*cell["width"]},{y+cell["height"]}')
                    moved=app.chrome()['music']['rect'];self.assertNotEqual((r['x'],r['y']),(moved['x'],moved['y']))
                    click(5,6)  # actual release-triggered episode action
                    wait_for(lambda:app.chrome()['music']['player']['position']>=1,description='muted AVPlayer advancing')
                    click(moved['width']-2,0)
                    c=app.chrome();self.assertFalse(c['music']['visible']);self.assertTrue(c['music']['player']['playing'])
                    self.assertIn('music_play',[control['name'] for control in c['header_controls']])
                    app.snap('music-minimized')
                    app.header_click('music_next')
                    wait_for(lambda:app.chrome()['music']['player']['index']==1,description='compact next')
                    self.assertTrue(app.chrome()['music']['player']['playing'])
                    app.header_click('music_previous')
                    wait_for(lambda:app.chrome()['music']['player']['index']==0,description='compact previous')
                    app.header_click('music_play')
                    paused=app.chrome()['music']['player'];self.assertFalse(paused['playing'])
                    time.sleep(.3)
                    self.assertLessEqual(abs(app.chrome()['music']['player']['position']-paused['position']),1)
                    app.header_click('music')
                    c=app.chrome();self.assertTrue(c['music']['visible']);self.assertFalse(c['music']['player']['loading'])
                    click(16,3)  # seek while paused
                    wait_for(lambda:app.chrome()['music']['player']['position']>=5,description='paused seek')
                    app.header_click('music_play')
                    before=app.chrome()['music'];self.assertTrue(before['player']['playing'])
                    # Exec-based update preserves the music selection and
                    # position as well as the actual terminal child.
                    with socket.socket(socket.AF_UNIX) as peer:
                        peer.settimeout(15);peer.connect(f'/tmp/hardcore-{os.getuid()}/{app.proc.pid}.sock')
                        peer.sendall(b'reload\n');reply=b''
                        while True:
                            part=peer.recv(4096)
                            if not part:break
                            reply+=part
                    self.assertEqual(reply,b'ok\n')
                    after=wait_for(lambda:(c if (c:=app.chrome())['music']['enabled'] else None),description='music restored after update')
                    self.assertEqual(after['music']['player']['index'],before['player']['index'])
                    self.assertGreaterEqual(after['music']['player']['position'],before['player']['position']-1)
                    self.assertTrue(after['music']['player']['playing'])
                    app.header_click('music_stop')
                    stopped=app.chrome()['music']['player'];self.assertFalse(stopped['playing']);self.assertEqual(stopped['position'],0)
                    self.assertEqual(app.state()['tabs'][0]['pid'],original)
                    self.assertEqual(len(app.state()['tabs']),1,'music never needs a shell tab')


if __name__=='__main__':unittest.main()
