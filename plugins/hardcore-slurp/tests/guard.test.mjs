import assert from 'node:assert/strict';
import {test} from 'node:test';
import {captureSnapshot} from '../hooks/snapshot.js';

let instance = 0;
const requestId = '0123456789abcdef0123456789abcdef';

async function engine(initial = {}) {
  const hooks = [];
  const registered = new Map();
  const timers = [];
  const repeating = [];
  const calls = [];
  const notices = [];
  const responses = [];
  const sequence = [];
  let sessionId = initial.sessionId ?? 'source';
  let draft = initial.draft ?? {text: '', cursor: 0};
  let agents = [];
  let backgroundCount = 0;
  let prepareAction;
  let armAction;
  let pollAction;
  let failArm = false;
  let brokerError;
  let cancelError;
  let fills = 0;
  let readAction;
  let guardCalls = 0;
  let verifyCalls = 0;

  async function dispatch(event, input, origin) {
    if (event === 'prompt.change') draft = {text: input.text, cursor: input.cursor};
    const selected = hooks.filter(hook => hook.event === event && hook.plugin !== origin &&
      (!hook.matcher || Object.entries(hook.matcher).every(([key, value]) => input[key] === value)));
    async function invoke(index, current) {
      const hook = selected[index];
      if (!hook) {
        if (event === 'prompt.edit') {
          draft = {text: current.text.slice(0, current.start) + current.inputText +
            current.text.slice(current.end), cursor: current.start + current.inputText.length};
          return draft;
        }
        if (event === 'command.run' && current.command === 'background') {
          assert.equal(current.args, '', 'guard must strip the internal request ID');
          backgroundCount++;
        }
        return {};
      }
      const next = value => invoke(index + 1, value);
      try { return await hook.handler(hook.api, current, next); }
      catch (error) {
        if (hook.catcher) return hook.catcher(hook.api, current, Object.assign(next, {error, called: false}));
        return next(current); // Claude skips failed hooks.
      }
    }
    const result = await invoke(0, input);
    if (event === 'command.run') responses.push({command: input.command, ...result});
    return result;
  }

  for (const plugin of ['hardcore-slurp', 'hardcore-slurp-driver']) {
    const api = {
      plugin: {name: plugin},
      session: {id: async () => sessionId, cwd: async () => '/work', messages: async () => initial.messages ?? []},
      agent: {list: async () => agents},
      prompt: {read: async () => {
        if (readAction) await readAction();
        return draft;
      }, fill: async ({text}) => {
        fills++;
        if (initial.fillRefused) return {isFilled: false, ...draft};
        draft = {text, cursor: text.length};
        return {isFilled: true, ...draft};
      }},
      ui: {log: async text => notices.push(text)},
      clock: {after: (ms, fn) => { timers.push(fn); return {cancel() {}}; },
        every: (ms, fn) => { repeating.push(fn); return {cancel() {}}; }, sleep: async () => {}, now: async () => Date.now()},
      command: {
        register: async command => registered.set(command.name, command),
        list: async () => [...registered.values()],
        run: async ({command, args = ''}) => {
          sequence.push({command, args});
          if (command === 'rename' && initial.renameError) throw new Error(initial.renameError);
          const result = await dispatch('command.run',
            {command, args, origin: {kind: 'plugin', name: plugin}}, plugin);
          if (command === 'rename' && initial.renameSessionId) sessionId = initial.renameSessionId;
          sequence.push({completed: command});
          return result;
        },
      },
      process: {run: async (argv, init) => {
        const payload = JSON.parse(init.stdin);
        const action = argv.at(-1);
        calls.push({action, payload});
        sequence.push({action, payload, timeoutMs: init.timeoutMs});
        if (action === 'claim') return {exitCode: 0,
          stdout: JSON.stringify(initial.claim ?? {ok: false})};
        if (action === 'poll') {
          if (pollAction) await pollAction();
          return {exitCode: 0, stdout: JSON.stringify(initial.poll ?? {ok: true})};
        }
        if (action === 'ready' && initial.readyError) return {exitCode: 1,
          stdout: JSON.stringify({ok: false, error: initial.readyError})};
        if (action === 'guard' && initial.guardError) return {exitCode: 1,
          stdout: JSON.stringify({ok: false, error: initial.guardError})};
        if (action === 'guard') {
          const value = initial.guardResponses?.[guardCalls++] ?? {ok: true, inputHeld: true};
          return {exitCode: 0, stdout: JSON.stringify(value)};
        }
        if (action === 'verify' && verifyCalls++ < (initial.verifyFailures ?? 0)) {
          throw new Error('Verification response was lost after native release');
        }
        if (action === 'cursor') {
          draft = await dispatch('prompt.edit', {origin: {kind: 'composer'}, key: {key: 'left'},
            text: draft.text, cursor: draft.cursor, start: draft.cursor - 1,
            end: draft.cursor - 1, inputText: ''});
        }
        if (action === 'prepare' && prepareAction) await prepareAction();
        if (action === 'arm' && armAction) await armAction();
        if (action === 'arm' && failArm) throw new Error('receiver disconnected');
        if (action === 'arm' && brokerError) return {exitCode: 1,
          stdout: JSON.stringify({ok: false, error: brokerError})};
        if (action === 'cancel' && cancelError) return {exitCode: 1,
          stdout: JSON.stringify({ok: false, error: cancelError})};
        return {exitCode: 0, stdout: JSON.stringify({ok: true, requestId,
          marker: 'hardcore-transfer-marker', originalName: 'Source title'})};
      }},
    };
    const on = (event, matcher, handler) => {
      if (typeof matcher === 'function') [handler, matcher] = [matcher, undefined];
      const hook = {plugin, event, matcher, handler, api};
      hooks.push(hook);
      return {catch: catcher => { hook.catcher = catcher; }};
    };
    const path = plugin === 'hardcore-slurp' ? '../hooks/register.js' : '../../hardcore-slurp-driver/hooks/register.js';
    const module = await import(`${path}?test=${++instance}`);
    module.register(on, {executable: '/trusted/hardcore'});
  }

  async function flush() {
    let limit = 40;
    while (timers.length) {
      assert.ok(limit-- > 0, 'unexpected repeated scheduling');
      await timers.shift()();
    }
  }
  await dispatch('session.start', {cwd: '/work', isInteractive: true, surface: 'terminal'});
  await flush();
  async function settle() {
    await dispatch('turn.start', {turnId: 'turn'});
    await dispatch('classic.Stop', {session_id: 'source', transcript_path: '/private/source.jsonl',
      cwd: '/work', background_tasks: [], session_crons: []});
    await dispatch('turn.complete', {turnId: 'turn', reason: 'answer', isAborted: false});
  }
  return {
    dispatch, settle, flush, calls, notices, responses, sequence,
    poll: async () => { for (const callback of repeating) await callback(); },
    readDraft: () => draft,
    get fills() { return fills; },
    command: (command, args = '') => dispatch('command.run', {command, args, origin: {kind: 'composer'}}),
    guardDispatch: (token = requestId) => dispatch('command.run', {
      command: 'hardcore-slurp-dispatch', args: token,
      origin: {kind: 'plugin', name: 'hardcore-slurp'},
    }, 'hardcore-slurp'),
    get backgroundCount() { return backgroundCount; },
    set draft(value) { draft = {text: value, cursor: value.length}; },
    set agents(value) { agents = value; },
    set prepareAction(value) { prepareAction = value; },
    set armAction(value) { armAction = value; },
    set pollAction(value) { pollAction = value; },
    set readAction(value) { readAction = value; },
    set failArm(value) { failArm = value; },
    set brokerError(value) { brokerError = value; },
    set cancelError(value) { cancelError = value; },
  };
}

test('late-loaded source refuses without contacting prepare', async () => {
  const e = await engine();
  assert.match((await e.command('hardcore-slurp')).text, /not started/);
  await e.flush();
  assert.equal(e.calls.some(call => call.action === 'prepare'), false);
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'ready'), false);
  assert.equal(e.notices.length, 0, 'an ordinary source without a claim is quiet');
});

test('claimed destination reports ready only after its rename command completed', async () => {
  const e = await engine({sessionId: 'destination', claim: {
    ok: true, requestId, originalName: 'Original conversation',
  }});
  const readyIndex = e.sequence.findIndex(item => item.action === 'ready');
  const renameIndex = e.sequence.findIndex(item => item.completed === 'rename');
  assert.ok(renameIndex >= 0 && readyIndex > renameIndex);
  assert.deepEqual(e.sequence[readyIndex].payload,
    {protocol: 1, requestId, sessionId: 'destination'});
  assert.equal(e.sequence[readyIndex].timeoutMs, 15000);
  assert.equal(e.notices.length, 0);
});

test('failed destination rename does not release the receiver', async () => {
  const e = await engine({sessionId: 'destination', renameError: 'command queue unavailable',
    claim: {ok: true, requestId, originalName: 'Original conversation'}});
  assert.equal(e.calls.some(call => call.action === 'ready'), false);
  assert.equal(e.notices.some(text => text.includes('command queue unavailable')), true);
});

test('a changed destination session cannot acknowledge the old request as ready', async () => {
  const e = await engine({sessionId: 'destination', renameSessionId: 'different-session',
    claim: {ok: true, requestId, originalName: 'Original conversation'}});
  assert.equal(e.calls.some(call => call.action === 'ready'), false);
  assert.equal(e.notices.some(text => text.includes('destination session changed')), true);
});

test('destination ready refusal reports the native reason only after a matched claim', async () => {
  const e = await engine({sessionId: 'destination', readyError: 'The destination process is not running.',
    claim: {ok: true, requestId, originalName: 'Original conversation'}});
  assert.equal(e.calls.some(call => call.action === 'ready'), true);
  assert.equal(e.notices.some(text => text.includes('The destination process is not running.')), true);
});

test('driver cannot background without a guarded pending request', async () => {
  const e = await engine();
  await e.guardDispatch();
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'arm'), false);
});

test('typed and malformed internal dispatcher commands never schedule background', async () => {
  const e = await engine();
  assert.match((await e.command('hardcore-slurp-dispatch', requestId)).text, /Start handoff/);
  assert.match((await e.guardDispatch('not-a-token')).text, /Start handoff/);
  await e.flush();
  assert.equal(e.backgroundCount, 0);
});

test('settled source prepares, arms and passes through the distinct guard once', async () => {
  const e = await engine();
  await e.settle();
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 1);
  assert.deepEqual(e.calls.filter(call => call.action !== 'claim').map(call => call.action), ['prepare', 'arm']);
  await e.guardDispatch();
  await e.flush();
  assert.equal(e.backgroundCount, 1, 'a replayed token cannot hand off twice');
});

test('wait arms without inference and moves only after a verified completed turn', async () => {
  const e = await engine();
  await e.command('hardcore-slurp', 'wait');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  await e.settle();
  await e.flush();
  assert.equal(e.backgroundCount, 1);
});

test('cancelling wait prevents later handoff', async () => {
  const e = await engine();
  await e.command('hardcore-slurp', 'wait');
  await e.command('hardcore-slurp', 'cancel');
  await e.settle();
  await e.flush();
  assert.equal(e.backgroundCount, 0);
});

test('an end-cursor draft is saved without submitting or clearing it', async () => {
  const e = await engine();
  await e.settle();
  e.draft = 'unfinished thought';
  await e.command('hardcore-slurp', 'wait');
  await e.flush();
  assert.equal(e.backgroundCount, 1);
  assert.deepEqual(e.calls.find(call => call.action === 'prepare').payload.snapshot.draft,
    {text: 'unfinished thought', cursor: 18});
  assert.deepEqual(e.readDraft(), {text: 'unfinished thought', cursor: 18});
});

test('a settled source with an image or collapsed paste never prepares a receiver', async () => {
  for (const text of ['[Image #1]', '[Pasted text #1 +119 lines]']) {
    const e = await engine();
    await e.settle();
    e.draft = text;
    assert.match((await e.command('hardcore-slurp')).text, /image or collapsed paste/);
    await e.flush();
    assert.equal(e.calls.some(call => call.action === 'prepare'), false);
    assert.equal(e.backgroundCount, 0);
    assert.equal(e.readDraft().text, text);
  }
});

test('native queue triggers a guarded handoff without a composer command', async () => {
  const e = await engine({poll: {ok: true, action: 'wait', intentId: requestId}});
  await e.settle();
  e.draft = 'leave my text here';
  await e.poll();
  await e.flush();
  assert.equal(e.backgroundCount, 1);
  const request = e.calls.find(call => call.action === 'prepare').payload;
  assert.equal(request.intentId, requestId);
  assert.equal(request.snapshot.draft.text, 'leave my text here');
  assert.equal(e.responses.some(response => response.command === 'hardcore-slurp'), false);
  assert.equal(e.readDraft().text, 'leave my text here');
});

test('native queue waits through unknown state without touching a draft', async () => {
  const e = await engine({poll: {ok: true, action: 'wait', intentId: requestId}});
  e.draft = 'not submitted';
  await e.poll();
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.readDraft().text, 'not submitted');
  await e.settle();
  await e.flush();
  assert.equal(e.backgroundCount, 1);
});

test('cancelling an external wait cancels its native intent', async () => {
  const e = await engine({poll: {ok: true, action: 'wait', intentId: requestId}});
  await e.poll();
  await e.flush();
  await e.command('hardcore-slurp', 'cancel');
  assert.deepEqual(e.calls.find(call => call.action === 'cancel-intent').payload,
    {protocol: 1, intentId: requestId, sessionId: 'source', cwd: '/work'});
  await e.settle();
  await e.flush();
  assert.equal(e.backgroundCount, 0);
});

test('cancellation during an outstanding poll cannot resurrect its request', async () => {
  const e = await engine({poll: {ok: true, action: 'wait', intentId: requestId}});
  await e.settle();
  e.pollAction = () => e.command('hardcore-slurp', 'cancel');
  await e.poll();
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'prepare'), false);
  const cancels = e.calls.filter(call => call.action === 'cancel-intent');
  assert.equal(cancels.length, 2);
  assert.equal(cancels[0].payload.intentId, undefined);
  assert.equal(cancels[1].payload.intentId, requestId);
});

test('destination verifies exact restored draft and message projection', async () => {
  const snapshot = await captureSnapshot({prompt: {read: async () => ({text: 'keep draft', cursor: 10})},
    session: {messages: async () => []}});
  const e = await engine({sessionId: 'destination', claim: {
    ok: true, requestId, originalName: 'Original conversation', snapshot,
  }});
  const verification = e.calls.find(call => call.action === 'verify');
  assert.deepEqual(verification.payload.snapshot, snapshot);
  assert.deepEqual(e.readDraft(), snapshot.draft);
});

test('destination with unavailable composer never claims verified restoration', async () => {
  const snapshot = await captureSnapshot({prompt: {read: async () => ({text: '', cursor: 0})},
    session: {messages: async () => []}});
  const e = await engine({sessionId: 'destination', fillRefused: true, claim: {
    ok: true, requestId, originalName: 'Original conversation', snapshot,
  }});
  assert.equal(e.calls.some(call => call.action === 'verify'), false);
  assert.equal(e.notices.some(text => text.includes('never became available')), true);
});

test('destination restores middle cursor through one guarded cursor-only edit', async () => {
  const snapshot = await captureSnapshot({prompt: {read: async () => ({text: 'alpha 🐈 beta\nsecond line', cursor: 8})},
    session: {messages: async () => []}});
  const e = await engine({sessionId: 'destination', claim: {
    ok: true, requestId, originalName: 'Original conversation', snapshot,
  }});
  assert.deepEqual(e.readDraft(), snapshot.draft);
  assert.equal(e.calls.filter(call => call.action === 'cursor').length, 1);
  assert.deepEqual(e.calls.find(call => call.action === 'verify').payload.snapshot, snapshot);
  assert.ok(e.notices.indexOf(`HC-CURSOR:${requestId}`) < e.notices.indexOf(`HC-RECEIPT:${requestId}`));
});

test('expired native input guard prevents all destination fills and releases failure', async () => {
  const snapshot = await captureSnapshot({prompt: {read: async () => ({text: 'saved', cursor: 5})},
    session: {messages: async () => []}});
  const e = await engine({sessionId: 'destination', guardError: 'Input guard expired', claim: {
    ok: true, requestId, originalName: 'Original conversation', snapshot,
  }});
  assert.equal(e.fills, 0);
  assert.equal(e.calls.some(call => call.action === 'verify'), false);
  assert.equal(e.calls.some(call => call.action === 'failed'), true);
});

test('lost verify reply retries acknowledgement without filling an unlocked composer', async () => {
  const snapshot = await captureSnapshot({prompt: {read: async () => ({text: 'saved', cursor: 5})},
    session: {messages: async () => []}});
  const e = await engine({sessionId: 'destination', verifyFailures: 1,
    guardResponses: [{ok: true, inputHeld: true}, {ok: true, alreadyVerified: true, inputReleased: true}],
    claim: {ok: true, requestId, originalName: 'Original conversation', snapshot}});
  assert.equal(e.fills, 1);
  assert.equal(e.calls.filter(call => call.action === 'verify').length, 2);
  assert.equal(e.calls.some(call => call.action === 'failed'), false);
  assert.equal(e.notices.filter(text => text === `HC-RECEIPT:${requestId}`).length, 1);
});

test('already-verified receipt never authorizes a fresh fill', async () => {
  const snapshot = await captureSnapshot({prompt: {read: async () => ({text: 'saved', cursor: 5})},
    session: {messages: async () => []}});
  const e = await engine({sessionId: 'destination',
    guardResponses: [{ok: true, alreadyVerified: true, inputReleased: true}],
    claim: {ok: true, requestId, originalName: 'Original conversation', snapshot}});
  assert.equal(e.fills, 0);
  assert.equal(e.calls.some(call => call.action === 'verify'), false);
  assert.equal(e.calls.some(call => call.action === 'failed'), true);
});

for (const when of ['prepareAction', 'armAction']) {
  test(`new work during ${when} cancels instead of invoking background`, async () => {
    const e = await engine();
    await e.settle();
    e[when] = () => e.dispatch('turn.start', {turnId: 'new-turn'});
    await e.command('hardcore-slurp');
    await e.flush();
    assert.equal(e.backgroundCount, 0);
    assert.equal(e.calls.some(call => call.action === 'cancel'), true);
  });
}

test('an agent spawned by a timer after Stop revokes readiness', async () => {
  const e = await engine();
  await e.settle();
  e.prepareAction = () => e.dispatch('agent.spawn', {tool_use_id: 'timer-agent'});
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'cancel'), true);
});

test('new draft after preparation and before driver dispatch refuses', async () => {
  const e = await engine();
  await e.settle();
  e.armAction = async () => { e.draft = 'do not lose this'; };
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'cancel'), true);
});

for (const phase of ['prepareAction', 'armAction']) {
  test(`an edit changed back to the original draft during ${phase} still cancels`, async () => {
    const e = await engine();
    await e.settle();
    e[phase] = async () => {
      await e.dispatch('prompt.edit', {text: '', start: 0, end: 0, inputText: 'x', cursor: 0,
        origin: {kind: 'composer'}});
      await e.dispatch('prompt.edit', {text: 'x', start: 0, end: 1, inputText: '', cursor: 1,
        origin: {kind: 'composer'}});
    };
    await e.command('hardcore-slurp');
    await e.flush();
    assert.equal(e.backgroundCount, 0);
    assert.equal(e.calls.some(call => call.action === 'cancel'), true);
    assert.equal(e.readDraft().text, '');
  });
}

test('history/stash/undo changes during preparation invalidate its draft snapshot', async () => {
  const e = await engine();
  await e.settle();
  e.prepareAction = async () => {
    await e.dispatch('prompt.change', {text: 'recalled', cursor: 8, origin: {kind: 'history'}});
    await e.dispatch('prompt.change', {text: '', cursor: 0, origin: {kind: 'clear'}});
  };
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'cancel'), true);
});

test('a new turn arriving during the final draft read cannot pass a stale state check', async () => {
  const e = await engine();
  await e.settle();
  e.armAction = () => {
    let reads = 0;
    e.readAction = async () => {
      if (++reads === 2) await e.dispatch('turn.start', {turnId: 'new-turn'});
    };
  };
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'cancel'), true);
});

test('failed native arm fails closed', async () => {
  const e = await engine();
  await e.settle();
  e.failArm = true;
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'cancel'), true);
});

test('cancel while native prepare waits prevents late completion from moving', async () => {
  const e = await engine();
  await e.settle();
  e.prepareAction = () => e.command('hardcore-slurp', 'cancel');
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.calls.some(call => call.action === 'arm'), false);
  assert.equal(e.calls.some(call => call.action === 'cancel'), true);
});

test('native JSON refusal keeps its useful reason even when the helper exits 1', async () => {
  const e = await engine();
  await e.settle();
  const reason = 'The prepared Hardcore receiver is no longer open.';
  e.brokerError = reason;
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 0);
  assert.equal(e.responses.some(response => response.command === 'background' &&
    response.text?.includes(reason)), true);
  assert.equal(e.calls.some(call => call.action === 'cancel'), true);
});

test('a core return retains dispatched status until source exits or cancellation succeeds', async () => {
  const e = await engine();
  await e.settle();
  await e.command('hardcore-slurp');
  await e.flush();
  assert.equal(e.backgroundCount, 1);
  assert.match((await e.command('hardcore-slurp', 'status')).text, /completing the handoff/);
  await e.guardDispatch();
  await e.flush();
  assert.equal(e.backgroundCount, 1);
  assert.match((await e.command('hardcore-slurp', 'cancel')).text, /cancelled/);
  await e.flush();
  assert.match((await e.command('hardcore-slurp', 'status')).text, /settled and ready/);
});

test('a cancellation refused after attachment preserves the request and title', async () => {
  const e = await engine();
  await e.settle();
  await e.command('hardcore-slurp');
  await e.flush();
  const renames = e.responses.filter(response => response.command === 'rename').length;
  e.cancelError = 'The destination has already attached.';
  assert.match((await e.command('hardcore-slurp', 'cancel')).text, /could not be cancelled/);
  await e.flush();
  assert.match((await e.command('hardcore-slurp', 'status')).text, /completing the handoff/);
  assert.equal(e.responses.filter(response => response.command === 'rename').length, renames);
});

test('ordinary user /bg remains unchanged when no handoff is pending', async () => {
  const e = await engine();
  await e.command('background');
  assert.equal(e.backgroundCount, 1);
});
