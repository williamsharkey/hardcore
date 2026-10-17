#!/usr/bin/env python3
"""Compile and test the dependency-free C renderer; Python is test tooling only."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TextileTests(unittest.TestCase):
    def test_c_renderer(self):
        with tempfile.TemporaryDirectory(prefix="hc-textile-") as directory:
            binary = str(Path(directory) / "test-textile")
            flags = ["-O2", "-std=c99", "-Wall", "-Wextra", "-Werror"]
            if os.environ.get("HC_SANITIZE"):
                flags += ["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            subprocess.run(shlex.split(os.environ.get("CC", "cc")) + flags +
                           [str(ROOT / "tests/test_textile.c"), "-o", binary], check=True)
            result = subprocess.run([binary, "--bench"], text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS:", result.stdout)
            print(result.stdout.strip())


if __name__ == "__main__":
    unittest.main()
