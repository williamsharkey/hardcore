import assert from 'node:assert/strict';
import {test} from 'node:test';
import {createState} from '../hooks/state.js';

const empty = {text: '', cursor: 0};
const stop = {session_id: 'source', transcript_path: '/private/session.jsonl',
  cwd: '/work', background_tasks: [], session_crons: []};
const complete = {turnId: 'turn', reason: 'answer', isAborted: false};

function started() {
  const state = createState();
  state.reset('source');
  state.start('turn');
  return state;
}

function ready() {
  const state = started();
  state.stop(stop, {});
  state.complete(complete);
  assert.equal(state.inspect([], empty).ok, true);
  return state;
}

test('late load, empty agents and empty draft do not prove idle', () => {
  const state = createState();
  state.reset('source');
  assert.equal(state.inspect([], empty).ok, false);
  state.complete(complete);
  assert.equal(state.inspect([], empty).ok, false);
});

test('a main Stop and matching normal turn completion prove settled', () => {
  assert.deepEqual(ready().inspect([], empty), {ok: true, revision: 2,
    sessionId: 'source', transcriptPath: '/private/session.jsonl', cwd: '/work', promptId: null});
});

test('late load during a turn can become settled from Stop and completion', () => {
  const state = createState();
  state.reset('source');
  state.stop(stop, {});
  state.complete(complete);
  assert.equal(state.inspect([], empty).ok, true);
});

for (const [name, event, result] of [
  ['missing background metadata', {...stop, background_tasks: undefined}, {}],
  ['missing scheduled-task metadata', {...stop, session_crons: undefined}, {}],
  ['running shell', {...stop, background_tasks: [{type: 'shell', status: 'running'}]}, {}],
  ['pending agent', {...stop, background_tasks: [{type: 'subagent', status: 'pending'}]}, {}],
  ['scheduled wakeup', {...stop, session_crons: [{id: 'wake'}]}, {}],
  ['another session', {...stop, session_id: 'other'}, {}],
  ['relative transcript', {...stop, transcript_path: 'relative'}, {}],
  ['subagent Stop', {...stop, agent_id: 'worker'}, {}],
  ['blocked stop hook', stop, {block: 'keep working'}],
  ['prevented continuation', stop, {preventContinuation: true}],
]) {
  test(`${name} does not permit handoff`, () => {
    const state = started();
    state.stop(event, result);
    state.complete(complete);
    assert.equal(state.inspect([], empty).ok, false);
  });
}

test('tool result alone cannot imply settled; unresolved tools veto Stop', () => {
  const state = started();
  state.toolStart('bash');
  state.stop(stop, {});
  state.complete(complete);
  assert.equal(state.inspect([], empty).ok, false);
  state.toolFinish('bash', {backgroundTaskId: 'job'});
  assert.equal(state.inspect([], empty).ok, false);
  state.stop(stop, {});
  state.complete(complete);
  assert.equal(state.inspect([], empty).ok, true);
});

test('intervening activity invalidates an observed Stop', () => {
  const state = started();
  state.stop(stop, {});
  state.invalidate('another prompt arrived');
  state.complete(complete);
  assert.equal(state.inspect([], empty).ok, false);
});

test('activity while another Stop hook awaits cannot certify a stale turn', () => {
  const state = started();
  const beforeOtherHooks = state.revision;
  state.invalidate('another prompt was queued');
  state.stop(stop, {}, beforeOtherHooks);
  state.complete(complete);
  assert.equal(state.inspect([], empty).ok, false);
});

test('active, pending and unknown agents refuse; completed agents permit', () => {
  const state = ready();
  for (const status of ['running', 'pending', 'queued', 'new-status', undefined]) {
    assert.equal(state.inspect([{status}], empty).ok, false);
  }
  assert.equal(state.inspect([{status: 'completed'}, {status: 'failed'}], empty).ok, true);
});

test('text drafts can be snapshotted without modifying the buffer', () => {
  const state = ready();
  for (const text of ['unfinished thought', ' ', '\n', '/hardcore-slurp']) {
    const draft = {text, cursor: text.length};
    assert.equal(state.inspect([], draft).ok, true);
    assert.equal(draft.text, text);
  }
  assert.equal(state.inspect([], {text: 'draft', cursor: 2}).ok, true);
});

test('missing buffers and unsupported cursor positions refuse before handoff', () => {
  const state = ready();
  for (const draft of [null, {text: '🐈', cursor: 1}, {text: 'draft', cursor: -1},
    {text: 'draft', cursor: 8}, {text: 'draft'}, {text: '', cursor: null}]) {
    assert.equal(state.inspect([], draft).ok, false);
  }
});

test('aborted, errored, refused and mismatched turns cannot settle', () => {
  for (const alteration of [{reason: 'aborted', isAborted: true}, {reason: 'error'},
    {reason: 'refusal'}, {turnId: 'other'}]) {
    const state = started();
    state.stop(stop, {});
    state.complete({...complete, ...alteration});
    assert.equal(state.inspect([], empty).ok, false);
  }
});

test('new session and new work revoke previously settled state', () => {
  const state = ready();
  state.start('new-turn');
  assert.equal(state.inspect([], empty).ok, false);
  state.reset('new-session');
  assert.equal(state.inspect([], empty).ok, false);
});
