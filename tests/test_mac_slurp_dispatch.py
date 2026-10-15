#!/usr/bin/env python3
"""Public slurp dispatch through an owned helper; never discovers user sessions."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / 'build/hardcore'


@unittest.skipUnless(sys.platform == 'darwin' and EXE.is_file(), 'built macOS executable required')
class SlurpDispatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='hc-slurp-dispatch-', dir='/private/tmp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root/'build').mkdir()
        (self.root/'tools').mkdir()
        (self.root/'run').mkdir(mode=0o700)
        self.exe = self.root/'build/hardcore'
        shutil.copy2(EXE, self.exe)
        self.helper = self.root/'tools/slurp_mac.py'
        self.helper.write_text('import json,sys\nprint(json.dumps(sys.argv[1:]))\nsys.exit(23)\n')
        self.env = dict(os.environ, HARDCORE_RUNTIME_DIR=str(self.root/'run'))

    def run_cli(self, *args):
        return subprocess.run([str(self.exe), *args], env=self.env,
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)

    def test_plain_slurp_selects_coordinator_and_preserves_failure(self):
        result = self.run_cli('slurp')
        self.assertEqual(result.returncode, 23, result.stdout+result.stderr)
        self.assertEqual(json.loads(result.stdout), ['--executable', str(self.exe)])
        self.assertEqual(list((self.root/'run').iterdir()), [])

    def test_targeted_slurp_uses_same_coordinator(self):
        result = self.run_cli('slurp', '--terminal', '/dev/ttys123')
        self.assertEqual(result.returncode, 23, result.stdout+result.stderr)
        self.assertEqual(json.loads(result.stdout),
                         ['--executable', str(self.exe), '--terminal', '/dev/ttys123'])

    def test_missing_coordinator_fails_without_silent_bridge(self):
        self.helper.unlink()
        result = self.run_cli('slurp')
        self.assertEqual(result.returncode, 1)
        self.assertIn('missing slurp coordinator', result.stderr)
        self.assertEqual(list((self.root/'run').iterdir()), [])

    def test_internal_native_route_rejects_bridge_and_conflicting_selectors(self):
        for args in [('--bridge',), ('--list',), ('--pid', '123'), ('--tmux', '$0')]:
            with self.subTest(args=args):
                self.assertEqual(self.run_cli('--slurp-native', *args).returncode, 2)

    def test_invalid_public_selector_does_not_run_coordinator(self):
        result = self.run_cli('slurp', '--terminal', '/dev/ttys123', '--pid', '123')
        self.assertEqual(result.returncode, 2)
        self.assertNotIn('--executable', result.stdout)


if __name__ == '__main__':
    unittest.main()
