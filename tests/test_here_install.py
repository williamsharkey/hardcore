#!/usr/bin/env python3
"""Portable launcher installation preserves GUI dispatch and argument boundaries."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("install_here", ROOT/"tools/install_here.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class InstallHere(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="hc here install ")
        self.root = Path(self.tmp.name)
        source = self.root/"source";source.mkdir()
        (source/"here.py").write_text("import json,sys; print(json.dumps(['here', *sys.argv[1:]]))\n")
        self.file_patch = patch.object(installer, "__file__", str(source/"install_here.py"))
        self.native_patch = patch.object(installer, "native_candidate", return_value=None)
        self.file_patch.start();self.native_patch.start()
        self.bindir = self.root/"local/bin"

    def tearDown(self):
        self.native_patch.stop();self.file_patch.stop();self.tmp.cleanup()

    def run_command(self, command, *args):
        return json.loads(subprocess.check_output([str(command), *args], text=True))

    def native(self):
        path = self.root/"GUI command"
        path.write_text("#!/usr/bin/env python3\nimport json,sys; print(json.dumps(['native', *sys.argv[1:]]))\n")
        path.chmod(0o755)
        return path

    def test_standalone_and_here_keep_literal_arguments(self):
        command = installer.install(self.bindir)
        args = ["--cwd", "a folder", "--", "literal; $HOME $(touch BAD)", "café"]
        self.assertEqual(self.run_command(command, "here", *args), ["here", *args])
        self.assertEqual(self.run_command(command, "--help"), ["here", "--help"])
        self.assertTrue(os.access(command, os.X_OK))

    def test_existing_native_symlink_survives_as_fallback_and_reinstall(self):
        native = self.native();self.bindir.mkdir(parents=True)
        (self.bindir/"hardcore").symlink_to(native)
        command = installer.install(self.bindir)
        self.assertEqual(self.run_command(command, "update", "--local"), ["native", "update", "--local"])
        self.assertEqual(self.run_command(command, "here", "--help"), ["here", "--help"])
        installer.install(self.bindir)
        self.assertEqual(self.run_command(command), ["native"])
        self.assertTrue(native.exists())

    def test_unrelated_existing_command_is_not_overwritten(self):
        self.bindir.mkdir(parents=True);target=self.bindir/"hardcore"
        target.write_text("user's existing command")
        with self.assertRaises(ValueError):installer.install(self.bindir)
        self.assertEqual(target.read_text(), "user's existing command")

    def test_invalid_native_fallback_does_not_replace_existing_link(self):
        self.bindir.mkdir(parents=True);target=self.bindir/"hardcore"
        target.symlink_to(self.root/"missing")
        with self.assertRaises(ValueError):installer.install(self.bindir)
        self.assertTrue(target.is_symlink())

    def test_native_alias_is_resolved_before_command_replacement(self):
        native = self.native();self.bindir.mkdir(parents=True)
        command = self.bindir/"hardcore";command.symlink_to(native)
        alias = self.root/"GUI alias";alias.symlink_to(command)
        installer.install(self.bindir, str(alias))
        stored = json.loads((self.bindir.parent/"share/hardcore-terminal/launcher.json").read_text())
        self.assertEqual(stored["native"], str(native.resolve()))
        self.assertEqual(self.run_command(command, "--version"), ["native", "--version"])

    def test_self_fallback_through_both_directory_and_command_aliases_is_refused(self):
        command = installer.install(self.bindir)
        old = command.read_bytes()
        directory_alias = self.root/"bin alias";directory_alias.symlink_to(self.bindir, target_is_directory=True)
        command_alias = self.root/"command alias";command_alias.symlink_to(command)
        for bindir, native in ((self.bindir, command_alias), (directory_alias, command)):
            with self.subTest(bindir=bindir), self.assertRaises(ValueError):
                installer.install(bindir, str(native))
            self.assertEqual(command.read_bytes(), old)

    def test_non_executable_fallback_leaves_existing_link_intact(self):
        native = self.native();self.bindir.mkdir(parents=True)
        command = self.bindir/"hardcore";command.symlink_to(native)
        native.chmod(0o644)
        with self.assertRaises(ValueError):installer.install(self.bindir)
        self.assertTrue(command.is_symlink())
        self.assertFalse((self.bindir.parent/"share").exists())

    def test_marker_inside_unrelated_command_is_not_an_owned_launcher(self):
        self.bindir.mkdir(parents=True);command=self.bindir/"hardcore"
        text = b"#!/bin/sh\n" + installer.MARKER + b"\necho keep\n"
        command.write_bytes(text)
        with self.assertRaises(ValueError):installer.install(self.bindir)
        self.assertEqual(command.read_bytes(), text)

    def test_powershell_native_discovery_decodes_unicode_paths_as_utf8(self):
        native = self.root/"GUI café 界.exe";native.write_bytes(b"fake native")
        calls = []
        def output(args, **kwargs):
            calls.append(args)
            if args[0] == "powershell.exe":
                return "C:\\Users\\café 界\\hardcore\\hardcore.exe".encode("utf-8")
            self.assertEqual(args[:2], ["wslpath", "-u"])
            self.assertIn("café 界", args[2])
            return str(native).encode("utf-8")
        self.native_patch.stop()
        with patch.dict(os.environ, {}, clear=True), \
             patch.object(installer.shutil, "which", side_effect=lambda name: name if name in ("powershell.exe", "wslpath") else None), \
             patch.object(installer.subprocess, "check_output", side_effect=output):
            self.assertEqual(installer.native_candidate(), str(native.resolve()))
        self.assertIn("[Console]::OutputEncoding", calls[0][-1])
        self.assertIn("System.Text.UTF8Encoding($false)", calls[0][-1])
        self.native_patch.start()


if __name__ == "__main__":
    unittest.main()
