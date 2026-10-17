#!/usr/bin/env python3
"""Owned macOS GUI launch checks; no user windows, shells, camera or music.

Run after building: python3 tests/test_mac_launch.py [path/to/hardcore]
Each case uses a private HOME/config/socket directory and a disposable shell.
"""
import json
import os
from pathlib import Path
import plistlib
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
EXE = Path(sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-")
           else ROOT / "build/hardcore").resolve()


def wait_for(fn, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError("timed out waiting for owned launch")


def exact(peer, length):
    result = b""
    while len(result) < length:
        chunk = peer.recv(length - len(result))
        if not chunk:
            raise EOFError("owned GUI closed its socket")
        result += chunk
    return result


def window_info(path):
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(1)
        peer.connect(str(path))
        data = plistlib.dumps({"op": "list"}, fmt=plistlib.FMT_BINARY)
        peer.sendall(struct.pack("!I", len(data)) + data)
        size, = struct.unpack("!I", exact(peer, 4))
        if not 0 < size < 1024 * 1024:
            raise ValueError("invalid owned GUI response")
        return plistlib.loads(exact(peer, size))


@unittest.skipUnless(sys.platform == "darwin", "macOS launch integration")
class LaunchTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(EXE.is_file(), "build Hardcore before running launch checks")
        self.temporary = tempfile.TemporaryDirectory(prefix="hc-launch-", dir="/tmp")
        self.root = Path(self.temporary.name).resolve()
        self.home = self.root / "home"
        self.home.mkdir()
        self.runtime = self.root / "run"
        self.config = self.root / "config"
        self.config.mkdir()
        (self.config / "config.ini").write_text("font=t\ntheme=g\n")
        self.cwd = self.root / "folder with 'quotes' and spaces"
        self.cwd.mkdir()
        self.snapshot = self.root / "shell.json"
        self.stub = self.root / "owned-shell"
        self.stub.write_text(f"#!{sys.executable}\n" + r'''
import json, os, sys
from pathlib import Path
Path(os.environ["HC_LAUNCH_SNAPSHOT"]).write_text(json.dumps({
    "pid": os.getpid(), "window": os.getppid(), "cwd": os.getcwd(),
    "sentinel": os.environ.get("HC_LAUNCH_SENTINEL"),
    "windowSession": os.getsid(os.getppid()),
    "terminal": os.environ.get("TERM_PROGRAM"),
}))
print("OWNED LAUNCH READY", flush=True)
while sys.stdin.readline():
    pass
''')
        self.stub.chmod(0o700)
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("HARDCORE_")}
        self.env.update(HOME=str(self.home), HARDCORE_RUNTIME_DIR=str(self.runtime),
                        HARDCORE_TEST_CONFIG=str(self.config), HARDCORE_NO_ACTIVATE="1",
                        HARDCORE_SHELL=str(self.stub), HC_LAUNCH_SNAPSHOT=str(self.snapshot),
                        HC_LAUNCH_SENTINEL="preserved 'value' with spaces")
        self.children = []
        self.owned = set()
        self.addCleanup(self.cleanup_owned)

    def collect(self):
        try:
            state = json.loads(self.snapshot.read_text())
            self.owned.update((state["pid"], state["window"]))
            return state
        except (OSError, json.JSONDecodeError):
            return None

    def windows(self):
        result = []
        for path in self.runtime.glob("*.sock"):
            try:
                state = window_info(path)
            except (OSError, EOFError, ValueError):
                continue
            self.owned.add(state["pid"])
            self.owned.update(tab["pid"] for tab in state["tabs"] if tab["pid"] > 1)
            result.append(state)
        return result

    def cleanup_owned(self):
        # Only PIDs learned from this case's private fixture and private sockets.
        self.collect()
        self.windows()
        self.owned.update(child.pid for child in self.children if child.poll() is None)
        for pid in self.owned:
            if pid > 1 and pid != os.getpid():
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for child in self.children:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=5)
        self.temporary.cleanup()

    def launch(self, *args, **options):
        return subprocess.run([str(EXE), *map(str, args)], env=self.env, cwd=self.cwd,
                              stdin=subprocess.DEVNULL, capture_output=True, text=True,
                              timeout=10, **options)

    def check_window(self, directory, detached=True):
        fixture = wait_for(self.collect)
        windows = wait_for(self.windows)
        self.assertEqual(len(windows), 1)
        self.assertEqual(windows[0]["pid"], fixture["window"])
        self.assertEqual(len(windows[0]["tabs"]), 1)
        self.assertEqual(windows[0]["tabs"][0]["pid"], fixture["pid"])
        self.assertEqual(fixture["cwd"], str(Path(directory).resolve()))
        self.assertEqual(fixture["sentinel"], self.env["HC_LAUNCH_SENTINEL"])
        self.assertEqual(fixture["terminal"], "hardcore")
        if detached:
            self.assertEqual(fixture["windowSession"], fixture["window"],
                             "GUI must own a session independent of its launching terminal")
        os.kill(fixture["window"], 0)
        return fixture

    def test_plain_command_returns_with_gui_alive_and_preserves_cwd_environment(self):
        read_fd, write_fd = os.pipe()
        try:
            result = self.launch(pass_fds=(write_fd,))
            self.assertEqual(result.returncode, 0, result.stderr)
            os.close(write_fd)
            write_fd = -1
            self.assertTrue(select.select([read_fd], [], [], 1)[0],
                            "detached GUI retained an unrelated inherited descriptor")
            self.assertEqual(os.read(read_fd, 1), b"")
            self.check_window(self.cwd)
        finally:
            os.close(read_fd)
            if write_fd >= 0:
                os.close(write_fd)

    def test_directory_argument_preserves_literal_spaces_and_quotes(self):
        target = self.cwd / "a 'second' directory"
        target.mkdir()
        result = self.launch(target.relative_to(self.cwd))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_window(target)

    def test_cd_option_launches_detached(self):
        result = self.launch("--cd", self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_window(self.root)

    def test_short_directory_option_launches_detached(self):
        result = self.launch("-d", self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_window(self.root)

    def test_foreground_option_keeps_gui_as_owned_child(self):
        child = subprocess.Popen([str(EXE), "--foreground", "--cd", str(self.cwd)],
                                 cwd=self.cwd, env=self.env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        fixture = self.check_window(self.cwd, detached=False)
        self.assertIsNone(child.poll())
        self.assertEqual(fixture["window"], child.pid)

    def test_owned_control_fixture_stays_foreground(self):
        self.env["HARDCORE_CTL"] = str(self.root / "control")
        child = subprocess.Popen([str(EXE), "--cd", str(self.cwd)], cwd=self.cwd,
                                 env=self.env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.children.append(child)
        fixture = self.check_window(self.cwd, detached=False)
        self.assertIsNone(child.poll())
        self.assertEqual(fixture["window"], child.pid)

    def test_invalid_directory_fails_without_starting_gui(self):
        file_path = self.root / "ordinary-file"
        file_path.write_text("not a directory")
        for args in (("missing-directory",), (file_path,), ("--cd", "missing-directory")):
            with self.subTest(args=args):
                result = self.launch(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("hardcore", result.stderr.lower())
                self.assertFalse(self.snapshot.exists())
                self.assertEqual(self.windows(), [])

    def test_cli_help_remains_synchronous(self):
        for args in (("--version",), ("--help",), ("here", "--help")):
            with self.subTest(args=args):
                result = self.launch(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.strip())
                self.assertFalse(self.snapshot.exists())
                self.assertEqual(self.windows(), [])

    def test_native_terminal_mode_requires_tty_without_launching_window(self):
        result = self.launch("here")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("interactive terminal", result.stderr)
        self.assertFalse(self.snapshot.exists())
        self.assertEqual(self.windows(), [])


if __name__ == "__main__":
    unittest.main()
