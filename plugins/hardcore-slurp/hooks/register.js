import {createState} from './state.js';
import {captureSnapshot, restoreSnapshot, verifySnapshot, sameDraft} from './snapshot.js';

const DRIVER = 'hardcore-slurp-driver';
const COMMAND = 'hardcore-slurp';
const DISPATCH = 'hardcore-slurp-dispatch';
const REQUEST_ID = /^[a-f0-9]{32}$/;

const state = createState();
let executable = 'hardcore';
let waiting = false;
let working = false;
let pending;
let titleChangedElsewhere = false;
let intent = 0;
let externalIntent;
let polling = false;
let receiving = false;
let composerRevision = 0;
let cursorRestore;

async function broker($, action, payload) {
  const result = await $.process.run([executable, 'claude', action], {
    stdin: JSON.stringify({protocol: 1, ...payload}),
    timeoutMs: action === 'prepare' ? 40000 : ['claim', 'ready', 'guard', 'cursor', 'verify'].includes(action) ? 15000 :
      ['failed', 'release'].includes(action) ? 7000 : 2000,
  });
  let response;
  try { response = JSON.parse(result.stdout); }
  catch {
    throw new Error(result.exitCode !== 0 ? 'Hardcore could not prepare the handoff.' :
      'Hardcore returned an invalid handoff response.');
  }
  if (!response || response.ok !== true) {
    throw new Error(typeof response?.error === 'string' ? response.error : 'Hardcore refused the handoff.');
  }
  if (result.exitCode !== 0) throw new Error('Hardcore could not complete the handoff request.');
  return response;
}

async function inspect($) {
  const revision = state.revision;
  const editing = composerRevision;
  const agents = await $.agent.list();
  const draft = await $.prompt.read();
  const snapshot = state.inspect(agents, draft);
  if (state.revision !== revision || composerRevision !== editing) {
    return {ok: false, reason: 'Claude changed while the handoff was being checked.'};
  }
  return {...snapshot, composerRevision: editing};
}

async function notice($, text) {
  try { await $.ui.log(text); } catch { /* a notice cannot weaken a guard */ }
}

async function cancel($, text) {
  intent++;
  waiting = false;
  const queuedIntent = externalIntent;
  const old = pending;
  if (!old) {
    try {
      await broker($, 'cancel-intent', {...(queuedIntent ? {intentId: queuedIntent} : {}),
        sessionId: await $.session.id(), cwd: await $.session.cwd()});
    } catch (error) {
      await notice($, `Hardcore could not cancel its queued request: ${String(error)}`);
      return false;
    }
  }
  if (old) {
    try { await broker($, 'cancel', {requestId: old.requestId, sourceSessionId: old.sourceSessionId}); }
    catch (error) {
      await notice($, `Hardcore could not cancel this handoff: ${String(error)}`);
      return false;
    }
    if (pending === old) pending = undefined;
    if (old.renamed && !titleChangedElsewhere && typeof old.originalName === 'string' && old.originalName) {
      // Commands cannot be awaited from another command's held event.
      $.clock.after(0, async () => {
        if (titleChangedElsewhere) return;
        try {
          if (await $.session.id() !== old.sourceSessionId) return;
          await $.command.run({command: 'rename', args: old.originalName});
        }
        catch { /* the conversation itself is still usable */ }
      });
    }
  }
  externalIntent = undefined;
  if (text) await notice($, text);
  return true;
}

async function prepare($) {
  if (working || pending) return;
  working = true;
  const startedIntent = intent;
  try {
    let snapshot = await inspect($);
    if (!snapshot.ok) {
      if (!waiting) await notice($, `Hardcore handoff stopped: ${snapshot.reason}`);
      return;
    }
    const commands = await $.command.list();
    if (!commands.some(command => command.name === DISPATCH)) {
      waiting = false;
      await notice($, 'Install and enable hardcore-slurp-driver alongside hardcore-slurp.');
      return;
    }
    const revision = snapshot.revision;
    const editing = snapshot.composerRevision;
    const saved = await captureSnapshot($);
    if (intent !== startedIntent) return;
    if (state.revision !== revision || composerRevision !== editing) {
      throw new Error('Claude changed while its prompt and history were being saved.');
    }
    const response = await broker($, 'prepare', {
      sourceSessionId: snapshot.sessionId, cwd: snapshot.cwd,
      snapshot: saved, ...(externalIntent ? {intentId: externalIntent} : {}),
    });
    if (!REQUEST_ID.test(response.requestId ?? '') || typeof response.marker !== 'string' ||
        !response.marker || response.marker.length > 256 || /[\r\n\0]/.test(response.marker)) {
      throw new Error('Hardcore returned an invalid handoff identity.');
    }
    pending = {...response, sourceSessionId: snapshot.sessionId, revision, composerRevision: editing, snapshot: saved,
      renamed: false, phase: 'prepared'};
    titleChangedElsewhere = false;
    if (intent !== startedIntent) {
      await cancel($);
      return;
    }
    snapshot = await inspect($);
    if (!snapshot.ok || snapshot.revision !== revision || !sameDraft(await $.prompt.read(), saved.draft) ||
        composerRevision !== editing || state.revision !== revision) {
      await cancel($, `Hardcore handoff stopped: ${snapshot.reason ?? 'Claude changed during preparation.'}`);
      return;
    }
    // Set this before awaiting: cancellation may happen while rename waits
    // in Claude's command queue, in which case restoration queues after it.
    pending.renamed = true;
    await $.command.run({command: 'rename', args: response.marker});
    if (!pending || pending.requestId !== response.requestId) return;
    snapshot = await inspect($);
    if (!snapshot.ok || snapshot.revision !== revision || !sameDraft(await $.prompt.read(), saved.draft) ||
        composerRevision !== editing || state.revision !== revision) {
      await cancel($, `Hardcore handoff stopped: ${snapshot.reason ?? 'Claude changed during preparation.'}`);
      return;
    }
    waiting = false;
    // The other plugin is intentional: calls from this plugin's timer skip
    // this plugin's hooks. The distinct driver makes the command traverse our
    // guard at actual dispatch while the two dependent plugins are enabled.
    await $.command.run({command: DISPATCH, args: response.requestId});
  } catch (error) {
    await cancel($, `Hardcore handoff stopped: ${String(error)}`);
  } finally {
    working = false;
  }
}

function later($) {
  $.clock.after(0, async () => {
    if (waiting) await prepare($);
  });
}

async function pollRequest($, sessionId) {
  if (polling || receiving || pending || working || await $.session.id() !== sessionId) return;
  polling = true;
  const startedIntent = intent;
  try {
    const cwd = await $.session.cwd();
    const response = await broker($, 'poll', {sessionId, cwd});
    if (response.action !== 'wait' || !REQUEST_ID.test(response.intentId ?? '')) return;
    if (intent !== startedIntent || await $.session.id() !== sessionId) {
      await broker($, 'cancel-intent', {sessionId, cwd, intentId: response.intentId});
      return;
    }
    if (response.intentId === externalIntent) {
      if (waiting) later($);
      return;
    }
    externalIntent = response.intentId;
    intent++;
    waiting = true;
    await notice($, 'Hardcore requested this session. Waiting for a verified completed turn; visible text drafts transfer without submission.');
    later($);
  } catch { /* An absent or older native helper must not affect Claude. */ }
  finally { polling = false; }
}

function restoreDestination($, startedSessionId, response) {
  let attempts = 0;
  let receiptEmitted = false;
  let restored = false;
  let deadline;
  const revision = state.revision;
  const restore = async () => {
    try {
      const now = await $.clock.now();
      deadline ??= now + 60000;
      if (now >= deadline) throw new Error('The destination verification deadline expired.');
      if (await $.session.id() !== startedSessionId || state.revision !== revision) {
        throw new Error('The receiving session changed.');
      }
      // Native input is held only for this exact request/PTY, with a deadline.
      // Recheck it before every fill so expiry cannot expose a racing editor.
      const guard = await broker($, 'guard', {requestId: response.requestId, sessionId: startedSessionId});
      if (guard.waitingForAttach === true) {
        if (++attempts >= 30) throw new Error('The receiver never finished attaching.');
        $.clock.after(500, restore);
        return;
      }
      if (guard.alreadyVerified === true) {
        if (!restored) throw new Error('A previously verified handoff cannot refill the receiving prompt.');
        // A verify reply can be lost after the native guard was released.
        // Retry only the readback/ack; never write into the unlocked composer.
      } else if (guard.inputHeld !== true) throw new Error('The receiving input guard is not active.');
      const snapshot = await (restored ? verifySnapshot($, response.snapshot) :
        restoreSnapshot($, response.snapshot, async draft => {
          const current = await $.prompt.read();
          if (current.text !== draft.text || current.cursor !== draft.text.length) {
            throw new Error('The receiving prompt changed before cursor restoration.');
          }
          cursorRestore = {requestId: response.requestId, draft};
          try {
            await $.ui.log(`HC-CURSOR:${response.requestId}`);
            await broker($, 'cursor', {requestId: response.requestId, sessionId: startedSessionId});
            for (let attempt = 0; attempt < 20; attempt++) {
              if (sameDraft(await $.prompt.read(), draft)) return;
              await $.clock.sleep(100);
            }
            throw new Error('The receiving prompt cursor did not restore exactly.');
          } finally { cursorRestore = undefined; }
        }));
      if (snapshot) {
        restored = true;
        if (!receiptEmitted) {
          // This travels through Claude's actual terminal stream. Hardcore
          // acknowledges the bound receiver only after it has rendered it.
          await $.ui.log(`HC-RECEIPT:${response.requestId}`);
          receiptEmitted = true;
        }
        try {
          await broker($, 'verify', {requestId: response.requestId, sessionId: startedSessionId, snapshot});
          return;
        } catch (error) {
          // The receiver is released by ready before its client execs attach.
          // Keep the exact draft and retry the bounded attachment barrier.
          if (++attempts >= 30) throw error;
        }
      } else if (++attempts >= 30) throw new Error('The receiving prompt never became available.');
      $.clock.after(500, restore);
    } catch (error) {
      try { await broker($, 'failed', {requestId: response.requestId, sessionId: startedSessionId,
        reason: String(error)}); } catch { /* Native guard also has a deadline. */ }
      await notice($, `Hardcore could not verify the moved prompt and history: ${String(error)} The source window should stay open.`);
    }
  };
  $.clock.after(500, restore);
}

/** @type {import('claude-code').Register} */
export function register(on, options) {
  executable = typeof options.executable === 'string' && options.executable
    ? options.executable : 'hardcore';
  on('session.start', async ($, e, next) => {
    const startedSessionId = await $.session.id();
    state.reset(startedSessionId);
    waiting = false;
    externalIntent = undefined;
    receiving = false;
    composerRevision++;
    cursorRestore = undefined;
    pending = undefined;
    intent++;
    await $.command.register({name: COMMAND,
      description: 'Move this settled Claude session into Hardcore',
      argumentHint: '[wait|cancel|status]', immediate: true});
    // A claimed destination restores its title and saved draft. No match is
    // normal; the private helper holds draft text and a history digest.
    $.clock.after(0, async () => {
      let claimed = false;
      try {
        if (await $.session.id() !== startedSessionId) return;
        const response = await broker($, 'claim', {
          sessionId: startedSessionId, cwd: await $.session.cwd(),
        });
        claimed = true;
        receiving = true;
        if (!REQUEST_ID.test(response.requestId ?? '') ||
            typeof response.originalName !== 'string' || !response.originalName) {
          throw new Error('Hardcore returned an invalid destination claim.');
        }
        // A public background-job row can appear before Claude has mounted its
        // command queue. Awaiting this local command supplies a startup barrier
        // before the receiver is allowed to attach to the new process.
        await $.command.run({command: 'rename', args: response.originalName});
        if (await $.session.id() !== startedSessionId) {
          throw new Error('The destination session changed during startup.');
        }
        await broker($, 'ready', {requestId: response.requestId, sessionId: startedSessionId});
        if (response.snapshot) restoreDestination($, startedSessionId, response);
      } catch (error) {
        // A source with no pending claim is normal; only a claimed destination
        // should report a failure to finish the connection.
        if (claimed) await notice($, `Hardcore could not finish connecting this Claude session: ${String(error)}`);
      }
    });
    $.clock.every(2000, () => pollRequest($, startedSessionId));
    return next(e);
  });

  on('command.run', {command: COMMAND}, async ($, e) => {
    const argument = e.args.trim();
    if (argument === 'cancel') {
      const cancelled = await cancel($);
      return {text: cancelled ? 'Hardcore handoff cancelled.' :
        'The handoff could not be cancelled. Check the Hardcore receiver before trying again.'};
    }
    if (argument && !['wait', 'status'].includes(argument)) {
      return {text: 'Use /hardcore-slurp, /hardcore-slurp wait, /hardcore-slurp status, or /hardcore-slurp cancel.'};
    }
    let snapshot;
    try { snapshot = await inspect($); }
    catch { return {text: 'Hardcore could not read Claude’s current state; no handoff attempted.'}; }
    if (argument === 'status') {
      return {text: pending ? pending.phase === 'dispatched' ?
        'Claude is completing the handoff. If it stays here, use /hardcore-slurp cancel before retrying.' :
        'Hardcore handoff is being prepared.' :
        snapshot.ok ? 'Claude is settled and ready for Hardcore.' :
          `${waiting ? 'Hardcore is waiting. ' : ''}${snapshot.reason}`};
    }
    if (pending || working) return {text: 'Hardcore handoff is already being prepared.'};
    if (!snapshot.ok && argument !== 'wait') return {text: `Hardcore handoff not started: ${snapshot.reason}`};
    intent++;
    waiting = true;
    later($);
    return {text: snapshot.ok ? 'Preparing this session in Hardcore.' :
      `Hardcore will move this session after a verified completed turn with no background work. ${snapshot.reason}`};
  }).catch(() => ({text: 'Hardcore could not verify this session; no handoff attempted.'}));

  on('command.run', {command: 'background'}, async ($, e, next) => {
    const fromDriver = e.origin.kind === 'plugin' && e.origin.name === DRIVER;
    if (!fromDriver && !pending) return next(e);
    // Hook errors normally fail open in Claude. Keep every check in this
    // handler and catch errors before next, so a failed helper cannot run /bg.
    try {
      if (!fromDriver || !pending || e.args !== pending.requestId) {
        return {text: 'Hardcore refused an unprepared background handoff.'};
      }
      const request = pending;
      if (request.phase !== 'prepared') return {text: 'This Hardcore handoff was already dispatched.'};
      let snapshot = await inspect($);
      if (!snapshot.ok || snapshot.revision !== request.revision || snapshot.sessionId !== request.sourceSessionId ||
          !sameDraft(await $.prompt.read(), request.snapshot.draft) || composerRevision !== request.composerRevision ||
          state.revision !== request.revision) {
        await cancel($);
        return {text: `Hardcore handoff stopped: ${snapshot.reason ?? 'Claude changed during preparation.'}`};
      }
      await broker($, 'arm', {requestId: request.requestId, sourceSessionId: request.sourceSessionId});
      snapshot = await inspect($);
      if (pending !== request || !snapshot.ok || snapshot.revision !== request.revision ||
          !sameDraft(await $.prompt.read(), request.snapshot.draft) || composerRevision !== request.composerRevision ||
          state.revision !== request.revision) {
        await cancel($);
        return {text: `Hardcore handoff stopped: ${snapshot.reason ?? 'Claude changed while the receiver was connecting.'}`};
      }
      // Core returns a component reference before its asynchronous handoff
      // exits. Retain the request so a refusal can be cancelled explicitly;
      // do not infer failure from a returned value or dispatch a second time.
      request.phase = 'dispatched';
      return next({...e, args: ''});
    } catch (error) {
      await cancel($);
      return {text: `Hardcore handoff stopped: ${String(error)}`};
    }
  }).catch(($, e, next) => next.called ? next(e) :
    ({text: 'Hardcore could not verify this background handoff; the source session was kept.'}));

  on('command.run', async ($, e, next) => {
    if (![COMMAND, DISPATCH, 'background'].includes(e.command)) {
      if (e.command === 'rename') titleChangedElsewhere = true;
      state.invalidate('Another command changed Claude; wait for its next completed turn.');
    }
    return next(e);
  });
  on('prompt.submit', ($, e, next) => {
    state.invalidate('A new prompt is queued or running.');
    return next(e);
  });
  on('prompt.edit', async ($, e, next) => {
    composerRevision++;
    const target = cursorRestore?.draft;
    if (target && e.origin.kind === 'composer' && e.key?.key === 'left' &&
        !e.key.ctrl && !e.key.meta && !e.key.shift && e.text === target.text &&
        e.cursor === target.text.length && e.inputText === '' && e.start === e.end && e.start < e.cursor) {
      cursorRestore = undefined;
      return {text: target.text, cursor: target.cursor};
    }
    const result = await next(e);
    if (waiting) later($);
    return result;
  });
  on('prompt.fill', async ($, e, next) => {
    composerRevision++;
    const result = await next(e);
    if (waiting) later($);
    return result;
  });
  on('prompt.change', ($, e, next) => {
    // History recall, undo, stash/clear and engine prefills do not all raise
    // prompt.edit. Keep an edit generation so an ABA edit is never missed.
    composerRevision++;
    if (waiting) later($);
    return next(e);
  });
  on('session.receive', ($, e, next) => {
    state.invalidate('A new message is queued or running.');
    return next(e);
  });
  on('turn.start', ($, e, next) => { state.start(e.turnId); return next(e); });
  on('agent.spawn', ($, e, next) => {
    state.invalidate('A new agent is starting.');
    return next(e);
  });
  on('tool.call', async ($, e, next) => {
    state.toolStart(e.tool_use_id);
    try { return await next(e); }
    finally { state.toolFinish(e.tool_use_id); }
  });
  on('classic.PostToolUse', ($, e, next) => {
    state.toolFinish(e.tool_use_id, e.tool_response);
    return next(e);
  });
  on('classic.PermissionRequest', ($, e, next) => {
    state.invalidate('A tool permission request is in progress.');
    return next(e);
  });
  on('classic.Elicitation', ($, e, next) => {
    state.invalidate('A tool is waiting for input.');
    return next(e);
  });
  on('classic.Stop', async ($, e, next) => {
    const revision = state.revision;
    const result = await next(e);
    state.stop(e, result, revision);
    return result;
  });
  on('turn.complete', async ($, e, next) => {
    const result = await next(e);
    state.complete(e);
    if (!e.agentId) later($);
    return result;
  });
  on('session.end', ($, e, next) => {
    state.invalidate('The Claude session ended.');
    waiting = false;
    pending = undefined;
    intent++;
    return next(e);
  });
}
