/* Hardcore replay v1. This module is also used by the dependency-free tests. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.HardcoreReplay = api;
})(typeof globalThis === 'object' ? globalThis : this, function () {
  'use strict';
  const ATLAS_SIZE = 4096, INSTANCE_BYTES = 20, QUAD_CACHE_BYTES = 8 * 1024 * 1024;
  const MAX_FRAME_PIXELS = 64 * 1024 * 1024;
  function fail(message) { throw new Error('Invalid recording: ' + message); }
  function number(value, label, min, max) {
    if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max) fail(label);
    return value;
  }
  function integer(value, label, min, max) {
    number(value, label, min, max);
    if (!Number.isInteger(value)) fail(label);
    return value;
  }
  function bytes(value, label, limit) {
    if (typeof value !== 'string' || value.length > Math.ceil(limit / 3) * 4 ||
        value.length % 4 || !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) fail(label + ' must be base64');
    const raw = typeof atob === 'function' ? atob(value) : Buffer.from(value, 'base64').toString('binary');
    const decoded = new Uint8Array(raw.length);
    for (let i = 0; i < raw.length; i++) decoded[i] = raw.charCodeAt(i);
    return decoded;
  }
  function rectangle(value, label, max = 32767) {
    if (!Array.isArray(value) || value.length !== 4) fail(label);
    return [number(value[0], label + ' x', -max, max), number(value[1], label + ' y', -max, max),
      number(value[2], label + ' width', 1, max), number(value[3], label + ' height', 1, max)];
  }
  function boolean(value, fallback) { return value === undefined ? fallback : !!value; }
  class QuadCache {
    constructor(limit = QUAD_CACHE_BYTES) { this.limit = limit; this.byteLength = 0; this.entries = new Map(); }
    remember(key, value) {
      if (this.entries.has(key)) { this.byteLength -= this.entries.get(key).byteLength; this.entries.delete(key); }
      if (value.byteLength > this.limit) return value;
      while (this.byteLength + value.byteLength > this.limit && this.entries.size) {
        const oldest = this.entries.keys().next().value;
        this.byteLength -= this.entries.get(oldest).byteLength; this.entries.delete(oldest);
      }
      this.entries.set(key, value); this.byteLength += value.byteLength;
      return value;
    }
    get(key, encoded) {
      if (this.entries.has(key)) { const value = this.entries.get(key); this.entries.delete(key); this.entries.set(key, value); return value; }
      return this.remember(key, bytes(encoded, 'frame quads', INSTANCE_BYTES * 1000000));
    }
  }
  function parseRecording(input) {
    if (typeof input === 'string') {
      let text = input.trim();
      // Accept the native bundle's data assignment without evaluating JavaScript.
      if (text.startsWith('window.HARDCORE_RECORDING')) {
        text = text.replace(/^window\.HARDCORE_RECORDING\s*=\s*/, '').replace(/;\s*$/, '');
      }
      try { input = JSON.parse(text); } catch (_) { fail('the replay is not valid JSON'); }
    }
    if (!input || input.version !== 1) fail('unsupported version');
    const durationMs = number(input.durationMs, 'duration', 0, 7 * 86400000);
    if (!Array.isArray(input.frames) || !input.frames.length || input.frames.length > 2000000) fail('frames are missing or excessive');
    let previous = -1;
    const quadCache = new QuadCache();
    const frames = input.frames.map((frame, index) => {
      if (!frame || typeof frame !== 'object') fail('frame ' + index);
      const t = number(frame.t, 'frame time', 0, durationMs);
      if (t < previous) fail('frame times must be ordered');
      previous = t;
      const w = integer(frame.w, 'frame width', 1, 32767), h = integer(frame.h, 'frame height', 1, 32767);
      if (w * h > MAX_FRAME_PIXELS) fail('frame dimensions are excessive');
      let quads = bytes(frame.quads, 'frame quads', INSTANCE_BYTES * 1000000);
      if (quads.length % INSTANCE_BYTES) fail('incomplete draw command');
      let view = new DataView(quads.buffer, quads.byteOffset, quads.byteLength);
      for (let p = 0; p < quads.length; p += INSTANCE_BYTES) {
        if (view.getInt16(p + 4, true) < view.getInt16(p, true) || view.getInt16(p + 6, true) < view.getInt16(p + 2, true)) fail('inverted draw rectangle');
        const u0 = view.getUint16(p + 8, true), v0 = view.getUint16(p + 10, true);
        const u1 = view.getUint16(p + 12, true), v1 = view.getUint16(p + 14, true);
        if (u0 !== 65535 && (u0 > u1 || v0 > v1 || u1 > ATLAS_SIZE || v1 > ATLAS_SIZE)) fail('draw command outside glyph atlas');
      }
      if (frame.atlas !== undefined && !Array.isArray(frame.atlas)) fail('glyph updates');
      const atlas = (frame.atlas || []).map(patch => {
        if (!patch || typeof patch !== 'object') fail('glyph patch');
        const x = integer(patch.x, 'glyph x', 0, ATLAS_SIZE - 1), y = integer(patch.y, 'glyph y', 0, ATLAS_SIZE - 1);
        const w = integer(patch.w, 'glyph width', 1, ATLAS_SIZE - x), h = integer(patch.h, 'glyph height', 1, ATLAS_SIZE - y);
        const data = bytes(patch.data, 'glyph pixels', w * h);
        if (data.length !== w * h) fail('glyph pixel count');
        return {x, y, w, h, data};
      });
      let camera = null;
      if (frame.camera) camera = {rect: rectangle(frame.camera.rect, 'camera rectangle'), visible: boolean(frame.camera.visible, true), mirrored: boolean(frame.camera.mirrored, true)};
      const encodedQuads = frame.quads;
      quadCache.remember(index, quads);
      // Retain only the bounded shared cache, not every frame's decoded array.
      quads = view = null;
      return {t, w, h, bg: integer(frame.bg ?? 0, 'background color', 0, 0xffffff), transparent: boolean(frame.transparent, false),
        get quads() { return quadCache.get(index, encodedQuads); }, atlas, camera};
    });
    previous = -1;
    if (input.pointer !== undefined && !Array.isArray(input.pointer)) fail('pointer track');
    let motionT = -Infinity, motionRunStart = -Infinity;
    const pointer = (input.pointer || []).map((event, index, all) => {
      if (!event || typeof event !== 'object') fail('pointer event');
      const t = number(event.t, 'pointer time', 0, durationMs);
      if (t < previous) fail('pointer times must be ordered');
      previous = t;
      const x = number(event.x, 'pointer x', -32767, 32767), y = number(event.y, 'pointer y', -32767, 32767);
      if (!['move', 'down', 'up', 'scroll'].includes(event.type || 'move')) fail('pointer event type');
      if ((event.type || 'move') === 'move' || (index && (x !== all[index - 1].x || y !== all[index - 1].y))) {
        if (t - motionT > 240) motionRunStart = t;
        motionT = t;
      }
      return {t, x, y, visible: boolean(event.visible, true), buttons: integer(event.buttons ?? 0, 'pointer buttons', 0, 31),
        type: event.type || 'move', motionT, motionRunStart, dx: number(event.dx ?? 0, 'scroll x', -100000, 100000), dy: number(event.dy ?? 0, 'scroll y', -100000, 100000)};
    });
    if (input.captions !== undefined && !Array.isArray(input.captions)) fail('captions');
    const captions = (input.captions || []).map(caption => {
      if (!caption || typeof caption.text !== 'string' || caption.text.length > 10000) fail('caption text');
      const start = number(caption.start, 'caption start', 0, durationMs), end = number(caption.end, 'caption end', start, durationMs);
      return {start, end, text: caption.text};
    }).sort((a, b) => a.start - b.start);
    let camera = null;
    if (input.camera) {
      const file = input.camera.file;
      if (typeof file !== 'string' || !/^[a-zA-Z0-9][a-zA-Z0-9._ -]*\.(?:mp4|mov|webm)$/i.test(file)) fail('camera filename');
      camera = {file, offsetMs: number(input.camera.offsetMs ?? 0, 'camera offset', -7 * 86400000, 7 * 86400000), mirrored: boolean(input.camera.mirrored, true)};
    }
    return {version: 1, durationMs, frames, pointer, captions, camera,
      title: typeof input.title === 'string' ? input.title.slice(0, 500) : 'Untitled recording',
      createdAt: typeof input.createdAt === 'string' ? input.createdAt : '', raw: input};
  }
  function indexAt(items, time, key = 't') {
    let lo = 0, hi = items.length;
    while (lo < hi) { const mid = (lo + hi) >>> 1; if (items[mid][key] <= time) lo = mid + 1; else hi = mid; }
    return lo - 1;
  }
  function frameAt(recording, time) { return Math.max(0, indexAt(recording.frames, time)); }
  function pointerAt(recording, time) {
    const index = indexAt(recording.pointer, time);
    if (index < 0) return null;
    const event = recording.pointer[index];
    return {...event, size: 14 + 9 * Math.min(1, Math.max(0, (time - event.motionRunStart) / 80)) * Math.max(0, 1 - (time - event.motionT) / 240)};
  }
  function clicksAt(recording, time) {
    const clicks = [];
    for (let i = indexAt(recording.pointer, time); i >= 0 && time - recording.pointer[i].t < 450; i--) {
      const event = recording.pointer[i];
      if (event.type === 'down' && event.visible) clicks.push({x: event.x, y: event.y, radius: 8 + 20 * (time - event.t) / 450, opacity: 1 - (time - event.t) / 450});
    }
    return clicks;
  }
  function scrollsAt(recording, time) {
    const scrolls = [];
    for (let i = indexAt(recording.pointer, time); i >= 0 && time - recording.pointer[i].t < 450; i--) {
      const event = recording.pointer[i];
      if (event.type !== 'scroll') continue;
      if (!event.visible || (!event.dx && !event.dy)) break;
      const horizontal = Math.abs(event.dx) > Math.abs(event.dy);
      scrolls.push({x: event.x + 20, y: event.y + 4, ux: horizontal ? Math.sign(event.dx) : 0,
        uy: horizontal ? 0 : Math.sign(event.dy), opacity: 1 - (time - event.t) / 450});
      break;
    }
    return scrolls;
  }
  function captionsAt(recording, time) { return recording.captions.filter(c => c.start <= time && time < c.end).map(c => c.text).join('\n'); }
  function cropRect(value, w, h) {
    if (value == null) return [0, 0, w, h];
    const crop = rectangle(value, 'crop rectangle');
    const [x, y, cw, ch] = crop.map(Math.round);
    if (x < 0 || y < 0 || cw < 1 || ch < 1 || x + cw > w || y + ch > h) fail('crop must fit the source frame');
    return [x, y, cw, ch];
  }
  function cameraFit(sourceWidth, sourceHeight, rect) {
    number(sourceWidth, 'camera source width', 1, 32767); number(sourceHeight, 'camera source height', 1, 32767);
    const [x, y, w, h] = rectangle(rect, 'camera rectangle');
    const scale = Math.min(w / sourceWidth, h / sourceHeight), dw = sourceWidth * scale, dh = sourceHeight * scale;
    return [x + (w - dw) / 2, y + (h - dh) / 2, dw, dh];
  }
  function editSpec(recording, options) {
    const startMs = number(options.startMs ?? 0, 'trim start', 0, recording.durationMs);
    const endMs = number(options.endMs ?? recording.durationMs, 'trim end', startMs, recording.durationMs);
    if (startMs === endMs && recording.durationMs) fail('trim must contain some time');
    const f = recording.frames[frameAt(recording, startMs)];
    return {version: 1, startMs, endMs, crop: options.crop == null ? null : cropRect(options.crop, f.w, f.h),
      captions: options.captions !== false, pointer: options.pointer !== false};
  }
  function timeLabel(ms, precision = false) {
    const total = Math.max(0, Math.floor(ms / 1000)), minutes = Math.floor(total / 60), seconds = total % 60;
    return minutes + ':' + String(seconds).padStart(2, '0') + (precision ? '.' + String(Math.floor(ms % 1000)).padStart(3, '0') : '');
  }
  function parseCaptions(text, durationMs) {
    const result = [];
    const stamp = '(?:(\\d{1,3}):)?(\\d{2}):(\\d{2})[,.](\\d{3})';
    const timing = new RegExp('^' + stamp + '\\s+-->\\s+' + stamp + '(?:\\s+.*)?$');
    for (const block of text.replace(/\r/g, '').trim().split(/\n\s*\n/)) {
      const lines = block.split('\n'), line = lines.findIndex(value => timing.test(value));
      if (line < 0) continue;
      const match = lines[line].match(timing);
      const ms = p => (Number(match[p] || 0) * 3600 + Number(match[p + 1]) * 60 + Number(match[p + 2])) * 1000 + Number(match[p + 3]);
      const start = ms(1), end = ms(5), caption = lines.slice(line + 1).join('\n').replace(/<[^>]*>/g, '').trim();
      if (start < durationMs && end > start && caption) result.push({start, end: Math.min(end, durationMs), text: caption});
    }
    return result.sort((a, b) => a.start - b.start);
  }
  class AtlasTimeline {
    constructor(recording, upload, clear) { this.recording = recording; this.upload = upload; this.clear = clear; this.index = -1; }
    seek(index) {
      integer(index, 'frame index', 0, this.recording.frames.length - 1);
      if (index < this.index) { this.clear(); this.index = -1; }
      for (let i = this.index + 1; i <= index; i++) for (const patch of this.recording.frames[i].atlas) this.upload(patch);
      this.index = index;
      return this.recording.frames[index];
    }
  }
  return {ATLAS_SIZE, INSTANCE_BYTES, QUAD_CACHE_BYTES, QuadCache, parseRecording, bytes, indexAt, frameAt, pointerAt, clicksAt, scrollsAt, captionsAt, cropRect, cameraFit, editSpec, timeLabel, parseCaptions, AtlasTimeline};
});
