#!/usr/bin/env python3
"""Owned Metal→replay→PNG/MP4 round trip; no camera or microphone is opened."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from PIL import Image, ImageChops
from test_chrome_mac import ROOT, NativeApp, wait_for
from test_tab_preview_extended_mac import browser_command


class NativeRecordingTests(unittest.TestCase):
    def test_clips_tv_background_export(self):
        with tempfile.TemporaryDirectory(prefix='hc-clips-native-', dir='/private/tmp') as folder:
            root=Path(folder)
            recordings=root/'recordings';recordings.mkdir()
            artifacts=ROOT/'build/clips-native'
            with patch.dict(os.environ, HARDCORE_TEST_RECORDINGS=str(recordings)):
                with NativeApp(root/'app',0,artifacts) as app:
                    app.feed(['Clips TUI export verification', '\x1b[32mSame terminal, same colors\x1b[0m'])
                    app.send(f'cmd record-fixture {recordings}/clip.partial.hcrec')
                    bundle=recordings/'clip.hcrec'
                    wait_for(lambda:(bundle/'replay.json').exists(),description='saved synthetic recording')
                    # A longer static replay lets us observe a real encoder job
                    # while its TV is hidden. No camera or microphone is used.
                    for name in ['manifest.json','replay.json']:
                        path=bundle/name;data=json.loads(path.read_text());data['durationMs']=12000
                        path.write_text(json.dumps(data))
                    (bundle/'recording.js').write_text('window.HARDCORE_RECORDING='+ (bundle/'replay.json').read_text()+';\n')
                    app.send('cmd clips')
                    state=wait_for(lambda:(s if (s:=app.chrome())['camera']['placed'] else None),description='Clips TV')
                    camera=state['camera']
                    self.assertTrue(camera['clips']);self.assertFalse(camera['session_running'])
                    self.assertEqual(camera['clips_count'],1)
                    self.assertEqual(len(app.state()['tabs']),1,'opening Clips must not create a popup or terminal tab')
                    app.snap('clips-library')
                    library_image=Image.open(artifacts/'clips-library.png')
                    rect=camera['rect'];cell=state['cell']
                    # One-cell action labels must draw their glyph, not just
                    # the button background (layout_text used to skip them).
                    gx=(rect['x']+rect['width']-2)*cell['width']
                    gy=rect['y']*cell['height']
                    minimize=library_image.crop((gx+1,gy+2,gx+cell['width']-1,gy+cell['height']-2)).convert('RGB')
                    self.assertGreater(len(minimize.getcolors() or []),1,'Clips minimize glyph is visible')

                    def click(col,row):
                        current=app.chrome();rect=current['camera']['rect'];cell=current['cell']
                        x=(rect['x']+col)*cell['width']+cell['width']//2
                        y=(rect['y']+row)*cell['height']+cell['height']//2
                        app.send(f'cmd pointer monitor-down {x},{y}')
                        app.send(f'cmd pointer monitor-up {x},{y}')

                    # Export from the TUI and minimize before the encoder ends.
                    click(10,camera['rect']['height']-4)
                    state=wait_for(lambda:(s if (s:=app.chrome())['exports'] else None),description='export job')
                    self.assertEqual(state['exports'][0]['state'],'running')
                    app.snap('clips-exporting')
                    rect=state['camera']['rect'];click(rect['width']-2,0)
                    self.assertFalse(app.chrome()['camera']['visible'])
                    completed=wait_for(lambda:(jobs[0] if (jobs:=app.chrome()['exports']) and jobs[0]['state']!='running' else None),
                                       timeout=60,description='background MP4 export')
                    self.assertEqual(completed['state'],'done',completed['message'])
                    self.assertEqual(completed['progress'],1)
                    movie=Path(completed['output']);self.assertGreater(movie.stat().st_size,1000)
                    self.assertEqual(movie.parent,recordings/'Exports')
                    app.send('cmd clips')
                    state=app.chrome();self.assertTrue(state['camera']['clips_exports'])
                    self.assertFalse(state['camera']['session_running'])
                    app.snap('clips-complete')
                    click(21,state['camera']['rect']['height']-4)
                    wait_for(lambda:len(app.state()['tabs'])==2,description='output video tab')
                    def loaded_host(host,site):
                        return next((tab for tab in browser_command(app,'list')['tabs']
                                     if tab['active'] and tab['host']==host and tab['site']==site),None)
                    def check_frame(tab):
                        x,y,w,h=tab['viewFrame'];bx,by,bw,bh=tab['viewBounds']
                        self.assertGreater(w,0);self.assertGreater(h,0)
                        self.assertGreater(bw-w,0,'the tab menu keeps its own space')
                        self.assertGreater(bh-h,0,'the title and browser controls keep their own space')
                        self.assertGreaterEqual(x,bx);self.assertGreaterEqual(y,by)
                        self.assertLessEqual(x+w,bx+bw+.1);self.assertLessEqual(y+h,by+bh+.1)
                    check_frame(wait_for(lambda:loaded_host('video','Video'),description='embedded MP4 loaded'))
                    self.assertIsNone(app.proc.poll())
                    # Replay also stays in the app, then closes independently
                    # of the already-completed export and its output file.
                    state=app.chrome();click(2,state['camera']['rect']['height']-4)
                    wait_for(lambda:len(app.state()['tabs'])==3,description='recording replay tab')
                    check_frame(wait_for(lambda:loaded_host('replay','Clips'),description='embedded replay loaded'))
                    app.send('cmd closetab')
                    wait_for(lambda:len(app.state()['tabs'])==2,description='closed replay tab')
                    self.assertTrue(movie.is_file())
                    self.assertEqual(app.chrome()['exports'][0]['state'],'done')

    def test_real_renderer_round_trip(self):
        with tempfile.TemporaryDirectory(prefix='hc-recording-native-', dir='/private/tmp') as folder:
            root = Path(folder)
            artifacts = ROOT / 'build/recording-native'
            with NativeApp(root / 'app', 0, artifacts) as app:
                app.feed(['', '', '\x1b[31mRed\x1b[0m green \x1b[48;2;12;48;72mbackground\x1b[0m',
                          'Unicode café 界 arrows → ↕', '\x1b[1mBold\x1b[0m and regular',
                          'Camera-free recorder round trip'])
                before = app.snap('source')
                draft = root / 'fixture.partial.hcrec'
                bundle = root / 'fixture.hcrec'
                app.send(f'cmd record-fixture {draft}')
                wait_for(lambda: (bundle / 'replay.json').exists(), description='finalized replay')
                payload = json.loads((bundle / 'replay.json').read_text())
                self.assertEqual(payload['version'], 1)
                self.assertGreater(len(payload['frames']), 0)
                self.assertIn('Camera-free recorder round trip', payload['frames'][0]['text'])
                self.assertEqual(payload['frames'][0]['w'], before.width)
                self.assertEqual(payload['frames'][0]['h'], before.height)
                self.assertTrue(payload['frames'][0]['atlas'])
                self.assertIsNone(payload['camera'])
                self.assertFalse((bundle / 'frames.jsonl').exists())
                self.assertTrue((bundle / 'recording.js').read_text().startswith('window.HARDCORE_RECORDING='))
                for asset in ['index.html', 'player.js', 'core.js', 'style.css', 'edits.js']:
                    self.assertTrue((bundle / asset).is_file())
                subprocess.run(['node', '-e', 'const c=require(process.argv[1]);c.parseRecording(require(process.argv[2]));',
                                str(ROOT / 'recording/core.js'), str(bundle / 'replay.json')], check=True)
                exported = root / 'replay.png'
                subprocess.run([str(ROOT / 'build/recording-export'), str(bundle), str(exported),
                                '--snapshot', '0', '--width', str(before.width), '--height', str(before.height),
                                '--no-pointer', '--no-captions'], check=True)
                source = Image.open(artifacts / 'source.png').convert('RGB')
                image = Image.open(exported).convert('RGB')
                self.assertEqual(source.size, image.size)
                # AppKit title fade may advance between independent captures;
                # compare the actual terminal and menu below the title band.
                region = (0, 60, source.width, source.height)
                difference = ImageChops.difference(source.crop(region), image.crop(region))
                changed = sum(any(p) for p in difference.get_flattened_data())
                print(f'Replay round trip: {changed} changed pixels below title band')
                self.assertLess(changed, source.width * source.height * .001,
                                f'{changed} pixels differed after replay/export')
                image.save(artifacts / 'replay.png')
                movie = root / 'round-trip.mp4'
                subprocess.run([str(ROOT / 'build/recording-export'), str(bundle), str(movie),
                                '--fps', '10', '--width', '640', '--no-pointer', '--no-captions'], check=True)
                self.assertGreater(movie.stat().st_size, 1000)
                self.assertIsNone(app.proc.poll(), 'owned terminal survived recording')


if __name__ == '__main__':
    unittest.main()
