#!/usr/bin/env python3
"""Offline native replay export regression tests, with synthetic camera + PCM audio."""
import base64
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
FRAMEWORKS = ['Foundation', 'AppKit', 'AVFoundation', 'CoreMedia', 'CoreVideo', 'CoreGraphics', 'ImageIO']


def quad(rect, color=(255, 255, 255, 255), uv=(65535, 0, 0, 0)):
    return struct.pack('<4h4H4B', *rect, *uv, *color)


def frame(t=0, w=320, h=180, bg=0x101010, quads=b'', atlas=None, **kwargs):
    return dict(t=t, w=w, h=h, bg=bg, transparent=False,
                quads=base64.b64encode(quads).decode(), atlas=atlas or [], **kwargs)


def replay(frames=None, duration=2000, **kwargs):
    return dict(version=1, durationMs=duration, frames=frames or [frame()],
                pointer=[], captions=[], **kwargs)


@unittest.skipUnless(sys.platform == 'darwin', 'AVFoundation exporter requires macOS')
class RecordingExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-recording-export-', dir='/private/tmp')
        cls.root = Path(cls.temp.name)
        cls.binary = cls.root / 'recording-export'
        cls.probe = cls.root / 'media-probe'
        flags = ['clang', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-fobjc-arc']
        frameworks = [word for framework in FRAMEWORKS for word in ['-framework', framework]]
        for source, binary in [('tools/recording_export.m', cls.binary), ('tests/fixtures/recording_media_probe.m', cls.probe)]:
            subprocess.run(flags + [str(ROOT / source)] + frameworks + ['-o', str(binary)], check=True)
        cls.camera_bundle = cls.root / 'camera-source'
        cls.camera_bundle.mkdir()
        with wave.open(str(cls.camera_bundle / 'tone.wav'), 'wb') as out:
            out.setparams((1, 2, 48000, 0, 'NONE', 'not compressed'))
            # Square tone with deterministic nonzero PCM samples.
            out.writeframes(b''.join(struct.pack('<h', 4000 if (i // 48) % 2 else -4000) for i in range(96000)))
        fixture = replay([frame(w=160, h=90, bg=0xFF0000,
                                quads=quad((80, 0, 160, 90), (0, 0, 255, 255))),
                          frame(t=1000, w=160, h=90, bg=0x00FF00)],
                         audio={'file': 'tone.wav', 'offsetMs': 0})
        (cls.camera_bundle / 'replay.json').write_text(json.dumps(fixture))
        cls.camera = cls.root / 'camera.mp4'
        result = subprocess.run([str(cls.binary), str(cls.camera_bundle), str(cls.camera), '--fps', '10'], capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.bundle = Path(tempfile.mkdtemp(dir=self.root))
        self.data = replay()

    def run_export(self, output='out.mp4', *args, success=True):
        (self.bundle / 'replay.json').write_text(json.dumps(self.data))
        path = self.bundle / output
        result = subprocess.run([str(self.binary), str(self.bundle), str(path), *args], capture_output=True, text=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(path.is_file())
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(path.exists())
            self.assertFalse(list(self.bundle.glob('*.partial')))
        return path, result

    def preview(self, time=0, name='frame.png', *args):
        path, _ = self.run_export(name, '--snapshot', str(time), *args)
        return Image.open(path).convert('RGB')

    def test_quad_orientation_alpha_atlas_and_persistent_updates(self):
        patch = dict(x=0, y=0, w=2, h=2, data=base64.b64encode(bytes([255, 0, 0, 255])).decode())
        quads = quad((0, 0, 40, 40), (255, 0, 0, 255)) + quad((60, 10, 80, 30), (0, 255, 0, 255), (0, 0, 2, 2)) + quad((100, 10, 120, 30), (255, 255, 255, 128))
        fractional = dict(x=2, y=0, w=1, h=1, data=base64.b64encode(bytes([200])).decode())
        quads += quad((140, 10, 160, 30), (255, 255, 255, 200), (2, 0, 3, 1))
        self.data = replay([frame(bg=0, quads=quads, atlas=[patch, fractional]), frame(t=500, bg=0, quads=quads)])
        image = self.preview(.7)
        self.assertEqual(image.getpixel((10, 10)), (255, 0, 0))
        self.assertEqual(image.getpixel((10, 170)), (0, 0, 0))
        self.assertEqual(image.getpixel((65, 15)), (0, 255, 0))
        self.assertEqual(image.getpixel((75, 15)), (0, 0, 0))
        self.assertEqual(image.getpixel((75, 25)), (0, 255, 0))
        self.assertEqual(image.getpixel((110, 20)), (128, 128, 128))
        self.assertEqual(image.getpixel((150, 20)), (157, 157, 157))

    def test_trim_crop_scale_and_h264_metadata(self):
        self.data = replay([frame(bg=0xFF0000), frame(t=500, bg=0x00FF00), frame(t=1000, bg=0x0000FF)])
        path, _ = self.run_export('out.mp4', '--start', '.5', '--end', '1.5', '--crop', '10,20,200,100', '--width', '400', '--fps', '10')
        png = self.bundle / 'decoded.png'
        result = subprocess.run([str(self.probe), str(path), '0', str(png)], capture_output=True, text=True, check=True)
        info = json.loads(result.stdout)
        self.assertEqual((info['width'], info['height']), (400, 200))
        self.assertAlmostEqual(info['duration'], 1, places=2)
        self.assertEqual(info['videoCodec'], int.from_bytes(b'avc1', 'big'))
        self.assertEqual(info['fps'], 10)
        rgb = Image.open(png).convert('RGB').getpixel((100, 100))
        self.assertGreater(rgb[1], 240)
        self.assertLess(rgb[0] + rgb[2], 40)

    def test_camera_offset_mirroring_composite_and_audio(self):
        shutil.copyfile(self.camera, self.bundle / 'camera.mp4')
        self.data = replay([frame(camera=dict(rect=[80, 40, 160, 90], visible=True, mirrored=True))],
                           camera={'file': 'camera.mp4', 'offsetMs': 250, 'mirrored': True})
        before = self.preview(.1, 'before.png')
        self.assertEqual(before.getpixel((100, 80)), (16, 16, 16))
        active = self.preview(.5, 'active.png')
        left, right = active.getpixel((100, 80)), active.getpixel((220, 80))
        self.assertGreater(left[2], 230)
        self.assertGreater(right[0], 230)
        path, _ = self.run_export('trimmed.mp4', '--start', '.5', '--end', '1.5', '--fps', '10')
        info = json.loads(subprocess.check_output([str(self.probe), str(path)]))
        self.assertTrue(info['audio'])
        self.assertEqual(info['audioCodec'], int.from_bytes(b'aac ', 'big'))
        self.assertEqual(info['sampleRate'], 48000)
        self.assertGreater(info['audioPeak'], 1000)
        self.assertAlmostEqual(info['firstAudible'], 0, delta=.03)
        self.assertAlmostEqual(info['duration'], 1, places=2)
        delayed, _ = self.run_export('delayed.mp4', '--start', '.1', '--end', '.8', '--fps', '10')
        delayed_info = json.loads(subprocess.check_output([str(self.probe), str(delayed)]))
        self.assertAlmostEqual(delayed_info['firstAudible'], .15, delta=.03)

    def test_camera_preserves_aspect_with_black_letterbox(self):
        shutil.copyfile(self.camera, self.bundle / 'camera.mp4')
        self.data = replay([frame(camera=dict(rect=[80, 20, 160, 120], visible=True, mirrored=False))],
                           camera={'file': 'camera.mp4', 'offsetMs': 0, 'mirrored': False})
        image = self.preview(.5)
        self.assertEqual(image.getpixel((100, 25)), (0, 0, 0))
        self.assertEqual(image.getpixel((100, 135)), (0, 0, 0))
        self.assertGreater(image.getpixel((100, 50))[0], 230)
        self.assertGreater(image.getpixel((220, 50))[2], 230)
        self.assertEqual(image.getpixel((75, 80)), (16, 16, 16))

    def test_pointer_click_cues_and_disable(self):
        self.data['pointer'] = [dict(t=0, x=100, y=50, visible=True, buttons=0, type='move'),
                                dict(t=100, x=100, y=50, visible=True, buttons=1, type='down')]
        pointer = self.preview(.2)
        disabled = self.preview(.2, 'hidden.png', '--no-pointer')
        self.assertNotEqual(pointer.tobytes(), disabled.tobytes())
        self.assertEqual(disabled.getpixel((100, 50)), (16, 16, 16))

    def test_scroll_gesture_cue_fades(self):
        self.data['pointer'] = [dict(t=0, x=100, y=50, visible=True, buttons=0, type='scroll', dx=0, dy=12)]
        active = self.preview(.1, 'scroll.png')
        faded = self.preview(.7, 'faded.png')
        self.assertGreater(active.getpixel((120, 54))[0], 150)
        self.assertEqual(faded.getpixel((120, 54)), (16, 16, 16))

    def test_bundle_captions_srt_crop_bottom_and_disable(self):
        self.data['captions'] = [dict(start=0, end=1000, text='A useful caption')]
        caption = self.preview(.2)
        no_caption = self.preview(.2, 'nocap.png', '--no-captions')
        self.assertNotEqual(caption.tobytes(), no_caption.tobytes())
        self.assertEqual(caption.crop((0, 0, 320, 80)).tobytes(), no_caption.crop((0, 0, 320, 80)).tobytes())
        srt = self.bundle / 'captions.srt'
        srt.write_text('1\n00:00:01,000 --> 00:00:02,000\nLater caption\n')
        early = self.preview(.2, 'early.png', '--captions', str(srt))
        late = self.preview(1.2, 'late.png', '--captions', str(srt), '--crop', '20,20,200,100', '--width', '400')
        self.assertEqual(early.tobytes(), no_caption.tobytes())
        self.assertEqual(late.size, (400, 200))
        self.assertGreater(max(late.convert('L').tobytes()), 200)

    def test_invalid_inputs_leave_no_output(self):
        cases = [('--start', '-1'), ('--end', '-1'), ('--snapshot', '-1'), ('--end', '0'), ('--fps', '100'), ('--width', '319'), ('--crop', '-1,0,10,10')]
        for args in cases:
            with self.subTest(args=args):
                self.run_export('invalid.mp4', *args, success=False)
        self.data['frames'][0]['quads'] = base64.b64encode(b'bad').decode()
        self.run_export('bad.mp4', success=False)
        self.data = replay(camera={'file': '../outside.mp4'})
        self.run_export('escape.mp4', success=False)

    def test_existing_output_is_preserved(self):
        original = self.bundle / 'existing.mp4'
        original.write_bytes(b'important')
        (self.bundle / 'replay.json').write_text(json.dumps(self.data))
        result = subprocess.run([str(self.binary), str(self.bundle), str(original)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(original.read_bytes(), b'important')


if __name__ == '__main__':
    unittest.main()
