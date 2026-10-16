#!/usr/bin/env python3
"""Install Hardcore's Claude adapter in one project, or inspect its configuration.

The official Claude CLI owns settings changes and enforces managed policy. This
helper never edits Claude settings or launches/restarts a Claude conversation.
Project scope still uses Claude's shared plugin cache; it does not enable the
adapter in other projects. A running session loads it with /reload-plugins.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

MARKETPLACE = "hardcore-slurp"
PLUGINS = ("hardcore-slurp", "hardcore-slurp-driver")
MIN_VERSION = (2, 1, 300)
MAX_JSON = 4 * 1024 * 1024


class AdapterError(RuntimeError):
    pass


def read_json(path):
    data = Path(path).read_bytes()
    if len(data) > MAX_JSON:
        raise AdapterError(f"Configuration is too large: {path}")
    try:
        return json.loads(data)
    except (ValueError, UnicodeError) as error:
        raise AdapterError(f"Invalid JSON in {path}") from error


def project_path(value):
    path = Path(value).expanduser().resolve(strict=True)
    if not path.is_dir():
        raise AdapterError(f"Project is not a directory: {path}")
    # Claude writes project settings through its CLI. Refuse redirection into
    # another project or the user's global settings through a symlink.
    for item in (path / ".claude", path / ".claude/settings.json"):
        if item.is_symlink():
            raise AdapterError(f"Refusing project settings symlink: {item}")
    return path


def bundled_marketplace():
    script = Path(__file__).resolve().parent
    for candidate in (script / "plugins", script.parent / "plugins"):
        if (candidate / ".claude-plugin/marketplace.json").is_file():
            return candidate
    raise AdapterError("Bundled Claude adapter is missing; rebuild or reinstall Hardcore")


def validate_bundle(value):
    market = Path(value).resolve(strict=True)
    catalog = read_json(market / ".claude-plugin/marketplace.json")
    if not isinstance(catalog, dict) or catalog.get("name") != MARKETPLACE:
        raise AdapterError(f"Expected bundled marketplace {MARKETPLACE}")
    entries = catalog.get("plugins")
    if not isinstance(entries, list):
        raise AdapterError("Bundled marketplace has no plugin list")
    plugins = {}
    for name in PLUGINS:
        matches = [item for item in entries if isinstance(item, dict) and item.get("name") == name]
        if len(matches) != 1 or matches[0].get("source") != f"./{name}":
            raise AdapterError(f"Bundled {name} must have exactly one local directory source")
        plugin = (market / name).resolve(strict=True)
        if plugin.parent != market or (market / name).is_symlink():
            raise AdapterError("Bundled adapter resolves outside its marketplace")
        manifest = read_json(plugin / ".claude-plugin/plugin.json")
        if not isinstance(manifest, dict) or manifest.get("name") != name:
            raise AdapterError("Bundled adapter has an unexpected plugin name")
        version = manifest.get("version")
        if not isinstance(version, str) or not re.fullmatch(r"\d+\.\d+\.\d+", version):
            raise AdapterError("Bundled adapter must declare a release version")
        plugins[name] = {"path": plugin, "version": version}
    return market, plugins


def run_claude(command, arguments, project):
    try:
        result = subprocess.run([command, *arguments], cwd=project,
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=60)
    except FileNotFoundError as error:
        raise AdapterError("Claude Code is not installed or is not on PATH") from error
    except subprocess.TimeoutExpired as error:
        raise AdapterError(f"Claude CLI timed out: {' '.join(arguments[:3])}") from error
    if result.returncode:
        detail = (result.stderr.strip() or result.stdout.strip())[-4000:]
        raise AdapterError(f"Claude CLI refused {' '.join(arguments[:3])} (exit {result.returncode})"
                           + (f":\n{detail}" if detail else ""))
    return result.stdout


def cli_json(command, arguments, project):
    output = run_claude(command, arguments, project)
    if len(output) > MAX_JSON:
        raise AdapterError("Claude CLI returned an oversized response")
    try:
        value = json.loads(output)
    except ValueError as error:
        raise AdapterError("Claude CLI returned an unrecognized JSON response") from error
    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        raise AdapterError("Claude CLI returned an unrecognized plugin list schema")
    return value


def check_claude(command, project):
    output = run_claude(command, ["--version"], project).strip()
    match = re.fullmatch(r"(\d+)\.(\d+)\.(\d+) \(Claude Code\)", output)
    if not match:
        raise AdapterError("Could not verify the Claude Code version; no changes made")
    version = tuple(map(int, match.groups()))
    if version < MIN_VERSION:
        raise AdapterError("Claude Code 2.1.300 or newer is required for the adapter; update Claude first")
    return ".".join(match.groups())


def configuration(command, project, version):
    rows = cli_json(command, ["plugin", "list", "--json"], project)
    matches = {name: [] for name in PLUGINS}
    for row in rows:
        name = next((name for name in PLUGINS if row.get("id") == f"{name}@{MARKETPLACE}"), None)
        if name is None or row.get("scope") != "project":
            continue
        owner = row.get("projectPath")
        if not isinstance(owner, str) or not Path(owner).is_absolute():
            raise AdapterError("Claude returned a project plugin without a valid project path")
        if Path(owner).resolve() == project:
            matches[name].append(row)
    result = {"project": str(project), "claudeVersion": version, "plugins": {},
              "state": "not-installed", "runtimeLoaded": None}
    for name, entries in matches.items():
        if len(entries) > 1:
            raise AdapterError("Claude returned multiple adapter installs for this project")
        item = {"id": f"{name}@{MARKETPLACE}", "state": "not-installed"}
        if entries:
            row = entries[0]
            if not isinstance(row.get("enabled"), bool) or not isinstance(row.get("projectEnabled"), bool):
                raise AdapterError("Claude did not report effective project enablement; adapter state is unknown")
            item.update(state="configured" if row["enabled"] and row["projectEnabled"] else "disabled",
                        version=row.get("version"))
        result["plugins"][name] = item
    states = {item["state"] for item in result["plugins"].values()}
    result["state"] = next(iter(states)) if len(states) == 1 else "partial"
    if result["state"] == "configured":
        result["reloadCommand"] = "/reload-plugins"
    return result


def check_marketplace(command, project, market):
    rows = cli_json(command, ["plugin", "marketplace", "list", "--json"], project)
    matches = [row for row in rows if row.get("name") == MARKETPLACE]
    if len(matches) > 1:
        raise AdapterError("Claude returned multiple marketplaces with the adapter's name")
    if matches:
        row = matches[0]
        source = row.get("path")
        if row.get("source") != "directory" or not isinstance(source, str) or Path(source).resolve() != market:
            raise AdapterError(f"Marketplace {MARKETPLACE} already points elsewhere; refusing to replace it")


def install(command, project, marketplace):
    market, plugins = validate_bundle(marketplace)
    version = check_claude(command, project)
    check_marketplace(command, project, market)
    for plugin in plugins.values():
        run_claude(command, ["plugin", "validate", str(plugin["path"])], project)
    # Do not use `install --marketplace`: that implicitly declares a marketplace
    # in user scope. Both explicit operations below are project-scoped.
    run_claude(command, ["plugin", "marketplace", "add", "--scope", "project", str(market)], project)
    for name in PLUGINS:
        run_claude(command, ["plugin", "install", "--scope", "project", f"{name}@{MARKETPLACE}"], project)
    result = configuration(command, project, version)
    updated = False
    for name, plugin in plugins.items():
        installed = result["plugins"][name]
        if installed.get("version") != plugin["version"] and installed["state"] != "not-installed":
            run_claude(command, ["plugin", "update", "--scope", "project", f"{name}@{MARKETPLACE}"], project)
            updated = True
    if updated:
        result = configuration(command, project, version)
    if result["state"] != "configured" or any(result["plugins"][name].get("version") != plugin["version"]
                                              for name, plugin in plugins.items()):
        raise AdapterError("Claude did not enable the bundled adapter for this project. "
                           "Check Claude's plugin policy and configuration; no session was restarted")
    return result


def handoff_helper():
    path=Path(__file__).resolve().with_name('claude_handoff.py')
    spec=importlib.util.spec_from_file_location('hardcore_claude_handoff',path)
    helper=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    return helper


def move(command,project,marketplace,source_session,*,expected_source_pid=None,
         expected_source_identity=None,validate_source=None):
    """Install if needed, then ask an existing loaded mod to move itself."""
    helper=handoff_helper()
    sid,cwd=helper.valid_source({'sourceSessionId':source_session,'cwd':str(project)})
    sources=[e for e in helper.jobs(command,cwd) if e.get('sessionId')==sid and e.get('kind')=='interactive']
    identity=helper.process_identity(sources[0].get('pid')) if len(sources)==1 else None
    if len(sources)!=1 or not identity:
        raise AdapterError('The requested source is not exactly one live interactive Claude session in this project')
    if expected_source_pid is not None or expected_source_identity is not None:
        if (type(expected_source_pid) is not int or expected_source_pid!=sources[0].get('pid') or
                not isinstance(expected_source_identity,str) or expected_source_identity!=identity):
            raise AdapterError('The discovered Terminal source process changed; request a new slurp')
    def revalidate():
        if helper.process_identity(sources[0].get('pid'))!=identity:
            raise AdapterError('The source process changed while preparing its adapter; request a new slurp')
        if validate_source is not None:
            validate_source()
    revalidate()
    # Resolve and validate the source before making any project settings changes.
    market,plugins=validate_bundle(marketplace)
    version=check_claude(command,project)
    configured=configuration(command,project,version)
    if configured['state']!='configured' or any(configured['plugins'][name].get('version')!=plugins[name]['version'] for name in PLUGINS):
        revalidate()
        configured=install(command,project,market)
    revalidate()
    queued=helper.queue({'sourceSessionId':sid,'cwd':cwd,'expectedSourcePid':sources[0]['pid'],
                         'expectedSourceIdentity':identity})
    return {**queued,'project':cwd,'sourceSessionId':sid,'runtimeLoaded':None,
            'reloadCommand':'/reload-plugins','adapterState':configured['state']}


def show_recovery(action,result):
    if action=='moves':
        if not result['moves']:print('No saved Claude handoffs.')
        for row in result['moves']:
            label=row.get('requestId') or row.get('intentId') or 'unknown'
            verified='; draft and current conversation checked' if row.get('draftVerifiedAt') else ''
            expired='; deadline expired' if row.get('expired') and row.get('state') not in ('attached','cancelled') else ''
            print(f"{label}  {row.get('status',row.get('state'))!s}{expired}{verified}  {row.get('project')!r}")
        if result['moves']:print('These are saved records. Use hardcore claude inspect REQUEST_ID to check current availability.')
        for error in result['unreadableRecords']:print(f"Could not read {error['record']}: {error['error']}")
    elif action=='inspect':
        print(f"Claude handoff {result['requestId']}: {result.get('status',result['state'])}")
        print('Source process available: '+('yes' if result['sourceAvailable'] else 'not verified'))
        print('Original destination process available: '+('yes' if result['destinationAvailable'] else 'not verified'))
        print('Receiving client available: '+('yes' if result['clientAvailable'] else 'not verified'))
        saved=result['savedDraft']
        if saved['available']:
            print(f"Saved draft: {saved['utf16Units']} UTF-16 units, cursor {saved['cursor']}.")
            print(f"Export without submitting: hardcore claude recover {result['requestId']} --export-draft NEW_FILE.json")
        else:print('No saved draft is available.')
        if result.get('lastFailure'):print('Last handoff error: '+result['lastFailure']['reason'])
        recovery=result['recovery']
        if recovery['kind']=='existing-live-job':
            print(f"Existing live job: {recovery['jobId']} in {recovery['project']!r}.")
            print(f"To attach manually while it remains live: claude attach {recovery['jobId']}")
        elif recovery['kind']=='source-still-running':print('The original source process is still running.')
        else:print('Check claude agents for recovery options; Hardcore has not restarted any session.')
        for error in result['observationErrors']:print('Availability check: '+error)
        print('Full shell and Terminal state are unverified; the source window is not eligible for automatic closure.')
    else:
        print(f"Saved the draft and cursor to {result['exportedDraft']!r} (private JSON file).")
        print('No prompt was submitted and no existing file was overwritten.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "status", "move", "moves", "inspect", "recover"))
    parser.add_argument("source_session",nargs='?',help="Live source session UUID, or saved handoff request ID")
    parser.add_argument("--project", help="Existing Claude project directory (default: current directory)")
    parser.add_argument("--marketplace", type=Path, help="Bundled marketplace directory")
    parser.add_argument("--json", action="store_true", help="Print configuration status as JSON")
    parser.add_argument('--export-draft',type=Path,help='Create a new private JSON draft recovery file')
    args = parser.parse_args(argv)
    if (args.action in ('move','inspect','recover')) != (args.source_session is not None):
        parser.error('move requires a source session UUID; inspect and recover require a request ID')
    if (args.action=='recover') != (args.export_draft is not None):
        parser.error('recover requires --export-draft NEW_FILE; other commands do not export drafts')
    try:
        if args.action in ('moves','inspect','recover'):
            helper=handoff_helper()
            result=(helper.list_moves() if args.action=='moves' else helper.inspect_move(args.source_session)
                    if args.action=='inspect' else helper.export_draft(args.source_session,args.export_draft))
            if args.json:print(json.dumps(result))
            else:show_recovery(args.action,result)
            return 0
        project = project_path(args.project if args.project is not None else Path.cwd())
        command = shutil.which("claude")
        if not command:
            raise AdapterError("Claude Code is not installed or is not on PATH")
        if args.action == "install":
            result = install(command, project, args.marketplace or bundled_marketplace())
        elif args.action == 'move':
            result = move(command,project,args.marketplace or bundled_marketplace(),args.source_session)
        else:
            version = check_claude(command, project)
            result = configuration(command, project, version)
        if args.json:
            print(json.dumps(result))
        elif args.action=='move':
            print(f"Hardcore handoff {result['state']} for Claude {result['sourceSessionId']}.")
            if result['state']=='queued':print('Queued for up to one hour, tied to this exact running source session.')
            print('The loaded adapter will move it after a verified completed turn with no background work.')
            print('If this session has not loaded the adapter yet, run /reload-plugins once in that Claude session.')
            print('The source Terminal stays open: this handoff does not transfer its shell or terminal state.')
        else:
            print(f"Claude adapter: {result['state']} in {project}")
            if result["state"] == "configured":
                print("In the existing Claude session, run /reload-plugins, then /hardcore-slurp.")
                print("Configuration is verified; loading and handoff readiness are checked inside that session.")
        return 0
    except (AdapterError, ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"hardcore: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
