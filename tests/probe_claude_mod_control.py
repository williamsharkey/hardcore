#!/usr/bin/env python3
"""Opt-in, no-inference probe of a mod loaded into an already-running Claude.

Uses a private CLAUDE_CONFIG_DIR, project, marketplace, and disposable PTY.
The source starts without plugins, hooks, a host, or tmux. The mod is installed
afterward with the documented project-scoped CLI, then /reload-plugins loads
it. A guard intercepts /bg; this probe never actually backgrounds a session.
Existing login is copied privately and removed on exit, never printed.
"""
import fcntl
import json
import os
from pathlib import Path
import pty
import re
import select
import shutil
import struct
import subprocess
import tempfile
import termios
import time


MODULE = r'''
export function register(on) {
  on('session.start', async ($, e, next) => {
    await $.fs.write('mod-started.json', JSON.stringify({
      sessionId: await $.session.id(), version: await $.session.version(),
    }));
    await $.command.register({name: 'hc-snapshot',
      description: 'Disposable handoff state probe', immediate: true});
    $.clock.every(200, async () => {
      await $.fs.write('mod-draft.json', JSON.stringify(await $.prompt.read()));
    });
    return next(e);
  });
  on('command.run', {command: 'hc-snapshot'}, async ($, e) => {
    const state = {sessionId: await $.session.id(),
      prompt: await $.prompt.read(), agents: await $.agent.list(),
      commands: (await $.command.list()).map(c => c.name), origin: e.origin};
    await $.fs.write('mod-state.json', JSON.stringify(state));
    // A command hook cannot await another command: the engine rejects that
    // potential deadlock. A documented timer runs outside the held event.
    $.clock.after(100, async () => {
      try {
        const result = await $.command.run({command: 'rename', args: 'HCMODDISPOSABLE'});
        await $.fs.write('mod-api-command.json', JSON.stringify({ok: true, result}));
      } catch (error) {
        await $.fs.write('mod-api-command.json', JSON.stringify({ok: false, error: String(error)}));
      }
    });
    return {text: 'HCMODSNAPSHOT: state recorded without inference'};
  });
  // /bg is an alias: command.run receives its canonical name, background.
  on('command.run', {command: 'background'}, async ($, e) => {
    await $.fs.write('mod-bg-guard.json', JSON.stringify({
      sessionId: await $.session.id(), command: e.command, origin: e.origin,
      decision: 'blocked by disposable research guard',
    }));
    return {text: 'HCMODBGBLOCKED: no handoff attempted'};
  });
}
'''


def probe(root):
    command = shutil.which('claude')
    assert command, 'claude is not installed'
    work = root/'work'; work.mkdir()
    config = root/'config'; config.mkdir(mode=0o700)
    project = work/'.claude'; project.mkdir()
    (project/'settings.json').write_text('{}')
    env = {k: v for k, v in os.environ.items() if not k.startswith('CLAUDE')
           and k not in ('TMUX', 'HARDCORE_WINDOW_PID', 'ANTHROPIC_API_KEY')}
    env.update(TERM='xterm-256color', COLORTERM='truecolor',
               CLAUDE_CONFIG_DIR=str(config), DISABLE_AUTOUPDATER='1',
               CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC='1')
    # Preserve onboarding and account metadata, but not project trust entries.
    original = Path.home()/'.claude.json'
    if original.exists():
        data = json.loads(original.read_text())
        keep = ('hasCompletedOnboarding', 'theme', 'lastOnboardingVersion',
                'oauthAccount', 'autoUpdates', 'hasSeenTasksHint')
        (config/'.claude.json').write_text(json.dumps({k: data[k] for k in keep if k in data}))
        os.chmod(config/'.claude.json', 0o600)
    credential = config/'.credentials.json'
    source_credential = Path.home()/'.claude/.credentials.json'
    secret = None
    if source_credential.exists():
        secret = source_credential.read_bytes()
    elif os.uname().sysname == 'Darwin':
        result = subprocess.run(['/usr/bin/security', 'find-generic-password',
            '-s', 'Claude Code-credentials', '-w'], capture_output=True, timeout=10)
        if result.returncode == 0:
            secret = result.stdout.strip()
    if secret:
        fd = os.open(credential, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'wb') as stream: stream.write(secret)
    del secret
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 32, 110, 0, 0))

    def setup():
        os.setsid(); fcntl.ioctl(0, termios.TIOCSCTTY, 0)

    proc = subprocess.Popen([command, '--setting-sources', 'project',
        '--strict-mcp-config', '--mcp-config', '{"mcpServers":{}}',
        '--no-chrome', '--tools', ''], cwd=work, env=env,
        stdin=slave, stdout=slave, stderr=slave, preexec_fn=setup)
    os.close(slave)
    raw = bytearray()

    def drain():
        while select.select([master], [], [], 0)[0]:
            try: data = os.read(master, 65536)
            except OSError: break
            if not data: break
            raw.extend(data)
            for query, reply in [(b'\x1b[6n', b'\x1b[1;1R'),
                    (b'\x1b[c', b'\x1b[?1;2c'),
                    (b'\x1b]11;?', b'\x1b]11;rgb:0000/0000/0000\x1b\\')]:
                if query in data: os.write(master, reply)
        return re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', raw.decode(errors='replace'))

    def pump(seconds):
        end = time.monotonic()+seconds
        while time.monotonic() < end:
            drain(); time.sleep(.05)

    def wait_file(name, timeout=20):
        end = time.monotonic()+timeout
        while time.monotonic() < end:
            pump(.1)
            path = work/name
            if path.exists():
                try: return json.loads(path.read_text())
                except json.JSONDecodeError: pass
            assert proc.poll() is None, 'source exited unexpectedly'
        raise AssertionError(f'{name} was not produced')

    def type_command(value):
        os.write(master, value.encode()); pump(.25); os.write(master, b'\r')

    def cli(args, name):
        result = subprocess.run([command, *args], cwd=work, env=env,
            text=True, capture_output=True, timeout=30)
        (root/name).write_text(result.stdout+result.stderr)
        assert result.returncode == 0, f'{name} failed; see private diagnostics'
        return result

    try:
        pump(4)
        if 'Yes,Itrustthisfolder' in re.sub(r'\s+', '', drain()):
            os.write(master, b'\x1b[B\r'); pump(3)
        assert proc.poll() is None
        assert not (work/'mod-started.json').exists()
        (root/'source-before-install.raw').write_bytes(raw)
        market = root/'marketplace'
        plugin = market/'plugins/hc-control-probe'
        (market/'.claude-plugin').mkdir(parents=True)
        (plugin/'.claude-plugin').mkdir(parents=True)
        (plugin/'hooks').mkdir()
        (plugin/'.claude-plugin/plugin.json').write_text(json.dumps({
            'name': 'hc-control-probe', 'version': '0.0.1',
            'description': 'Disposable no-inference handoff coordination probe',
            'author': {'name': 'Hardcore research'}}))
        (plugin/'hooks/hooks.json').write_text('{"modules":["./register.js"]}')
        (plugin/'hooks/register.js').write_text(MODULE)
        (market/'.claude-plugin/marketplace.json').write_text(json.dumps({
            'name': 'hc-control-probe-market', 'owner': {'name': 'Hardcore research'},
            'plugins': [{'name': 'hc-control-probe', 'source': './plugins/hc-control-probe'}]}))
        cli(['plugin', 'validate', str(plugin)], 'validate.log')
        cli(['plugin', 'marketplace', 'add', '--scope', 'project', str(market)], 'marketplace.log')
        cli(['plugin', 'install', '--scope', 'project', 'hc-control-probe@hc-control-probe-market'], 'install.log')
        type_command('/reload-plugins')
        started = wait_file('mod-started.json')
        type_command('/hc-snapshot')
        state = wait_file('mod-state.json')
        api = wait_file('mod-api-command.json')
        draft_text = 'HCMOD UNSENT DRAFT: exact text, no Enter'
        os.write(master, draft_text.encode())
        deadline = time.monotonic()+5
        draft = {}
        while time.monotonic() < deadline:
            draft = wait_file('mod-draft.json', timeout=1)
            if draft.get('text') == draft_text: break
            pump(.2)
        assert draft.get('text') == draft_text, 'mod could not read the unsent draft'
        # Clear only our own synthetic draft before testing the /bg guard.
        os.write(master, b'\x15'); pump(.3)
        type_command('/bg')
        guard = wait_file('mod-bg-guard.json')
        pump(1)
        assert started['sessionId'] == state['sessionId'] == guard['sessionId']
        assert proc.poll() is None, 'interposed /bg exited source'
        assert guard['command'] == 'background'
        assert 'HCMODBGBLOCKED' in drain()
        assert api['ok'], api.get('error')
        assert 'HCMODDISPOSABLE' in json.dumps(api['result'])
        result = {'sourcePid': proc.pid, 'sourceStillRunning': True,
            'noLaunchTimePlugin': True, 'noModelPromptSent': True,
            'started': started, 'snapshot': state, 'commandApi': api,
            'backgroundGuard': guard, 'unsentDraftObserved': draft}
        (root/'result.json').write_text(json.dumps(result, indent=2))
        print('PASS mod loaded after plain Claude launch; source PID unchanged')
        print('PASS immediate local command read exact session identity and prompt state')
        print('PASS command API invoked local command; mod intercepted /bg without handoff')
        print('PASS mod read exact unsent draft without submitting it')
        print('LIMITATION no running-tool preservation or PTY transfer tested')
    except BaseException as error:
        (root/'failure.json').write_text(json.dumps({'error': str(error), 'sourcePid': proc.pid}, indent=2))
        raise
    finally:
        (root/'source.raw').write_bytes(raw)
        if proc.poll() is None: proc.kill()
        proc.wait(timeout=10)
        os.close(master)


def main():
    root = Path(tempfile.mkdtemp(prefix='hc-claude-late-mod-', dir='/tmp'))
    try:
        probe(root)
    finally:
        # This also runs if credential copying, PTY creation, or Popen fails.
        (root/'config/.credentials.json').unlink(missing_ok=True)
        (root/'config/.claude.json').unlink(missing_ok=True)
        shutil.rmtree(root/'config/backups', ignore_errors=True)
        print('Private diagnostics:', root)


if __name__ == '__main__':
    main()
