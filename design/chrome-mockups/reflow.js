/*
 * Local, display-only paragraph reflow for the Hardcore chrome prototype.
 * Browser: window.HardcoreReflow.reflow(lines, options)
 * Node:    require('./reflow.js').reflow(lines, options)
 *
 * options = {columns, dock: {x, y, width, height, side: 'right'|'left'}, enabled: true,
 *            maxBlankRows: 3}
 * Coordinates are zero-based grid cells; this prototype counts Unicode code
 * points as cells, matching its bitmap renderer. Width-sensitive glyphs and
 * raw ANSI/control sequences are protected; this is not a terminal parser.
 *
 * The returned lines always have the input height. Paragraphs may use their
 * own rows and immediately following blank rows, never displace later text.
 * Existing row capacity is tried before blank rows. Every accepted layout
 * keeps indentation and whole words, and reduces occupied dock cells.
 * A left dock shifts only intersecting rows to its right edge, adding the
 * source indentation; rows outside the dock use the full window width.
 * No terminal contents are edited: callers render this separate line array.
 * Always reflow the original terminal rows when moving/resizing the dock.
 * A second pass with the same dock is harmless (accepted rows already clear
 * it), but strings alone cannot recover source indentation after a dock move.
 *
 * Result: {lines, changes, blocks, markers, stats}
 * changes: accepted paragraphs, before/after rows, rowOffsets (dock-induced
 *          horizontal shift for each after row), and overlap measurements.
 * blocks: prose candidates and protected runs, with status and explanation.
 * markers: changed nonblank rows, at column offset+indent-1. For unindented text,
 *          column=null and placed=false: the renderer must not invent a gutter.
 * stats: collision counts, changed paragraph/line counts, and blank rows used.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  if (root) root.HardcoreReflow = api;
})(typeof window !== 'undefined' ? window : null, function () {
  'use strict';

  const cells = s => Array.from(s).length;
  const rightTrim = s => s.replace(/ +$/, '');
  const integer = (value, fallback) => Number.isFinite(value) ? Math.floor(value) : fallback;
  const lexicalWord = word => /[A-Za-z\u00c0-\u024f]/.test(word);

  function classify(line, fenced) {
    const text = rightTrim(line);
    const body = text.trimStart();
    const indent = text.length - body.length;
    const words = body ? body.split(/ +/) : [];
    const info = {text, body, indent, words};
    const protectedLine = reason => ({...info, type: 'protected', reason});
    if (/^\s*$/.test(text)) return {...info, type: 'blank'};
    if (/^(?:```|~~~)/.test(body)) return protectedLine('Code fence');
    if (fenced) return protectedLine('Fenced code');
    if (/\t|\x1b|[\x00-\x08]/.test(text)) return protectedLine('Tabs, escapes, or control characters');
    if (/^[-_=\s\d.,:;!?—–]+$/u.test(body)) return protectedLine('Numeric heading or separator');
    if (/^(?:[$%>#❯›]|(?:\S+@\S+|\S*:\S*)[#$%>]\s)/.test(body)) return protectedLine('Prompt or quoted command');
    if (/^(?:[-*+•◦▪⏺]\s|\d+[.)]\s|\[[ xX]\]\s)/.test(body)) return protectedLine('List boundary');
    if (/[\u2500-\u257f|]/.test(body) || /\S {2,}\S/.test(body)) return protectedLine('Table, terminal chrome, or aligned columns');
    if (/\ufffd/.test(body)) return protectedLine('Incomplete or malformed capture glyph');
    if (/[\p{M}\u1100-\u11ff\u2e80-\ua4cf\uac00-\ud7af\uf900-\ufaff\ufe00-\ufe0f\uff01-\uff60\uffe0-\uffe6\u{10000}-\u{10ffff}]/u.test(body)) return protectedLine('Wide or combining glyphs need terminal cell metadata');
    if (/[{}=]|=>|::|`|(?:https?|file):\/\//.test(body)) return protectedLine('Code, URL, or literal syntax');
    if (/^(?:const|let|var|function|class|def|import|from|return|export|if|else|for|while|switch|case|try|catch|throw|print|printf|console\.)\b/.test(body)) return protectedLine('Code-like statement');
    if (/^(?:\/|~\/|\.\.?\/|[A-Za-z]:\\)\S/.test(body)) return protectedLine('Path or command');
    if (/^[\w./-]+\s*\([^)]*\)\s*;?$/.test(body) || /[;{}]$/.test(body)) return protectedLine('Code-like statement');
    if (words.some(word => /[_\\]/.test(word)) || words.filter(lexicalWord).length < Math.ceil(words.length * 0.6)) return protectedLine('Identifiers or structured data');
    if (!words.some(lexicalWord)) return protectedLine('Non-prose line');
    const titleWords = words.filter(word => !/^(?:a|an|and|or|the|of|to|for|in|with)$/.test(word));
    if (words.length <= 8 && cells(body) <= 60 && titleWords.length && titleWords.every(word => /^[A-Z][A-Za-z'-]*$/.test(word))) return protectedLine('Title-style heading');
    if (words.length <= 8 && cells(body) <= 60 && /:$/.test(body)) return protectedLine('Section label');
    return {...info, type: 'prose'};
  }

  function overlap(lines, dock) {
    let occupied = 0;
    const end = Math.min(lines.length, dock.y + dock.height);
    for (let row = Math.max(0, dock.y); row < end; row++) {
      const chars = Array.from(lines[row]);
      for (let col = Math.max(0, dock.x); col < Math.min(chars.length, dock.x + dock.width); col++) {
        if (!/\s/u.test(chars[col])) occupied++;
      }
    }
    return occupied;
  }

  function rowLayout(row, columns, dock) {
    if (row < dock.y || row >= dock.y + dock.height || !dock.width) return {offset: 0, width: columns};
    if (dock.side === 'left') {
      const offset = Math.min(columns, Math.max(0, dock.x + dock.width));
      return {offset, width: columns - offset};
    }
    return {offset: 0, width: Math.min(columns, Math.max(0, dock.x))};
  }

  function wrap(words, indent, startRow, rowBudget, columns, dock) {
    const output = [], rowOffsets = [];
    let word = 0;
    for (let offset = 0; offset < rowBudget && word < words.length; offset++) {
      const layout = rowLayout(startRow + offset, columns, dock);
      const width = layout.width - indent;
      const first = word;
      let used = 0;
      while (word < words.length) {
        const next = cells(words[word]) + (word === first ? 0 : 1);
        if (used + next > width) break;
        used += next;
        word++;
      }
      // A word that cannot fit is never split, hidden, or moved past a blank
      // gap within a paragraph. The underlay will remain beneath this text.
      if (first === word) return {ok: false, reason: 'A whole word cannot fit beside the dock'};
      output.push(' '.repeat(layout.offset + indent) + words.slice(first, word).join(' '));
      rowOffsets.push(layout.offset);
    }
    if (word < words.length) return {ok: false, reason: 'Not enough local rows for all whole words'};
    return {ok: true, lines: output, rowOffsets};
  }

  function reflow(input, options = {}) {
    if (!Array.isArray(input) || input.some(line => typeof line !== 'string')) throw new TypeError('reflow expects an array of strings');
    const lines = input.slice();
    const columns = Math.max(0, integer(options.columns, 128));
    const d = options.dock || {};
    const side = d.side === 'left' ? 'left' : 'right';
    const dock = {side, x: integer(d.x, side === 'left' ? 0 : columns), y: integer(d.y, 0), width: Math.max(0, integer(d.width, 0)), height: Math.max(0, integer(d.height, 0))};
    const maxBlankRows = Math.max(0, integer(options.maxBlankRows, 3));
    const changes = [], blocks = [], markers = [];
    const beforeOverlap = overlap(lines, dock);
    const result = () => ({lines, changes, blocks, markers, stats: {
      enabled: options.enabled !== false,
      paragraphsChanged: changes.length,
      linesChanged: lines.reduce((total, line, row) => total + (line !== input[row] ? 1 : 0), 0),
      blankRowsUsed: changes.reduce((total, change) => total + change.consumedBlankRows, 0),
      overlapBefore: beforeOverlap, overlapAfter: overlap(lines, dock),
      cellsFreed: beforeOverlap - overlap(lines, dock),
      protectedBlocks: blocks.filter(block => block.status === 'protected').length,
      rejectedBlocks: blocks.filter(block => block.status === 'rejected').length
    }});
    if (options.enabled === false || !columns || !dock.width || !dock.height) return result();

    // Classify once from original rows. Accepted reflow cannot recruit a later
    // block or accidentally turn new output into another paragraph candidate.
    let fenced = false;
    const infos = input.map(line => {
      const info = classify(line, fenced);
      if (/^(?:```|~~~)/.test(info.body)) fenced = !fenced;
      return info;
    });
    for (let row = 0; row < infos.length;) {
      const first = infos[row];
      if (first.type === 'blank') { row++; continue; }
      if (first.type === 'protected' || first.words.length < 2) {
        const reason = first.reason || 'Isolated heading or identifier';
        const startRow = row++;
        while (row < infos.length && infos[row].type === 'protected' && infos[row].reason === reason) row++;
        blocks.push({startRow, endRow: row - 1, status: 'protected', reason});
        continue;
      }

      const startRow = row;
      const indent = first.indent;
      row++;
      while (row < infos.length && infos[row].type === 'prose' && infos[row].indent === indent) {
        const previous = infos[row - 1], next = infos[row];
        // A new capitalized line after a short row is ambiguous: it could be
        // another instruction/heading. Prefer literal rows over a false join.
        if (/^[A-Z]/.test(next.body) && cells(previous.body) < columns * 0.72) break;
        if (/:$/.test(previous.body)) break;
        // A one-word tail can continue a wrapped paragraph; a new standalone
        // capitalized title following a completed sentence cannot.
        if (next.words.length === 1 && /[.!?:]$/.test(previous.body)) break;
        row++;
      }
      const sourceEndRow = row - 1;
      const sourceRows = row - startRow;
      const block = {startRow, endRow: sourceEndRow, indent, sourceRows, status: 'unchanged', reason: ''};
      blocks.push(block);
      const originalOverlap = overlap(input, {...dock, y: Math.max(startRow, dock.y), height: Math.max(0, Math.min(row, dock.y + dock.height) - Math.max(startRow, dock.y))});
      block.overlapBefore = originalOverlap;
      if (!originalOverlap) { block.reason = 'Paragraph already clears the dock'; continue; }
      if (infos.slice(startRow, row).some(info => cells(info.text) > columns)) {
        block.status = 'rejected'; block.reason = 'Source line exceeds the visible grid'; continue;
      }
      // Do not reinterpret continuation rows under a bullet as a free prose
      // block; list indentation carries structure even when a line looks plain.
      if (startRow > 0 && infos[startRow - 1].reason === 'List boundary') {
        block.status = 'protected'; block.reason = 'List continuation'; continue;
      }
      let blankRows = 0;
      while (blankRows < maxBlankRows && row + blankRows < infos.length && infos[row + blankRows].type === 'blank') blankRows++;
      const words = infos.slice(startRow, row).flatMap(info => info.words);
      let attempt, accepted = null;
      for (let extra = 0; extra <= blankRows; extra++) {
        attempt = wrap(words, indent, startRow, sourceRows + extra, columns, dock);
        if (attempt.ok) { accepted = attempt; break; }
      }
      if (!accepted) {
        block.status = 'rejected'; block.reason = attempt.reason;
        block.availableBlankRows = blankRows;
        continue;
      }
      const rowsUsed = accepted.lines.length;
      const consumedBlankRows = Math.max(0, rowsUsed - sourceRows);
      const affectedRows = Math.max(sourceRows, rowsUsed);
      const before = input.slice(startRow, startRow + affectedRows);
      const after = accepted.lines.concat(Array(Math.max(0, sourceRows - rowsUsed)).fill(''));
      const rowOffsets = accepted.rowOffsets.concat(Array(Math.max(0, sourceRows - rowsUsed)).fill(0));
      const candidate = lines.slice();
      after.forEach((line, offset) => { candidate[startRow + offset] = line; });
      const gained = overlap(lines, dock) - overlap(candidate, dock);
      if (gained <= 0) { block.status = 'rejected'; block.reason = 'Rewrapping would not free occupied dock cells'; continue; }
      after.forEach((line, offset) => {
        const index = startRow + offset;
        lines[index] = line;
        if (line.trim() && rightTrim(line) !== rightTrim(input[index])) markers.push({row: index, column: indent ? rowOffsets[offset] + indent - 1 : null, placed: indent > 0, offset: rowOffsets[offset], indent, blockStart: startRow});
      });
      const change = {startRow, endRow: startRow + affectedRows - 1, sourceEndRow, rowsUsed, consumedBlankRows, before, after, rowOffsets, side, indent, overlapBefore: originalOverlap, overlapAfter: originalOverlap - gained};
      changes.push(change);
      Object.assign(block, {status: 'reflowed', endRow: change.endRow, rowsUsed, consumedBlankRows, overlapAfter: change.overlapAfter,
        reason: consumedBlankRows ? `Used ${consumedBlankRows} following blank ${consumedBlankRows === 1 ? 'row' : 'rows'}; later text stays fixed` : 'Used spare capacity within the paragraph’s existing rows'});
      row += consumedBlankRows;
    }
    return result();
  }

  return {reflow};
});
