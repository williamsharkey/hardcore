#!/usr/bin/env python3
"""Owned Metal→replay→PNG/MP4 round trip; no camera or microphone is opened."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from PIL import Image, ImageChops
from test_chrome_mac import ROOT, NativeApp, wait_for


class NativeRecordingTests(unittest.TestCase):
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
