#!/usr/bin/env python3
"""Run noninteractive Mac slurp verification on saved data and owned fixtures.

No Terminal automation, clipboard access, Claude/model calls, installation, or
changes to existing sessions. Default also clones disposable zsh shells and
starts a new Hardcore receiver; --offline skips those live stages. Headless
regressions can use owned PTY pairs without starting shells.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import signal
import stat
import subprocess
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]
SUITES = (
    ('archive-tests', 'tests/test_mac_terminal_archive.py'),
    ('archive-workflow-tests', 'tests/test_manual_mac_terminal_archive.py'),
    ('handoff-workflow-tests', 'tests/test_manual_mac_terminal_handoff.py'),
    ('retirement-tests', 'tests/test_mac_terminal_retire.py'),
    ('owned-probe-lifecycle-tests', 'tests/test_mac_probe_lifecycle.py'),
    ('native-import-resize-tests', 'tests/test_mac_import_resize.py'),
    ('native-frame-order-tests', 'tests/test_mac_frame_order.py'),
    ('verification-runner-tests', 'tests/test_verify_mac_slurp.py'),
)
FALSE_SCOPE = {'fullTerminalState': False, 'automaticClosure': False,
               'freshTerminalCapture': False, 'historicalTerminalCaptureReplayed': False, 'realClaudeSession': False,
               'clipboardAccess': False, 'existingSessionChanged': False}
LOG_LIMIT = 8 * 1024 * 1024


def now():
    return datetime.now(timezone.utc).isoformat(timespec='seconds')


def atomic_json(path: Path, value):
    temporary = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(fd, 'w') as stream:
            json.dump(value, stream, indent=2, ensure_ascii=False)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def private_bytes(path: Path, data: bytes):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)


def archive_module():
    spec = importlib.util.spec_from_file_location('slurp_manual_archive', ROOT / 'tools/manual_mac_terminal_archive.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def discover_capture(validator, base=Path('/private/tmp')):
    """Only inspect original captures bearing this fixture's generated prefix."""
    candidates = []
    for path in base.glob('hc-terminal-archive-*/capture'):
        try:
            if not path.is_symlink() and path.is_dir():
                candidates.append((path.stat().st_mtime_ns, path))
        except OSError:
            continue
    rejected = 0
    for _, path in sorted(candidates, reverse=True)[:200]:
        try:
            return validator(path), rejected
        except (OSError, ValueError):
            rejected += 1
    return None, rejected


def copy_capture(validated, directory: Path):
    source_run, token, expected, source_capture, rtf, plain = validated
    directory.mkdir(mode=0o700)
    target = directory / 'capture'
    target.mkdir(mode=0o700)
    private_bytes(directory / 'fixture.txt', expected.encode('utf-8'))
    atomic_json(directory / 'manifest.json', {'fixtureToken': token, 'fullTerminalState': False})
    private_bytes(target / 'original.rtf', rtf)
    private_bytes(target / 'original.txt', plain)
    return target, {'sourceRun': str(source_run), 'sourceCapture': str(source_capture),
                    'privateSnapshot': str(target), 'historicalReplayOnly': True}


def child_environment(directory: Path):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('CLAUDE', 'HARDCORE', 'ANTHROPIC', 'DYLD_'))
           and key not in ('TMUX', 'TMUX_PANE', 'PYTHONPATH', 'PYTHONSTARTUP')}
    home = directory / 'home'
    home.mkdir(mode=0o700)
    env.update(HOME=str(home), PYTHONUNBUFFERED='1')
    return env


def stop_owned(process, grace=3):
    """Signal only the new process group created for this owned child."""
    actions = []
    if process.poll() is not None:
        return actions
    try:
        os.killpg(process.pid, signal.SIGTERM)
        actions.append('SIGTERM-owned-process-group')
    except ProcessLookupError:
        pass
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
            actions.append('SIGKILL-owned-process-group')
        except ProcessLookupError:
            pass
        process.wait(timeout=grace)
    return actions


def collect(process, streams, timeout, limit=LOG_LIMIT):
    """Drain both pipes while limiting stored bytes; never block on stdin."""
    counts = {name: {'bytesObserved': 0, 'bytesStored': 0, 'truncated': False} for name in streams}
    selector = selectors.DefaultSelector()
    for name, (pipe, destination) in streams.items():
        os.set_blocking(pipe.fileno(), False)
        selector.register(pipe, selectors.EVENT_READ, (name, destination))
    deadline = time.monotonic() + timeout
    drained_by = None
    termination = []
    timed_out = False
    drain_incomplete = False
    try:
        while selector.get_map() or process.poll() is None:
            stamp = time.monotonic()
            if not timed_out and process.poll() is None and stamp >= deadline:
                timed_out = True
                termination = stop_owned(process)
            if process.poll() is not None:
                if drained_by is None:
                    drained_by = time.monotonic() + 1
                if selector.get_map() and time.monotonic() >= drained_by:
                    drain_incomplete = True
                    break
            for key, _ in selector.select(.05):
                name, destination = key.data
                try:
                    part = os.read(key.fd, 65536)
                except BlockingIOError:
                    continue
                if not part:
                    selector.unregister(key.fileobj)
                    continue
                row = counts[name]
                row['bytesObserved'] += len(part)
                kept = part[:max(0, limit - row['bytesStored'])]
                destination.write(kept)
                row['bytesStored'] += len(kept)
                row['truncated'] = row['bytesObserved'] > row['bytesStored']
        return {'exitCode': process.wait(timeout=3), 'timedOut': timed_out,
                'termination': termination, 'logStreams': counts, 'logDrainIncomplete': drain_incomplete}
    finally:
        selector.close()
        for pipe, _ in streams.values():
            pipe.close()


def diagnostic_paths(logs):
    paths = []
    pattern = re.compile(r'(?:Private diagnostics|Private results|New private attempt|Report):\s+(/[^\r\n]+)')
    for path in logs:
        with path.open('rb') as stream:
            text = stream.read(2 * 1024 * 1024).decode('utf-8', errors='replace')
        for found in pattern.findall(text):
            found = found.strip()
            if found not in paths:
                paths.append(found)
    return paths[:64]


def outcome(stages):
    selected = [stage for stage in stages if stage.get('selected')]
    if any(stage['status'] in ('failed', 'timed-out', 'interrupted') for stage in selected):
        return 'failed', 1
    if any(stage['status'] != 'passed' for stage in selected):
        return 'incomplete', 2
    return ('passed', 0) if selected else ('incomplete', 2)


class Verification:
    def __init__(self, directory: Path, scope: str):
        self.directory = directory
        self.started = time.monotonic()
        self.env = child_environment(directory)
        self.report = {'format': 'hardcore-mac-slurp-verification-v1', 'status': 'running',
            'scope': scope, 'startedAt': now(), 'repository': str(ROOT),
            'resultsDirectory': str(directory), 'stages': [], **FALSE_SCOPE}
        self.save()

    def summary(self):
        lines = [f'Hardcore Mac slurp verification: {self.report["status"].upper()}',
                 f'Scope: {self.report["scope"]}', f'Results: {self.directory}', '']
        for stage in self.report['stages']:
            elapsed = f' ({stage["elapsedSeconds"]:.2f}s)' if 'elapsedSeconds' in stage else ''
            lines.append(f'{stage["status"].upper()}: {stage["name"]}{elapsed}')
            if stage.get('reason'):
                lines.append('  ' + stage['reason'])
            for key in ('stdoutLog', 'stderrLog'):
                if stage.get(key):
                    lines.append('  ' + key + ': ' + stage[key])
            if any(value.get('truncated') for value in stage.get('logStreams', {}).values()):
                lines.append(f'  Log storage capped at {LOG_LIMIT} bytes per stream; truncation counts are in report.json.')
        lines.extend(['', 'Saved captures are historical replays; no new Terminal capture is taken.',
            'No Terminal UI, clipboard, real Claude/model calls, or existing sessions are accessed.',
            'Passing these checks does not establish full Terminal state transfer or authorize automatic closure.'])
        return '\n'.join(lines) + '\n'

    def save(self):
        atomic_json(self.directory / 'report.json', self.report)
        temporary = self.directory / ('SUMMARY.' + uuid.uuid4().hex + '.tmp')
        private_bytes(temporary, self.summary().encode('utf-8'))
        os.replace(temporary, self.directory / 'SUMMARY.txt')

    def record(self, name, status, reason, selected=True, **details):
        stage = {'name': name, 'status': status, 'selected': selected, 'reason': reason, **details}
        self.report['stages'].append(stage)
        self.save()
        print(f'{status.upper()}: {name}: {reason}', flush=True)
        return stage

    def run_stage(self, name, arguments, timeout=120, evidence_report=None):
        folder = self.directory / f'{len(self.report["stages"]) + 1:02d}-{name}'
        folder.mkdir(mode=0o700)
        stdout_path, stderr_path = folder / 'stdout.log', folder / 'stderr.log'
        stage = {'name': name, 'status': 'running', 'selected': True,
                 'command': list(map(str, arguments)), 'startedAt': now(), 'timeoutSeconds': timeout,
                 'logLimitBytesPerStream': LOG_LIMIT,
                 'stdoutLog': str(stdout_path), 'stderrLog': str(stderr_path)}
        self.report['stages'].append(stage)
        self.save()
        print(f'RUNNING: {name}', flush=True)
        started = time.monotonic()
        process = None
        interrupted = None
        try:
            # Enter stdout's owner before attempting stderr, so a second-open
            # failure cannot leak the already-created descriptor.
            with os.fdopen(os.open(stdout_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stdout, \
                 os.fdopen(os.open(stderr_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), 'wb') as stderr:
                process = subprocess.Popen(stage['command'], cwd=ROOT, env=self.env,
                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    start_new_session=True, close_fds=True)
                stage['pid'] = process.pid
                self.save()
                try:
                    captured = collect(process, {'stdout': (process.stdout, stdout), 'stderr': (process.stderr, stderr)}, timeout)
                    stage.update(captured)
                    if captured['timedOut']:
                        stage.update(status='timed-out', reason=f'Exceeded the {timeout:g}-second stage deadline.')
                    elif captured['logDrainIncomplete']:
                        stage.update(status='failed', reason='An owned descendant kept output pipes open after its stage exited.')
                    else:
                        stage['status'] = 'passed' if stage['exitCode'] == 0 else 'failed'
                        if stage['exitCode'] != 0:
                            stage['reason'] = f'Check exited with status {stage["exitCode"]}.'
                except KeyboardInterrupt as error:
                    interrupted = error
                    stage.update(status='interrupted', reason='Interrupted by the user.')
                    stage['termination'] = stop_owned(process)
                    stage['exitCode'] = process.returncode
        except KeyboardInterrupt as error:
            stage.update(status='interrupted', reason='Interrupted by the user.')
            interrupted = error
        except Exception as error:
            stage.update(status='incomplete' if process is None else 'failed',
                         reason=f'Owned check could not {"start" if process is None else "complete"}: '
                                f'{type(error).__name__}: {str(error)[:2000]}')
        finally:
            # collect(), polling, and its normal timeout cleanup can themselves
            # fail. This final owner check must still retire our direct child
            # and persist a non-running result before independent stages run.
            if process is not None:
                try:
                    if process.poll() is None:
                        stage.setdefault('termination', []).extend(stop_owned(process))
                except Exception as error:
                    stage['cleanupError'] = f'{type(error).__name__}: {str(error)[:2000]}'
                    stage.update(status='failed', reason='Owned process-group cleanup did not complete.')
                    try:
                        if process.poll() is None:
                            process.kill()
                            stage.setdefault('termination', []).append('SIGKILL-owned-direct-child-fallback')
                            process.wait(timeout=3)
                    except Exception as fallback:
                        stage['directChildCleanupError'] = f'{type(fallback).__name__}: {str(fallback)[:2000]}'
                stage['exitCode'] = process.returncode
                for pipe in (process.stdout, process.stderr):
                    if pipe is not None:
                        pipe.close()
            stage.update(finishedAt=now(), elapsedSeconds=round(time.monotonic() - started, 3))
            existing = [path for path in (stdout_path, stderr_path) if path.exists()]
            try:
                stage['diagnosticPaths'] = diagnostic_paths(existing)
            except OSError as error:
                stage.update(status='failed', reason=f'Could not finish reading owned check logs: {error}', diagnosticPaths=[])
            if evidence_report is not None:
                stage['evidenceReport'] = str(evidence_report)
                if stage['status'] == 'passed':
                    try:
                        evidence = json.loads(archive_module().read_private(evidence_report, 4 * 1024 * 1024))
                        if evidence.get('status') != 'passed':
                            raise ValueError('evidence report does not confirm a passed run')
                        stage['counts'] = evidence.get('counts', {})
                    except (OSError, ValueError, AttributeError) as error:
                        stage.update(status='failed', reason=f'Missing or invalid probe evidence: {error}')
            self.save()
            print(f'{stage["status"].upper()}: {name} ({stage["elapsedSeconds"]:.2f}s)', flush=True)
        if interrupted is not None:
            raise interrupted
        return stage

    def finish(self):
        status, code = outcome(self.report['stages'])
        self.report.update(status=status, exitCode=code, finishedAt=now(),
                           elapsedSeconds=round(time.monotonic() - self.started, 3),
                           selectedStagesPassed=status == 'passed')
        self.save()
        print(f'\n{status.upper()} ({self.report["scope"]}). Summary: {self.directory / "SUMMARY.txt"}', flush=True)
        return code


def python_check(runner, name, script, extra=(), timeout=120, evidence_report=None):
    path = ROOT / script
    if not path.is_file():
        return runner.record(name, 'incomplete', f'Required repository probe is missing: {path}')
    return runner.run_stage(name, [sys.executable, str(path), *map(str, extra)], timeout, evidence_report)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true', help='run deterministic checks and saved-data replay; skip live shell/GUI probes')
    parser.add_argument('--no-gui', action='store_true', help='run owned PTY checks but skip the native receiver')
    parser.add_argument('--capture', type=Path, help='explicit saved fixture capture; default discovers the latest valid original capture')
    parser.add_argument('--executable', type=Path, default=ROOT / 'build/hardcore')
    parser.add_argument('--repeat', type=int, default=1, help='owned PTY/native repetitions (1 through 10; default 1)')
    parser.add_argument('--output-dir', type=Path, help='new results directory; default creates a private /private/tmp directory')
    args = parser.parse_args(argv)
    if not 1 <= args.repeat <= 10:
        parser.error('--repeat must be between 1 and 10')
    if sys.platform != 'darwin':
        parser.error('this verification requires macOS')
    os.umask(0o077)
    if args.output_dir:
        directory = args.output_dir.expanduser().absolute()
        directory.mkdir(mode=0o700)
    else:
        directory = Path(tempfile.mkdtemp(prefix='hc-slurp-verification-', dir='/private/tmp')).resolve()
    scope = 'offline' if args.offline else 'owned-pty' if args.no_gui else 'owned-native'
    runner = Verification(directory, scope)
    print(f'Private results: {directory}', flush=True)
    try:
        for name, script in SUITES:
            python_check(runner, name, script)
        snapshot = None
        try:
            module = archive_module()
            if args.capture:
                selected, rejected = module.saved_capture(args.capture), 0
            else:
                selected, rejected = discover_capture(module.saved_capture)
            runner.report['captureDiscovery'] = {'explicit': bool(args.capture), 'rejectedCandidates': rejected}
            if selected:
                snapshot, metadata = copy_capture(selected, directory / 'replay-fixture')
                runner.report['savedCapture'] = metadata
                runner.save()
                replay = python_check(runner, 'saved-capture-replay', 'tools/manual_mac_terminal_archive.py',
                                      ['--decode-capture', snapshot], timeout=180)
                runner.report['historicalTerminalCaptureReplayed'] = replay['status'] == 'passed'
                runner.save()
            else:
                runner.record('saved-capture-replay', 'skipped' if args.offline else 'incomplete',
                    'No valid saved original fixture capture was found; historical replay was not tested.',
                    selected=not args.offline)
        except (OSError, ValueError) as error:
            runner.record('saved-capture-replay', 'failed' if args.capture else 'incomplete',
                          f'Saved capture could not be validated or copied: {error}')
        robust = directory / 'archive-robustness'
        robust_args = ['--output-dir', robust, *(['--capture', snapshot] if snapshot else [])]
        python_check(runner, 'archive-robustness', 'tests/probe_mac_archive_robustness.py',
                     robust_args, timeout=360, evidence_report=robust / 'report.json')
        for number in range(1, args.repeat + 1):
            suffix = f'-{number}' if args.repeat > 1 else ''
            if args.offline:
                runner.record('owned-zsh-state' + suffix, 'skipped', 'Excluded by --offline.', selected=False)
            else:
                python_check(runner, 'owned-zsh-state' + suffix, 'tests/probe_mac_zsh_clone_state.py', timeout=180)
            if args.offline or args.no_gui:
                runner.record('owned-native-receiver' + suffix, 'skipped',
                              'Excluded by --offline.' if args.offline else 'Excluded by --no-gui.', selected=False)
            elif not args.executable.is_file() or not os.access(args.executable, os.X_OK):
                runner.record('owned-native-receiver' + suffix, 'incomplete',
                    'Hardcore executable is missing or not executable. Build it with ' +
                    shlex.join(['make', '-C', str(ROOT)]) + '; then rerun, or supply --executable PATH.')
            else:
                python_check(runner, 'owned-native-receiver' + suffix, 'tests/probe_mac_zsh_hardcore.py',
                             ['--executable', args.executable.resolve()], timeout=120)
        return runner.finish()
    except KeyboardInterrupt:
        runner.report.update(status='interrupted', exitCode=130, finishedAt=now())
        runner.save()
        print(f'Interrupted. Partial results: {directory / "report.json"}', flush=True)
        return 130


if __name__ == '__main__':
    raise SystemExit(main())
