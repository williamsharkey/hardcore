// Pure, deliberately conservative state tracking. Neither a quiet screen nor
// the absence of observed events proves that a late-loaded session is idle.
import {draftProblem} from './snapshot.js';

export function createState() {
  let revision = 0;
  let sessionId;
  let turnId;
  let stopped;
  let settled = false;
  let reason = 'Wait for Claude to finish a turn after this plugin loads.';
  const tools = new Set();
  const background = new Map();

  function invalidate(message) {
    revision++;
    settled = false;
    stopped = undefined;
    reason = message;
  }

  return {
    reset(id) {
      sessionId = id;
      turnId = undefined;
      tools.clear();
      background.clear();
      invalidate('Wait for Claude to finish a turn after this plugin loads.');
    },
    invalidate,
    start(id) {
      turnId = id;
      invalidate('Claude is still working.');
    },
    toolStart(id) {
      tools.add(id);
      invalidate('A tool call or its permission request is still in progress.');
    },
    toolFinish(id, response) {
      tools.delete(id);
      // This data is diagnostic only. Stop's explicit background_tasks list is
      // authoritative; a PostToolUse result alone never proves quiescence.
      if (response && typeof response === 'object') {
        const task = response.backgroundTaskId ?? response.task_id;
        if (typeof task === 'string') background.set(task, id);
      }
    },
    stop(event, result, observedRevision = revision) {
      if (event.agent_id) return;
      settled = false;
      stopped = undefined;
      if (observedRevision !== revision) {
        reason = 'Claude changed while its stop hooks were running.';
        return;
      }
      if (event.session_id !== sessionId) {
        invalidate('The Claude session changed; wait for its next completed turn.');
        return;
      }
      if (result?.block || result?.preventContinuation) {
        reason = 'Another hook prevented Claude from settling.';
        return;
      }
      if (!Array.isArray(event.background_tasks) || !Array.isArray(event.session_crons)) {
        reason = 'This Claude version did not report its background work and scheduled tasks.';
        return;
      }
      if (event.background_tasks.length || event.session_crons.length) {
        reason = 'Background work or scheduled tasks are still registered with Claude.';
        return;
      }
      if (tools.size) {
        reason = 'A tool call has not reported completion.';
        return;
      }
      if (typeof event.transcript_path !== 'string' || !event.transcript_path.startsWith('/') ||
          typeof event.cwd !== 'string' || !event.cwd.startsWith('/')) {
        reason = 'Claude did not provide an absolute transcript path and working directory.';
        return;
      }
      stopped = {
        revision, sessionId, transcriptPath: event.transcript_path, cwd: event.cwd,
        promptId: event.prompt_id ?? null,
      };
      reason = 'Waiting for Claude to finish its stop hooks.';
    },
    complete(event) {
      if (event.agentId) return;
      if (turnId !== undefined && event.turnId !== turnId) return;
      turnId = undefined;
      if (event.reason !== 'answer' || event.isAborted) {
        invalidate('The last turn did not finish normally; wait for a completed turn.');
        return;
      }
      if (stopped && stopped.revision === revision && !tools.size) {
        settled = true;
        background.clear();
        reason = '';
      }
    },
    inspect(agents, draft) {
      if (!settled || !stopped) return {ok: false, revision, reason};
      if (!Array.isArray(agents)) {
        return {ok: false, revision, reason: 'Claude did not provide its agent list.'};
      }
      // Unknown/new statuses fail closed, including queued and pending agents.
      if (agents.some(agent => !['completed', 'failed', 'killed'].includes(agent.status))) {
        return {ok: false, revision, reason: 'An agent is still active or has an unknown status.'};
      }
      const problem = draftProblem(draft);
      if (problem) return {ok: false, revision, reason: problem};
      return {ok: true, ...stopped};
    },
    get revision() { return revision; },
  };
}
