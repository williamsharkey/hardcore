/* Hairline Lab: experimental bitmap variations derived from Terminus 12x24.
 * Original glyphs: Copyright (C) 2020 Dimitar Toshkov Zhekov, SIL OFL 1.1.
 * This is a shape experiment, not a production font or a general font scaler.
 * See the accompanying attribution and original OFL license.
 */
(function (root) {
  'use strict';

  const WIDTH = 12;
  const HEIGHT = 24;
  const ASCENT = 19; // Baseline boundary; the last ordinary ink row is 18.
  const CAP_TOP = 4;
  const X_TOP = 8;
  const LIGHT_ARMS = Object.freeze({
    0x2500: 'lr', 0x2502: 'ud',
    0x250c: 'dr', 0x2510: 'dl', 0x2514: 'ur', 0x2518: 'ul',
    0x251c: 'udr', 0x2524: 'udl', 0x252c: 'dlr', 0x2534: 'ulr',
    0x253c: 'udlr',
    0x2574: 'l', 0x2575: 'u', 0x2576: 'r', 0x2577: 'd',
  });

  function integerOption(value, fallback, min, max, name) {
    const n = value === undefined ? fallback : value;
    if (!Number.isInteger(n) || n < min || n > max) {
      throw new RangeError(`${name} must be an integer from ${min} to ${max}`);
    }
    return n;
  }

  function pixelColumns(row, width) {
    const columns = [];
    for (let x = 0; x < width; x++) {
      if (row & (1 << (width - 1 - x))) columns.push(x);
    }
    return columns;
  }

  function isolatedColumns(columns) {
    return columns.every((x, i) => !i || x - columns[i - 1] > 1);
  }

  // A seam may cross only isolated, one-pixel strokes, in the same order,
  // with at most a one-pixel diagonal step. Never duplicate a crossbar, cap,
  // serif, corner or horizontal run. Every shipped ASCII alphanumeric has
  // such a seam. If a different input does not, fail visibly instead of
  // silently falling back to nearest-neighbour scaling.
  function findSeams(rows, start, width) {
    const candidates = [];
    for (let y = start + 2; y < ASCENT - 2; y++) {
      const upper = pixelColumns(rows[y - 1], width);
      const lower = pixelColumns(rows[y], width);
      if (!upper.length || upper.length !== lower.length ||
          !isolatedColumns(upper) || !isolatedColumns(lower) ||
          !upper.every((x, i) => Math.abs(x - lower[i]) <= 1)) continue;
      candidates.push({y, upper, lower});
    }
    return candidates;
  }

  function checkBase(base) {
    if (!base || base.width !== WIDTH || base.height !== HEIGHT || base.ascent !== ASCENT ||
        !base.glyphs || typeof base.glyphs !== 'object') {
      throw new TypeError('Hairline Lab requires a 12x24 source with ascent 19');
    }
  }

  function checkGlyph(rows, cp) {
    if (!Number.isInteger(cp) || cp < 0 || cp > 0x10ffff ||
        !Array.isArray(rows) || rows.length !== HEIGHT ||
        rows.some(row => !Number.isInteger(row) || row < 0 || row >= (1 << WIDTH))) {
      throw new TypeError(`Invalid 12x24 bitmap for ${cp}`);
    }
  }

  function settings(options) {
    const requestedCellHeight = integerOption(options.cellHeight, HEIGHT, HEIGHT, 32, 'cellHeight');
    const xHeightDelta = integerOption(options.xHeightDelta, 0, 0, 6, 'xHeightDelta');
    const capHeightDelta = integerOption(options.capHeightDelta, 0, 0, 6, 'capHeightDelta');
    if (options.redrawBoxes !== undefined && typeof options.redrawBoxes !== 'boolean') {
      throw new TypeError('redrawBoxes must be a boolean');
    }
    const glyphSeams = options.glyphSeams === undefined ? {} : options.glyphSeams;
    if (!glyphSeams || typeof glyphSeams !== 'object' || Array.isArray(glyphSeams)) {
      throw new TypeError('glyphSeams must map codepoints to insertion-boundary arrays');
    }
    for (const [key, boundaries] of Object.entries(glyphSeams)) {
      const cp = Number(key);
      if (!Number.isInteger(cp) || cp < 0 || cp > 0x10ffff || !Array.isArray(boundaries) ||
          boundaries.length > 6 || boundaries.some(y => !Number.isInteger(y) || y < 0 || y > HEIGHT)) {
        throw new TypeError(`Invalid insertion boundaries for ${key}`);
      }
    }
    // Caps, the f hook and i/j dots start at row 4. Retain two clear rows even
    // with six rows of extension. Cell-height growth is leading, not scaling.
    const cellHeight = Math.max(requestedCellHeight, 22 + Math.max(xHeightDelta, capHeightDelta));
    return {
      requestedCellHeight, cellHeight, xHeightDelta, capHeightDelta,
      topOffset: cellHeight - HEIGHT, topPaddingAdded: cellHeight - requestedCellHeight,
      ascenderDelta: Math.max(0, xHeightDelta - 2),
      redrawBoxes: options.redrawBoxes === true, glyphSeams,
    };
  }

  function planGlyph(rows, codepoint, config) {
    const lowerCase = codepoint >= 97 && codepoint <= 122;
    const capitalOrDigit = (codepoint >= 65 && codepoint <= 90) || (codepoint >= 48 && codepoint <= 57);
    const delta = lowerCase ? config.xHeightDelta : capitalOrDigit ? config.capHeightDelta : 0;
    const start = lowerCase ? X_TOP : CAP_TOP;
    const safe = lowerCase || capitalOrDigit ? findSeams(rows, start, WIDTH) : [];
    if (delta && !safe.length) {
      throw new Error(`No safe one-pixel seam for U+${codepoint.toString(16).toUpperCase()}`);
    }
    const requested = config.glyphSeams[codepoint] || [];
    const seams = Array.from({length: delta}, (_, index) => {
      // Spread additions through the body. In e +2, this chooses boundaries
      // 11 and 15, on opposite sides of its single-row crossbar at source 13.
      const target = requested[index] === undefined ?
        Math.round(start + (ASCENT - 1 - start) * (index + 1) / (delta + 1)) : requested[index];
      let selected = safe[0];
      for (const candidate of safe) {
        if (Math.abs(candidate.y - target) < Math.abs(selected.y - target)) selected = candidate;
      }
      return {index, requested: target, actual: selected.y, targetRow: null};
    });
    // Equal-boundary insertions remain individually addressable in stable
    // override-array order. Changing height only offsets their output rows.
    for (const boundary of new Set(seams.map(seam => seam.actual))) {
      const group = seams.filter(seam => seam.actual === boundary);
      const below = seams.filter(seam => seam.actual > boundary).length;
      group.forEach((seam, index) => {
        seam.targetRow = boundary + config.topOffset - below - group.length + index;
      });
    }
    return {
      codepoint, delta, lowerCase, seams, safeBoundaries: safe.map(seam => seam.y),
      requestedCellHeight: config.requestedCellHeight, cellHeight: config.cellHeight,
      topOffset: config.topOffset,
    };
  }

  function extendGlyph(rows, plan, config) {
    const out = Array(config.cellHeight).fill(0);
    for (let y = 0; y < HEIGHT; y++) {
      let shift = plan.seams.filter(seam => seam.actual > y).length;
      if (plan.lowerCase && y < X_TOP) {
        const protectedTop = plan.codepoint === 102 || plan.codepoint === 105 || plan.codepoint === 106;
        // Move f's hook with its crossbar, and i/j dots with their stems.
        // Otherwise raising the body collapses the hook neck or dot gap.
        // Plain ascenders remain fixed at +0..2;
        // then rise just enough to stay two rows above the larger x-height.
        shift = protectedTop ? plan.delta : config.ascenderDelta;
      }
      const target = y + config.topOffset - shift;
      if (target >= 0) out[target] |= rows[y];
      else if (rows[y]) throw new Error('Variation would clip the top of a glyph');
    }
    // Only isolated one-pixel paths bridge each gap. Whole horizontal rows
    // are never copied into an insertion, even when the user requests one.
    for (const boundary of new Set(plan.seams.map(seam => seam.actual))) {
      const group = plan.seams.filter(seam => seam.actual === boundary);
      const upper = pixelColumns(rows[boundary - 1], WIDTH);
      const lower = pixelColumns(rows[boundary], WIDTH);
      for (let i = 0; i < upper.length; i++) {
        group.forEach((seam, index) => {
          const x = Math.round(upper[i] + (lower[i] - upper[i]) * (index + 1) / (group.length + 1));
          out[seam.targetRow] |= 1 << (WIDTH - 1 - x);
        });
      }
    }
    return out;
  }

  function lightBox(codepoint, width, height) {
    const arms = LIGHT_ARMS[codepoint];
    if (!arms) return null;
    const rows = Array(height).fill(0);
    const cx = Math.floor((width - 1) / 2);
    const cy = Math.floor((height - 1) / 2);
    const bit = 1 << (width - 1 - cx);
    if (arms.includes('u')) for (let y = 0; y <= cy; y++) rows[y] |= bit;
    if (arms.includes('d')) for (let y = cy; y < height; y++) rows[y] |= bit;
    if (arms.includes('l')) for (let x = 0; x <= cx; x++) rows[cy] |= 1 << (width - 1 - x);
    if (arms.includes('r')) for (let x = cx; x < width; x++) rows[cy] |= 1 << (width - 1 - x);
    return rows;
  }

  function makeFont(base, options = {}) {
    checkBase(base);
    const config = settings(options);
    const {cellHeight, requestedCellHeight, xHeightDelta, capHeightDelta, redrawBoxes,
      topOffset, topPaddingAdded, ascenderDelta} = config;
    const glyphs = {};
    const placements = {};
    const safeBoundaries = {};
    for (const [key, original] of Object.entries(base.glyphs)) {
      const cp = Number(key);
      checkGlyph(original, cp);
      const plan = planGlyph(original, cp, config);
      const box = redrawBoxes ? lightBox(cp, WIDTH, cellHeight) : null;
      glyphs[key] = box || extendGlyph(original, plan, config);
      if (plan.safeBoundaries.length) {
        placements[key] = plan.seams;
        safeBoundaries[key] = plan.safeBoundaries;
      }
    }
    return {
      ...base,
      id: `hairline-lab-${cellHeight}-${xHeightDelta}-${capHeightDelta}-${redrawBoxes ? 1 : 0}`,
      name: 'Hairline Lab',
      sourceName: base.name,
      height: cellHeight,
      ascent: ASCENT + topOffset,
      glyphs,
      metrics: {
        baseline: ASCENT + topOffset,
        baselineInk: ASCENT - 1 + topOffset,
        capTop: CAP_TOP - capHeightDelta + topOffset,
        xTop: X_TOP - xHeightDelta + topOffset,
        ascenderTop: CAP_TOP - ascenderDelta + topOffset,
        topOffset,
        descent: HEIGHT - ASCENT,
      },
      experiment: {
        xHeightDelta, capHeightDelta, cellHeight, requestedCellHeight,
        redrawBoxes, topPaddingAdded, ascenderDelta,
        method: 'Distributed sparse one-pixel seam insertion; no bitmap scaling',
        placements, safeBoundaries,
        boxCodepoints: redrawBoxes ? Object.keys(LIGHT_ARMS).map(Number) : [],
        limitations: [
          'Only ASCII a-z, A-Z and 0-9 have their contours modified.',
          'Plain lowercase ascenders stay fixed through +2, then rise to retain two rows above x-height; the f hook and i/j dots move with x-height to preserve their shape and spacing.',
          'Extra cell height adds leading above the source glyph, keeping its five-pixel descent.',
          'Cell height grows automatically when needed to preserve two clear rows above capitals, the f hook and i/j dots.',
          'Only basic light straight box strokes, corners, tees and half-lines are redrawn.',
          'These experimental contours require visual review before use as a font.',
        ],
      },
    };
  }

  function getGlyphPlan(base, codepoint, options = {}) {
    checkBase(base);
    const rows = base.glyphs[codepoint];
    checkGlyph(rows, codepoint);
    return planGlyph(rows, codepoint, settings(options));
  }

  root.HairlineVariant = Object.freeze({makeFont, getGlyphPlan});
  if (typeof module !== 'undefined' && module.exports) module.exports = root.HairlineVariant;
})(typeof globalThis !== 'undefined' ? globalThis : this);
