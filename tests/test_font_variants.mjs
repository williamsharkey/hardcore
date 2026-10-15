/* Exercise the actual shipped Terminus bitmaps, not a synthetic imitation. */
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {gunzipSync} from 'node:zlib';
import {test} from 'node:test';
import '../experiments/font-lab/variant-engine.js';

const {makeFont, getGlyphPlan} = globalThis.HairlineVariant;
const bdf = gunzipSync(readFileSync(new URL('../font/terminus/ter-u24n.bdf.gz', import.meta.url))).toString('latin1');
const glyphs = {};
for (const block of bdf.split('STARTCHAR ').slice(1)) {
  const cp = Number(block.match(/^ENCODING (\d+)$/m)?.[1]);
  if (!Number.isInteger(cp)) continue;
  const [width, height, xOffset, yOffset] = block.match(/^BBX (.+)$/m)[1].split(' ').map(Number);
  const advance = Number(block.match(/^DWIDTH (\d+)/m)[1]);
  if (advance !== 12) continue;
  const bitmap = block.split('BITMAP\n')[1].split('ENDCHAR')[0].trim().split('\n');
  const rows = Array(24).fill(0);
  for (let y = 0; y < height; y++) {
    const target = 19 - yOffset - height + y;
    if (target < 0 || target >= rows.length) continue;
    const bits = parseInt(bitmap[y], 16) >>> (bitmap[y].length * 4 - width);
    const shift = 12 - width - xOffset;
    rows[target] = (shift >= 0 ? bits << shift : bits >>> -shift) & 4095;
  }
  glyphs[cp] = rows;
}
const base = {id: 'terminus-24', name: 'Terminus 12x24', width: 12, height: 24, ascent: 19, glyphs};
const alnum = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789';
const copies = structuredClone(base);
const bitmap = (font, char) => font.glyphs[char.codePointAt(0)];
const top = rows => rows.findIndex(Boolean);
const count = row => row.toString(2).replaceAll('0', '').length;
const solidBlocks = rows => rows.slice(1).reduce((n, row, i) => {
  const shared = row & rows[i];
  return n + count(shared & (shared << 1));
}, 0);

function components(rows) {
  const pixels = new Set();
  rows.forEach((row, y) => {
    for (let x = 0; x < 12; x++) if (row & (1 << (11 - x))) pixels.add(y * 12 + x);
  });
  let n = 0;
  while (pixels.size) {
    n++;
    const start = pixels.values().next().value;
    const pending = [start];
    pixels.delete(start);
    while (pending.length) {
      const point = pending.pop(), x = point % 12, y = Math.floor(point / 12);
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        if (x + dx < 0 || x + dx >= 12 || y + dy < 0) continue;
        const next = (y + dy) * 12 + x + dx;
        if (pixels.delete(next)) pending.push(next);
      }
    }
  }
  return n;
}

test('source metrics and unchanged variant match every shipped glyph bit for bit', () => {
  assert.match(bdf, /^FONT_ASCENT 19$/m);
  assert.match(bdf, /^FONT_DESCENT 5$/m);
  assert.ok(Object.keys(glyphs).length > 1300);
  assert.equal(top(bitmap(base, 'I')), 4);
  assert.equal(top(bitmap(base, 'x')), 8);
  assert.deepEqual(makeFont(base).glyphs, base.glyphs);
});

test('all 441 control combinations preserve baseline, descenders, range and connectivity', () => {
  for (let cellHeight = 24; cellHeight <= 32; cellHeight++) {
    for (let xHeightDelta = 0; xHeightDelta <= 6; xHeightDelta++) {
      for (let capHeightDelta = 0; capHeightDelta <= 6; capHeightDelta++) {
        const font = makeFont(base, {cellHeight, xHeightDelta, capHeightDelta});
        const actualHeight = Math.max(cellHeight, 22 + Math.max(xHeightDelta, capHeightDelta));
        const offset = actualHeight - 24;
        assert.equal(font.height, actualHeight);
        assert.equal(font.experiment.requestedCellHeight, cellHeight);
        assert.equal(font.experiment.topPaddingAdded, actualHeight - cellHeight);
        assert.equal(font.ascent, 19 + offset);
        assert.equal(font.metrics.descent, 5);
        for (const char of alnum) {
          const rows = bitmap(font, char), original = bitmap(base, char);
          assert.equal(rows.length, actualHeight);
          assert.ok(rows.every(row => Number.isInteger(row) && row >= 0 && row < 4096));
          assert.deepEqual(rows.slice(offset + 17), original.slice(17), `${char}: baseline/descender changed`);
          assert.equal(components(rows), components(original), `${char}: lost or joined a component`);
          assert.ok(solidBlocks(rows) <= solidBlocks(original), `${char}: added a solid 2x2 block`);
        }
        for (const char of 'ACEHIOXYZ0123456789') {
          assert.equal(top(bitmap(font, char)), 4 - capHeightDelta + offset, char);
        }
        for (const char of 'acegmnopqrsuvwxyz') {
          assert.equal(top(bitmap(font, char)), 8 - xHeightDelta + offset, char);
        }
        for (const char of 'bdhklt') {
          assert.equal(top(bitmap(font, char)), 4 - Math.max(0, xHeightDelta - 2) + offset, `${char}: ascender misplaced`);
        }
        for (const char of 'fij') {
          assert.equal(top(bitmap(font, char)), 4 - xHeightDelta + offset, `${char}: upper feature misplaced`);
          assert.deepEqual(bitmap(font, char).slice(4 - xHeightDelta + offset, 10 - xHeightDelta + offset),
            bitmap(base, char).slice(4, 10), `${char}: upper feature compressed`);
        }
        for (const char of alnum) assert.ok(top(bitmap(font, char)) >= 2, `${char}: top margin lost`);
      }
    }
  }
});

test('capital I retains single-row bars and a single-column stem at every setting', () => {
  for (let capHeightDelta = 0; capHeightDelta <= 6; capHeightDelta++) {
    const font = makeFont(base, {capHeightDelta});
    const rows = bitmap(font, 'I'), offset = font.metrics.topOffset;
    const start = 4 - capHeightDelta + offset;
    assert.equal(rows[start], bitmap(base, 'I')[4]);
    assert.equal(rows[18 + offset], bitmap(base, 'I')[18]);
    for (let y = start + 1; y < 18 + offset; y++) assert.equal(count(rows[y]), 1);
    assert.equal(rows.filter(row => count(row) > 1).length, 2);
  }
});

test('inserted rows are isolated pixels, never repeated horizontal bars', () => {
  for (let delta = 1; delta <= 6; delta++) {
    const font = makeFont(base, {xHeightDelta: delta, capHeightDelta: delta});
    for (const char of alnum) {
      const cp = char.codePointAt(0), placements = font.experiment.placements[cp];
      assert.equal(placements.length, delta);
      for (const placement of placements) {
        const row = bitmap(font, char)[placement.targetRow];
        assert.ok(row, `${char}: disconnected seam`);
        assert.equal(row & (row << 1), 0, `${char}: seam thickened a horizontal stroke`);
      }
    }
  }
});

test('e +2 inserts once above and once below its one-row crossbar', () => {
  const font = makeFont(base, {xHeightDelta: 2});
  const plan = getGlyphPlan(base, 101, {xHeightDelta: 2});
  assert.deepEqual(plan.seams.map(seam => seam.actual), [11, 15]);
  assert.deepEqual(plan.seams, font.experiment.placements[101]);
  const original = bitmap(base, 'e'), result = bitmap(font, 'e');
  const crossbar = original[13];
  assert.equal(count(crossbar), 9);
  assert.deepEqual(result.map((row, y) => row === crossbar ? y : -1).filter(y => y >= 0), [12]);
  assert.equal(13 - 8 + 1, 12 - top(result)); // The eye grew one row.
  assert.equal(18 - 13 + 1, 18 - 12); // The lower opening grew one row.
  assert.ok(plan.seams[0].targetRow < 12);
  assert.ok(plan.seams[1].targetRow > 12);
});

test('f +2 / caps +4 / cell 26 preserves the complete original hook above its crossbar', () => {
  const font = makeFont(base, {xHeightDelta: 2, capHeightDelta: 4, cellHeight: 26});
  const original = bitmap(base, 'f'), rows = bitmap(font, 'f');
  // The source has a rounded shoulder, three stem rows, then the crossbar.
  // At these settings the two-pixel rise cancels the added two rows of leading.
  assert.equal(font.height, 26);
  assert.deepEqual(rows.slice(4, 9), original.slice(4, 9));
  assert.deepEqual(rows.slice(19), original.slice(17));
  assert.equal(rows.filter(row => row === original[8]).length, 1);
});

test('f hook and crossbar keep their exact spacing across growth, cell heights and arrow positions', () => {
  const original = bitmap(base, 'f');
  const source = {...base, glyphs: {102: original}};
  for (let cellHeight = 24; cellHeight <= 32; cellHeight++) {
    for (let xHeightDelta = 0; xHeightDelta <= 6; xHeightDelta++) {
      // Include automatic, repeated and mixed insertion positions.
      const choices = [undefined, [10, 16, 11, 15, 12, 14]];
      for (let requested = 0; requested <= 24; requested++) choices.push(Array(6).fill(requested));
      for (const boundaries of choices) {
        const font = makeFont(source, {cellHeight, xHeightDelta,
          glyphSeams: boundaries ? {102: boundaries} : {}});
        const rows = bitmap(font, 'f'), offset = font.metrics.topOffset;
        const hookTop = 4 + offset - xHeightDelta;
        assert.equal(top(rows), hookTop);
        assert.deepEqual(rows.slice(hookTop, hookTop + 5), original.slice(4, 9));
        assert.deepEqual(rows.slice(17 + offset), original.slice(17));
        assert.equal(components(rows), 1);
        assert.equal(solidBlocks(rows), 0);
        assert.equal(rows.filter(row => row === original[4]).length, 1);
        assert.equal(rows.filter(row => row === original[8]).length, 1);
      }
    }
  }
});

test('all approximate insertion positions snap safely for every alphanumeric and growth amount', () => {
  for (const char of alnum) {
    const cp = char.codePointAt(0), original = bitmap(base, char);
    const source = {...base, glyphs: {[cp]: original}};
    for (let delta = 1; delta <= 6; delta++) {
      for (let requested = 0; requested <= 24; requested++) {
        const options = {
          xHeightDelta: delta, capHeightDelta: delta,
          glyphSeams: {[cp]: Array(delta).fill(requested)},
        };
        const plan = getGlyphPlan(source, cp, options);
        const font = makeFont(source, options), rows = bitmap(font, char);
        const offset = font.metrics.topOffset;
        assert.deepEqual(plan.seams, font.experiment.placements[cp]);
        assert.equal(components(rows), components(original), `${char} +${delta} at ${requested}: connectivity`);
        assert.ok(solidBlocks(rows) <= solidBlocks(original), `${char}: thickened a join`);
        assert.deepEqual(rows.slice(17 + offset), original.slice(17));
        assert.equal(new Set(plan.seams.map(seam => seam.targetRow)).size, delta);
        for (const seam of plan.seams) {
          assert.ok(plan.safeBoundaries.includes(seam.actual));
          assert.equal(seam.requested, requested);
          const distance = Math.abs(seam.actual - requested);
          assert.ok(plan.safeBoundaries.every(y => Math.abs(y - requested) >= distance));
          const row = rows[seam.targetRow];
          assert.ok(row);
          assert.equal(row & (row << 1), 0, `${char}: unsafe inserted row`);
        }
      }
    }
  }
});

test('per-glyph edits retain insertion identities, support repeats, and preserve inactive settings', () => {
  const glyphSeams = {101: [16, 10, 13, 14, 10, 16]};
  const before = structuredClone(glyphSeams);
  const source = {...base, glyphs: {101: bitmap(base, 'e'), 111: bitmap(base, 'o')}};
  const small = makeFont(source, {xHeightDelta: 2, glyphSeams});
  const large = makeFont(source, {xHeightDelta: 6, glyphSeams});
  assert.deepEqual(small.experiment.placements[101].map(seam => seam.actual), [16, 10]);
  assert.deepEqual(large.experiment.placements[101].map(seam => seam.actual), [16, 10, 12, 15, 10, 16]);
  assert.deepEqual(large.experiment.placements[101].map(seam => seam.index), [0, 1, 2, 3, 4, 5]);
  assert.deepEqual(glyphSeams, before);
  assert.deepEqual(bitmap(small, 'o'), bitmap(makeFont(source, {xHeightDelta: 2}), 'o'));
  const partial = getGlyphPlan(source, 101, {xHeightDelta: 2, glyphSeams: {101: [16]}});
  assert.deepEqual(partial.seams.map(seam => seam.actual), [16, 15]);
  assert.deepEqual(getGlyphPlan(source, 101, {xHeightDelta: 0, glyphSeams}).seams, []);
  const repeated = getGlyphPlan(source, 101, {xHeightDelta: 2, glyphSeams: {101: [10, 10]}});
  assert.equal(repeated.seams[1].targetRow - repeated.seams[0].targetRow, 1);
  const moved = getGlyphPlan(source, 101, {cellHeight: 32, xHeightDelta: 2, glyphSeams});
  assert.deepEqual(moved.seams.map(seam => seam.actual), [16, 10]);
  assert.deepEqual(moved.seams.map(seam => seam.targetRow), small.experiment.placements[101].map(seam => seam.targetRow + 8));
});

test('non-alphanumerics have no editable insertion rows and remain bitmap-identical', () => {
  const plan = getGlyphPlan(base, 63, {xHeightDelta: 6, capHeightDelta: 6, glyphSeams: {63: [12]}});
  assert.equal(plan.delta, 0);
  assert.deepEqual(plan.seams, []);
  assert.deepEqual(plan.safeBoundaries, []);
  assert.equal(plan.cellHeight, 28);
  const font = makeFont(base, {xHeightDelta: 6, capHeightDelta: 6, glyphSeams: {63: [12]}});
  assert.deepEqual(bitmap(font, '?'), Array(4).fill(0).concat(bitmap(base, '?')));
});

test('x-height and capital controls are independent and punctuation is unchanged', () => {
  const capitals = makeFont(base, {capHeightDelta: 2});
  const lower = makeFont(base, {xHeightDelta: 2});
  for (const char of 'abcdefghijklmnopqrstuvwxyz') assert.deepEqual(bitmap(capitals, char), bitmap(base, char));
  for (const char of 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789') assert.deepEqual(bitmap(lower, char), bitmap(base, char));
  const extended = makeFont(base, {cellHeight: 32, xHeightDelta: 2, capHeightDelta: 2});
  for (const [cp, rows] of Object.entries(glyphs)) {
    if (alnum.includes(String.fromCodePoint(Number(cp)))) continue;
    assert.deepEqual(extended.glyphs[cp], Array(8).fill(0).concat(rows), `Unrequested change to U+${Number(cp).toString(16)}`);
  }
  assert.equal(components(bitmap(lower, 'i')), 2);
  assert.equal(components(bitmap(lower, 'j')), 2);
});

test('cell height alone adds leading without scaling or changing any source bit', () => {
  for (let cellHeight = 24; cellHeight <= 32; cellHeight++) {
    const font = makeFont(base, {cellHeight});
    for (const [cp, rows] of Object.entries(glyphs)) {
      assert.deepEqual(font.glyphs[cp], Array(cellHeight - 24).fill(0).concat(rows));
    }
  }
});

test('optional basic light boxes are one pixel and join at every cell boundary', () => {
  for (let cellHeight = 24; cellHeight <= 32; cellHeight++) {
    const font = makeFont(base, {cellHeight, redrawBoxes: true});
    const cy = Math.floor((cellHeight - 1) / 2), bit = 1 << 6;
    assert.deepEqual(bitmap(font, '│'), Array(cellHeight).fill(bit));
    assert.deepEqual(bitmap(font, '─'), Array.from({length: cellHeight}, (_, y) => y === cy ? 4095 : 0));
    for (const char of '┌┐┬├┤┼') assert.equal(bitmap(font, char).at(-1), bit);
    for (const char of '└┘┴├┤┼') assert.equal(bitmap(font, char)[0], bit);
    for (const char of '┌└┬┴├┼') assert.ok(bitmap(font, char)[cy] & 1);
    for (const char of '┐┘┬┴┤┼') assert.ok(bitmap(font, char)[cy] & 2048);
    if (cellHeight === 24) {
      for (const char of '─│┌┐└┘├┤┬┴┼') assert.deepEqual(bitmap(font, char), bitmap(base, char), char);
    }
    // Rounded, dashed, double and heavy boxes are deliberately untouched.
    for (const char of '╭╮╯╰═║━┃┄┆') {
      if (bitmap(base, char)) assert.deepEqual(bitmap(font, char), Array(cellHeight - 24).fill(0).concat(bitmap(base, char)));
    }
  }
});

test('source objects are never mutated or shared with generated bitmaps', () => {
  const font = makeFont(base, {cellHeight: 32, xHeightDelta: 2, capHeightDelta: 2, redrawBoxes: true});
  assert.deepEqual(base, copies);
  bitmap(font, 'I')[0] = 4095;
  assert.deepEqual(base, copies);
});

test('invalid dimensions, options and malformed bitmaps fail visibly', () => {
  for (const options of [{cellHeight: 23}, {cellHeight: 33}, {cellHeight: 24.5},
    {xHeightDelta: -1}, {capHeightDelta: 7}, {xHeightDelta: NaN}, {capHeightDelta: '2'}, {redrawBoxes: 1},
    {glyphSeams: null}, {glyphSeams: []}, {glyphSeams: {101: [25]}}, {glyphSeams: {101: [-1]}},
    {glyphSeams: {101: [11.5]}}, {glyphSeams: {101: Array(7).fill(10)}}, {glyphSeams: {101: '11'}}]) {
    assert.throws(() => makeFont(base, options));
  }
  for (const overrides of [{width: 14}, {height: 28}, {ascent: 20},
    {glyphs: {65: Array(24).fill(4096)}}, {glyphs: {65: Array(23).fill(0)}}]) {
    assert.throws(() => makeFont({...base, ...overrides}));
  }
  assert.throws(() => makeFont({...base, glyphs: {65: Array(24).fill(4095)}}, {capHeightDelta: 1}), /safe one-pixel seam/);
});
