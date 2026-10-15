import assert from 'node:assert/strict';
import {test} from 'node:test';
import {captureSnapshot, draftProblem, historySnapshot, restoreSnapshot} from '../hooks/snapshot.js';

const messages = [{role: 'user', content: [{type: 'text', text: 'Keep PINEAPPLE'}]},
  {role: 'assistant', content: [{type: 'text', text: 'PINEAPPLE'}]}];

function composer(initial, options = {}) {
  let draft = initial;
  let fills = 0;
  return {
    session: {messages: async args => {
      assert.deepEqual(args, {as: 'api'});
      return options.messages ?? messages;
    }},
    prompt: {
      read: async () => draft,
      fill: async ({text, mode}) => {
        assert.equal(mode, 'replace');
        fills++;
        if (options.refuse) return {isFilled: false, ...draft};
        draft = {text: options.rewrite ?? text, cursor: (options.rewrite ?? text).length};
        return {isFilled: true, ...draft};
      },
    },
    get draft() { return draft; }, get fills() { return fills; },
  };
}

test('multiline Unicode draft and projected history restore without model submission', async () => {
  const text = 'unfinished 🐈 thought\nnext line\t ';
  const source = composer({text, cursor: text.length});
  const expected = await captureSnapshot(source);
  assert.deepEqual(source.draft, expected.draft);
  const target = composer({text: '', cursor: 0});
  assert.deepEqual(await restoreSnapshot(target, expected), expected);
  assert.deepEqual(target.draft, source.draft);
  assert.equal(expected.history.complete, false, 'projection is never full transcript proof');
});

test('an unavailable composer cannot verify even an empty draft', async () => {
  const expected = await captureSnapshot(composer({text: '', cursor: 0}));
  const target = composer({text: '', cursor: 0}, {refuse: true});
  assert.equal(await restoreSnapshot(target, expected), null);
  assert.equal(target.fills, 1);
});

test('different receiving draft is untouched', async () => {
  const expected = await captureSnapshot(composer({text: 'original', cursor: 8}));
  const target = composer({text: 'new typing', cursor: 10});
  await assert.rejects(restoreSnapshot(target, expected), /different draft/);
  assert.equal(target.draft.text, 'new typing');
  assert.equal(target.fills, 0);
});

test('different or shortened history never fills the prompt', async () => {
  const expected = await captureSnapshot(composer({text: 'original', cursor: 8}));
  for (const changed of [messages.slice(0, 1), [{...messages[0], content: [{type: 'text', text: 'wrong'}]}, messages[1]]]) {
    const target = composer({text: '', cursor: 0}, {messages: changed});
    await assert.rejects(restoreSnapshot(target, expected), /history/);
    assert.equal(target.fills, 0);
  }
});

test('extra destination messages preserve the verified source prefix', async () => {
  const expected = await captureSnapshot(composer({text: 'original', cursor: 8}));
  const target = composer({text: '', cursor: 0}, {messages: [...messages,
    {role: 'assistant', content: [{type: 'text', text: 'local command output'}]}]});
  assert.deepEqual(await restoreSnapshot(target, expected), expected);
});

test('a different plugin rewriting the draft fails exact readback', async () => {
  const expected = await captureSnapshot(composer({text: 'original', cursor: 8}));
  const target = composer({text: '', cursor: 0}, {rewrite: 'modified'});
  await assert.rejects(restoreSnapshot(target, expected), /exact draft/);
});

test('middle cursor is captured exactly and restored through a supplied cursor hook', async () => {
  const source = composer({text: 'original', cursor: 2});
  const snapshot = await captureSnapshot(source);
  const target = composer({text: '', cursor: 0});
  await assert.rejects(restoreSnapshot(target, snapshot), /cursor cannot be restored/);
  assert.deepEqual(source.draft, {text: 'original', cursor: 2});
});

test('canonical history hash ignores object key insertion order', async () => {
  const left = [{role: 'user', content: [{type: 'text', text: 'hello'}]}];
  const right = [{content: [{text: 'hello', type: 'text'}], role: 'user'}];
  assert.deepEqual(await historySnapshot(left), await historySnapshot(right));
});

test('rich history digest includes image and thinking block contents', async () => {
  const rich = [{role: 'user', content: [{type: 'image', source: {type: 'base64', data: 'one'}}]},
    {role: 'assistant', content: [{type: 'thinking', thinking: 'reason', signature: 'signed'}]}];
  const changed = structuredClone(rich);
  changed[0].content[0].source.data = 'two';
  assert.notEqual((await historySnapshot(rich)).digest, (await historySnapshot(changed)).digest);
  changed[0].content[0].source.data = 'one';
  changed[1].content[0].thinking = 'different';
  assert.notEqual((await historySnapshot(rich)).digest, (await historySnapshot(changed)).digest);
});

test('an old text projection cannot pass as rich API history', async () => {
  await assert.rejects(historySnapshot([{role: 'user', text: 'hello', toolUses: []}]), /rich conversation/);
});

test('appended blocks and text on the last user message retain the exact saved prefix', async () => {
  const original = [...messages, {role: 'user', content: [
    {type: 'image', source: {type: 'base64', data: 'pixels'}},
    {type: 'text', text: 'local command 🐈'}]}];
  const expected = await historySnapshot(original);
  assert.deepEqual(expected.tail, {blocks: 2, textLength: 'local command 🐈'.length});
  const target = structuredClone(original);
  target[2].content[1].text += '\n';
  target[2].content.push({type: 'text', text: 'new rename metadata'});
  assert.deepEqual(await historySnapshot(target, expected), expected);
  assert.equal(target[2].content[1].text, 'local command 🐈\n', 'verification must not mutate real history');
});

test('prefix proof refuses changes to earlier bytes, blocks and media', async () => {
  const original = [...messages, {role: 'user', content: [
    {type: 'image', source: {type: 'base64', data: 'pixels'}},
    {type: 'text', text: 'saved tail'}]}];
  const expected = await historySnapshot(original);
  for (const edit of [
    value => { value[0].content[0].text = 'wrong'; },
    value => { value[2].content[0].source.data = 'wrong'; },
    value => { value[2].content[1].text = 'wrong tail plus an appended suffix'; },
  ]) {
    const changed = structuredClone(original);
    edit(changed);
    assert.notEqual((await historySnapshot(changed, expected)).digest, expected.digest);
  }
  const shortened = structuredClone(original);
  shortened[2].content.pop();
  await assert.rejects(historySnapshot(shortened, expected), /saved boundary/);
});

test('a non-text last user block is compared completely', async () => {
  const original = [{role: 'user', content: [{type: 'image', source: {data: 'pixels'}}]}];
  const expected = await historySnapshot(original);
  assert.deepEqual(expected.tail, {blocks: 1, textLength: null});
  const target = structuredClone(original);
  target[0].content.push({type: 'text', text: 'later command'});
  assert.deepEqual(await historySnapshot(target, expected), expected);
  target[0].content[0].source.data += 'changed';
  assert.notEqual((await historySnapshot(target, expected)).digest, expected.digest);
});

test('image and collapsed-paste labels refuse rather than lose their hidden payloads', async () => {
  for (const text of ['[Image #1]', 'before [Image #23] after', '[Image]',
    '[Pasted text #1 +119 lines]', 'look at [Pasted text #3 +2 lines]']) {
    const source = composer({text, cursor: text.length});
    await assert.rejects(captureSnapshot(source), /image or collapsed paste/);
    assert.equal(source.draft.text, text);
    assert.equal(source.fills, 0);
  }
  assert.equal(draftProblem({text: 'An ordinary [bracketed] draft', cursor: 28}), null);
});
