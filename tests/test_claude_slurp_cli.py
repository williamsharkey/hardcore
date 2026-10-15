#!/usr/bin/env python3
"""Offline adapter-install checks; never touch the user's Claude configuration."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("claude_slurp", Path(__file__).resolve().parents[1] / "tools/claude_slurp.py")
adapter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(adapter)


class InstallTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="hc-adapter-cli-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.project = self.root / "project with spaces;$(false)"
        self.project.mkdir()
        settings = self.project / ".claude/settings.json"
        settings.parent.mkdir()
        self.original = '{"permissions":{"deny":["Bash(rm:*)"]},"custom":"keep me"}\n'
        settings.write_text(self.original)
        self.market = self.root / "bundled plugins"
        (self.market / ".claude-plugin").mkdir(parents=True)
        self.catalog = {"name": adapter.MARKETPLACE, "plugins": []}
        for name in adapter.PLUGINS:
            plugin = self.market / name
            (plugin / ".claude-plugin").mkdir(parents=True)
            (plugin / ".claude-plugin/plugin.json").write_text(json.dumps({"name": name, "version": "0.1.0"}))
            self.catalog["plugins"].append({"name": name, "source": f"./{name}"})
        self.write_catalog()
        self.calls = []
        self.claude_version = "2.1.300 (Claude Code)\n"
        self.marketplaces = []
        self.rows = [self.row(name) for name in adapter.PLUGINS]
        self.failed = None

    def write_catalog(self):
        (self.market / ".claude-plugin/marketplace.json").write_text(json.dumps(self.catalog))

    def row(self, name, **extra):
        return {"id": f"{name}@{adapter.MARKETPLACE}", "version": "0.1.0",
                "scope": "project", "projectPath": str(self.project),
                "enabled": True, "projectEnabled": True, **extra}

    def cli(self, command, args, project):
        self.assertEqual(command, "/mock/claude")
        self.assertEqual(project, self.project)
        self.calls.append(args)
        if self.failed and args[:len(self.failed)] == self.failed:
            raise adapter.AdapterError("managed policy refused this plugin")
        if args == ["--version"]:
            return self.claude_version
        if args == ["plugin", "marketplace", "list", "--json"]:
            return json.dumps(self.marketplaces)
        if args == ["plugin", "list", "--json"]:
            return json.dumps(self.rows)
        return "ok\n"

    def install(self):
        with mock.patch.object(adapter, "run_claude", side_effect=self.cli):
            return adapter.install("/mock/claude", self.project, self.market)

    def test_installs_both_with_project_scope_without_editing_settings(self):
        result = self.install()
        self.assertEqual(result["state"], "configured")
        self.assertIsNone(result["runtimeLoaded"])
        mutations = [args for args in self.calls if args[:2] == ["plugin", "install"] or args[:3] == ["plugin", "marketplace", "add"]]
        self.assertEqual(mutations, [
            ["plugin", "marketplace", "add", "--scope", "project", str(self.market)],
            *[["plugin", "install", "--scope", "project", f"{name}@{adapter.MARKETPLACE}"] for name in adapter.PLUGINS]])
        self.assertEqual((self.project / ".claude/settings.json").read_text(), self.original)

    def test_unsupported_or_unknown_version_prevents_mutation(self):
        for version in ("2.1.287 (Claude Code)", "2.1.299 (Claude Code)", "2.1.30 (Claude Code)", "unknown", "2.1.300-beta (Claude Code)"):
            with self.subTest(version=version):
                self.calls.clear()
                self.claude_version = version
                with self.assertRaises(adapter.AdapterError):
                    self.install()
                self.assertEqual(self.calls, [["--version"]])

    def test_minimum_version_is_compared_numerically(self):
        self.claude_version = "2.1.300 (Claude Code)"
        self.assertEqual(self.install()["state"], "configured")

    def test_marketplace_collision_prevents_mutation(self):
        self.marketplaces = [{"name": adapter.MARKETPLACE, "source": "github", "repo": "someone/else"}]
        with self.assertRaisesRegex(adapter.AdapterError, "already points elsewhere"):
            self.install()
        self.assertEqual(len(self.calls), 2)

    def test_repeated_install_accepts_same_local_marketplace(self):
        self.marketplaces = [{"name": adapter.MARKETPLACE, "source": "directory", "path": str(self.market)}]
        self.assertEqual(self.install()["state"], "configured")

    def test_policy_failure_stops_before_install(self):
        self.failed = ["plugin", "marketplace", "add"]
        with self.assertRaisesRegex(adapter.AdapterError, "managed policy"):
            self.install()
        self.assertFalse(any(args[:2] == ["plugin", "install"] for args in self.calls))

    def test_missing_disabled_or_unrecognized_driver_never_counts_as_ready(self):
        for rows in ([self.row(adapter.PLUGINS[0])],
                     [self.row(adapter.PLUGINS[0]), self.row(adapter.PLUGINS[1], enabled=False)],
                     [self.row(adapter.PLUGINS[0]), self.row(adapter.PLUGINS[1], projectEnabled=None)]):
            with self.subTest(rows=rows):
                self.rows = rows
                with self.assertRaises(adapter.AdapterError):
                    self.install()

    def test_other_project_and_user_install_do_not_count_as_ready(self):
        self.rows = [self.row(name, projectPath=str(self.root / "other")) for name in adapter.PLUGINS]
        self.rows += [self.row(name, scope="user") for name in adapter.PLUGINS]
        with self.assertRaisesRegex(adapter.AdapterError, "did not enable"):
            self.install()

    def test_unknown_list_schema_fails_closed(self):
        for rows in ({"plugins": self.rows}, ["unexpected"], [self.row(adapter.PLUGINS[0], projectPath=None)]):
            self.rows = rows
            with self.assertRaises(adapter.AdapterError):
                self.install()

    def test_remote_or_duplicate_source_is_rejected_before_cli(self):
        for source in ({"source": "github", "repo": "evil/plugin"}, "../elsewhere"):
            self.catalog["plugins"][0]["source"] = source
            self.write_catalog()
            with self.assertRaisesRegex(adapter.AdapterError, "local directory source"):
                self.install()
            self.assertEqual(self.calls, [])

    def test_project_settings_symlinks_are_rejected(self):
        settings = self.project / ".claude/settings.json"
        settings.unlink()
        outside = self.root / "settings.json"
        outside.write_text(self.original)
        settings.symlink_to(outside)
        with self.assertRaisesRegex(adapter.AdapterError, "settings symlink"):
            adapter.project_path(self.project)
        self.assertEqual(outside.read_text(), self.original)

    def test_status_does_not_install_and_cannot_claim_runtime_loaded(self):
        with mock.patch.object(adapter.shutil, "which", return_value="/mock/claude"), \
             mock.patch.object(adapter, "run_claude", side_effect=self.cli), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            code = adapter.main(["status", "--project", str(self.project), "--json"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(output.getvalue())["state"], "configured")
        self.assertIsNone(json.loads(output.getvalue())["runtimeLoaded"])
        self.assertEqual(self.calls, [["--version"], ["plugin", "list", "--json"]])

    def test_project_defaults_to_current_directory(self):
        with mock.patch.object(adapter.Path, "cwd", return_value=self.project), \
             mock.patch.object(adapter.shutil, "which", return_value="/mock/claude"), \
             mock.patch.object(adapter, "run_claude", side_effect=self.cli), \
             contextlib.redirect_stdout(io.StringIO()) as output:
            self.assertEqual(adapter.main(["status", "--json"]), 0)
        self.assertEqual(json.loads(output.getvalue())["project"], str(self.project))

    def test_cli_never_uses_shell_or_interactive_stdin(self):
        result = subprocess.CompletedProcess([], 0, stdout="ok", stderr="")
        with mock.patch.object(adapter.subprocess, "run", return_value=result) as run:
            adapter.run_claude("/path with spaces/claude", ["plugin", "validate", str(self.market)], self.project)
        args, options = run.call_args
        self.assertEqual(args[0], ["/path with spaces/claude", "plugin", "validate", str(self.market)])
        self.assertFalse(options.get("shell", False))
        self.assertEqual(options["stdin"], subprocess.DEVNULL)


    def move_helper(self):
        helper=mock.Mock()
        sid='11111111-1111-4111-8111-111111111111'
        helper.valid_source.return_value=(sid,str(self.project))
        helper.jobs.return_value=[{'sessionId':sid,'kind':'interactive','pid':1234}]
        helper.process_identity.return_value='source instance'
        helper.queue.return_value={'ok':True,'intentId':'ab'*16,'state':'queued'}
        return helper,sid

    def test_move_queues_existing_configured_session_without_typing_or_reinstalling(self):
        helper,sid=self.move_helper()
        with mock.patch.object(adapter,'handoff_helper',return_value=helper), \
             mock.patch.object(adapter,'run_claude',side_effect=self.cli), \
             mock.patch.object(adapter,'install',side_effect=AssertionError('already current')):
            result=adapter.move('/mock/claude',self.project,self.market,sid)
        self.assertEqual(result['state'],'queued')
        self.assertIsNone(result['runtimeLoaded'])
        helper.queue.assert_called_once_with({'sourceSessionId':sid,'cwd':str(self.project),
                                             'expectedSourcePid':1234,'expectedSourceIdentity':'source instance'})
        self.assertFalse(any(args[:2]==['plugin','install'] for args in self.calls))

    def test_move_validates_exact_live_source_before_changing_project_configuration(self):
        helper,sid=self.move_helper()
        for rows in ([],[{'sessionId':sid,'kind':'background','pid':1234}],
                     helper.jobs.return_value*2):
            helper.jobs.return_value=rows
            with mock.patch.object(adapter,'handoff_helper',return_value=helper), \
                 mock.patch.object(adapter,'install',side_effect=AssertionError('invalid source')), \
                 self.assertRaisesRegex(adapter.AdapterError,'live interactive'):
                adapter.move('/mock/claude',self.project,self.market,sid)
            helper.queue.assert_not_called()

    def test_move_updates_old_adapter_before_queueing_and_stops_if_install_fails(self):
        helper,sid=self.move_helper()
        self.rows=[self.row(name,version='0.0.1') for name in adapter.PLUGINS]
        configured={'state':'configured'}
        with mock.patch.object(adapter,'handoff_helper',return_value=helper), \
             mock.patch.object(adapter,'run_claude',side_effect=self.cli), \
             mock.patch.object(adapter,'install',return_value=configured) as install:
            adapter.move('/mock/claude',self.project,self.market,sid)
        install.assert_called_once_with('/mock/claude',self.project,self.market)
        helper.queue.reset_mock()
        with mock.patch.object(adapter,'handoff_helper',return_value=helper), \
             mock.patch.object(adapter,'run_claude',side_effect=self.cli), \
             mock.patch.object(adapter,'install',side_effect=adapter.AdapterError('managed policy')), \
             self.assertRaisesRegex(adapter.AdapterError,'managed policy'):
            adapter.move('/mock/claude',self.project,self.market,sid)
        helper.queue.assert_not_called()

    def test_recovery_inspection_does_not_require_claude_or_modify_project(self):
        helper=mock.Mock()
        helper.list_moves.return_value={'ok':True,'moves':[],'unreadableRecords':[],'liveChecked':False}
        helper.inspect_move.return_value={'ok':True,'state':'attaching'}
        for argv in (['moves','--json'],['inspect','ab'*16,'--json']):
            with mock.patch.object(adapter,'handoff_helper',return_value=helper), \
                 mock.patch.object(adapter.shutil,'which',side_effect=AssertionError('no executable required')), \
                 mock.patch.object(adapter.Path,'cwd',side_effect=FileNotFoundError('former project was deleted')), \
                 mock.patch.object(adapter,'project_path',side_effect=AssertionError('no project mutation')), \
                 contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(adapter.main(argv),0)
            self.assertTrue(json.loads(output.getvalue())['ok'])
        helper.inspect_move.assert_called_once_with('ab'*16)

    def test_recover_exports_only_explicit_destination_without_install_or_attach(self):
        helper=mock.Mock()
        helper.export_draft.return_value={'ok':True,'submitted':False}
        output=self.root/'draft.json'
        with mock.patch.object(adapter,'handoff_helper',return_value=helper), \
             mock.patch.object(adapter,'run_claude',side_effect=AssertionError('recovery must not run Claude')), \
             contextlib.redirect_stdout(io.StringIO()) as stdout:
            self.assertEqual(adapter.main(['recover','ab'*16,'--export-draft',str(output),'--json']),0)
        self.assertFalse(json.loads(stdout.getvalue())['submitted'])
        helper.export_draft.assert_called_once_with('ab'*16,output)
        for argv in (['recover','ab'*16],['inspect','ab'*16,'--export-draft',str(output)],['moves','ab'*16]):
            with contextlib.redirect_stderr(io.StringIO()),self.assertRaises(SystemExit):adapter.main(argv)


@unittest.skipUnless(os.environ.get("HC_TEST_CLAUDE_CLI") == "1", "opt in with HC_TEST_CLAUDE_CLI=1; no credentials or model requests")
class OfficialCLIIntegration(unittest.TestCase):
    def test_private_install_reinstall_status_and_update(self):
        self.assertIsNotNone(shutil.which("claude"), "Claude CLI is required for this opt-in check")
        with tempfile.TemporaryDirectory(prefix="hc-adapter-install-") as temporary:
            root = Path(temporary).resolve()
            project = root / "project"
            (project / ".claude").mkdir(parents=True)
            initial = {"permissions": {"deny": ["Bash(rm:*)"]}, "env": {"HC_INSTALL_SENTINEL": "keep"}}
            (project / ".claude/settings.json").write_text(json.dumps(initial))
            config = root / "config"
            config.mkdir(mode=0o700)
            market = root / "plugins"
            (market / ".claude-plugin").mkdir(parents=True)
            entries = []
            for name in adapter.PLUGINS:
                plugin = market / name
                (plugin / ".claude-plugin").mkdir(parents=True)
                manifest = {"name": name, "version": "0.1.0", "description": "Disposable CLI fixture"}
                if name == "hardcore-slurp-driver":
                    manifest["dependencies"] = ["hardcore-slurp"]
                (plugin / ".claude-plugin/plugin.json").write_text(json.dumps(manifest))
                (plugin / "hooks").mkdir()
                (plugin / "hooks/hooks.json").write_text('{"modules":["./register.js"]}')
                (plugin / "hooks/register.js").write_text("export function register(on) {}\n")
                entries.append({"name": name, "source": "./" + name})
            (market / ".claude-plugin/marketplace.json").write_text(json.dumps({
                "name": adapter.MARKETPLACE, "owner": {"name": "Hardcore CLI test"}, "plugins": entries}))
            env = os.environ.copy()
            env.update(CLAUDE_CONFIG_DIR=str(config), DISABLE_AUTOUPDATER="1", CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC="1")

            def invoke(action):
                result = subprocess.run([sys.executable, str(Path(adapter.__file__).resolve()), action,
                    "--project", str(project), "--marketplace", str(market), "--json"],
                    env=env, capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stderr)
                data = json.loads(result.stdout)
                self.assertEqual(data["state"], "configured")
                self.assertIsNone(data["runtimeLoaded"])
                return data

            for action in ("install", "status", "install"):
                invoke(action)
            refused = subprocess.run([shutil.which("claude"), "plugin", "disable", "--scope", "project",
                f"hardcore-slurp@{adapter.MARKETPLACE}"], cwd=project, env=env,
                capture_output=True, text=True, timeout=30)
            self.assertNotEqual(refused.returncode, 0, "guard was disabled while its dependent driver stayed enabled")
            invoke("status")
            # An app update can replace the directory-backed bundle while an
            # older version is cached. Both records must advance project-locally.
            for name in adapter.PLUGINS:
                manifest = market / name / ".claude-plugin/plugin.json"
                data = json.loads(manifest.read_text())
                data["version"] = "0.1.1"
                manifest.write_text(json.dumps(data))
            updated = invoke("install")
            self.assertTrue(all(item["version"] == "0.1.1" for item in updated["plugins"].values()))
            settings = json.loads((project / ".claude/settings.json").read_text())
            self.assertTrue(all(settings.get(key) == value for key, value in initial.items()))
            installed = json.loads((config / "plugins/installed_plugins.json").read_text())
            for name in adapter.PLUGINS:
                rows = installed["plugins"][f"{name}@{adapter.MARKETPLACE}"]
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]["scope"], "project")
                self.assertEqual(Path(rows[0]["projectPath"]).resolve(), project)


if __name__ == "__main__":
    unittest.main()
