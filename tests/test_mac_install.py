#!/usr/bin/env python3
"""Installer regressions using private fake bundles; no live windows reload."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("install_mac", Path(__file__).resolve().parents[1] / "tools/install_mac.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="hc-mac-install-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "source/hardcore.app"
        (self.source / "Contents/MacOS").mkdir(parents=True)
        (self.source / "Contents/MacOS/hardcore").write_text("new")
        self.appdir = self.root / "Applications"
        self.bindir = self.root / "bin with spaces"
        self.app = self.appdir / self.source.name
        self.backup = self.app.with_name(self.app.name + ".previous")
        self.calls = []

    def old_app(self, location=None, text="old"):
        location = location or self.app
        (location / "Contents/MacOS").mkdir(parents=True)
        (location / "Contents/MacOS/hardcore").write_text(text)

    def process(self, argv, **kwargs):
        self.calls.append((argv, kwargs))
        self.assertEqual(kwargs["stdin"], subprocess.DEVNULL)
        return subprocess.CompletedProcess(argv, 0, stdout="hardcore-mac-reload-1\n")

    def install(self, **options):
        with mock.patch.object(installer.subprocess, "run", side_effect=self.process), \
             mock.patch.dict(os.environ, {"PATH": "/usr/bin:/bin"}), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            installer.install(self.source, options.get("appdir", self.appdir), self.bindir)
        return output.getvalue()

    def test_verified_install_keeps_old_app_and_reloads_after_publication(self):
        self.old_app()
        output = self.install()
        self.assertEqual((self.app / "Contents/MacOS/hardcore").read_text(), "new")
        self.assertEqual((self.backup / "Contents/MacOS/hardcore").read_text(), "old")
        self.assertEqual((self.bindir / "hardcore").resolve(), self.app / "Contents/MacOS/hardcore")
        self.assertEqual(self.calls[0][0][:4], ["/usr/bin/codesign", "--verify", "--deep", "--strict"])
        self.assertEqual(self.calls[1][0][1], "--mac-update-protocol")
        self.assertEqual(self.calls[2][0], [str(self.app / "Contents/MacOS/hardcore"), "--reload"])
        self.assertIn("export PATH=", output)
        self.assertEqual(list(self.appdir.glob(".hardcore-install-*")), [])

    def test_existing_previous_bundle_survives_reinstall(self):
        self.old_app()
        self.old_app(self.backup, "older")
        self.install()
        archived = list(self.appdir.glob("hardcore.app.previous-*"))
        self.assertEqual(len(archived), 1)
        self.assertEqual((archived[0] / "Contents/MacOS/hardcore").read_text(), "older")

    def test_copy_failure_keeps_original_app_and_command(self):
        self.old_app()
        with mock.patch.object(installer.shutil, "copytree", side_effect=OSError("copy failed")):
            with self.assertRaisesRegex(OSError, "copy failed"):
                self.install()
        self.assertEqual((self.app / "Contents/MacOS/hardcore").read_text(), "old")
        self.assertEqual(self.calls, [])
        self.assertFalse(self.backup.exists())

    def test_validation_failure_keeps_original_app(self):
        self.old_app()
        for result in (subprocess.CalledProcessError(1, "codesign"),
                       subprocess.CompletedProcess([], 0, stdout="unknown protocol\n")):
            with self.subTest(result=result), \
                 mock.patch.object(installer.subprocess, "run", side_effect=result if isinstance(result, Exception) else None,
                                   return_value=result):
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    installer.install(self.source, self.appdir, self.bindir)
            self.assertEqual((self.app / "Contents/MacOS/hardcore").read_text(), "old")
            self.assertFalse(self.backup.exists())

    def test_publication_failure_rolls_back_original(self):
        self.old_app()
        real_rename = os.rename
        def fail_publish(source, target):
            if Path(source).parent.name.startswith(".hardcore-install-"):
                raise OSError("publish failed")
            return real_rename(source, target)
        with mock.patch.object(installer.os, "rename", side_effect=fail_publish):
            with self.assertRaisesRegex(OSError, "publish failed"):
                self.install()
        self.assertEqual((self.app / "Contents/MacOS/hardcore").read_text(), "old")
        self.assertFalse(self.backup.exists())
        self.assertEqual(len(self.calls), 2)

    def test_failed_rollback_retains_recovery_bundle(self):
        self.old_app()
        real_rename = os.rename
        def fail_publish_and_restore(source, target):
            if Path(target) == self.app:
                raise OSError("rename failed")
            return real_rename(source, target)
        with mock.patch.object(installer.os, "rename", side_effect=fail_publish_and_restore), \
             contextlib.redirect_stderr(io.StringIO()) as error:
            with self.assertRaisesRegex(OSError, "rename failed"):
                self.install()
        self.assertEqual((self.backup / "Contents/MacOS/hardcore").read_text(), "old")
        self.assertIn(str(self.backup), error.getvalue())

    def test_source_alias_and_nested_destination_are_refused(self):
        alias = self.root / "source-alias"
        alias.symlink_to(self.source.parent, target_is_directory=True)
        for appdir in (self.source.parent, alias, self.source / "nested"):
            with self.subTest(appdir=appdir), self.assertRaises(SystemExit):
                self.install(appdir=appdir)
            self.assertEqual((self.source / "Contents/MacOS/hardcore").read_text(), "new")
        self.assertEqual(self.calls, [])

    def test_unrelated_command_and_app_symlink_are_refused(self):
        self.bindir.mkdir()
        command = self.bindir / "hardcore"
        command.write_text("keep")
        with self.assertRaises(SystemExit):
            self.install()
        self.assertEqual(command.read_text(), "keep")
        command.unlink()
        self.appdir.mkdir()
        target = self.root / "other.app"
        self.old_app(target)
        self.app.symlink_to(target, target_is_directory=True)
        with self.assertRaises(SystemExit):
            self.install()
        self.assertEqual((target / "Contents/MacOS/hardcore").read_text(), "old")

    def test_reload_failure_still_reports_successful_install(self):
        real_process = self.process
        def timeout_reload(argv, **kwargs):
            if argv[-1] == "--reload":
                raise subprocess.TimeoutExpired(argv, 120)
            return real_process(argv, **kwargs)
        self.process = timeout_reload
        output = self.install()
        self.assertIn("Installed; could not finish reloading", output)
        self.assertTrue((self.bindir / "hardcore").is_symlink())
        self.assertEqual((self.app / "Contents/MacOS/hardcore").read_text(), "new")

    def portable_command(self):
        self.bindir.mkdir(parents=True, exist_ok=True)
        command = self.bindir/"hardcore"
        command.write_bytes(installer.LEGACY_TERMINAL_LAUNCHER_HEADER + b"print('portable launcher')\n")
        command.chmod(0o755)
        return command

    def test_portable_first_install_migrates_atomically_after_validation(self):
        command = self.portable_command();original = command.read_bytes()
        real_process = self.process;real_replace = os.replace;replacements = []
        def checked_process(argv, **kwargs):
            if argv[-1] != "--reload":
                self.assertFalse(command.is_symlink())
                self.assertEqual(command.read_bytes(), original)
            return real_process(argv, **kwargs)
        def checked_replace(source, target):
            self.assertEqual(Path(target), command)
            self.assertTrue(Path(source).is_symlink())
            self.assertEqual(command.read_bytes(), original)
            self.assertEqual(len(self.calls), 2)  # signature and protocol passed
            self.assertEqual((self.app/"Contents/MacOS/hardcore").read_text(), "new")
            replacements.append(target);return real_replace(source, target)
        self.process = checked_process
        with mock.patch.object(installer.os, "replace", side_effect=checked_replace):
            self.install()
        self.assertEqual(replacements, [command])
        self.assertEqual(command.resolve(), (self.app/"Contents/MacOS/hardcore").resolve())
        self.assertFalse(list(self.bindir.glob(".hardcore-command-*")))

    def test_validation_failure_keeps_portable_launcher_and_old_app(self):
        self.old_app();command = self.portable_command();original = command.read_bytes()
        for result in (subprocess.CalledProcessError(1, "codesign"),
                       subprocess.CompletedProcess([], 0, stdout="unknown protocol\n")):
            with self.subTest(result=result), \
                 mock.patch.object(installer.subprocess, "run", side_effect=result if isinstance(result, Exception) else None,
                                   return_value=result):
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    installer.install(self.source, self.appdir, self.bindir)
            self.assertFalse(command.is_symlink())
            self.assertEqual(command.read_bytes(), original)
            self.assertEqual((self.app/"Contents/MacOS/hardcore").read_text(), "old")
            self.assertFalse(self.backup.exists())

    def test_partial_or_embedded_portable_marker_does_not_allow_overwrite(self):
        self.bindir.mkdir();command = self.bindir/"hardcore"
        for text in (b"#!/bin/sh\n# HARDCORE_TERMINAL_LAUNCHER_V1\necho keep\n",
                     b"#!/usr/bin/env python3\n# HARDCORE_TERMINAL_LAUNCHER_V10\n"):
            command.write_bytes(text)
            with self.assertRaises(SystemExit):self.install()
            self.assertEqual(command.read_bytes(), text)
            self.assertFalse(self.calls)

    def test_command_changed_during_validation_is_preserved(self):
        self.old_app();command = self.portable_command();real_process = self.process
        def changed_process(argv, **kwargs):
            command.write_text("user replaced this command")
            return real_process(argv, **kwargs)
        self.process = changed_process
        with self.assertRaisesRegex(SystemExit, "command changed"):
            self.install()
        self.assertEqual(command.read_text(), "user replaced this command")
        self.assertEqual((self.app/"Contents/MacOS/hardcore").read_text(), "old")
        self.assertFalse(self.backup.exists())


if __name__ == "__main__":
    unittest.main()
