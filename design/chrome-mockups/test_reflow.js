'use strict';
const assert = require('node:assert/strict');
const {reflow} = require('./reflow.js');
const words = lines => lines.join(' ').trim().split(/\s+/);
const opts = (x, height = 20, y = 0, columns = 48) => ({columns, dock: {x, y, width: columns - x, height}, enabled: true, maxBlankRows: 3});
const leftOpts = (width = 28, height = 20, y = 0, columns = 50) => ({columns, dock: {side: 'left', x: 0, y, width, height}, enabled: true, maxBlankRows: 3});
let cases = 0;
function check(name, run) { run(); cases++; console.log(`PASS ${name}`); }

check('fox paragraph consumes the blank row without moving the next header', () => {
  const original = ['123', '  the quick brown fox jumps over', '  the sleeping', '  dog', '', '456', '  hello world'];
  const actual = reflow(original, opts(17));
  assert.deepEqual(actual.lines, ['123', '  the quick brown', '  fox jumps over', '  the sleeping', '  dog', '456', '  hello world']);
  assert.deepEqual(words(actual.lines), words(original));
  assert.equal(actual.stats.blankRowsUsed, 1);
  assert.equal(actual.stats.paragraphsChanged, 1);
  assert.deepEqual(actual.markers.map(({row, column}) => ({row, column})), [1, 2, 3, 4].map(row => ({row, column: 1})));
});

check('multiple original lines join as one paragraph, retaining over / the boundary', () => {
  const original = ['123', '  the quick brown fox jumps over', '  the quick brown fox jumps over', '  the sleeping dog', '', '456'];
  const actual = reflow(original, opts(22));
  assert.deepEqual(actual.lines, ['123', '  the quick brown fox', '  jumps over the quick', '  brown fox jumps over', '  the sleeping dog', '456']);
  assert.deepEqual(words(actual.lines), words(original));
  assert.equal(actual.stats.overlapAfter, 0);
});

check('last-line slack can reflow without any blank row', () => {
  const original = ['123', '  the quick brown fox jumps', '  over the sleeping', '  dog', '456'];
  const actual = reflow(original, opts(22));
  assert.deepEqual(actual.lines, ['123', '  the quick brown fox', '  jumps over the', '  sleeping dog', '456']);
  assert.equal(actual.stats.blankRowsUsed, 0);
  assert.equal(actual.changes[0].rowsUsed, 3);
  assert.match(actual.blocks.find(block => block.status === 'reflowed').reason, /existing rows/);
});

check('a realistic long first row redistributes into its short final row', () => {
  const original = ['  The terminal menu needs a little room beside this paragraph while all of the running commands stay in place', '  with words intact', '456'];
  assert.ok(original[0].length > 100);
  const actual = reflow(original, opts(100, 10, 0, 128));
  assert.equal(actual.stats.paragraphsChanged, 1);
  assert.equal(actual.stats.blankRowsUsed, 0);
  assert.ok(actual.lines[0].length <= 100);
  assert.ok(actual.lines[1].length > original[1].length);
  assert.equal(actual.lines[2], '456');
  assert.deepEqual(words(actual.lines), words(original));
});

check('existing rows are preferred even when following blank rows are available', () => {
  const original = ['  the quick brown fox jumps', '  over the sleeping', '  dog', '', '', '456'];
  const actual = reflow(original, opts(22));
  assert.equal(actual.stats.blankRowsUsed, 0);
  assert.deepEqual(actual.lines.slice(3), original.slice(3));
});

check('second pass is idempotent and no further rows are consumed', () => {
  const original = ['123', '  the quick brown fox jumps over', '  the sleeping', '  dog', '', '456'];
  const first = reflow(original, opts(17));
  const second = reflow(first.lines, opts(17));
  assert.deepEqual(second.lines, first.lines);
  assert.equal(second.changes.length, 0);
});

check('no row drift when several separate paragraphs consume local blanks', () => {
  const original = ['123', '  the quick brown fox jumps over', '  the sleeping', '  dog', '', '456', '  we keep the next section in place', '', '', '789'];
  const actual = reflow(original, opts(17));
  assert.equal(actual.lines.length, original.length);
  for (const row of [0, 5, 9]) assert.equal(actual.lines[row], original[row]);
  assert.deepEqual(words(actual.lines), words(original));
  assert.equal(actual.changes.length, 2);
});

check('cannot borrow blank lines beyond an intervening heading', () => {
  const original = ['  the quick brown fox jumps over', '456', '', '', '789'];
  const actual = reflow(original, opts(17));
  assert.deepEqual(actual.lines, original);
  assert.equal(actual.blocks[0].status, 'rejected');
  assert.ok(actual.stats.overlapAfter > 0);
});

check('code, table columns, prompts, list continuations and fenced text stay literal', () => {
  const examples = [
    ['  const target = something();', '', '', '456'],
    ['  name      long value here', '', '', '456'],
    ['  name | very long column value', '', '', '456'],
    ['$ the quick brown fox jumps over', '', '', '456'],
    ['- one list item', '  continuation that should remain literal', '', '', '456'],
    ['```text', 'the quick brown fox jumps over the dog', '', '```', '', '456'],
    ['  paragraph_variable another_variable', '', '', '456']
  ];
  for (const original of examples) assert.deepEqual(reflow(original, opts(17)).lines, original);
});

check('long words never split, truncate, or concatenate identifiers', () => {
  const original = ['  extraordinarilylongword ordinary words', '', '', '456'];
  const actual = reflow(original, opts(17));
  assert.deepEqual(actual.lines, original);
  assert.match(actual.blocks[0].reason, /whole word/);
});

check('dock affects only its own rows, using full width above and below', () => {
  const original = ['  the quick brown fox jumps', '  over the sleeping dog', '  and then walks home', '', '456'];
  const actual = reflow(original, opts(22, 1, 1));
  assert.deepEqual(words(actual.lines), words(original));
  assert.ok(actual.lines[0].length > 22);
  assert.ok(actual.lines[1].length <= 22);
  assert.equal(actual.lines[4], '456');
  assert.equal(actual.stats.overlapAfter, 0);
});

check('unindented prose gets an unplaced marker instead of a reserved column', () => {
  const original = ['the quick brown fox jumps', 'over the sleeping', 'dog', '456'];
  const actual = reflow(original, opts(20));
  assert.ok(actual.changes.length);
  assert.ok(actual.markers.length);
  assert.ok(actual.markers.every(marker => marker.column === null && !marker.placed));
  assert.ok(actual.lines.every(line => !line.startsWith(' ')));
});

check('blank row budget is respected and whitespace-only blanks are usable', () => {
  const original = ['  the quick brown fox jumps over the sleeping dog', '     ', '', '', '456'];
  assert.deepEqual(reflow(original, {...opts(17, 20, 0, 60), maxBlankRows: 0}).lines, original);
  const actual = reflow(original, {...opts(17, 20, 0, 60), maxBlankRows: 3});
  assert.equal(actual.stats.blankRowsUsed, 3);
  assert.equal(actual.lines[4], '456');
});

check('completed short sentences retain intentional line boundaries', () => {
  const original = ['  First do the important work.', '  Then stop.', '456'];
  const actual = reflow(original, opts(22));
  assert.deepEqual(actual.lines, original);
  assert.equal(actual.changes.length, 0);
});

check('disabled reflow and already clear paragraphs are unchanged', () => {
  const original = ['  the quick brown fox jumps over', '  the sleeping', '  dog', '', '456'];
  assert.deepEqual(reflow(original, {...opts(17), enabled: false}).lines, original);
  assert.deepEqual(reflow(original, opts(40)).lines, original);
  assert.deepEqual(original, ['  the quick brown fox jumps over', '  the sleeping', '  dog', '', '456']);
});

check('padded capture rows preserve words and unaffected original rows', () => {
  const original = ['123', '  the quick brown fox jumps over', '  the sleeping', '  dog', '', '456'].map(line => line.padEnd(48));
  const actual = reflow(original, opts(17));
  assert.deepEqual(words(actual.lines), words(original));
  assert.equal(actual.lines[0], original[0]);
  assert.equal(actual.lines[5], original[5]);
  assert.equal(actual.lines[1], '  the quick brown');
});

check('zero-height dock and width smaller than indentation return safely', () => {
  const original = ['    the quick brown fox jumps over', '', '456'];
  assert.deepEqual(reflow(original, opts(17, 0)).lines, original);
  const narrow = reflow(original, opts(3));
  assert.deepEqual(narrow.lines, original);
  assert.equal(narrow.blocks[0].status, 'rejected');
  assert.deepEqual(reflow(original, {...opts(17), columns: 0}).lines, original);
});

check('same-indent headings and short capitalized instructions do not join', () => {
  const headings = ['Setup Instructions', 'Font and Theme', '', '456'];
  assert.deepEqual(reflow(headings, opts(10)).lines, headings);
  const labels = ['Important notes:', 'the quick brown fox jumps over', '456'];
  assert.deepEqual(reflow(labels, opts(17)).lines, labels);
  const instructions = ['Turn on the lamp', 'Open the door', '456'];
  assert.deepEqual(reflow(instructions, opts(10)).lines, instructions);
});

check('raw ANSI and uncertain-width characters remain literal', () => {
  const examples = [
    ['  the quick \x1b[31mbrown fox jumps over', '', '', '456'],
    ['  the quick e\u0301 brown fox jumps over', '', '', '456'],
    ['  the quick 🦊 brown fox jumps over', '', '', '456'],
    ['  the quick 漢字 brown fox jumps over', '', '', '456'],
    ['  incomplete � capture with long prose', '', '', '456']
  ];
  for (const original of examples) assert.deepEqual(reflow(original, opts(17)).lines, original);
});

check('horizontal terminal chrome cannot join preceding prose', () => {
  const original = ['  the quick brown fox jumps over', '──commits─by─default─and─more──────────────────', '', '', '456'];
  const actual = reflow(original, opts(17));
  assert.deepEqual(actual.lines, original);
  assert.ok(actual.blocks.some(block => /terminal chrome/.test(block.reason)));
});

check('imported terminal captures retain word order and fixed protected rows', () => {
  const fs = require('node:fs');
  const path = require('node:path');
  const captures = JSON.parse(fs.readFileSync(path.join(__dirname, 'captures.json'), 'utf8'));
  for (const capture of captures) {
    const actual = reflow(capture.lines, opts(100, 36, 0, 128));
    assert.equal(actual.lines.length, capture.lines.length);
    assert.deepEqual(words(actual.lines), words(capture.lines));
    for (const block of actual.blocks.filter(block => block.status === 'protected')) {
      assert.deepEqual(actual.lines.slice(block.startRow, block.endRow + 1), capture.lines.slice(block.startRow, block.endRow + 1));
    }
  }
});

check('same module exposes the browser global without CommonJS', () => {
  const fs = require('node:fs');
  const vm = require('node:vm');
  const sandbox = {window: {}};
  vm.runInNewContext(fs.readFileSync(require.resolve('./reflow.js'), 'utf8'), sandbox);
  assert.equal(typeof sandbox.window.HardcoreReflow.reflow, 'function');
  assert.equal(sandbox.window.HardcoreReflow.reflow(['hello world'], opts(40)).lines[0], 'hello world');
});

check('left dock shifts and wraps the fox into its following blank row', () => {
  const original = ['123', '  the quick brown fox jumps over', '  the sleeping', '  dog', '', '456', '  hello world'];
  const actual = reflow(original, leftOpts(28, 4, 1, 45));
  assert.deepEqual(actual.lines, ['123', ' '.repeat(30) + 'the quick brown', ' '.repeat(30) + 'fox jumps over', ' '.repeat(30) + 'the sleeping', ' '.repeat(30) + 'dog', '456', '  hello world']);
  assert.deepEqual(words(actual.lines), words(original));
  assert.equal(actual.stats.blankRowsUsed, 1);
  assert.equal(actual.stats.overlapAfter, 0);
  assert.deepEqual(actual.changes[0].rowOffsets, [28, 28, 28, 28]);
  assert.ok(actual.markers.every(marker => marker.column === 29 && marker.offset === 28));
});

check('left dock uses final-line slack without needing blank rows', () => {
  const original = ['123', '  the quick brown fox jumps', '  over the sleeping', '  dog', '456'];
  const actual = reflow(original, leftOpts(28, 3, 1));
  assert.deepEqual(actual.lines, ['123', ' '.repeat(30) + 'the quick brown fox', ' '.repeat(30) + 'jumps over the', ' '.repeat(30) + 'sleeping dog', '456']);
  assert.equal(actual.stats.blankRowsUsed, 0);
  assert.equal(actual.stats.overlapAfter, 0);
  assert.deepEqual(words(actual.lines), words(original));
});

check('short left-overlapping prose shifts safely without borrowing rows', () => {
  const original = ['123', '  hello world', '456'];
  const actual = reflow(original, leftOpts(28, 1, 1));
  assert.equal(actual.lines[1], ' '.repeat(30) + 'hello world');
  assert.equal(actual.stats.blankRowsUsed, 0);
  assert.equal(actual.changes[0].rowsUsed, 1);
  assert.equal(actual.changes[0].side, 'left');
  assert.equal(actual.changes[0].indent, 2);
});

check('left paragraph uses full width above and below a shorter dock', () => {
  const original = [
    '  one two three four five six seven eight nine ten eleven',
    '  twelve thirteen fourteen fifteen sixteen seventeen',
    '  eighteen nineteen twenty',
    '456'
  ];
  const actual = reflow(original, leftOpts(28, 1, 1, 60));
  assert.equal(actual.stats.paragraphsChanged, 1);
  assert.equal(actual.stats.overlapAfter, 0);
  assert.deepEqual(words(actual.lines), words(original));
  assert.deepEqual(actual.changes[0].rowOffsets, [0, 28, 0]);
  assert.ok(actual.lines[0].startsWith('  one'));
  assert.ok(actual.lines[0].length > 32);
  assert.match(actual.lines[1], /^ {30}\S/);
  assert.match(actual.lines[2], /^ {2}\S/);
  assert.ok(actual.lines[2].length > 32);
  assert.equal(actual.lines[3], '456');
  assert.ok(actual.lines.every(line => line.length <= 60));
});

check('left dock preserves fixed boundaries and prefers existing rows to blank capacity', () => {
  const original = ['123', '  the quick brown fox jumps', '  over the sleeping', '  dog', '', '456', '  hello world', '', '789'];
  const actual = reflow(original, leftOpts());
  assert.equal(actual.lines.length, original.length);
  for (const row of [0, 4, 5, 7, 8]) assert.equal(actual.lines[row], original[row]);
  assert.equal(actual.stats.blankRowsUsed, 0);
  assert.equal(actual.changes.length, 2);
  assert.deepEqual(words(actual.lines), words(original));
  const repeated = reflow(actual.lines, leftOpts());
  assert.deepEqual(repeated.lines, actual.lines);
  assert.equal(repeated.changes.length, 0);
});

check('left dock leaves whole paragraphs literal when complete safe placement is impossible', () => {
  const examples = [
    ['  extraordinarilylongword ordinary words', '', '', '456'],
    ['  the quick brown fox jumps over the sleeping dog', '456', '', '789']
  ];
  for (const original of examples) {
    const actual = reflow(original, leftOpts(28, 20, 0, 45));
    assert.deepEqual(actual.lines, original);
    assert.equal(actual.blocks[0].status, 'rejected');
    assert.ok(actual.stats.overlapAfter > 0);
    assert.equal(actual.markers.length, 0);
  }
});

check('left dock keeps code, boxes, aligned columns and raw styling literal', () => {
  const examples = [
    ['  const target = something();', '', '', '456'],
    ['  name      long value here', '', '', '456'],
    ['  │ the quick brown fox jumps over │', '', '', '456'],
    ['  the quick \x1b[48;5;24mbrown fox jumps over', '', '', '456'],
    ['- one list item', '  continuation that should remain literal', '', '', '456'],
    ['```text', 'the quick brown fox jumps over the dog', '', '```', '', '456']
  ];
  for (const original of examples) assert.deepEqual(reflow(original, leftOpts()).lines, original);
});

check('left dock cannot steal a menu cell for unindented debug markers', () => {
  const original = ['the quick brown fox jumps', 'over the sleeping', 'dog', '456'];
  const actual = reflow(original, leftOpts(28, 3, 0, 48));
  assert.equal(actual.changes.length, 1);
  assert.ok(actual.markers.length);
  assert.ok(actual.markers.every(marker => marker.column === null && !marker.placed && marker.offset === 28));
  assert.ok(actual.lines.slice(0, 3).every(line => /^ {28}\S/.test(line)));
  assert.deepEqual(words(actual.lines), words(original));
});

check('left defaults to the left edge and handles an unusably narrow window', () => {
  const original = ['  hello world', '', '456'];
  const implicitLeft = {columns: 50, dock: {side: 'left', width: 28, y: 0, height: 1}};
  assert.equal(reflow(original, implicitLeft).lines[0], ' '.repeat(30) + 'hello world');
  for (const columns of [0, 27, 28, 29, 30]) {
    assert.deepEqual(reflow(original, leftOpts(28, 20, 0, columns)).lines, original);
  }
  assert.deepEqual(reflow(original, {...leftOpts(), enabled: false}).lines, original);
});

check('real captures also preserve words and protected rows beside a left dock', () => {
  const fs = require('node:fs');
  const path = require('node:path');
  const captures = JSON.parse(fs.readFileSync(path.join(__dirname, 'captures.json'), 'utf8'));
  for (const capture of captures) {
    const actual = reflow(capture.lines, leftOpts(28, 28, 3, 128));
    assert.equal(actual.lines.length, capture.lines.length);
    assert.deepEqual(words(actual.lines), words(capture.lines));
    for (const block of actual.blocks.filter(block => block.status === 'protected')) {
      assert.deepEqual(actual.lines.slice(block.startRow, block.endRow + 1), capture.lines.slice(block.startRow, block.endRow + 1));
    }
  }
});

console.log(`\n${cases} local reflow tests passed.`);
