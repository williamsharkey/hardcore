/* Exercise the actual embedded player's bridge UI without a browser or devices. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const elements = new Map(), listeners = new Map(), sent = [], colors = new Map();
function element(id) {
  if (!elements.has(id)) elements.set(id, {
    hidden: false, value: '', textContent: '', classes: new Set(), handlers: new Map(),
    style: {setProperty(name, value) { colors.set(name, value); }},
    classList: {add(name) { element(id).classes.add(name); }, remove(name) { element(id).classes.delete(name); },
      toggle(name, on) { if (on) element(id).classes.add(name); else element(id).classes.delete(name); }},
    addEventListener(name, fn) { this.handlers.set(name, fn); },
    removeAttribute(name) { delete this[name]; },
    getContext() { return {}; }, focus() {}, showModal() { this.open = true; }, close() { this.open = false; },
    click() { if (this.onclick) this.onclick({preventDefault() {}}); }
  });
  return elements.get(id);
}
const document = {getElementById: element, body: element('body'), documentElement: element('html'),
  addEventListener(name, fn) { listeners.set(name, fn); }};
const window = {HardcoreReplay: require('../recording/core.js'),
  webkit: {messageHandlers: {hardcoreRecording: {postMessage(value) { sent.push(value); }}}}, addEventListener() {}};
vm.runInNewContext(fs.readFileSync(require.resolve('../recording/player.js'), 'utf8'), {window, document, console});
const player = window.hardcoreRecordingPlayer;
assert.ok(element('body').classes.has('embedded'));
element('open-button').click(); assert.equal(sent.at(-1).action, 'open');
element('files-button').click(); assert.equal(sent.at(-1).action, 'files');
player.hostTheme({bg: '#123abc', text: 'url(https://bad.test)', accent: '#aabbcc;position:fixed', surface: '#001122'});
assert.equal(colors.get('--bg'), '#123abc'); assert.equal(colors.get('--surface'), '#001122');
assert.equal(colors.has('--text'), false); assert.equal(colors.has('--accent'), false);
player.hostStatus(null); player.hostStatus({message: {evil: 'value'}});
player.hostStatus({id: 'job1', kind: 'mp4', state: 'running', progress: .25, message: 'Exporting 1/4'});
assert.equal(element('job-progress').value, .25); assert.equal(element('job-progress').hidden, false);
assert.equal(element('job-output').hidden, true);
player.hostStatus({id: 'job1', kind: 'mp4', state: 'running', progress: Infinity, message: 'Working'});
assert.equal('value' in element('job-progress'), false, 'unknown progress remains indeterminate');
player.hostStatus({id: 'job1', kind: 'mp4', state: 'running', progress: 100, message: 'Finishing'});
assert.equal(element('job-progress').value, 1, 'progress is bounded');
player.hostStatus({id: 'job1', kind: 'mp4', state: 'done', progress: 1, message: '<script>literal export title</script>', output: '/not/sent/to/native.mp4'});
assert.equal(element('job-message').textContent, '<script>literal export title</script>');
assert.equal(element('job-output').hidden, false); assert.equal(element('job-progress').hidden, true);
element('job-output').click(); assert.equal(sent.at(-1).id, 'job1');
assert.equal(sent.at(-1).action, 'output'); assert.equal(Object.hasOwn(sent.at(-1), 'output'), false, 'only job identity crosses back, not a path');
player.hostStatus({id: 'job2', kind: 'zip', state: 'done', message: 'ZIP ready'});
assert.equal(element('job-output').textContent, 'Show ZIP in Files');
player.hostStatus({state: 'failed', message: 'Invalid crop'});
assert.equal(element('status').textContent, 'Invalid crop'); assert.ok(element('status').classes.has('error'));
element('captions-import').click(); assert.equal(element('captions-dialog').open, true);
assert.equal(sent.filter(value => value.action === 'captionPicker').length, 0, 'caption paste needs no native dialog');
(async () => {
  await listeners.get('drop')({preventDefault() {}, dataTransfer: {files: [], items: []}});
  assert.match(element('status').textContent, /Open another recording from Clips/);
  assert.equal(sent.filter(value => value.action === 'export').length, 0, 'drop cannot replace a privileged replay with another bundle');
  console.log('PASS embedded recording host: bounded progress, literal output, job-id boundary, palette, inline captions, bound document');
})().catch(error => { console.error(error); process.exitCode = 1; });
