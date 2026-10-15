#!/usr/bin/env python3
"""Discover eligible macOS sessions and request their supported handoffs.

Claude sessions are matched by live kernel PID, project and process identity.
The adapter is installed only for that matched source. A queued request is not
a completed transfer; ordinary unsupported Terminal sessions remain in place.
"""
import argparse
from collections import Counter
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import unicodedata

MAX_OUTPUT = 4 * 1024 * 1024
TTY = re.compile(r'/dev/ttys[0-9]{1,55}\Z')


class SlurpError(RuntimeError):
    pass


def safe_text(value, *, multiline=False, limit=2000):
    """Render labels/errors as text, never terminal controls or bidi commands."""
    text = str(value)
    clean = []
    for char in text[:limit]:
        if char == '\n' and multiline:
            clean.append(char)
        elif char.isspace():
            clean.append(' ')
        elif unicodedata.category(char).startswith('C'):
            clean.append('\\u%04x' % ord(char))
        else:
            clean.append(char)
    if len(text) > limit:
        clean.append('…')
    return ''.join(clean)


def load_adapter():
    path = Path(__file__).resolve().with_name('claude_slurp.py')
    spec = importlib.util.spec_from_file_location('hardcore_slurp_adapter', path)
    adapter = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(adapter)
    return adapter


def run_native(executable, arguments, *, timeout):
    try:
        result = subprocess.run([str(executable), *arguments], stdin=subprocess.DEVNULL,
                                capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError) as error:
        raise SlurpError('Hardcore command failed: '+safe_text(error)) from error
    if len(result.stdout) + len(result.stderr) > MAX_OUTPUT:
        raise SlurpError('Hardcore command returned too much output')
    return result


def discover(executable):
    result = run_native(executable, ['--slurp-sources'], timeout=30)
    if result.returncode:
        raise SlurpError('Could not discover sessions: '+safe_text(result.stderr or result.stdout))
    try:
        value = json.loads(result.stdout)
    except (ValueError, UnicodeError) as error:
        raise SlurpError('Hardcore returned invalid session metadata') from error
    if not isinstance(value, dict) or any(not isinstance(value.get(key), list) or
            len(value[key]) > 1024 or any(not isinstance(row, dict) for row in value[key])
            for key in ('terminal', 'hardcore', 'tmux')):
        raise SlurpError('Hardcore returned an unsupported session metadata schema')
    if 'terminalDiscoveryFailed' in value and type(value['terminalDiscoveryFailed']) is not bool:
        raise SlurpError('Hardcore returned an invalid Terminal discovery status')
    return value


def source_label(source):
    cwd = source.get('cwd')
    folder = Path(cwd).name if isinstance(cwd, str) and cwd else 'unknown folder'
    program = source.get('program') or source.get('shell') or 'unknown program'
    return safe_text(f"{source.get('tty', 'unknown TTY')} {folder}:{program}", limit=500)


def source_program_version(source):
    """The running image's kernel-reported version, never the CLI on PATH."""
    version = source.get('programVersion', '')
    if not isinstance(version, str) or (version and not re.fullmatch(r'[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}', version)):
        raise SlurpError('Source has an invalid running program version')
    return version


def require_supported_runtime(source, minimum):
    version = source_program_version(source)
    if version and tuple(map(int, version.split('.'))) < minimum:
        required = '.'.join(map(str, minimum))
        raise SlurpError(f'Running Claude Code {version} is older than {required} required by automatic handoff; '
                         'updating the installed CLI does not update this running session; '
                         'no plugin installation or handoff was requested')


def source_identity(source):
    tty, pid, cwd = source.get('tty'), source.get('pid'), source.get('cwd')
    if (not isinstance(tty, str) or not TTY.fullmatch(tty) or type(pid) is not int or pid <= 1 or
            not isinstance(cwd, str) or not cwd.startswith('/') or '\0' in cwd or source.get('program') != 'claude' or
            type(source.get('window')) is not int or source['window'] <= 0 or
            type(source.get('host')) is not int or source['host'] <= 1 or type(source.get('managed')) is not bool):
        raise SlurpError('Source lacks an exact live Terminal/Claude identity')
    project = Path(cwd).resolve(strict=True)
    if not project.is_dir():
        raise SlurpError('Source project is not a directory')
    return (tty, source['window'], source['host'], pid, str(project), source['program'], source['managed'],
            source_program_version(source))


def match_claude(source, command, helper):
    identity = source_identity(source)
    process = helper.process_identity(identity[3])
    if not isinstance(process, str) or not process:
        raise SlurpError('Source Claude process is no longer running')
    rows = helper.jobs(command, identity[4])
    matches = []
    for row in rows:
        cwd = row.get('cwd')
        if (row.get('kind') == 'interactive' and type(row.get('pid')) is int and row['pid'] == identity[3] and
                isinstance(cwd, str) and cwd.startswith('/') and Path(cwd).resolve() == Path(identity[4]) and
                helper.valid_session_id(row.get('sessionId'))):
            matches.append(row)
    if len(matches) != 1:
        raise SlurpError('No unique live interactive Claude session matches this Terminal PID and project')
    return matches[0]['sessionId'], identity, process


def revalidate_source(executable, identity, process, helper):
    discovery = discover(executable)
    if discovery.get('terminalDiscoveryFailed'):
        raise SlurpError('Terminal source could not be revalidated')
    rows = [row for row in discovery['terminal'] if row.get('tty') == identity[0]]
    if len(rows) != 1 or source_identity(rows[0]) != identity or helper.process_identity(identity[3]) != process:
        raise SlurpError('Terminal source changed while preparing its handoff; request a new slurp')


def bootstrap_adapter(source, project, queued_result):
    """Reserved for a verified runtime-loading mechanism; never types commands.

    Returning None leaves activation explicitly unverified. A future mechanism
    may return {"runtimeLoaded": True}; it must establish that for this exact
    source and still cannot declare the handoff complete.
    """
    return None


def verified_result(result, source_session, helper):
    request = result.get('requestId')
    if not isinstance(request, str) or not re.fullmatch(r'[0-9a-f]{32}', request):
        return False
    state = helper.progress({'requestId': request})
    return (state.get('ok') is True and state.get('sourceSessionId') == source_session and
            state.get('state') == 'attached' and state.get('draftVerified') is True and
            state.get('historyProjectionVerified') is True and state.get('clientAttachedVerified') is True)


def adapter_acknowledged(result, session, process, helper, *, timeout=3):
    """Read the private intent receipt; observing a poll is not a transfer."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            record = helper.read_json(helper.intent_path(session))
            observed = record.get('observedAt')
            if (record.get('intentId') == result.get('intentId') and record.get('sourceSessionId') == session and
                    record.get('sourceIdentity') == process):
                request = record.get('requestId')
                if (record.get('state') == 'prepared' and isinstance(request, str) and
                        re.fullmatch(r'[0-9a-f]{32}', request) and result.get('requestId') in (None, request)):
                    # Preparation may race ahead of queue() returning. Preserve
                    # the newly bound request so verification can inspect it.
                    result.update(requestId=request, state='prepared')
                    return True
                if type(observed) in (int, float) and 0 <= time.time() - observed < 10:
                    return True
        except (ValueError, OSError):
            pass
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(.1, remaining))


def transfer_native(executable, terminal, stdout, stderr):
    try:
        arguments = ['--slurp-native'] + (['--terminal', terminal] if terminal is not None else [])
        native = run_native(executable, arguments, timeout=120)
        if native.stdout:
            print(safe_text(native.stdout, multiline=True, limit=MAX_OUTPUT), end='' if native.stdout.endswith('\n') else '\n', file=stdout)
        if native.stderr:
            print(safe_text(native.stderr, multiline=True, limit=MAX_OUTPUT), end='' if native.stderr.endswith('\n') else '\n', file=stderr)
        if native.returncode:
            print(f'hardcore: native session handoff failed (exit {native.returncode}); inspect the original windows.', file=stderr)
            return 1
    except SlurpError as error:
        print('hardcore: '+safe_text(error), file=stderr)
        return 1
    return 0


def coordinate(executable, terminal=None, *, adapter=None, bootstrap=None, stdout=None, stderr=None):
    stdout, stderr = stdout or sys.stdout, stderr or sys.stderr
    bootstrap = bootstrap or bootstrap_adapter
    counts = {'queued': 0, 'verified': 0, 'unsupported': 0, 'failed': 0}
    try:
        discovery = discover(executable)
        sources = discovery['terminal']
        if discovery.get('terminalDiscoveryFailed'):
            counts['failed'] += 1
            print('hardcore: Terminal session discovery is unavailable; native transfers will still be attempted.', file=stderr)
        if terminal is not None:
            sources = [row for row in sources if row.get('tty') == terminal]
            if not sources and not discovery.get('terminalDiscoveryFailed'):
                raise SlurpError('The requested Terminal TTY was not found')
    except (SlurpError, OSError, ValueError) as error:
        print('hardcore: '+safe_text(error), file=stderr)
        counts['failed'] += 1
        sources = []
    # Gather native windows before Claude can create a guarded receiver. A
    # later global collection could move that receiver and invalidate receipts.
    counts['failed'] += transfer_native(executable, terminal, stdout, stderr)
    tty_counts = Counter(row.get('tty') for row in sources if isinstance(row.get('tty'), str))
    duplicates = {tty for tty, count in tty_counts.items() if count > 1}
    for source in sources:
        if source.get('managed') is True:
            continue  # The native path owns portable-host transfers.
        label = source_label(source)
        if isinstance(source.get('tty'), str) and source.get('tty') in duplicates:
            counts['failed'] += 1
            print(f'hardcore: {label}: ambiguous duplicate TTY; source retained.', file=stderr)
            continue
        if source.get('program') != 'claude':
            counts['unsupported'] += 1
            print(f'hardcore: {label}: direct transfer is unavailable; source retained in Terminal.', file=stderr)
            continue
        try:
            adapter = adapter or load_adapter()
            # A newer `claude` on PATH does not upgrade an already-running
            # native image. Refuse known unsupported runtimes before any CLI
            # session matching, configuration, plugin install or queueing.
            require_supported_runtime(source, adapter.MIN_VERSION)
            command = shutil.which('claude')
            if not command:
                raise SlurpError('Claude Code is not installed or is not on PATH')
            helper = adapter.handoff_helper()
            session, identity, process = match_claude(source, command, helper)
            project = Path(identity[4])
            def validate():
                revalidate_source(executable, identity, process, helper)
            validate()
            result = adapter.move(command, project, adapter.bundled_marketplace(), session,
                                  expected_source_pid=identity[3], expected_source_identity=process,
                                  validate_source=validate)
            if not isinstance(result, dict) or result.get('ok') is not True or result.get('state') not in ('queued', 'prepared', 'attached'):
                raise SlurpError('Claude adapter did not return a recognized handoff request')
            # Installation/queueing is not evidence that this running session
            # has loaded its adapter or that a receiving client is verified.
            runtime = None
            try:
                runtime = bootstrap(source, project, result)
            except Exception as error:
                print(f'hardcore: {label}: handoff is queued; adapter activation was not verified: {safe_text(error)}', file=stderr)
            verified = False
            try:
                verified = verified_result(result, session, helper)
            except (ValueError, OSError, subprocess.SubprocessError) as error:
                print(f'hardcore: {label}: completion is not verified: {safe_text(error)}', file=stderr)
            loaded = False
            if not verified:
                prior_request = result.get('requestId')
                loaded = (isinstance(runtime, dict) and runtime.get('runtimeLoaded') is True) or adapter_acknowledged(result, session, process, helper)
                if loaded or result.get('requestId') != prior_request:
                    try:
                        verified = verified_result(result, session, helper)
                    except (ValueError, OSError, subprocess.SubprocessError) as error:
                        print(f'hardcore: {label}: completion is not verified: {safe_text(error)}', file=stderr)
            if verified:
                counts['verified'] += 1
                print(f'hardcore: {label}: Claude conversation, draft and receiving client verified.', file=stdout)
            else:
                counts['queued'] += 1
                if loaded:
                    detail = 'receiver verification pending' if result.get('state') == 'prepared' else 'waiting for Claude’s next safe completed turn'
                    print(f'hardcore: {label}: handoff acknowledged; {detail} (transfer pending).', file=stdout)
                else:
                    print(f'hardcore: {label}: Claude handoff queued; transfer is pending.', file=stdout)
                    print('  If this running Claude has not loaded the adapter, run /reload-plugins there once. '
                          'The loaded adapter waits for a verified completed turn with no background work.', file=stdout)
        except Exception as error:
            counts['failed'] += 1
            print(f'hardcore: {label}: {safe_text(error)}; source retained.', file=stderr)
    if counts['queued'] or counts['verified']:
        print('Claude source Terminal windows remain open; their parent shell and Terminal state are not transferred.', file=stdout)
    print('Slurp: {verified} Claude verified, {queued} Claude queued (pending), '
          '{unsupported} unsupported, {failed} failed.'.format(**counts), file=stdout)
    return 1 if counts['failed'] or counts['unsupported'] else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path, help='Exact running Hardcore executable')
    parser.add_argument('--terminal', help='Limit discovery and native transfer to this Terminal TTY')
    args = parser.parse_args(argv)
    if args.terminal is not None and not TTY.fullmatch(args.terminal):
        parser.error('--terminal requires /dev/ttys followed by decimal digits')
    if not args.executable.is_absolute():
        parser.error('--executable requires an absolute path')
    try:
        return coordinate(args.executable, args.terminal)
    except (OSError, ImportError, ValueError) as error:
        print('hardcore: '+safe_text(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
