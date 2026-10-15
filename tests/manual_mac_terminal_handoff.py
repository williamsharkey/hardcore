#!/usr/bin/env python3
"""User-operated Terminal.app fixture: Claude inherits the real Terminal TTY.

This script never drives Terminal's UI, allocates a source PTY, or types input.
The user launches it and operates Claude. Only the disposable project receives
plugins, installed after the original source process has registered itself.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
FORMAT = 'hardcore-manual-terminal-test-v1'
PROMPT = 'Reply with exactly HC_TERMINAL_READY and nothing else.'
DRAFT = 'HC TERMINAL UNSENT café 🚀 END'
CURSOR = len(DRAFT.encode('utf-16-le')) // 2 - 3
OBSERVER_NAME = 'hc-terminal-observer'
OBSERVATION_PHASES = {'waiting-for-reload', 'waiting-for-observer', 'waiting-for-test-reply',
                      'waiting-for-draft', 'waiting-for-cursor', 'draft-ready'}
PHASE_HELP = {
    'preparing': 'Review INSTRUCTIONS.txt, then press Return in the original Terminal to start.',
    'source-started': 'Accept Claude onboarding/project trust if prompted. The watcher is finding this source session.',
    'installing-adapter': 'Wait while the adapter and observer are installed for this disposable project.',
    'waiting-for-reload': 'In the original Claude session, enter /reload-plugins once, then check this status again.',
    'waiting-for-observer': 'Waiting for a fresh observer update. Leave Claude open; if this persists, enter /reload-plugins again.',
    'waiting-for-test-reply': 'Send the test prompt below if you have not sent it yet; otherwise wait for Claude to finish its reply.',
    'waiting-for-draft': 'After the test reply, enter the exact draft below without Return.',
    'waiting-for-cursor': 'The draft text matches. Place the cursor immediately before END: from the end, press Left three times. Do not press Return.',
    'draft-ready': 'The draft and cursor match. Stop typing while the watcher requests the move.',
    'requesting-move': 'Stop typing. The watcher is asking the source adapter to move this session.',
    'waiting-for-verification': 'Stop typing. Wait for the destination draft, cursor, conversation, and rendered receiver to be verified.',
    'passed': 'Verification passed. Leave both windows open for inspection; use the cleanup command when finished.',
    'failed': 'The test stopped. Read the saved error and logs; if source Claude remains open, use /exit in that disposable session.',
    'interrupted': 'The test was interrupted. If source Claude remains open, use /exit in that disposable session.',
    'cleaned': 'Disposable processes stopped and copied login removed. Diagnostic files remain.',
}
OBSERVER = r'''
const sourceSessionId = __SOURCE_SESSION_ID__;
export function register(on) {
  on('session.start', async ($, e, next) => {
    $.clock.every(250, async () => {
      const sessionId = await $.session.id();
      await $.fs.write('terminal-probe-state-' + sessionId + '.json', JSON.stringify({
        sessionId, prompt: await $.prompt.read(), observedAt: Date.now(),
      }));
    });
    return next(e);
  });
  on('turn.complete', async ($, e, next) => {
    const result = await next(e);
    const sessionId = await $.session.id();
    await $.fs.write('terminal-probe-turn-' + sessionId + '.json', JSON.stringify({
      sessionId, reason: e.reason, answer: e.answer, isAborted: e.isAborted,
      turnId: e.turnId, observedAt: Date.now(),
    }));
    return result;
  });
}
'''


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    with temporary.open('w') as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
    temporary.chmod(0o600)
    temporary.replace(path)


def read_json(path):
    try:
        if path.stat().st_size > 4 * 1024 * 1024:
            return None
        value = json.loads(path.read_text())
        return value if isinstance(value, dict) else None
    except (OSError, ValueError):
        return None


def identity(pid):
    if type(pid) is not int or pid < 2:
        return None
    result = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=', '-o', 'args='],
                            capture_output=True, text=True, timeout=5)
    return result.stdout.strip() or None


def private_environment(root, original):
    env = {key: value for key, value in original.items()
           if not key.startswith(('CLAUDE', 'HARDCORE', 'ANTHROPIC')) and key not in ('TMUX', 'TMUX_PANE')}
    env.update(HOME=str(root / 'home'), CLAUDE_CONFIG_DIR=str(root / 'config'),
               HARDCORE_RUNTIME_DIR=str(root / 'run'), HARDCORE_SHELL='/bin/zsh',
               DISABLE_AUTOUPDATER='1', CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1')
    return env


def copy_login(config, original_home):
    """Copy only existing login/onboarding to this private disposable config."""
    original = read_json(original_home / '.claude.json')
    if original:
        keep = ('hasCompletedOnboarding', 'theme', 'lastOnboardingVersion',
                'oauthAccount', 'autoUpdates', 'hasSeenTasksHint')
        write_json(config / '.claude.json', {key: original[key] for key in keep if key in original})
    source = original_home / '.claude/.credentials.json'
    secret = source.read_bytes() if source.is_file() else None
    if secret is None:
        result = subprocess.run(['/usr/bin/security', 'find-generic-password',
            '-s', 'Claude Code-credentials', '-w'], capture_output=True, timeout=15)
        if result.returncode == 0:
            secret = result.stdout.strip()
    if secret:
        fd = os.open(config / '.credentials.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(secret)
    # Never put credentials or complete environments in diagnostics.


def launch_source(command, project, env):
    # None means inherit the user's real Terminal descriptors. Deliberately no
    # shell, pipes, pty, start_new_session, preexec_fn, host or input injection.
    return subprocess.Popen([command, '--setting-sources', 'project',
        '--strict-mcp-config', '--mcp-config', '{"mcpServers":{}}', '--no-chrome', '--tools', ''],
        cwd=project, env=env, stdin=None, stdout=None, stderr=None)


def draft_ready(state, turn, session_id, now_ms=None):
    now_ms = time.time() * 1000 if now_ms is None else now_ms
    return (isinstance(state, dict) and isinstance(turn, dict) and
        state.get('sessionId') == session_id == turn.get('sessionId') and
        state.get('prompt') == {'text': DRAFT, 'cursor': CURSOR} and
        type(state.get('observedAt')) in (int, float) and
        math.isfinite(state['observedAt']) and
        0 <= now_ms - state['observedAt'] < 3000 and
        type(turn.get('observedAt')) in (int, float) and math.isfinite(turn['observedAt']) and
        0 <= now_ms - turn['observedAt'] < 20 * 60 * 1000 and
        turn['observedAt'] <= state['observedAt'] and
        turn.get('reason') == 'answer' and turn.get('isAborted') is False and
        isinstance(turn.get('answer'), str) and turn['answer'].strip() == 'HC_TERMINAL_READY')


def observation_phase(state, turn, session_id, now_ms=None, observer_loaded=False):
    """Explain what the user should do without displaying arbitrary composer text."""
    now_ms = time.time() * 1000 if now_ms is None else now_ms
    if not isinstance(state, dict) or state.get('sessionId') != session_id:
        return 'waiting-for-observer' if observer_loaded else 'waiting-for-reload'
    observed = state.get('observedAt')
    if type(observed) not in (int, float) or not math.isfinite(observed) or not 0 <= now_ms - observed < 3000:
        return 'waiting-for-observer'
    # Reuse the completion gate with the fixture's expected draft. This only
    # describes progress; the actual move still requires draft_ready below.
    expected = {**state, 'prompt': {'text': DRAFT, 'cursor': CURSOR}}
    if not draft_ready(expected, turn, session_id, now_ms):
        return 'waiting-for-test-reply'
    prompt = state.get('prompt')
    if not isinstance(prompt, dict) or prompt.get('text') != DRAFT:
        return 'waiting-for-draft'
    if prompt != {'text': DRAFT, 'cursor': CURSOR}:
        return 'waiting-for-cursor'
    return 'draft-ready'


def load_run(root):
    root = root.expanduser().resolve(strict=True)
    info = root.stat()
    data = read_json(root / 'result.json')
    if (not root.is_dir() or info.st_uid != os.getuid() or info.st_mode & 0o077 or not data or
            data.get('format') != FORMAT or data.get('root') != str(root)):
        raise RuntimeError('Not an owned private manual Terminal test directory.')
    return root, data


def status_text(root, data, observe=False):
    """Format a read-only progress view; never call a CLI or inspect processes."""
    saved = data.get('status', 'unknown')
    phase = 'cleaned' if data.get('copiedLoginRemoved') else saved
    if observe and phase in OBSERVATION_PHASES:
        session_id = data.get('sourceSessionId')
        if isinstance(session_id, str) and re.fullmatch(r'[a-f0-9-]{36}', session_id):
            project = root / 'project'
            phase = observation_phase(read_json(project / f'terminal-probe-state-{session_id}.json'),
                                      read_json(project / f'terminal-probe-turn-{session_id}.json'), session_id,
                                      observer_loaded=bool(data.get('reloadObservedAt')))
    lines = ['Hardcore Terminal test', f'Run: {root}', f'Phase: {phase}',
             PHASE_HELP.get(phase, 'Inspect result.json and INSTRUCTIONS.txt for this run.')]
    if phase != saved and phase != 'cleaned':
        lines.append(f'Watcher last saved phase: {saved}; the phase above reflects its latest observer files.')
    if phase in ('waiting-for-test-reply', 'waiting-for-draft', 'waiting-for-cursor', 'draft-ready'):
        lines.extend([f'Test prompt (submit): {PROMPT}', f'Test draft (do not submit): {DRAFT}'])
    if phase == 'waiting-for-verification':
        handoff = data.get('handoff')
        if isinstance(handoff, dict) and isinstance(handoff.get('state'), str):
            lines.append(f'Handoff record: {handoff["state"]}')
    if phase in ('failed', 'interrupted') and isinstance(data.get('error'), str):
        # Errors are controlled coordinator diagnostics; never show saved draft,
        # raw CLI logs, credentials, or arbitrary observer text in this view.
        lines.append(f'Saved error: {data["error"]}')
    if phase in ('passed', 'cleaned'):
        lines.append('This is the saved outcome; status does not check whether processes are still running.')
    lines.extend(['Terminal remains open; shell state is not transferred.',
                  f'Persistent instructions: {root / "INSTRUCTIONS.txt"}',
                  'Status only reads local diagnostic files; it does not move, resume, or stop anything.'])
    return '\n'.join(lines) + '\n'


def show_status(root):
    root, data = load_run(root)
    print(status_text(root, data, observe=True), end='')


def write_status(root, data):
    text = status_text(root, data)
    path = root / 'STATUS.txt'
    if not path.exists() or path.read_text() != text:
        temporary = path.with_suffix('.txt.tmp')
        temporary.write_text(text)
        temporary.chmod(0o600)
        temporary.replace(path)


def record_verified(record, source_id, source_pid, source_identity):
    if not isinstance(record, dict):
        return False
    snapshot = record.get('snapshot') or {}
    if (not isinstance(snapshot, dict) or not isinstance(record.get('nativeReceipt'), dict) or
            not isinstance(snapshot.get('history'), dict)):
        return False
    receipt = record['nativeReceipt']
    bound_receipt = all(record.get(key) is not None and receipt.get(key) == record[key]
                        for key in ('requestId', 'windowPid', 'receiverPid', 'receiverTty'))
    return (record.get('sourceSessionId') == source_id and record.get('sourcePid') == source_pid and
        record.get('sourceIdentity') == source_identity and record.get('state') == 'attached' and
        bool(record.get('snapshotVerifiedAt')) and bool(record.get('inputReleasedAt')) and
        bound_receipt and receipt.get('ok') is True and receipt.get('rendered') is True and
        snapshot.get('draft') == {'text': DRAFT, 'cursor': CURSOR} and
        (snapshot.get('history') or {}).get('format') == 'api')


class Run:
    def __init__(self, root, command, executable):
        self.root, self.command, self.executable = root, command, executable
        self.project = root / 'project'
        self.project.mkdir()
        (self.project / '.claude').mkdir()
        write_json(self.project / '.claude/settings.json', {})
        for name in ('home', 'config', 'run'):
            (root / name).mkdir(mode=0o700)
        self.env = private_environment(root, os.environ)
        self.process = None
        self.data = {'format': FORMAT, 'root': str(root), 'project': str(self.project),
            'claude': command, 'executable': executable, 'startedAt': time.time(),
            'sourceHosted': False, 'sourcePtyAllocated': False, 'terminalClosed': False,
            'shellStateTransferred': False, 'status': 'preparing'}
        self.save()

    def save(self, **changes):
        previous = self.data.get('status')
        self.data.update(changes)
        if previous != self.data.get('status') or 'phaseHistory' not in self.data:
            self.data['statusUpdatedAt'] = time.time()
            history = self.data.setdefault('phaseHistory', [])
            history.append({'phase': self.data['status'], 'at': self.data['statusUpdatedAt']})
            self.data['phaseHistory'] = history[-64:]
        write_json(self.root / 'result.json', self.data)
        write_status(self.root, self.data)

    def cli(self, arguments, log, executable=None):
        result = subprocess.run([executable or self.command, *arguments], cwd=self.project,
            env=self.env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=90)
        (self.root / log).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{log}: command failed with exit {result.returncode}; inspect that private log.')
        return result.stdout

    def install(self):
        self.save(status='installing-adapter')
        # Copy the installed bundle to avoid changing the user's marketplace or
        # observing a partially replaced app during this manual fixture.
        market = self.root / 'plugins'
        bundled = Path(self.executable).parents[1] / 'Resources/plugins'
        shutil.copytree(bundled, market)
        manifest = market / 'hardcore-slurp/.claude-plugin/plugin.json'
        value = json.loads(manifest.read_text())
        value['userConfig']['executable']['default'] = self.executable
        write_json(manifest, value)
        self.cli(['claude', 'install', '--project', str(self.project), '--marketplace', str(market), '--json'],
                 'adapter-install.log', self.executable)
        observer_market = self.root / 'observer'
        plugin = observer_market / OBSERVER_NAME
        (observer_market / '.claude-plugin').mkdir(parents=True)
        (plugin / '.claude-plugin').mkdir(parents=True)
        (plugin / 'hooks').mkdir()
        write_json(plugin / '.claude-plugin/plugin.json', {'name': OBSERVER_NAME, 'version': '0.0.1',
            'description': 'Read-only observer for this disposable manual Terminal test'})
        write_json(plugin / 'hooks/hooks.json', {'modules': ['./register.js']})
        (plugin / 'hooks/register.js').write_text(OBSERVER.replace('__SOURCE_SESSION_ID__',
                                                               json.dumps(self.data['sourceSessionId'])))
        write_json(observer_market / '.claude-plugin/marketplace.json', {'name': OBSERVER_NAME,
            'owner': {'name': 'Hardcore test'}, 'plugins': [{'name': OBSERVER_NAME, 'source': './' + OBSERVER_NAME}]})
        for arguments, log in (
            (['plugin', 'validate', str(plugin)], 'observer-validate.log'),
            (['plugin', 'marketplace', 'add', '--scope', 'project', str(observer_market)], 'observer-marketplace.log'),
            (['plugin', 'install', '--scope', 'project', f'{OBSERVER_NAME}@{OBSERVER_NAME}'], 'observer-install.log')):
            self.cli(arguments, log)
        self.save(status='waiting-for-reload', installedAfterSource=True, installedAt=time.time())

    def source_session(self):
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError('Claude exited before its source session was registered.')
            rows = json.loads(self.cli(['agents', '--json', '--all', '--cwd', str(self.project)], 'source-agents.json'))
            matches = [row for row in rows if row.get('kind') == 'interactive' and row.get('pid') == self.process.pid]
            if len(matches) == 1 and re.fullmatch(r'[a-f0-9-]{36}', matches[0].get('sessionId', '')):
                return matches[0]['sessionId']
            time.sleep(.5)
        raise RuntimeError('Claude did not register the exact original source PID within two minutes.')

    def monitor(self, session_id, source_identity):
        state_path = self.project / f'terminal-probe-state-{session_id}.json'
        turn_path = self.project / f'terminal-probe-turn-{session_id}.json'
        deadline = time.monotonic() + 20 * 60
        stable_since = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError('Source exited before the expected unsent draft was ready.')
            state, turn = read_json(state_path), read_json(turn_path)
            phase = observation_phase(state, turn, session_id,
                                      observer_loaded=bool(self.data.get('reloadObservedAt')))
            if phase != self.data.get('status'):
                evidence = {}
                if phase not in ('waiting-for-reload', 'waiting-for-observer') and not self.data.get('reloadObservedAt'):
                    evidence['reloadObservedAt'] = state['observedAt'] / 1000
                self.save(status=phase, **evidence)
            if draft_ready(state, turn, session_id):
                if stable_since is None:
                    stable_since = time.monotonic()
                if time.monotonic() - stable_since >= 1:
                    break
            else:
                stable_since = None
            time.sleep(.2)
        else:
            raise RuntimeError('Timed out waiting for reload, test reply and exact draft/cursor. Nothing was moved.')
        if identity(self.process.pid) != source_identity:
            raise RuntimeError('Original source identity changed before the move.')
        self.save(status='requesting-move', sourceDraft=state, sourceTurn=turn)
        moved = json.loads(self.cli(['claude', 'move', session_id, '--project', str(self.project),
            '--marketplace', str(self.root / 'plugins'), '--json'], 'move.json', self.executable))
        self.save(status='waiting-for-verification', intentId=moved['intentId'])
        requests = self.root / 'home/Library/Caches/hardcore/claude-handoff'
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            for path in requests.glob('*.json'):
                if not re.fullmatch(r'[a-f0-9]{32}\.json', path.name):
                    continue
                record = read_json(path)
                if not record or record.get('sourceSessionId') != session_id:
                    continue
                self.save(handoff=record)
                if record_verified(record, session_id, self.process.pid, source_identity):
                    destination = read_json(self.project / f"terminal-probe-state-{record['destinationSession']}.json")
                    if not destination or destination.get('sessionId') != record['destinationSession'] or \
                            destination.get('prompt') != {'text': DRAFT, 'cursor': CURSOR} or \
                            type(destination.get('observedAt')) not in (int, float) or \
                            not 0 <= time.time() * 1000 - destination['observedAt'] < 3000 or \
                            destination['observedAt'] < record['snapshotVerifiedAt'] * 1000:
                        continue
                    inspection = json.loads(self.cli(['claude', 'inspect', record['requestId'], '--json'],
                                                    'inspection.json', self.executable))
                    if not inspection.get('destinationAvailable') or not inspection.get('clientAvailable'):
                        raise RuntimeError('Saved verification exists, but its destination/client is no longer available.')
                    if self.process.poll() is None:
                        continue
                    if self.process.returncode != 0:
                        raise RuntimeError(f'Source exited with unexpected status {self.process.returncode}.')
                    self.save(status='passed', completedAt=time.time(), destinationDraft=destination,
                        inspection=inspection, sourceExitCode=0, draftVerified=True,
                        currentConversationVerified=True, renderedReceiverVerified=True)
                    return
                if record.get('state') in ('failed', 'cancelled'):
                    raise RuntimeError('Handoff stopped; saved draft and diagnostics remain in this run directory.')
            time.sleep(.2)
        raise RuntimeError('Handoff did not finish verification within 150 seconds; inspect the saved record.')


def cleanup(root):
    root, data = load_run(root)
    record = data.get('handoff') or {}
    env = private_environment(root, os.environ)
    bound_processes = [(data.get('sourcePid'), data.get('sourceIdentity')),
                       (record.get('windowPid'), record.get('windowIdentity')),
                       (record.get('receiverPid'), record.get('receiverIdentity')),
                       (record.get('destinationPid'), record.get('destinationIdentity'))]
    for pid, expected in bound_processes:
        actual = identity(pid)
        if actual and (not expected or actual != expected):
            raise RuntimeError('A recorded process identity changed; copied login retained for inspection.')
    listed = subprocess.run([data['claude'], 'agents', '--json', '--all', '--cwd', data['project']],
        cwd=data['project'], env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
    if listed.returncode:
        raise RuntimeError('Cannot check disposable sessions; copied login retained for inspection.')
    known_pids = {pid for pid, expected in bound_processes if expected}
    for row in json.loads(listed.stdout):
        if row.get('pid') and identity(row['pid']) and row['pid'] not in known_pids:
            raise RuntimeError('An unrecorded process still uses the disposable project; copied login retained.')
    # Stop only the exact background session/process recorded by this fixture.
    if record.get('destinationPid') and identity(record['destinationPid']) == record.get('destinationIdentity'):
        result = subprocess.run([data['claude'], 'stop', record['jobId']], cwd=data['project'], env=env,
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise RuntimeError('Could not stop the disposable background session; credentials retained.')
    for pid, expected in bound_processes[:3]:
        if expected and identity(pid) == expected:
            os.kill(pid, signal.SIGTERM)
    deadline = time.monotonic() + 10
    while any(expected and identity(pid) == expected for pid, expected in bound_processes):
        if time.monotonic() >= deadline:
            raise RuntimeError('A disposable process is still exiting; copied login retained. Retry cleanup shortly.')
        time.sleep(.2)
    # Keep diagnostic records, but remove copied auth and its possible backups.
    for name in ('.credentials.json', '.claude.json'):
        (root / 'config' / name).unlink(missing_ok=True)
    shutil.rmtree(root / 'config/backups', ignore_errors=True)
    data.update(cleanedAt=time.time(), copiedLoginRemoved=True)
    write_json(root / 'result.json', data)
    write_status(root, data)
    print(f'Disposable test stopped and copied login removed. Diagnostics remain: {root}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--check', action='store_true', help='check dependencies without starting Claude or a GUI')
    action.add_argument('--cleanup', type=Path, metavar='RUN_DIRECTORY', help='stop this disposable test and remove its copied login')
    action.add_argument('--status', type=Path, metavar='RUN_DIRECTORY', help='read progress and next step without contacting Claude or changing this run')
    args = parser.parse_args()
    if args.status:
        show_status(args.status)
        return 0
    os.umask(0o077)
    if args.cleanup:
        cleanup(args.cleanup)
        return 0
    if sys.platform != 'darwin':
        raise RuntimeError('Run this fixture on macOS.')
    command = shutil.which('claude')
    executable = Path.home() / 'Applications/hardcore.app/Contents/MacOS/hardcore'
    if not command or not executable.is_file():
        raise RuntimeError('Claude on PATH and the installed ~/Applications/hardcore.app are required.')
    version = subprocess.run([command, '--version'], stdin=subprocess.DEVNULL,
                             capture_output=True, text=True, check=True, timeout=15).stdout.strip()
    match = re.fullmatch(r'(\d+)\.(\d+)\.(\d+) \(Claude Code\)', version)
    if not match or tuple(map(int, match.groups())) < (2, 1, 300):
        raise RuntimeError('This fixture requires Claude Code 2.1.300 or newer.')
    if args.check:
        print(f'Ready: {version}; installed Hardcore found. No session started.')
        return 0
    if not all(os.isatty(fd) for fd in (0, 1, 2)) or os.environ.get('TERM_PROGRAM') != 'Apple_Terminal':
        raise RuntimeError('Run this command yourself in a fresh Apple Terminal window, without redirection.')
    if os.environ.get('TMUX') or os.environ.get('HARDCORE_WINDOW_PID'):
        raise RuntimeError('Use a plain Terminal shell, outside tmux or Hardcore.')
    tty = os.ttyname(0)
    if any(os.ttyname(fd) != tty for fd in (1, 2)):
        raise RuntimeError('All three standard descriptors must belong to the same Terminal TTY.')
    original_termios = termios.tcgetattr(0)
    original_sigint = signal.getsignal(signal.SIGINT)
    base = Path.home() / 'Desktop/Hardcore-Terminal-Test-Results'
    base.mkdir(mode=0o700, exist_ok=True)
    if base.is_symlink() or base.stat().st_mode & 0o077:
        raise RuntimeError('The test-results folder must be a private directory (0700).')
    root = Path(tempfile.mkdtemp(prefix='run-', dir=base)).resolve()
    fixture = Run(root, command, str(executable))
    cleanup_command = shlex.join([str(Path.home() / 'Desktop/hardcore-terminal-test.command'), '--cleanup', str(root)])
    status_command = shlex.join([str(Path.home() / 'Desktop/hardcore-terminal-test.command'), '--status', str(root)])
    instructions = (f'Hardcore real Terminal test\nResults: {root}\n\n'
        'This starts a fresh disposable Claude conversation directly on this Terminal TTY.\n'
        'The adapter is installed AFTER Claude starts. No Terminal input is automated.\n'
        'It uses your existing Claude login and one small model response.\n\n'
        f'Progress and next action stay in: {root / "STATUS.txt"}\n'
        'You can also read current status from a second shell (repeat whenever needed):\n'
        f'{status_command}\n'
        'Do not enter the status command into Claude. No visible banner is required.\n\n'
        '1. Accept Claude onboarding/project trust if prompted.\n'
        '2. When status says waiting-for-reload, enter /reload-plugins in Claude.\n'
        '   Wait for status waiting-for-test-reply before continuing.\n'
        f'3. Send: {PROMPT}\n'
        f'4. After the reply, type (or paste): {DRAFT}\n'
        '   Do NOT press Return. Press Left exactly three times, then stop typing.\n'
        '5. The watcher requests the move. Leave both windows open after PASS.\n\n'
        'This verifies Claude draft/cursor/context; it does not move your parent shell\n'
        'or close Terminal. The destination remains available for inspection.\n'
        'The private run folder retains temporary login data until test cleanup:\n'
        f'{cleanup_command}\n')
    (root / 'INSTRUCTIONS.txt').write_text(instructions)
    print(instructions, flush=True)
    input('Press Return to start the disposable Claude session: ')
    try:
        copy_login(root / 'config', Path.home())
        fixture.process = launch_source(command, fixture.project, fixture.env)
        # Claude retains normal Ctrl+C behavior. Ignore it only in this parent
        # after spawning, so cancelling a Claude turn does not abort monitoring.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        source_identity = identity(fixture.process.pid)
        fixture.save(status='source-started', sourcePid=fixture.process.pid, sourceIdentity=source_identity,
            terminalTty=tty, termProgram=os.environ.get('TERM_PROGRAM'), parentPid=os.getppid(),
            sourceProcessGroup=os.getpgid(fixture.process.pid), terminalForegroundGroup=os.tcgetpgrp(0))
        session_id = fixture.source_session()
        tty_info = subprocess.run(['ps', '-p', str(fixture.process.pid), '-o', 'tty='],
            capture_output=True, text=True, check=True).stdout.strip()
        if tty_info != tty.removeprefix('/dev/') or not source_identity:
            raise RuntimeError('Source Claude did not retain the original Terminal TTY/identity.')
        fixture.save(sourceSessionId=session_id, sourceTty=tty_info)
        fixture.install()
        fixture.monitor(session_id, source_identity)
        print('\nPASS: original Terminal Claude exited normally; Hardcore verified the exact\n'
              'unsent draft, middle cursor, current conversation, and rendered receiver.\n'
              'Leave the Hardcore window and this Terminal window open for inspection.', flush=True)
        return 0
    except (Exception, KeyboardInterrupt) as error:
        fixture.save(status='interrupted' if isinstance(error, KeyboardInterrupt) else 'failed', error=str(error))
        # Do not read user input or restore termios while Claude still owns this
        # foreground TTY. Let the user exit the disposable source normally.
        print('\r\nTest paused: ' + (str(error) or 'interrupted') +
              '\r\nIf Claude is still open, use /exit in that disposable session.\r\n', flush=True)
        if fixture.process is not None:
            while fixture.process.poll() is None:
                try:
                    fixture.process.wait(timeout=1)
                except (subprocess.TimeoutExpired, KeyboardInterrupt):
                    pass
        return 1
    finally:
        signal.signal(signal.SIGINT, original_sigint)
        if fixture.process is None or fixture.process.poll() is not None:
            termios.tcsetattr(0, termios.TCSADRAIN, original_termios)
        print(f'\nResults: {root}\nRead status:\n{status_command}\nCleanup when finished inspecting:\n{cleanup_command}', flush=True)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (Exception, KeyboardInterrupt) as error:
        print(f'Terminal test: {error or "cancelled"}', file=sys.stderr)
        raise SystemExit(1)
