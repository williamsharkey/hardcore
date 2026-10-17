#!/usr/bin/env python3
"""Verify native privacy metadata survives copying the single executable.

Foundation-only fixtures use the actual Makefile linker flags. No camera,
permission request, Apple event, GUI activation, or microphone access occurs.
"""
import json
from pathlib import Path
import plistlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#import <Foundation/Foundation.h>
int main(void) {
    @autoreleasepool {
        NSDictionary *info=NSBundle.mainBundle.infoDictionary;
        if(!info)return 1;
        NSData *json=[NSJSONSerialization dataWithJSONObject:info options:0 error:NULL];
        if(!json)return 2;
        fwrite(json.bytes,1,json.length,stdout);
    }
    return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Mach-O embedded Info.plist')
class ExecutableInfo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='hc-executable-info-')
        cls.root = Path(cls.temp.name)
        cls.source = cls.root / 'fixture.m'
        cls.source.write_text(SOURCE)
        flags = subprocess.run(['make', '--no-print-directory', '-s', '-f', 'Makefile', '-f', '-', '__test_executable_info_flags'],
                               input='__test_executable_info_flags:\n\t@printf \'%s\\n\' \'$(MAC_INFO_FLAGS)\'\n',
                               cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
        if '__info_plist' not in flags:
            raise RuntimeError('Normal executable link does not embed privacy metadata')
        cls.binary = cls.root / 'hardcore'
        subprocess.run(['clang', '-arch', 'arm64', '-arch', 'x86_64', str(cls.source),
                        '-framework', 'Foundation', *shlex.split(flags), '-o', str(cls.binary)],
                       cwd=ROOT, check=True, capture_output=True, text=True)
        cls.expected = plistlib.loads((ROOT / 'tools/ExecutableInfo.plist').read_bytes())

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def read(self, path):
        return json.loads(subprocess.check_output([str(path)], cwd=self.root, timeout=10))

    def test_bare_executable_contains_camera_metadata(self):
        self.assertEqual(self.read(self.binary), self.expected)
        self.assertTrue(self.read(self.binary)['NSCameraUsageDescription'])

    def test_copy_and_rename_needs_no_sibling_plist(self):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            copied = Path(folder) / 'renamed hardcore'
            shutil.copy2(self.binary, copied)
            self.assertEqual(self.read(copied), self.expected)
            self.assertEqual(list(Path(folder).iterdir()), [copied])

    def test_real_app_metadata_takes_precedence(self):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            contents = Path(folder) / 'fixture.app' / 'Contents'
            executable = contents / 'MacOS' / 'hardcore'
            executable.parent.mkdir(parents=True)
            shutil.copy2(self.binary, executable)
            app_info = plistlib.loads((ROOT / 'tools/Info.plist').read_bytes())
            app_info['NSCameraUsageDescription'] = 'Bundle-specific camera description'
            (contents / 'Info.plist').write_bytes(plistlib.dumps(app_info))
            info = self.read(executable)
            self.assertEqual(info['NSCameraUsageDescription'], app_info['NSCameraUsageDescription'])
            for key in ['CFBundleIdentifier', 'CFBundleIconFile', 'NSHighResolutionCapable', 'CFBundleDocumentTypes']:
                self.assertEqual(info[key], app_info[key])

    def test_embedded_common_metadata_matches_app(self):
        app_info = plistlib.loads((ROOT / 'tools/Info.plist').read_bytes())
        for key, value in self.expected.items():
            self.assertEqual(value, app_info[key], key)

    def test_both_universal_architectures_contain_metadata(self):
        output = subprocess.check_output(['otool', '-arch', 'all', '-l', str(self.binary)], text=True)
        self.assertEqual(output.count('sectname __info_plist'), 2)


if __name__ == '__main__':
    unittest.main()
