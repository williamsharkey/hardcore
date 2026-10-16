#!/usr/bin/env python3
"""Opt-in, no-model bootstrap probe on a disposable plain PTY.

Demonstrates late installation needs reload and that Claude's own Ctrl+S stash
can preserve a known nonempty text draft/cursor around that reload. It does not
automate Terminal.app or certify unknown focus, existing stashes, or images.
"""
import argparse
import base64
import json
from pathlib import Path
import shlex
import tempfile

from probe_claude_handoff_guard import Probe


CURSOR_MODULE = r'''
export function register(on) {
  let desired;
  let edits = [];
  let changes = [];
  on('session.start', async ($, e, next) => {
    await $.command.register({name: 'hc-cursor-probe',
      description: 'Disposable prompt cursor restoration probe', immediate: true});
    $.clock.every(150, async () => {
      await $.fs.write('cursor-state.json', JSON.stringify({
        prompt: await $.prompt.read(), desired, edits, changes,
      }));
    });
    return next(e);
  });
  on('command.run', {command: 'hc-cursor-probe'}, async ($, e) => {
    $.clock.after(100, async () => {
      desired = {text: 'alpha 🐈 beta\nsecond line', cursor: 8};
      await $.prompt.fill({text: desired.text});
    });
    return {};
  });
  on('prompt.edit', ($, e, next) => {
    edits.push(e);
    if (edits.length > 20) edits.shift();
    if (desired && e.text === desired.text && e.cursor === desired.text.length &&
        e.inputText === '' && e.start < e.cursor && e.start === e.end) {
      const restored = desired;
      desired = null;
      return restored;
    }
    return next(e);
  });
  on('prompt.change', ($, e, next) => {
    changes.push(e);
    if (changes.length > 20) changes.shift();
    return next(e);
  });
}
'''


def probe_cursor(root, paste=None):
    fixture = Probe(root)
    plugin = root / 'cursor-plugin'
    (plugin / '.claude-plugin').mkdir(parents=True)
    (plugin / 'hooks').mkdir()
    (plugin / '.claude-plugin/plugin.json').write_text(json.dumps({
        'name': 'hc-cursor-probe', 'version': '0.0.1',
        'description': 'Disposable prompt cursor restoration probe',
        'author': {'name': 'Hardcore research'}}))
    (plugin / 'hooks/hooks.json').write_text('{"modules":["./register.js"]}')
    (plugin / 'hooks/register.js').write_text(CURSOR_MODULE)
    wrapper = root / 'claude-wrapper'
    wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(fixture.command) +
        ' --plugin-dir ' + shlex.quote(str(plugin)) + ' "$@"\n')
    wrapper.chmod(0o700)
    fixture.command = str(wrapper)
    try:
        fixture.start()
        state_path = fixture.work / 'cursor-state.json'
        if paste:
            fixture.wait_json(state_path, lambda value: value['prompt']['text'] == '')
            if paste == 'text':
                content = '\n'.join(f'Line {index}: synthetic text for collapsed-paste preservation.' for index in range(120))
            else:
                picture = fixture.work / 'pixel.png'
                picture.write_bytes(base64.b64decode(
                    'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+yXioAAAAASUVORK5CYII='))
                content = str(picture)
            fixture.send(b'\x1b[200~' + content.encode() + b'\x1b[201~')
            fixture.pump(3)
            after = json.loads(state_path.read_text())
            result = {'pasteKind': paste, 'input': content, 'after': after,
                      'sourceAlive': fixture.proc.poll() is None, 'modelPromptsSent': 0}
            (root / 'result.json').write_text(json.dumps(result, indent=2))
            print('Observed paste composer:', json.dumps(after['prompt'], ensure_ascii=False))
            return result
        fixture.command_line('/hc-cursor-probe')
        before = fixture.wait_json(state_path, lambda value:
            value.get('desired') is not None and value['prompt']['text'].startswith('alpha'))
        fixture.send(b'\x1b[D')
        after = fixture.wait_json(state_path, lambda value:
            value.get('desired') is None and value['prompt']['cursor'] == 8)
        assert after['prompt']['text'] == before['prompt']['text']
        assert fixture.proc.poll() is None
        result = {'before': before, 'after': after, 'sourceAlive': True,
                  'modelPromptsSent': 0, 'noTextInserted': True}
        (root / 'result.json').write_text(json.dumps(result, indent=2))
        print('PASS one cursor-only edit restored the exact Unicode middle cursor without changing text')
        return result
    finally:
        fixture.close()


def probe(root):
    fixture = Probe(root)
    try:
        fixture.start()
        draft = 'HCPROBE draft 🐈 with cursor in the middle'
        fixture.send(draft.encode())
        fixture.pump(.4)
        fixture.send(b'\x1b[D' * 7)
        fixture.pump(.4)
        fixture.observer()
        fixture.pump(10)
        state_path = fixture.work / 'handoff-probe-state.json'
        assert not state_path.exists(), 'This Claude build auto-loads late plugins; revise the bootstrap assumption'
        fixture.send(b'\x13')  # Documented chat:stash; source focus is known in this fixture.
        fixture.pump(.4)
        fixture.command_line('/reload-plugins')
        expected = {'text': draft, 'cursor': len(draft.encode('utf-16-le')) // 2 - 7}
        after = fixture.wait_json(state_path, lambda value: value['prompt'] == expected)
        assert fixture.proc.poll() is None
        result = {'sourcePid': fixture.proc.pid, 'sourceAlive': True,
                  'installedLoadedWithoutReload': False,
                  'stashAutomaticallyRestored': True, 'prompt': after['prompt'],
                  'sessionId': after['sessionId'], 'modelPromptsSent': 0,
                  'limitations': ['Known composer focus', 'No existing stash', 'Text draft only',
                                  'No Terminal.app automation']}
        (root / 'result.json').write_text(json.dumps(result, indent=2))
        print('PASS late install needed reload; stash preserved exact Unicode draft and middle cursor')
        return result
    finally:
        fixture.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--cursor', action='store_true',
        help='Probe supported prompt.edit cursor restoration instead of plugin bootstrap')
    parser.add_argument('--paste', choices=['text', 'image'],
        help='Inspect public composer visibility of a synthetic large-text or image-file paste')
    args = parser.parse_args()
    root = args.output or Path(tempfile.mkdtemp(prefix='hc-claude-bootstrap-', dir='/tmp'))
    root.mkdir(mode=0o700, exist_ok=True)
    root.chmod(0o700)
    print(root, flush=True)
    if args.paste:
        probe_cursor(root, args.paste)
    else:
        (probe_cursor if args.cursor else probe)(root)


if __name__ == '__main__':
    main()
