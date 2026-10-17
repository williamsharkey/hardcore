#!/usr/bin/env python3
"""Exercise native `hardcore here` dispatch in owned terminals only.

The fixture copies only the built executable into a path
containing spaces. Raw child probes report arguments, terminal size and input
independently of the frontend. No real application window or user PTY is used.
"""
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
import uuid


ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build" / "hardcore"

PROBE = r'''
import fcntl,json,os,select,signal,struct,sys,termios,tty
from pathlib import Path
path=Path(sys.argv[1]);received=bytearray();resizes=[]
def size():return list(struct.unpack('HHHH',fcntl.ioctl(0,termios.TIOCGWINSZ,bytes(8)))[:2])
def record(stopped=0):
    value=dict(pid=os.getpid(),parent=os.getppid(),argv=sys.argv[2:],cwd=os.getcwd(),
               tty=[os.isatty(i) for i in (0,1,2)],size=size(),resizes=resizes,
               received=received.hex(),stopped=stopped,
               term=os.environ.get('TERM'),color=os.environ.get('COLORTERM'),
               program=os.environ.get('TERM_PROGRAM'))
    temporary=path.with_suffix('.tmp');temporary.write_text(json.dumps(value));temporary.replace(path)
def resize(*_):resizes.append(size());record()
def stop(sig,_):
    try:record(sig)
    finally:os._exit(128+sig)
signal.signal(signal.SIGWINCH,resize)
signal.signal(signal.SIGHUP,stop);signal.signal(signal.SIGTERM,stop)
tty.setraw(0);record()
os.write(1,b'\x1b[38;2;12;34;56;48;2;65;43;21mHERE_COLOR_PROBE\x1b[0m\r\n')
while True:
    if select.select([0],[],[],.1)[0]:
        block=os.read(0,4096)
        if not block:break
        received.extend(block);record()
'''


def tty_settings(fd):
    value = termios.tcgetattr(fd)
    # Darwin adds PENDIN itself when restoring ICANON. It is pending-input
    # bookkeeping, not a user mode; even a bare setraw/tcsetattr pair sets it.
    value[3] &= ~getattr(termios, 'PENDIN', 0)
    return value


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


class OwnedTerminal:
    def __init__(self, argv, env, cwd):
        self.master, self.slave = pty.openpty()
        self.output = bytearray()
        self.process = None
        self.original = tty_settings(self.slave)
        self.size(24, 100)
        try:
            # The fixture retains the host slave for its post-exit termios
            # assertion. Making the frontend its controlling-session leader
            # would make macOS revoke that slave when the frontend exits.
            # The application itself still creates a real controlling PTY for
            # the child probe via forkpty, exactly as it does in Terminal.
            self.process = subprocess.Popen(argv, stdin=self.slave, stdout=self.slave,
                stderr=self.slave, env=env, cwd=cwd, start_new_session=True)
        except BaseException:
            os.close(self.master);os.close(self.slave)
            raise

    def size(self, rows, cols):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH",rows,cols,0,0))

    def pump(self, timeout=.05):
        if select.select([self.master], [], [], timeout)[0]:
            try:
                block = os.read(self.master, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
            else:
                self.output.extend(block)

    def wait(self, condition, description, timeout=8):
        deadline = time.monotonic()+timeout
        while time.monotonic() < deadline:
            self.pump()
            result = condition()
            if result:
                return result
        raise AssertionError(f"Timed out: {description}; frontend exit={self.process.poll()}; "
                             f"output tail={bytes(self.output[-2000:])!r}")

    def send(self, data):
        os.write(self.master, data)

    def exited(self):
        self.wait(lambda: self.process.poll() is not None, "terminal frontend exit")
        self.pump(0)
        return self.process.returncode

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.exited()
            except AssertionError:
                self.process.kill();self.process.wait(timeout=5)
        os.close(self.master);os.close(self.slave)


@unittest.skipUnless(os.name == "posix" and EXE.is_file(), "built POSIX executable required")
class HereDispatch(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hc here dispatch ",dir="/tmp")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        contents = self.root / "A bundle with spaces.app" / "Contents"
        (contents / "MacOS").mkdir(parents=True)
        (contents / "Resources").mkdir()
        self.exe = contents / "MacOS" / "hardcore"
        shutil.copy2(EXE, self.exe)
        self.session = "dispatch-" + uuid.uuid4().hex[:12]
        self.addCleanup(self.stop_session)
        self.cwd = self.root / "child folder's $literal";self.cwd.mkdir()
        self.home = self.root / "private-home";self.home.mkdir()
        self.runtime = self.root / "runtime";self.runtime.mkdir(mode=0o700)
        self.control = self.root / "unexpected GUI FIFO"
        self.env = dict(os.environ, HOME=str(self.home), TERM="xterm-256color",
                        COLORTERM="truecolor", LC_ALL="en_US.UTF-8",
                        HARDCORE_CTL=str(self.control), HARDCORE_NO_ACTIVATE="1",
                        HARDCORE_RUNTIME_DIR=str(self.runtime),
                        HARDCORE_TEST_CONFIG=str(self.root / "config"))

    def stop_session(self):
        subprocess.run([str(self.exe), "here", "--stop", self.session], stdin=subprocess.DEVNULL,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=self.env,
                       cwd=self.root, timeout=5)

    def no_gui(self):
        self.assertFalse(self.control.exists(), "terminal mode created the GUI test control FIFO")
        self.assertEqual(list(self.runtime.iterdir()), [], "terminal mode created native GUI runtime sockets")

    def run_cli(self, *args):
        result = subprocess.run([str(self.exe), *args], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, env=self.env, cwd=self.root, timeout=10)
        self.no_gui()
        return result

    def test_native_and_helper_help_do_not_start_gui(self):
        native = self.run_cli("--help")
        self.assertEqual(native.returncode, 0, native.stderr)
        self.assertIn("hardcore here", native.stdout+native.stderr)
        helper = self.run_cli("here", "--help")
        self.assertEqual(helper.returncode, 0, helper.stderr)
        self.assertIn("Ctrl-B", helper.stdout)
        self.assertIn("--cwd", helper.stdout)
        self.assertNotIn("Traceback", helper.stderr)

    def test_noninteractive_call_fails_clearly_without_gui(self):
        result = self.run_cli("here", "--", sys.executable, "-c", "raise SystemExit(77)")
        self.assertEqual(result.returncode, 2, result.stdout+result.stderr)
        self.assertIn("interactive terminal", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertNotIn("\x1b[?1049h", result.stdout)

    def probe(self):
        script = self.root / "literal probe.py";script.write_text(PROBE)
        report = self.root / "probe.json"
        arguments = ["a space", "--help", "café 🚀", "$HOME", "$(touch SHOULD_NOT_EXIST)",
                     "semi;colon", "quote'and\"double", "two\nlines", ""]
        terminal = OwnedTerminal([str(self.exe), "here", "--session", self.session, "--cwd", str(self.cwd), "--",
            sys.executable, "-u", str(script), str(report), *arguments], self.env, self.root)
        self.addCleanup(terminal.close)
        def current():
            try:return json.loads(report.read_text())
            except FileNotFoundError:return None
        first = terminal.wait(current,"owned child probe startup")
        # Fallback cleanup names only the exact disposable child created above.
        def reap_probe():
            if alive(first["pid"]):
                try:os.killpg(first["pid"],signal.SIGKILL)
                except ProcessLookupError:pass
        self.addCleanup(reap_probe)
        self.assertEqual(first["argv"], arguments)
        self.assertEqual(first["cwd"], str(self.cwd))
        self.assertEqual(first["tty"], [True,True,True])
        terminal.wait(lambda: current()["size"] == [23,71], "sidebar-adjusted source PTY size")
        first = current()
        self.assertEqual(first["size"], [23,71])
        self.assertEqual((first["term"],first["color"],first["program"]),
                         ("xterm-256color","truecolor","hardcore-here"))
        self.assertNotEqual(first["pid"],terminal.process.pid)
        self.assertNotEqual(first["parent"],terminal.process.pid)
        self.assertTrue(alive(first["parent"]), "persistent PTY owner missing")
        self.assertFalse((self.cwd / "SHOULD_NOT_EXIST").exists())
        terminal.wait(lambda: b"HERE_COLOR_PROBE" in terminal.output,"source text rendered by terminal frontend")
        self.assertIn(b"38;2;12;34;56;48;2;65;43;21m",terminal.output)
        self.assertIn(b"\x1b[?1049h",terminal.output)
        self.assertNotEqual(termios.tcgetattr(terminal.slave),terminal.original)
        self.no_gui()
        return terminal,current,first

    def restored(self, terminal, first, stopped=True):
        self.assertEqual(terminal.exited(),0)
        self.assertEqual(tty_settings(terminal.slave),terminal.original,
                         "terminal frontend failed to restore all host termios flags")
        self.assertIn(b"\x1b[?1049l",terminal.output)
        self.assertIn(b"\x1b[?25h",terminal.output)
        self.assertIn(b"\x1b[?1000l",terminal.output)
        self.assertIn(b"\x1b[?2004l",terminal.output)
        if stopped:
            terminal.wait(lambda: not alive(first["pid"]),"owned child process cleanup")
        else:
            self.assertTrue(alive(first["pid"]), "detach killed the owned shell")
        self.no_gui()

    def test_packaged_real_pty_preserves_argv_colors_resize_and_quit(self):
        terminal,current,first = self.probe()
        payload = "literal input café 🚀".encode()
        terminal.send(payload+b"\x02\x02")
        terminal.wait(lambda: current()["received"]==(payload+b"\x02").hex(),"literal input routed to owned child")
        terminal.size(31,121)
        os.kill(terminal.process.pid,signal.SIGWINCH)
        resized=terminal.wait(lambda: current() if current()["size"]==[30,92] else None,"host resize propagated to source PTY")
        self.assertIn([30,92],resized["resizes"])
        self.assertEqual(resized["pid"],first["pid"])
        terminal.send(b"\x02q")
        terminal.wait(lambda: b"End all session processes? y/N" in terminal.output,"explicit quit confirmation")
        terminal.send(b"Y")
        self.restored(terminal,first)
        self.assertEqual(current()["received"],(payload+b"\x02").hex(),"quit shortcut leaked into the source process")

    def test_signal_exit_restores_host_and_detaches_owned_child(self):
        terminal,current,first = self.probe()
        terminal.process.send_signal(signal.SIGTERM)
        self.restored(terminal,first,stopped=False)
        self.stop_session()
        terminal.wait(lambda: not alive(first["pid"]), "explicit detached session cleanup")


if __name__ == "__main__":
    unittest.main(verbosity=2)
