// Claude 2.1.300 exposes its current conversation in Messages API form, including
// image/document blocks. The normalized context is capped at 4096 messages and
// excludes the system prompt, tools and older compacted-away history.
export function draftProblem(draft) {
  if (!draft || typeof draft.text !== 'string' || !Number.isInteger(draft.cursor) ||
      draft.cursor < 0 || draft.cursor > draft.text.length) return 'Claude did not provide a valid prompt buffer.';
  if (draft.cursor && draft.cursor < draft.text.length &&
      /[\uD800-\uDBFF]/.test(draft.text[draft.cursor - 1]) &&
      /[\uDC00-\uDFFF]/.test(draft.text[draft.cursor])) return 'The prompt cursor splits a Unicode character.';
  // Real 2.1.300 paste probes show that read/change expose only these labels,
  // not their image bytes or collapsed text. Copying a label loses its payload.
  // A literal label also refuses: ambiguity is safer than silent draft loss.
  if (/\[(?:Image(?: #\d+)?|Pasted text #\d+[^\]\r\n]*)\]/.test(draft.text)) {
    return 'The draft contains an image or collapsed paste that Claude cannot export. Expand or remove it before handoff.';
  }
  if (draft.text.length > 16000) return 'The unsent draft is too large for a verified handoff.';
  return null;
}

export function sameDraft(left, right) {
  return left?.text === right?.text && left?.cursor === right?.cursor;
}

function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.keys(value).sort().filter(key => value[key] !== undefined)
      .map(key => [key, canonical(value[key])]));
  }
  return value;
}

export async function historySnapshot(messages, expected) {
  const count = expected?.count ?? messages?.length;
  if (!Array.isArray(messages) || !Number.isInteger(count) || count < 0 ||
      count > 4096 || messages.length < count) throw new Error('Claude did not provide the expected conversation history.');
  if (messages.some(message => !['user', 'assistant'].includes(message?.role) ||
      !Array.isArray(message.content) || message.content.some(block =>
        !block || typeof block !== 'object' || typeof block.type !== 'string'))) {
    throw new Error('Claude did not provide its rich conversation history; version 2.1.300 or later is required.');
  }
  const saved = messages.slice(0, count);
  const last = saved.at(-1);
  // Claude merges adjacent user messages: our local rename/dispatch commands
  // may append blocks, or a newline to the last text block. Save an exact
  // boundary and compare only that original prefix, without filtering content.
  const tail = expected ? expected.tail : last?.role === 'user' && last.content.length ? {
    blocks: last.content.length,
    textLength: last.content.at(-1).type === 'text' && typeof last.content.at(-1).text === 'string'
      ? last.content.at(-1).text.length : null,
  } : undefined;
  if (tail) {
    if (last?.role !== 'user' || last.content.length < tail.blocks) {
      throw new Error('The destination conversation history lost its saved boundary.');
    }
    const content = last.content.slice(0, tail.blocks);
    if (tail.textLength !== null) {
      const end = content.at(-1);
      if (end?.type !== 'text' || typeof end.text !== 'string' || end.text.length < tail.textLength) {
        throw new Error('The destination conversation history lost its saved text boundary.');
      }
      content[content.length - 1] = {...end, text: end.text.slice(0, tail.textLength)};
    }
    saved[saved.length - 1] = {...last, content};
  }
  const bytes = new TextEncoder().encode(JSON.stringify(canonical(saved)));
  const hashed = await crypto.subtle.digest('SHA-256', bytes);
  const digest = [...new Uint8Array(hashed)].map(value => value.toString(16).padStart(2, '0')).join('');
  return {format: 'api', digest, count, complete: false, ...(tail ? {tail} : {})};
}

export async function captureSnapshot($) {
  const draft = await $.prompt.read();
  const problem = draftProblem(draft);
  if (problem) throw new Error(problem);
  return {draft: {text: draft.text, cursor: draft.cursor},
    history: await historySnapshot(await $.session.messages({as: 'api'}))};
}

export function validSnapshot(value) {
  const tail = value?.history?.tail;
  const tailValid = tail === undefined || tail && Object.keys(tail).sort().join(',') === 'blocks,textLength' &&
    value.history.count > 0 && Number.isInteger(tail.blocks) && tail.blocks > 0 && tail.blocks <= 65536 &&
    (tail.textLength === null || Number.isInteger(tail.textLength) && tail.textLength >= 0 && tail.textLength <= 16777216);
  return value && !draftProblem(value.draft) && value.history &&
    /^[a-f0-9]{64}$/.test(value.history.digest ?? '') &&
    Number.isInteger(value.history.count) && value.history.count >= 0 &&
    value.history.count <= 4096 && value.history.format === 'api' && value.history.complete === false && tailValid;
}

export async function restoreSnapshot($, expected, restoreCursor) {
  if (!validSnapshot(expected)) throw new Error('The saved handoff snapshot is invalid.');
  const history = await historySnapshot(await $.session.messages({as: 'api'}), expected.history);
  if (history.digest !== expected.history.digest) throw new Error('The destination conversation history differs from the source.');
  const existing = await $.prompt.read();
  // Never overwrite anything typed into the receiving window while it opens.
  if (existing.text && !sameDraft(existing, expected.draft)) {
    throw new Error('The receiving prompt already contains a different draft; it was left untouched.');
  }
  // Even an empty or already matching read is not proof that a composer exists.
  const filled = await $.prompt.fill({text: expected.draft.text, mode: 'replace'});
  if (!filled.isFilled) return null;
  if (expected.draft.cursor !== expected.draft.text.length) {
    if (typeof restoreCursor !== 'function') throw new Error('The receiving cursor cannot be restored.');
    await restoreCursor(expected.draft);
  }
  return verifySnapshot($, expected);
}

export async function verifySnapshot($, expected) {
  if (!validSnapshot(expected)) throw new Error('The saved handoff snapshot is invalid.');
  const history = await historySnapshot(await $.session.messages({as: 'api'}), expected.history);
  if (history.digest !== expected.history.digest) throw new Error('The destination conversation history differs from the source.');
  const actual = await $.prompt.read();
  if (!sameDraft(actual, expected.draft)) throw new Error('The receiving prompt did not preserve the exact draft.');
  return {draft: {text: actual.text, cursor: actual.cursor}, history};
}
