#!/usr/bin/env python3
"""Files operations and an owned-PTY UI check; never opens the user's files/Trash."""
import ctypes
import curses
import errno
import fcntl
import importlib.util
import os
from pathlib import Path
import pty
import select
import stat
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "files.py"
spec = importlib.util.spec_from_file_location("hardcore_files", SCRIPT)
files = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = files
spec.loader.exec_module(files)


class OwnedFolder(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hc-files-test-")
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def write(self, name, data=b"contents"):
        path = self.root / name
        path.write_bytes(data)
        return path


class ListingTests(OwnedFolder):
    def test_folders_stay_first_and_hidden_filter_are_independent(self):
        (self.root / "Zoo").mkdir()
        (self.root / "alpha").mkdir()
        for name in ("Apple.txt", "bee.txt", ".secret"):
            self.write(name)
        names = lambda **kw: [e.name for e in files.list_entries(self.root, **kw)]
        self.assertEqual(names(), ["alpha", "Zoo", "Apple.txt", "bee.txt"])
        self.assertEqual(names(reverse=True), ["Zoo", "alpha", "bee.txt", "Apple.txt"])
        self.assertEqual(names(query="APPLE"), ["Apple.txt"])
        self.assertEqual(names(query="secret"), [])
        self.assertEqual(names(query="secret", hidden=True), [".secret"])

    def test_size_modified_and_kind_sort(self):
        a, b = self.write("a.txt", b"1234"), self.write("b.csv", b"1")
        os.utime(a, (200, 200))
        os.utime(b, (100, 100))
        for kind in ("size", "modified", "kind"):
            self.assertEqual([e.name for e in files.list_entries(self.root, sort=kind)], ["b.csv", "a.txt"])
        with self.assertRaises(ValueError):
            files.list_entries(self.root, sort="invalid")

    def test_directory_and_broken_symlinks(self):
        (self.root / "folder").mkdir()
        (self.root / "linked").symlink_to("folder")
        (self.root / "broken").symlink_to("missing")
        entries = {e.name: e for e in files.list_entries(self.root)}
        self.assertTrue(entries["linked"].is_dir)
        self.assertTrue(entries["linked"].is_link)
        self.assertFalse(entries["broken"].is_dir)
        self.assertTrue(entries["broken"].is_link)


class FileOperationsTests(OwnedFolder):
    def test_file_copy_preserves_bytes_and_mode(self):
        source = self.write("original.txt", b"binary\0\xff\n")
        source.chmod(0o640)
        target = files.copy_path(source, self.root / "copied.txt")
        self.assertEqual(target.read_bytes(), source.read_bytes())
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o640)

    def test_folder_copy_preserves_symlinks_without_following_them(self):
        source = self.root / "folder"
        source.mkdir()
        (source / "data").write_text("value")
        (source / "link").symlink_to("data")
        (source / "broken").symlink_to("absent")
        target = files.copy_path(source, self.root / "copy")
        self.assertEqual((target / "data").read_text(), "value")
        self.assertEqual(os.readlink(target / "link"), "data")
        self.assertEqual(os.readlink(target / "broken"), "absent")
        self.assertTrue((source / "data").exists())

    def test_copy_and_move_symlink_itself(self):
        referent = self.write("referent")
        link = self.root / "link"
        link.symlink_to("referent")
        copied = files.copy_path(link, self.root / "copied")
        moved = files.move_path(link, self.root / "moved")
        self.assertEqual(os.readlink(copied), "referent")
        self.assertEqual(os.readlink(moved), "referent")
        self.assertFalse(files.exists(link))
        self.assertEqual(referent.read_bytes(), b"contents")

    def test_no_overwrite_including_broken_symlink(self):
        source, target = self.write("source"), self.write("target", b"keep me")
        broken = self.root / "broken"
        broken.symlink_to("missing")
        for operation in (files.copy_path, files.move_path):
            for occupied in (target, broken, source):
                with self.subTest(operation=operation.__name__, target=occupied.name):
                    with self.assertRaises(FileExistsError):
                        operation(source, occupied)
        self.assertEqual(target.read_bytes(), b"keep me")
        self.assertEqual(os.readlink(broken), "missing")
        self.assertTrue(source.exists())

    def test_destination_race_cannot_replace_file(self):
        source = self.write("source")
        original = files.check_destination
        for number, operation in enumerate((files.copy_path, files.move_path)):
            target = self.root / f"target{number}"
            def raced(*args):
                result = original(*args)
                target.write_bytes(b"racing writer")
                return result
            with mock.patch.object(files, "check_destination", side_effect=raced):
                with self.assertRaises(FileExistsError):
                    operation(source, target)
            self.assertEqual(target.read_bytes(), b"racing writer")
            self.assertEqual(source.read_bytes(), b"contents")

    def test_reject_directory_descendant_including_alias(self):
        source = self.root / "folder"
        source.mkdir()
        alias = self.root / "alias"
        alias.symlink_to("folder", target_is_directory=True)
        for target in (source / "copy", alias / "copy"):
            for operation in (files.copy_path, files.move_path):
                with self.assertRaises(ValueError):
                    operation(source, target)
        self.assertEqual(list(source.iterdir()), [])

    def test_create_and_rename_are_exclusive(self):
        folder = files.create_folder(self.root, "new folder")
        renamed = files.rename_path(folder, "café 🚀")
        self.assertTrue(renamed.is_dir())
        self.assertFalse(folder.exists())
        with self.assertRaises(FileExistsError):
            files.create_folder(self.root, "café 🚀")
        for invalid in ("", ".", "..", "../outside", "a/b", "a\n", "\0"):
            with self.assertRaises(ValueError):
                files.rename_path(renamed, invalid)

    def test_destination_resolution_and_copy_suggestion(self):
        source = self.write("report.txt")
        folder = self.root / "target"
        folder.mkdir()
        self.assertEqual(files.destination_for(source, "target", self.root), folder / source.name)
        self.assertEqual(files.destination_for(source, "renamed.txt", self.root), self.root / "renamed.txt")
        self.assertEqual(files.suggested_copy(source).name, "report copy.txt")
        (self.root / "report copy.txt").symlink_to("missing")
        self.assertEqual(files.suggested_copy(source).name, "report copy 2.txt")

    def test_special_files_and_missing_destination_parent_are_refused(self):
        fifo = self.root / "pipe"
        os.mkfifo(fifo)
        for operation in (files.copy_path, files.move_path):
            with self.assertRaises(ValueError):
                operation(fifo, self.root / "pipe-copy")
        source = self.write("source")
        with self.assertRaises(FileNotFoundError):
            files.copy_path(source, self.root / "missing" / "target")
        self.assertTrue(source.exists())

    def test_trash_collision_uses_unique_name_and_never_deletes(self):
        trash = self.root / "owned-trash"
        trash.mkdir()
        original = trash / "report.txt"
        original.write_bytes(b"previous item")
        source = self.write("report.txt", b"new item")
        target = files.trash_path(source, trash)
        self.assertFalse(source.exists())
        self.assertNotEqual(target, original)
        self.assertEqual(target.suffix, ".txt")
        self.assertEqual(target.read_bytes(), b"new item")
        self.assertEqual(original.read_bytes(), b"previous item")

    def test_trash_folder_and_link_preserve_contents(self):
        trash = self.root / "owned-trash"
        folder = files.create_folder(self.root, "folder")
        (folder / "data").write_bytes(b"keep")
        moved = files.trash_path(folder, trash)
        self.assertEqual((moved / "data").read_bytes(), b"keep")
        link = self.root / "link"
        link.symlink_to("absent")
        moved_link = files.trash_path(link, trash)
        self.assertTrue(moved_link.is_symlink())
        self.assertEqual(os.readlink(moved_link), "absent")

    def test_trash_refuses_itself_and_cross_volume_keeps_source(self):
        trash = self.root / "owned-trash"
        trash.mkdir()
        with self.assertRaises(ValueError):
            files.trash_path(trash, trash)
        source = self.write("source")
        with mock.patch.object(files, "move_path", side_effect=OSError(errno.EXDEV, "different device")):
            with self.assertRaisesRegex(OSError, "original is unchanged"):
                files.trash_path(source, trash)
        self.assertTrue(source.exists())
        self.assertEqual(list(trash.iterdir()), [])


class PreviewAndRenderingTests(OwnedFolder):
    def test_preview_is_bounded_and_cannot_inject_terminal_controls(self):
        source = self.write("text", b"hello\x1b[31m\tworld\n" + b"x" * 100)
        lines = files.read_preview(source, limit=32)
        self.assertIn(r"hello\x1b[31m\tworld", lines)
        self.assertTrue(lines[-1].startswith("[Preview limited"))
        self.assertNotIn("\x1b", "".join(lines))

    def test_binary_empty_unicode_and_fifo(self):
        self.assertIn("Binary file", files.read_preview(self.write("binary", b"\0\xff"))[0])
        self.assertEqual(files.read_preview(self.write("empty", b"")), ["[Empty file]"])
        self.assertEqual(files.read_preview(self.write("unicode", "café 界 🚀".encode())), ["café 界 🚀"])
        fifo = self.root / "pipe"
        os.mkfifo(fifo)
        with self.assertRaises(ValueError):
            files.read_preview(fifo)

    def test_control_and_bidi_names_display_unambiguously(self):
        self.assertEqual(files.safe_text("a\n\t\x1b\u202eb"), r"a\n\t\x1b\u202eb")
        self.assertEqual(files.width("e\u0301界"), 3)
        for columns in range(12):
            self.assertLessEqual(files.width(files.clipped("café 界 e\u0301", columns)), columns)

    def test_wrapped_confirmation_preserves_full_long_path(self):
        source = "/tmp/" + "café-界" * 60 + "\x1b"
        lines = files.wrapped_lines([source], 25)
        self.assertEqual("".join(lines), files.safe_text(source))
        self.assertTrue(all(files.width(line) <= 25 for line in lines))

    def test_external_open_uses_literal_absolute_argv(self):
        path = self.root / "-$(touch INJECTION); café.txt"
        with mock.patch.object(files.sys, "platform", "darwin"), mock.patch.object(files.subprocess, "Popen") as launch:
            files.open_external(path)
            self.assertEqual(launch.call_args.args[0], ["open", str(path)])
            self.assertNotIn("shell", launch.call_args.kwargs)
            files.open_external(path, preview=True)
            self.assertEqual(launch.call_args.args[0], ["qlmanage", "-p", str(path)])

    def test_external_open_in_wsl_hands_windows_its_own_path(self):
        path = self.root / "-$(touch INJECTION); café.txt"
        done = mock.Mock(returncode=0, stdout="C:\\x\\-$(touch INJECTION); café.txt\n")
        with mock.patch.object(files.sys, "platform", "linux"), mock.patch.dict(files.os.environ, {"WSL_DISTRO_NAME": "Ubuntu"}), \
             mock.patch.object(files.subprocess, "run", return_value=done) as convert, mock.patch.object(files.subprocess, "Popen") as launch:
            files.open_external(path)
            self.assertEqual(convert.call_args.args[0], ["wslpath", "-w", str(path)])
            self.assertEqual(launch.call_args.args[0], ["explorer.exe", "C:\\x\\-$(touch INJECTION); café.txt"])
            self.assertNotIn("shell", launch.call_args.kwargs)


class BrowserTests(OwnedFolder):
    def test_history_and_selection_survive_back_and_forward(self):
        for name in ("a", "b"):
            (self.root / name).mkdir()
        browser = files.Browser(self.root)
        browser.selected = 1
        browser.navigate("b")
        self.assertEqual(browser.directory, self.root / "b")
        browser.travel()
        self.assertEqual(browser.current().name, "b")
        browser.travel(forward=True)
        self.assertEqual(browser.directory, self.root / "b")
        browser.travel()
        browser.navigate("a")
        self.assertFalse(browser.forward)

    def test_failed_navigation_preserves_current_state(self):
        self.write("entry")
        browser = files.Browser(self.root)
        browser.query = "entry"
        before = (browser.directory, browser.entries, browser.query, list(browser.back))
        with self.assertRaises(FileNotFoundError):
            browser.navigate("missing")
        self.assertEqual((browser.directory, browser.entries, browser.query, browser.back), before)

    def test_refresh_filter_and_empty_selection(self):
        first, second = self.write("first"), self.write("second")
        browser = files.Browser(self.root)
        browser.selected = 1
        browser.reverse = True
        browser.refresh()
        self.assertEqual(browser.current().name, "second")
        browser.query = "first"
        browser.refresh()
        self.assertEqual(browser.current().name, "first")
        first.unlink()
        browser.refresh()
        self.assertIsNone(browser.current())
        self.assertEqual(browser.selected, 0)

    def test_cancelled_operation_never_mutates(self):
        source = self.write("source")
        browser = files.Browser(self.root)
        ui = object.__new__(files.Interface)
        ui.browser = browser
        ui.prompt = mock.Mock(return_value="target")
        ui.confirm = mock.Mock(return_value=False)
        for key in ("r", "c", "m", "t"):
            ui.operate(key)
            self.assertEqual(source.read_bytes(), b"contents")
            self.assertFalse((self.root / "target").exists())


class CommandAndUITests(OwnedFolder):
    def test_help_and_noninteractive_error(self):
        help_result = subprocess.run([sys.executable, str(SCRIPT), "--help"], capture_output=True, text=True)
        self.assertEqual(help_result.returncode, 0)
        self.assertIn("Trash", help_result.stdout)
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.root)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("interactive terminal", result.stderr)

    def test_real_curses_create_cancel_rename_and_quit(self):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 100, 0, 0))
        env = dict(os.environ, TERM="xterm-256color", LC_ALL="en_US.UTF-8")
        process = subprocess.Popen([sys.executable, str(SCRIPT), str(self.root)], stdin=slave,
                                   stdout=slave, stderr=slave, env=env, start_new_session=True)
        os.close(slave)
        captured = bytearray()

        def wait_for(fragment):
            deadline = time.monotonic() + 5
            target = fragment.encode()
            while time.monotonic() < deadline:
                if target in captured:
                    captured.clear()
                    return
                if select.select([master], [], [], max(0, deadline-time.monotonic()))[0]:
                    try:
                        block = os.read(master, 65536)
                    except OSError as error:
                        self.fail(f"PTY closed before {fragment!r}: {error}; output {bytes(captured)!r}")
                    if not block:
                        break
                    captured.extend(block)
            self.fail(f"Missing {fragment!r}; output {bytes(captured)!r}")

        try:
            wait_for("Files")
            os.write(master, b"n")
            wait_for("New folder name")
            os.write(master, b"created\r")
            wait_for("Created created")
            self.assertTrue((self.root / "created").is_dir())
            os.write(master, b"r")
            wait_for("Rename to")
            os.write(master, b"\x15renamed\r")
            wait_for("Proceed")
            os.write(master, b"n")
            wait_for("Files")
            self.assertTrue((self.root / "created").is_dir())
            self.assertFalse((self.root / "renamed").exists())
            os.write(master, b"r")
            wait_for("Rename to")
            os.write(master, b"\x15renamed\r")
            wait_for("Proceed")
            os.write(master, b"y")
            wait_for("Renamed to renamed")
            self.assertTrue((self.root / "renamed").is_dir())
            self.assertFalse((self.root / "created").exists())
            os.write(master, b"q")
            # endwin may drain terminal output before restoring the TTY.
            deadline = time.monotonic() + 5
            while process.poll() is None and time.monotonic() < deadline:
                if select.select([master], [], [], 0.05)[0]:
                    try:
                        os.read(master, 65536)
                    except OSError:
                        break
            self.assertEqual(process.wait(timeout=5), 0)
        finally:
            os.close(master)
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)


if __name__ == "__main__":
    unittest.main()
