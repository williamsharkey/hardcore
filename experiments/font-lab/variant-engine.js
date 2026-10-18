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
    const requestedCellHeight = integerOption(options.cellHeight, HEIGHT, HEIGHT, 36, 'cellHeight');
    const arrows = options.arrows ?? null;
    if (arrows !== null && (!Array.isArray(arrows) || arrows.length > 24 || arrows.some(a =>
      !a || !['all','lower','caps'].includes(a.scope) || !['up','down'].includes(a.direction) ||
      !Number.isInteger(a.row) || a.row < 0 || a.row > HEIGHT))) throw new TypeError('Invalid explicit insertion arrows');
    const xHeightDelta = integerOption(arrows ? arrows.filter(a=>a.scope!=='caps').length : options.xHeightDelta, 0, 0, 12, 'xHeightDelta');
    const capHeightDelta = integerOption(arrows ? arrows.filter(a=>a.scope!=='lower').length : options.capHeightDelta, 0, 0, 12, 'capHeightDelta');
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
          boundaries.length > 12 || boundaries.some(y => !Number.isInteger(y) || y < 0 || y > HEIGHT)) {
        throw new TypeError(`Invalid insertion boundaries for ${key}`);
      }
    }
    const globalSeams = options.globalSeams ?? [];
    if (!Array.isArray(globalSeams) || globalSeams.length > 12 || globalSeams.some(seam =>
      !seam || !Number.isInteger(seam.row) || seam.row < 0 || seam.row > HEIGHT ||
      !['up', 'down'].includes(seam.direction))) {
      throw new TypeError('globalSeams must contain up to twelve {row: 0..24, direction: up/down} arrows');
    }
    const placementMode = options.placementMode ?? 'glyph'; // Preserve version-one recipes.
    if (!['global', 'glyph'].includes(placementMode)) throw new TypeError('Invalid placementMode');
    const descenderDelta = integerOption(options.descenderDelta, 0, 0, 6, 'descenderDelta');
    const dotRaise = integerOption(options.dotRaise, 0, 0, 4, 'dotRaise');
    const ascenderRaise = integerOption(options.ascenderRaise, 0, 0, 4, 'ascenderRaise');
    const linkAscenders = options.linkAscenders === true;
    const ascenderDelta = Math.max(0, xHeightDelta - 2, linkAscenders ? capHeightDelta : 0) + ascenderRaise;
    const eBarShift = integerOption(options.eBarShift, 0, -2, 2, 'eBarShift');
    // Caps, the f hook and i/j dots start at row 4. Retain two clear rows even
    // at maximum extension. Cell-height growth is leading, not scaling.
    const cellHeight = Math.max(requestedCellHeight, HEIGHT + descenderDelta, 22 + descenderDelta + Math.max(
      xHeightDelta + Math.max(0, dotRaise), capHeightDelta,
      ascenderDelta));
    return {
      requestedCellHeight, cellHeight, xHeightDelta, capHeightDelta, arrows,
      topOffset: cellHeight - HEIGHT - descenderDelta, topPaddingAdded: cellHeight - requestedCellHeight,
      ascenderDelta, linkAscenders,
      redrawBoxes: options.redrawBoxes === true, glyphSeams, globalSeams, placementMode, descenderDelta, dotRaise, eBarShift,
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
    const requested = config.placementMode === 'glyph' ? config.glyphSeams[codepoint] || [] : [];
    const explicit = config.arrows?.map((arrow,globalIndex)=>({...arrow,globalIndex})).filter(a=>lowerCase ? a.scope!=='caps' : capitalOrDigit ? a.scope!=='lower' : false);
    const seams = Array.from({length: delta}, (_, index) => {
      // Spread additions through the body. In e +2, this chooses boundaries
      // 11 and 15, on opposite sides of its single-row crossbar at source 13.
      const globalCount = Math.max(config.xHeightDelta, config.capHeightDelta);
      const globalIndex = explicit ? explicit[index].globalIndex : Math.round((index + .5) * globalCount / delta - .5);
      const globalArrow = explicit ? explicit[index] : config.placementMode === 'global' ? config.globalSeams[globalIndex] || {row:Math.round(8 + 10 * (globalIndex + 1) / (Math.max(config.xHeightDelta, config.capHeightDelta) + 1)),direction:'up'} : null;
      const target = globalArrow ? globalArrow.row : requested[index] === undefined ?
        Math.round(start + (ASCENT - 1 - start) * (index + 1) / (delta + 1)) : requested[index];
      const directional = globalArrow ? safe.filter(candidate => globalArrow.direction === 'up' ?
        candidate.y <= target : candidate.y >= target) : safe;
      // A glyph can have no safe row in the preferred direction. Use its nearest
      // safe row and report the fallback, rather than breaking a stem or bar.
      const candidates = directional.length ? directional : safe;
      let selected = candidates[0];
      for (const candidate of candidates) {
        if (Math.abs(candidate.y - target) < Math.abs(selected.y - target)) selected = candidate;
      }
      return {index, globalIndex: explicit || config.placementMode === 'global' ? globalIndex : index, requested: target, actual: selected.y, targetRow: null,
        direction: globalArrow?.direction ?? 'nearest', fallback: !!globalArrow && !directional.length};
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
        if ((plan.codepoint === 105 || plan.codepoint === 106) && y < 7) shift += config.dotRaise;
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
    // Join a raised plain ascender back into the body without copying a bar.
    if (plan.lowerCase && ![102, 105, 106].includes(plan.codepoint) && config.ascenderDelta > plan.delta) {
      const mask = rows[X_TOP - 1] & rows[X_TOP];
      const isolated = mask & ~(mask << 1) & ~(mask >>> 1);
      for (let y = X_TOP + config.topOffset - config.ascenderDelta;
        y < X_TOP + config.topOffset - plan.delta; y++) out[y] |= isolated;
    }
    if (config.descenderDelta && 'gjpqy'.includes(String.fromCodePoint(plan.codepoint))) {
      const boundary = ASCENT + config.topOffset;
      const upper = out[boundary - 1], lower = out[boundary];
      const bridge = upper & lower;
      out.splice(boundary, 0, ...Array(config.descenderDelta).fill(bridge));
      out.length = config.cellHeight;
    }
    if (plan.codepoint === 101 && config.eBarShift) {
      // Move only the interior of e's crossbar; keep the left contour intact.
      const bars = out.map((mask,y)=>({mask,y, columns:pixelColumns(mask,WIDTH)}))
        .filter(({columns,y})=>columns.length >= 5 && y > 0 && out[y-1] && out[y+1]);
      const bar = bars.find(({y})=>y > out.findIndex(Boolean) + 1);
      if (bar) {
        const top = out.findIndex(Boolean), bottom = out.findLastIndex(Boolean);
        const target = Math.max(top+2, Math.min(bottom-2, bar.y + config.eBarShift));
        const left = 1 << (WIDTH - 1 - bar.columns[0]);
        out[bar.y] &= left;
        out[target] |= bar.mask;
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

  // A fixed canvas has no typography-derived minimum, padding or automatic growth.
  // Guide rows are explicit pixel coordinates; crossing them intentionally folds
  // or compresses the contours. Collapsed rows merge ink rather than vanish.
  const SOURCE_LINES = Object.freeze({capTop:4,xTop:8,baseline:18,descender:22,
    ascender:4,dot:4,eBar:13,numeral:4});
  function makeFixedFont(base, options) {
    const height=options.cellHeight ?? HEIGHT;
    if(!Number.isSafeInteger(height)||height<1)throw new RangeError('Canvas height must be a positive whole number');
    const requested={...SOURCE_LINES,...options.lineRows};
    const lines={};
    for(const [key,value] of Object.entries(requested)) {
      if(!Number.isSafeInteger(value))throw new TypeError('Guide rows must be whole numbers');
      lines[key]=Math.max(0,Math.min(height-1,value));
    }
    const arrows=options.arrows ?? [];
    if(!Array.isArray(arrows)||arrows.some(a=>!a||!['all','lower','caps'].includes(a.scope)||
      !['up','down'].includes(a.direction)||!Number.isInteger(a.row)||a.row<0||a.row>HEIGHT))throw new TypeError('Invalid insertion arrows');
    const glyphs={},placements={},safeBoundaries={},rowMaps={};
    for(const [key,rows] of Object.entries(base.glyphs)) {
      const cp=Number(key);checkGlyph(rows,cp);
      const lower=cp>=97&&cp<=122,cap=cp>=65&&cp<=90,digit=cp>=48&&cp<=57;
      const safe=lower||cap||digit?findSeams(rows,lower?X_TOP:CAP_TOP,WIDTH):[];
      const seams=arrows.map((a,globalIndex)=>({...a,globalIndex})).filter(a=>lower?a.scope!=='caps':cap||digit?a.scope!=='lower':false).map(a=>{
        const directional=safe.filter(s=>a.direction==='up'?s.y<=a.row:s.y>=a.row), candidates=directional.length?directional:safe;
        const actual=candidates.length?candidates.reduce((best,s)=>Math.abs(s.y-a.row)<Math.abs(best.y-a.row)?s:best).y:a.row;
        return {globalIndex:a.globalIndex,requested:a.row,actual,direction:a.direction,fallback:!directional.length};
      });
      const top=lower?(('ij'.includes(String.fromCodePoint(cp)))?lines.dot:'bdhkl'.includes(String.fromCodePoint(cp))?lines.ascender:lines.capTop):digit?lines.numeral:lines.capTop;
      const anchors=[[0,0],[4,top]];
      if(lower)anchors.push([8,lines.xTop]);
      if(cp===101)anchors.push([13,lines.eBar]);
      anchors.push([18,lines.baseline],[22,lines.descender],[23,height-1]);
      const weight=y=>1+seams.filter(a=>a.actual===y).length;
      const map=Array.from({length:HEIGHT},(_,y)=>{
        const i=anchors.findIndex((a,index)=>index<anchors.length-1&&y>=a[0]&&y<=anchors[index+1][0]);
        const [a,b]=[anchors[Math.max(0,i)],anchors[Math.max(0,i)+1]];
        let total=0,part=0;
        for(let r=a[0]+1;r<=b[0];r++){total+=weight(r);if(r<=y)part+=weight(r);}
        return Math.round(a[1]+(b[1]-a[1])*(total?part/total:0));
      });
      const out=Array(height).fill(0);
      for(let y=0;y<HEIGHT;y++) {
        out[map[y]]|=rows[y];
        if(!y||map[y]===map[y-1])continue;
        // Join existing neighboring contour pixels, not whole horizontal bars.
        const above=pixelColumns(rows[y-1],WIDTH),below=pixelColumns(rows[y],WIDTH);
        // Reserve vertical matches before diagonal joins so a corner cannot
        // create two parallel bridges into the same one-pixel stem.
        const used=new Set(above.filter(x=>below.includes(x)));
        for(const x of above) {
          const target=below.includes(x)?x:below.find(v=>!used.has(v)&&Math.abs(v-x)===1);
          if(target===undefined)continue;
          used.add(target);
          const distance=Math.abs(map[y]-map[y-1]),sign=Math.sign(map[y]-map[y-1]);
          for(let n=1;n<distance;n++)out[map[y-1]+sign*n]|=1<<(WIDTH-1-Math.round(x+(target-x)*n/distance));
        }
      }
      glyphs[key]=options.redrawBoxes?lightBox(cp,WIDTH,height)||out:out;
      if(safe.length){
        placements[key]=seams.map(a=>({...a,targetRow:map[Math.max(0,Math.min(23,a.actual))]}));
        safeBoundaries[key]=safe.map(a=>a.y);
      }
      rowMaps[key]=map;
    }
    return {...base,id:'hairline-fixed-'+height,name:'Hairline Lab',height,ascent:lines.baseline+1,glyphs,
      metrics:{...lines,baseline:lines.baseline+1,baselineInk:lines.baseline,ascenderTop:lines.ascender,topOffset:0,descent:height-lines.baseline-1},
      fixedLines:lines,experiment:{fixedCanvas:true,placements,safeBoundaries,rowMaps,cellHeight:height,requestedCellHeight:height,topPaddingAdded:0,
        xHeightDelta:arrows.filter(a=>a.scope!=='caps').length,capHeightDelta:arrows.filter(a=>a.scope!=='lower').length}};
  }

  function makeFont(base, options = {}) {
    checkBase(base);
    if(options.fixedCanvas)return makeFixedFont(base,options);
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
      note: `Experimental Terminus-derived design: +${xHeightDelta} lowercase, +${capHeightDelta} capitals, ${cellHeight}px cell. One-pixel stem insertion; width stays 12px.`,
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
        descent: HEIGHT - ASCENT + config.descenderDelta,
      },
      experiment: {
        xHeightDelta, capHeightDelta, cellHeight, requestedCellHeight,
        redrawBoxes, topPaddingAdded, ascenderDelta, linkAscenders: config.linkAscenders,
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
    if(options.fixedCanvas){const font=makeFixedFont(base,options);return {codepoint,seams:font.experiment.placements[codepoint]||[],safeBoundaries:font.experiment.safeBoundaries[codepoint]||[],cellHeight:font.height,requestedCellHeight:font.height,topOffset:0};}
    const rows = base.glyphs[codepoint];
    checkGlyph(rows, codepoint);
    return planGlyph(rows, codepoint, settings(options));
  }

  function suggestGlobalSeams(base, options = {}) {
    checkBase(base);
    const config = settings(options), count = Math.max(config.xHeightDelta, config.capHeightDelta);
    const plans = Array.from('abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789', c => {
      const cp = c.codePointAt(0), rows = base.glyphs[cp];
      checkGlyph(rows, cp);
      return findSeams(rows, cp >= 97 ? X_TOP : CAP_TOP, WIDTH).map(s => s.y);
    }).filter(rows => rows.length);
    const candidates = [...new Set(plans.flat())].sort((a,b)=>a-b);
    if (count && !candidates.length) throw new Error('No safe one-pixel seams to suggest');
    return Array.from({length:count}, (_,i) => {
      const target = 8 + 10 * (i+1)/(count+1);
      const score = y => Math.abs(y-target)*.5 + plans.reduce((n,rows)=>n+Math.min(...rows.map(r=>Math.abs(r-y))),0)/plans.length;
      const row = candidates.reduce((a,b)=>score(a)<=score(b)?a:b);
      const cost = direction => plans.reduce((sum,rows)=>{
        const side=rows.filter(y=>direction==='up'?y<=row:y>=row);
        return sum+(side.length?Math.min(...side.map(y=>Math.abs(y-row))):HEIGHT);
      },0);
      return {row,direction:cost('up')<=cost('down')?'up':'down'};
    });
  }

  // The editing model stores each inserted row explicitly. Old recipes retain
  // their original rendering until the first edit; A/B references stay exact.
  function editingArrows(base, options = {}) {
    if (options.arrows != null) { if(!options.fixedCanvas)settings(options); return options.arrows.map(a=>({...a})); }
    const config = settings(options), lower = getGlyphPlan(base,101,options).seams,
      caps = getGlyphPlan(base,72,options).seams;
    if (config.placementMode === 'global') {
      const count = Math.max(config.xHeightDelta,config.capHeightDelta);
      return Array.from({length:count},(_,i)=>{
        const l=lower.find(a=>a.globalIndex===i), c=caps.find(a=>a.globalIndex===i), seam=l||c;
        return {row:seam.requested,direction:seam.direction,scope:l&&c?'all':l?'lower':'caps'};
      });
    }
    const arrows = lower.map(a=>({row:a.actual,direction:'up',scope:'lower'}));
    for(const seam of caps) {
      const common=arrows.find(a=>a.scope==='lower' && a.row===seam.actual);
      if(common)common.scope='all';else arrows.push({row:seam.actual,direction:'up',scope:'caps'});
    }
    return arrows;
  }

  function resizeArrows(base, options, scope, count) {
    if(!['lower','caps'].includes(scope) || !Number.isInteger(count) || count<0 || count>12) throw new RangeError('Invalid line height');
    const other = scope==='lower'?'caps':'lower', arrows=editingArrows(base,options);
    let current=arrows.filter(a=>a.scope!==other).length;
    for(let i=arrows.length-1;i>=0 && current>count;i--) if(arrows[i].scope!==other) {
      if(arrows[i].scope==='all')arrows[i]={...arrows[i],scope:other};else arrows.splice(i,1);
      current--;
    }
    const suggestions=suggestGlobalSeams(base,{...options,arrows:null,xHeightDelta:count,capHeightDelta:count});
    while(current<count) {arrows.push({...suggestions[current],scope});current++;}
    return arrows;
  }

  // Overall size grows both letter bodies and their cell by the same amount.
  // Keep manual placements wherever possible rather than resetting the design.
  function resizeOverall(base, options, height) {
    integerOption(height, HEIGHT, HEIGHT, 36, 'overallHeight');
    const config = settings(options), delta = height - config.cellHeight;
    let arrows = resizeArrows(base, options, 'lower', Math.max(0, Math.min(12, config.xHeightDelta + delta)));
    arrows = resizeArrows(base, {...options, arrows}, 'caps', Math.max(0, Math.min(12, config.capHeightDelta + delta)));
    // Share identical requests across letter groups to keep the drag rail compact.
    for (let i=0;i<arrows.length;i++) if(arrows[i].scope==='lower') {
      const j=arrows.findIndex(a=>a.scope==='caps'&&a.row===arrows[i].row&&a.direction===arrows[i].direction);
      if(j>=0){arrows[i]={...arrows[i],scope:'all'};arrows.splice(j,1);if(j<i)i--;}
    }
    return {arrows, cellHeight: height};
  }

  function changedGlyphs(before, after) {
    return Object.keys(after.glyphs).filter(cp=>{
      const a=before.glyphs[cp]||[], b=after.glyphs[cp];
      return a.length!==b.length || a.some((row,y)=>row!==b[y]);
    });
  }

  function nextArrowMove(base, options, index, direction, codepoint) {
    const arrows=editingArrows(base,options), arrow=arrows[index];
    if(!arrow || ![-1,1].includes(direction))return null;
    const before=makeFont(base,options);
    let first=null;
    for(let row=arrow.row+direction;row>=0&&row<=HEIGHT;row+=direction) {
      const next=arrows.map((a,i)=>i===index?{...a,row}:a);
      const after=makeFont(base,{...options,arrows:next});
      const changed=changedGlyphs(before,after);
      if(!changed.length)continue;
      const result={arrows:next,changed};
      if(!first)first=result;
      if(changed.includes(String(codepoint)))return result;
    }
    return first;
  }

  root.HairlineVariant = Object.freeze({makeFont, getGlyphPlan, suggestGlobalSeams, editingArrows, resizeArrows, changedGlyphs, nextArrowMove, resizeOverall});
  if (typeof module !== 'undefined' && module.exports) module.exports = root.HairlineVariant;
})(typeof globalThis !== 'undefined' ? globalThis : this);
