/* Run: node tests/test_recording.js. No GUI, camera, user data, or packages. */
'use strict';
const assert = require('node:assert/strict');
const R = require('../recording/core.js');
const encoded = values => Buffer.from(values).toString('base64');
function quad({rect = [1, 2, 8, 9], uv = [0, 0, 2, 2], color = [255, 128, 0, 128]} = {}) {
  const bytes = Buffer.alloc(20);
  rect.forEach((value, index) => bytes.writeInt16LE(value, index * 2));
  uv.forEach((value, index) => bytes.writeUInt16LE(value, 8 + index * 2));
  color.forEach((value, index) => bytes.writeUInt8(value, 16 + index));
  return bytes.toString('base64');
}
function fixture() {
  return {version: 1, title: 'Pixel replay', durationMs: 3000,
    camera: {file: 'camera.mp4', offsetMs: 50, mirrored: true},
    frames: [
      {t: 0, w: 100, h: 80, bg: 0x112233, quads: quad(), atlas: [{x: 0, y: 0, w: 2, h: 2, data: encoded([0, 255, 255, 0])}]},
      {t: 1000, w: 100, h: 80, bg: 0, quads: quad(), atlas: [{x: 0, y: 0, w: 1, h: 1, data: encoded([128])}]},
      {t: 2000, w: 120, h: 100, bg: 0xffffff, quads: quad({uv: [65535, 0, 0, 0]})},
      {t: 2000, w: 120, h: 100, bg: 0xabcdef, quads: ''}],
    pointer: [{t: 10, x: 5, y: 6, type: 'move'}, {t: 90, x: 9, y: 6, type: 'move'}, {t: 100, x: 9, y: 6, type: 'down', buttons: 1}, {t: 120, x: 9, y: 6, type: 'up'}, {t: 500, x: 9, y: 6, type: 'move'}],
    captions: [{start: 300, end: 700, text: 'one'}, {start: 600, end: 900, text: 'two'}]};
}
const replay = R.parseRecording(fixture());
assert.equal(replay.frames[0].quads.length, 20);
assert.deepEqual(Array.from(replay.frames[0].quads.slice(16)), [255, 128, 0, 128]);
assert.equal(new DataView(replay.frames[0].quads.buffer).getInt16(4, true), 8);
assert.equal(R.parseRecording('window.HARDCORE_RECORDING = ' + JSON.stringify(fixture()) + ';').title, 'Pixel replay');
assert.equal(R.frameAt(replay, -1), 0);
assert.equal(R.frameAt(replay, 999.999), 0);
assert.equal(R.frameAt(replay, 1000), 1);
assert.equal(R.frameAt(replay, 2000), 3, 'latest frame wins for equal timestamps');
assert.equal(R.frameAt(replay, 3000), 3);
assert.equal(R.indexAt([], 0), -1);

// Seeking backwards must restore overwritten glyph slots, including updates
// from frames skipped by the playhead; forward seeking must not clear them.
let atlas = new Uint8Array(16), resets = 0, uploads = 0;
const timeline = new R.AtlasTimeline(replay, patch => {
  uploads++;
  for (let y = 0; y < patch.h; y++) for (let x = 0; x < patch.w; x++) atlas[(y + patch.y) * 4 + x + patch.x] = patch.data[y * patch.w + x];
}, () => { atlas.fill(0); resets++; });
timeline.seek(3); assert.equal(atlas[0], 128); assert.equal(atlas[1], 255); assert.equal(atlas[4], 255); assert.equal(uploads, 2);
timeline.seek(3); assert.equal(uploads, 2, 'same-frame seek avoids atlas uploads');
timeline.seek(0); assert.equal(resets, 1); assert.equal(atlas[0], 0); assert.equal(atlas[1], 255);
timeline.seek(2); assert.equal(atlas[0], 128); assert.equal(resets, 1);
assert.throws(() => timeline.seek(4), /frame index/);

assert.equal(R.pointerAt(replay, 0), null);
assert.equal(R.pointerAt(replay, 10).size, 14, 'movement starts at normal size');
assert.equal(R.pointerAt(replay, 90).size, 23, 'sustained movement grows cursor');
assert.equal(R.pointerAt(replay, 120).x, 9, 'button events hold exact pointer coordinates');
assert.equal(R.pointerAt(replay, 330).size, 14, 'idle cursor returns to native size');
assert.equal(R.pointerAt(replay, 500).size, 14, 'separate movement starts a new growth animation');
assert.deepEqual(R.clicksAt(replay, 99), []);
assert.deepEqual(R.clicksAt(replay, 100), [{x: 9, y: 6, radius: 8, opacity: 1}]);
assert.equal(R.clicksAt(replay, 325)[0].radius, 18);
assert.deepEqual(R.clicksAt(replay, 550), []);
const scrolling = R.parseRecording({...fixture(), pointer: [
  {t: 100, x: 10, y: 20, type: 'scroll', dx: 2, dy: -5},
  {t: 200, x: 10, y: 20, type: 'scroll', dx: 8, dy: 1},
  {t: 300, x: 10, y: 20, type: 'scroll', dx: 0, dy: 0}]});
assert.deepEqual(R.scrollsAt(scrolling, 99), []);
assert.deepEqual(R.scrollsAt(scrolling, 100), [{x: 30, y: 24, ux: 0, uy: -1, opacity: 1}]);
assert.equal(R.scrollsAt(scrolling, 200)[0].ux, 1);
assert.equal(R.scrollsAt(scrolling, 250).length, 1, 'only latest scroll event draws a cue');
assert.equal(R.scrollsAt(scrolling, 300).length, 0, 'latest zero-delta event suppresses the arrow');
assert.deepEqual(R.scrollsAt(scrolling, 650), []);
assert.equal(R.captionsAt(replay, 299), ''); assert.equal(R.captionsAt(replay, 600), 'one\ntwo'); assert.equal(R.captionsAt(replay, 700), 'two'); assert.equal(R.captionsAt(replay, 900), '');
assert.equal(R.timeLabel(61002, true), '1:01.002');
assert.deepEqual(R.cropRect([10, 20, 30, 40], 100, 80), [10, 20, 30, 40]);
assert.deepEqual(R.cameraFit(160, 90, [0, 0, 160, 120]), [0, 15, 160, 90], 'wide camera has 15px black bars in a 4:3 TV');
assert.deepEqual(R.cameraFit(90, 160, [10, 20, 160, 120]), [56.25, 20, 67.5, 120], 'portrait camera stays centered without cropping');
assert.throws(() => R.cameraFit(0, 90, [0, 0, 160, 120]), /camera source width/);
assert.throws(() => R.cropRect([90, 20, 30, 40], 100, 80), /crop must fit/);
assert.throws(() => R.cropRect([-1, 0, 30, 40], 100, 80), /crop must fit/);
assert.throws(() => R.editSpec(replay, {startMs: 100, endMs: 99}), /trim end/);
assert.throws(() => R.editSpec(replay, {startMs: 100, endMs: 100}), /trim must contain/);
assert.throws(() => R.editSpec(replay, {startMs: 0, endMs: Infinity}), /trim end/);
assert.deepEqual(R.editSpec(replay, {startMs: 100, endMs: 200, pointer: false}), {version: 1, startMs: 100, endMs: 200, crop: null, pointer: false, captions: true});
assert.deepEqual(R.parseCaptions('WEBVTT\n\n00:00.100 --> 00:00.500\nHello\nworld\n\n2\n00:00:02,000 --> 00:00:05,000\n<b>Last</b>\n', 3000), [{start: 100, end: 500, text: 'Hello\nworld'}, {start: 2000, end: 3000, text: 'Last'}]);

function rejects(change, expression) { const input = fixture(); change(input); assert.throws(() => R.parseRecording(input), expression); }
rejects(x => x.version = 2, /version/);
rejects(x => x.durationMs = NaN, /duration/);
rejects(x => x.frames = [], /frames/);
rejects(x => x.frames[1].t = -1, /frame time/);
rejects(x => x.frames[2].t = 500, /ordered/);
rejects(x => x.frames[0].w = 100.5, /frame width/);
rejects(x => x.frames[0].h = 0, /frame height/);
rejects(x => { x.frames[0].w = 32767; x.frames[0].h = 32767; }, /excessive/);
rejects(x => x.frames[0].quads = '!!!!', /base64/);
rejects(x => x.frames[0].quads = encoded([0, 1]), /incomplete draw/);
rejects(x => x.frames[0].quads = quad({rect: [5, 0, 4, 9]}), /inverted/);
rejects(x => x.frames[0].quads = quad({uv: [4090, 0, 4100, 10]}), /outside glyph atlas/);
rejects(x => x.frames[0].atlas[0].data = encoded([255]), /pixel count/);
rejects(x => x.frames[0].atlas[0].x = 4095, /glyph width/);
rejects(x => x.frames[0].atlas[0].data = 'A', /base64/);
rejects(x => x.frames[0].camera = {rect: [0, 0, -1, 10]}, /camera rectangle/);
rejects(x => x.pointer[1].t = 9, /pointer times/);
rejects(x => x.pointer[0].x = Infinity, /pointer x/);
rejects(x => x.pointer[0].buttons = 32, /pointer buttons/);
rejects(x => x.pointer[0].type = 'execute', /event type/);
rejects(x => x.captions[0].end = 299, /caption end/);
rejects(x => x.camera.file = '../secret.mp4', /camera filename/);
rejects(x => x.camera.file = 'https://example.com/video.mp4', /camera filename/);
assert.throws(() => R.parseRecording('window.HARDCORE_RECORDING = {}; alert(1)'), /valid JSON/);

// Draw bytes have a bounded LRU lifetime even while the JSON remains loaded.
const cache = new R.QuadCache(40);
const a = cache.get('a', quad()), b = cache.get('b', quad({color: [2, 3, 4, 5]}));
assert.equal(cache.byteLength, 40);
assert.equal(cache.get('a', quad()), a, 'cache reads reuse bytes');
cache.get('c', quad({color: [6, 7, 8, 9]}));
assert(!cache.entries.has('b'), 'least recently used frame evicted');
assert(cache.entries.has('a'));
assert.notEqual(cache.get('b', quad({color: [2, 3, 4, 5]})), b, 'evicted frame decodes again when seeking back');
const oversized = encoded(Buffer.alloc(60));
assert.equal(cache.get('huge', oversized).length, 60);
assert(!cache.entries.has('huge'), 'one oversized frame does not exceed cache bound');
assert.equal(cache.byteLength, 40);
assert.deepEqual(Array.from(replay.frames[0].quads), Array.from(R.bytes(fixture().frames[0].quads, 'fixture', 20)));
console.log('PASS recording replay: native byte layout, atlas seek, timestamp boundaries, pointer cues, trim/crop, captions, malformed bundles');
