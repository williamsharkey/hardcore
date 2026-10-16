(function () {
  'use strict';
  const R = window.HardcoreReplay, $ = id => document.getElementById(id);
  const canvas = $('screen'), stage = $('stage'), viewport = $('viewport'), video = $('camera');
  const context = canvas.getContext('2d', {alpha: false});
  let recording, renderer, time = 0, playing = false, anchor = 0, anchorTime = 0;
  let trimStart = 0, trimEnd = 0, crop = null, cropping = false, cropDrag = null;
  let animation = 0, cameraReady = false, objectURLs = [];
  const preferences = {pointer: true, captions: true};
  function status(message = '', error = false) { $('status').textContent = message; $('status').classList.toggle('error', error); }
  function host(message) {
    const bridge = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.hardcoreRecording;
    if (!bridge) return false;
    bridge.postMessage(message); return true;
  }
  class NativeRenderer {
    constructor(record) {
      this.canvas = document.createElement('canvas');
      const gl = this.gl = this.canvas.getContext('webgl', {alpha: true, antialias: false, premultipliedAlpha: false, preserveDrawingBuffer: true});
      if (!gl) throw new Error('This browser cannot draw the recording. Open it in Safari, Chrome, or Firefox with WebGL enabled.');
      const instancing = this.instancing = gl.getExtension('ANGLE_instanced_arrays');
      if (!instancing) throw new Error('This browser does not support the drawing features this recording needs.');
      const vertex = 'attribute vec2 corner; attribute vec4 rect; attribute vec4 atlasRect; attribute vec4 color; uniform vec2 size; varying vec2 uv; varying vec4 tint; void main(){vec2 p=mix(rect.xy,rect.zw,corner); gl_Position=vec4(p.x*2.0/size.x-1.0,1.0-p.y*2.0/size.y,0.0,1.0); uv=atlasRect.x==65535.0?vec2(-1.0):mix(atlasRect.xy,atlasRect.zw,corner)/4096.0; tint=color;}';
      const fragment = 'precision highp float; uniform sampler2D atlas; varying vec2 uv; varying vec4 tint; void main(){float a=uv.x<0.0?1.0:texture2D(atlas,uv).r; gl_FragColor=vec4(tint.rgb,tint.a*a);}';
      const shader = (type, source) => { const s = gl.createShader(type); gl.shaderSource(s, source); gl.compileShader(s); if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error('The replay shader could not compile.'); return s; };
      const program = gl.createProgram(), vs = shader(gl.VERTEX_SHADER, vertex), fs = shader(gl.FRAGMENT_SHADER, fragment);
      gl.attachShader(program, vs); gl.attachShader(program, fs); gl.linkProgram(program); gl.deleteShader(vs); gl.deleteShader(fs);
      if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error('The replay renderer could not start.');
      this.program = program; gl.useProgram(program);
      this.size = gl.getUniformLocation(program, 'size');
      const corners = this.corners = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, corners);
      gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([0,0, 1,0, 0,1, 1,0, 0,1, 1,1]), gl.STATIC_DRAW);
      const corner = gl.getAttribLocation(program, 'corner'); gl.enableVertexAttribArray(corner); gl.vertexAttribPointer(corner, 2, gl.FLOAT, false, 0, 0);
      this.instances = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, this.instances);
      for (const [name, type, normalized, offset] of [['rect', gl.SHORT, false, 0], ['atlasRect', gl.UNSIGNED_SHORT, false, 8], ['color', gl.UNSIGNED_BYTE, true, 16]]) {
        const location = gl.getAttribLocation(program, name); gl.enableVertexAttribArray(location);
        gl.vertexAttribPointer(location, 4, type, normalized, 20, offset); instancing.vertexAttribDivisorANGLE(location, 1);
      }
      this.texture = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, this.texture);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1); gl.uniform1i(gl.getUniformLocation(program, 'atlas'), 0);
      gl.enable(gl.BLEND); gl.blendFuncSeparate(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA, gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
      this.resetAtlas();
      this.timeline = new R.AtlasTimeline(record, patch => gl.texSubImage2D(gl.TEXTURE_2D, 0, patch.x, patch.y, patch.w, patch.h, gl.LUMINANCE, gl.UNSIGNED_BYTE, patch.data), () => this.resetAtlas());
      this.frameIndex = -1;
    }
    resetAtlas() { const gl = this.gl; gl.texImage2D(gl.TEXTURE_2D, 0, gl.LUMINANCE, R.ATLAS_SIZE, R.ATLAS_SIZE, 0, gl.LUMINANCE, gl.UNSIGNED_BYTE, null); }
    draw(index) {
      if (this.frameIndex === index) return this.canvas;
      const gl = this.gl, frame = this.timeline.seek(index);
      if (this.canvas.width !== frame.w || this.canvas.height !== frame.h) { this.canvas.width = frame.w; this.canvas.height = frame.h; }
      gl.viewport(0, 0, frame.w, frame.h); gl.uniform2f(this.size, frame.w, frame.h);
      gl.clearColor((frame.bg >> 16 & 255) / 255, (frame.bg >> 8 & 255) / 255, (frame.bg & 255) / 255, frame.transparent ? 0 : 1); gl.clear(gl.COLOR_BUFFER_BIT);
      gl.bindBuffer(gl.ARRAY_BUFFER, this.instances); gl.bufferData(gl.ARRAY_BUFFER, frame.quads, gl.DYNAMIC_DRAW);
      this.instancing.drawArraysInstancedANGLE(gl.TRIANGLES, 0, 6, frame.quads.byteLength / 20);
      this.frameIndex = index; return this.canvas;
    }
    destroy() { this.gl.deleteBuffer(this.instances); this.gl.deleteBuffer(this.corners); this.gl.deleteTexture(this.texture); this.gl.deleteProgram(this.program); this.gl.getExtension('WEBGL_lose_context')?.loseContext(); }
  }
  function currentFrame() { return recording.frames[R.frameAt(recording, time)]; }
  function activeCrop(frame) {
    if (cropping || !crop) return [0, 0, frame.w, frame.h];
    // Resizes during a recording keep source pixel coordinates; expose any area
    // beyond the current frame as background, just as the native exporter does.
    return crop;
  }
  function resizeStage() {
    if (!recording) return;
    const [,, w, h] = activeCrop(currentFrame()), zoom = $('zoom').value;
    const scale = zoom === 'fit' ? Math.min((viewport.clientWidth - 2) / w, Math.max(120, window.innerHeight - 290) / h, 1) : Number(zoom);
    stage.style.width = Math.max(1, Math.round(w * scale)) + 'px';
    stage.style.height = Math.max(1, Math.round(h * scale)) + 'px';
    showCropBox();
  }
  function drawCamera(frame) {
    const placement = frame.camera;
    if (!cameraReady || !recording.camera || !placement || !placement.visible || time < recording.camera.offsetMs || video.readyState < 2) return;
    const [x, y, w, h] = placement.rect;
    const [dx, dy, dw, dh] = R.cameraFit(video.videoWidth, video.videoHeight, placement.rect);
    context.save(); context.beginPath(); context.rect(x, y, w, h); context.clip(); context.imageSmoothingEnabled = true;
    context.fillStyle = '#000'; context.fillRect(x, y, w, h);
    if (placement.mirrored ?? recording.camera.mirrored) { context.translate(x + w, y); context.scale(-1, 1); context.drawImage(video, dx - x, dy - y, dw, dh); }
    else context.drawImage(video, dx, dy, dw, dh);
    context.restore();
  }
  function drawPointer() {
    const pointer = R.pointerAt(recording, time);
    for (const click of R.clicksAt(recording, time)) {
      context.save(); context.strokeStyle = 'rgba(255,255,255,' + click.opacity + ')'; context.lineWidth = 2;
      context.beginPath(); context.arc(click.x, click.y, click.radius, 0, Math.PI * 2); context.stroke(); context.restore();
    }
    for (const scroll of R.scrollsAt(recording, time)) {
      const {x, y, ux, uy, opacity} = scroll;
      const tipX = x + ux * 6, tipY = y + uy * 6;
      context.save(); context.strokeStyle = '#ffca6a'; context.globalAlpha = opacity; context.lineWidth = 2; context.lineCap = 'round'; context.lineJoin = 'round';
      context.beginPath(); context.moveTo(x - ux * 6, y - uy * 6); context.lineTo(tipX, tipY);
      context.moveTo(tipX - ux * 4 - uy * 4, tipY - uy * 4 + ux * 4); context.lineTo(tipX, tipY); context.lineTo(tipX - ux * 4 + uy * 4, tipY - uy * 4 - ux * 4);
      context.stroke(); context.restore();
    }
    if (!pointer || !pointer.visible) return;
    context.save(); context.translate(pointer.x, pointer.y); context.scale(pointer.size / 14, pointer.size / 14);
    context.beginPath(); context.moveTo(0, 0); context.lineTo(0, 14); context.lineTo(3.8, 10.3); context.lineTo(6.5, 16.5); context.lineTo(9, 15.4); context.lineTo(6.2, 9.3); context.lineTo(11.5, 9.2); context.closePath();
    context.fillStyle = '#fff'; context.strokeStyle = '#161616'; context.lineWidth = 1.2; context.lineJoin = 'round'; context.fill(); context.stroke(); context.restore();
  }
  function wrapText(text, maxWidth) {
    const lines = [];
    for (const paragraph of text.split('\n')) {
      let line = '';
      for (const word of paragraph.split(/\s+/)) {
        if (line && context.measureText(line + ' ' + word).width > maxWidth) { lines.push(line); line = ''; }
        // Break a long token at code points so captions never spill offscreen.
        if (context.measureText(word).width > maxWidth) {
          if (line) { lines.push(line); line = ''; }
          for (const char of word) { if (line && context.measureText(line + char).width > maxWidth) { lines.push(line); line = ''; } line += char; }
        } else line += (line ? ' ' : '') + word;
      }
      lines.push(line);
    }
    return lines;
  }
  function drawCaption() {
    const text = R.captionsAt(recording, time); if (!text) return;
    const w = canvas.width, h = canvas.height, fontSize = Math.max(16, h * .032), padding = Math.max(16, h * .04);
    context.save(); context.font = '500 ' + fontSize + 'px -apple-system, BlinkMacSystemFont, sans-serif'; context.textAlign = 'center'; context.textBaseline = 'top';
    const lines = wrapText(text, w * .9 - 24), lineHeight = fontSize * 1.25;
    const boxWidth = Math.min(w, Math.max(...lines.map(line => context.measureText(line).width)) + 24), boxHeight = lines.length * lineHeight + 16;
    const x = (w - boxWidth) / 2, y = Math.max(0, h - padding - boxHeight);
    context.fillStyle = 'rgba(0,0,0,.76)'; context.beginPath();
    if (context.roundRect) context.roundRect(x, y, boxWidth, boxHeight, 5); else context.rect(x, y, boxWidth, boxHeight);
    context.fill(); context.fillStyle = '#fff'; lines.forEach((line, i) => context.fillText(line, w / 2, y + 8 + i * lineHeight)); context.restore();
  }
  function render() {
    if (!recording) return;
    const index = R.frameAt(recording, time), frame = recording.frames[index], rect = activeCrop(frame), [x, y, w, h] = rect;
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; resizeStage(); }
    context.fillStyle = '#' + frame.bg.toString(16).padStart(6, '0'); context.fillRect(0, 0, w, h);
    context.save(); context.translate(-x, -y); context.imageSmoothingEnabled = false;
    context.drawImage(renderer.draw(index), 0, 0); drawCamera(frame); if (preferences.pointer) drawPointer(); context.restore();
    if (preferences.captions) drawCaption();
    $('seek').value = time; $('position').textContent = R.timeLabel(time); showCropBox();
  }
  function cameraTime() { return Math.max(0, (time - (recording.camera?.offsetMs || 0)) / 1000); }
  function syncCamera(force = false) {
    if (!cameraReady) return;
    const target = cameraTime(), beforeStart = time < recording.camera.offsetMs;
    video.playbackRate = Number($('speed').value);
    if ((force || Math.abs(video.currentTime - target) > .15) && !video.seeking && target <= video.duration) video.currentTime = target;
    if (playing && !beforeStart && video.paused) video.play().catch(() => status('Camera audio needs a click on Play to start.'));
    else if (!playing || beforeStart) video.pause();
  }
  function seek(ms) { if (!recording) return; time = Math.max(trimStart, Math.min(trimEnd, Number(ms))); anchorTime = time; anchor = performance.now(); syncCamera(true); render(); }
  function pause() { playing = false; video.pause(); cancelAnimationFrame(animation); $('play').textContent = '▶'; $('play').setAttribute('aria-label', 'Play recording'); }
  function tick(now) {
    if (!playing) return;
    time = Math.min(trimEnd, anchorTime + (now - anchor) * Number($('speed').value)); syncCamera(); render();
    if (time >= trimEnd) pause(); else animation = requestAnimationFrame(tick);
  }
  function play() {
    if (!recording) return;
    if (playing) { pause(); return; }
    if (time >= trimEnd) time = trimStart;
    playing = true; anchorTime = time; anchor = performance.now(); $('play').textContent = 'Ⅱ'; $('play').setAttribute('aria-label', 'Pause recording');
    syncCamera(true); animation = requestAnimationFrame(tick);
  }
  function updateSummary() {
    if (!recording) return;
    $('trim-start').value = (trimStart / 1000).toFixed(3); $('trim-end').value = (trimEnd / 1000).toFixed(3);
    $('seek').min = trimStart; $('seek').max = trimEnd;
    $('edit-summary').textContent = R.timeLabel(trimEnd - trimStart, true) + ' selected' + (crop ? '  ·  ' + crop[2] + ' × ' + crop[3] + ' px' : '');
    $('crop-size').textContent = crop ? crop[2] + ' × ' + crop[3] : '';
    $('crop-reset').disabled = !crop;
  }
  function updateTrim(start, end) {
    try { const spec = R.editSpec(recording, {startMs: start, endMs: end, crop}); trimStart = spec.startMs; trimEnd = spec.endMs; status(); }
    catch (error) { status(error.message, true); }
    updateSummary(); seek(time);
  }
  function showCropBox() {
    const box = $('crop-box'); box.hidden = !cropping || !crop;
    if (box.hidden) return;
    const f = currentFrame(); const [x, y, w, h] = crop;
    Object.assign(box.style, {left: x / f.w * 100 + '%', top: y / f.h * 100 + '%', width: w / f.w * 100 + '%', height: h / f.h * 100 + '%'});
  }
  function toggleCrop() { if (!recording) return; pause(); cropping = !cropping; stage.classList.toggle('cropping', cropping); $('crop-toggle').setAttribute('aria-pressed', String(cropping)); $('crop-toggle').textContent = cropping ? 'Apply crop' : 'Crop'; status(cropping ? 'Drag across the recording to choose the crop. Select Apply crop when finished.' : ''); resizeStage(); render(); }
  function localPoint(event) { const bounds = stage.getBoundingClientRect(), frame = currentFrame(); return [Math.max(0, Math.min(frame.w, Math.round((event.clientX - bounds.left) * frame.w / bounds.width))), Math.max(0, Math.min(frame.h, Math.round((event.clientY - bounds.top) * frame.h / bounds.height)))]; }
  stage.addEventListener('pointerdown', event => { if (!cropping) return; cropDrag = localPoint(event); stage.setPointerCapture(event.pointerId); event.preventDefault(); });
  stage.addEventListener('pointermove', event => { if (!cropDrag) return; const end = localPoint(event), x = Math.min(cropDrag[0], end[0]), y = Math.min(cropDrag[1], end[1]); if (Math.abs(end[0] - cropDrag[0]) && Math.abs(end[1] - cropDrag[1])) crop = [x, y, Math.abs(end[0] - cropDrag[0]), Math.abs(end[1] - cropDrag[1])]; showCropBox(); updateSummary(); });
  stage.addEventListener('pointerup', () => { cropDrag = null; }); stage.addEventListener('pointercancel', () => { cropDrag = null; });
  function download(name, data, type = 'application/json') { const url = URL.createObjectURL(new Blob([data], {type})), link = document.createElement('a'); link.href = url; link.download = name; link.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); }
  function getEdits() { return {...R.editSpec(recording, {startMs: trimStart, endMs: trimEnd, crop, ...preferences}), captionItems: recording.captions}; }
  function saveEdits() {
    if (!recording) return;
    try {
      const edits = getEdits();
      if (host({action: 'saveEdits', ...edits})) status('Saving edit settings in Hardcore…');
      else { download('edits.json', JSON.stringify(edits, null, 2) + '\n'); status('Edit settings downloaded.'); }
    } catch (error) { status(error.message, true); }
  }
  function applyEdits(rawEdits) {
    if (!recording) return;
    try {
      const edits = R.editSpec(recording, rawEdits);
      const captions = rawEdits.captionItems ? R.parseRecording({...recording.raw, captions: rawEdits.captionItems}).captions : recording.captions;
      trimStart = edits.startMs; trimEnd = edits.endMs; crop = edits.crop;
      preferences.captions = edits.captions; preferences.pointer = edits.pointer; recording.captions = captions;
      $('captions-toggle').checked = edits.captions; $('pointer-toggle').checked = edits.pointer;
      updateSummary(); resizeStage(); seek(trimStart);
    } catch (error) { status('Edit settings could not be loaded: ' + error.message, true); }
  }
  function exportVideo() {
    if (!recording) return;
    pause();
    let edits; try { edits = getEdits(); } catch (error) { status(error.message, true); return; }
    if (host({action: 'export', ...edits})) { status('Choose where to save the exported video in Hardcore.'); return; }
    $('export-note').textContent = 'For a crisp video export, open this recording in Hardcore. You can also use the native exporter with these settings.';
    let command = 'recording-export "Recording.hcrec" "Recording.mp4" --start ' + (trimStart / 1000).toFixed(3) + ' --end ' + (trimEnd / 1000).toFixed(3);
    if (crop) command += ' --crop ' + crop.join(','); if (!preferences.pointer) command += ' --no-pointer'; if (!preferences.captions) command += ' --no-captions';
    $('export-command').textContent = command;
    $('export-dialog').showModal();
  }
  async function load(raw, files = [], baseURL = '') {
    pause(); status();
    const parsed = R.parseRecording(raw), next = new NativeRenderer(parsed);
    renderer?.destroy(); objectURLs.forEach(url => URL.revokeObjectURL(url)); objectURLs = [];
    recording = parsed; renderer = next; time = 0; trimStart = 0; trimEnd = parsed.durationMs; crop = null; cropping = false; cameraReady = false;
    stage.classList.remove('cropping'); $('crop-toggle').textContent = 'Crop'; $('crop-toggle').setAttribute('aria-pressed', 'false');
    video.pause(); video.removeAttribute('src'); video.load();
    if (parsed.camera) {
      const file = files.find(file => file.name === parsed.camera.file);
      if (file) { const url = URL.createObjectURL(file); objectURLs.push(url); video.src = url; }
      else if (baseURL !== null) video.src = baseURL + encodeURIComponent(parsed.camera.file);
      else status('The camera file is missing. Open the entire recording folder to include it.');
    }
    $('welcome').hidden = true; $('editor').hidden = false; $('export-button').disabled = false;
    $('recording-title').textContent = parsed.title; document.title = parsed.title + ' · Hardcore';
    const first = parsed.frames[0]; $('recording-detail').textContent = first.w + ' × ' + first.h + ' px' + (parsed.createdAt ? '  ·  ' + (Number.isNaN(Date.parse(parsed.createdAt)) ? parsed.createdAt : new Date(parsed.createdAt).toLocaleString()) : '');
    $('duration').textContent = R.timeLabel(parsed.durationMs); $('trim-start').max = $('trim-end').max = parsed.durationMs / 1000;
    updateSummary(); resizeStage(); render();
    const editsFile = files.find(file => file.name === 'edits.json');
    if (editsFile) {
      try { applyEdits(JSON.parse(await editsFile.text()));
      } catch (error) { status('The recording opened, but its edit settings could not be loaded: ' + error.message, true); }
    }
  }
  async function openFiles(files) {
    if (!files.length) return;
    const replay = files.find(f => f.name === 'replay.json') || files.find(f => f.name === 'recording.js') || files.find(f => f.name === 'manifest.json');
    if (!replay) { status('Choose a recording folder containing replay.json or recording.js.', true); return; }
    try { await load(await replay.text(), files, null); } catch (error) { status(error.message, true); }
  }
  function openFolder() { if (!host({action: 'open'})) $('folder-picker').click(); }
  $('open-button').onclick = $('welcome-open').onclick = openFolder;
  $('library-link').onclick = event => { event.preventDefault(); if (!host({action: 'library'})) openFolder(); };
  $('folder-picker').onchange = event => openFiles(Array.from(event.target.files));
  $('play').onclick = play; $('seek').oninput = event => seek(event.target.value);
  $('speed').onchange = () => { anchor = performance.now(); anchorTime = time; syncCamera(true); };
  $('zoom').onchange = resizeStage; window.addEventListener('resize', resizeStage);
  $('trim-start').onchange = event => updateTrim(Number(event.target.value) * 1000, trimEnd);
  $('trim-end').onchange = event => updateTrim(trimStart, Number(event.target.value) * 1000);
  $('mark-start').onclick = () => updateTrim(time, trimEnd); $('mark-end').onclick = () => updateTrim(trimStart, time);
  $('crop-toggle').onclick = toggleCrop; $('crop-reset').onclick = () => { crop = null; updateSummary(); resizeStage(); render(); };
  $('pointer-toggle').onchange = event => { preferences.pointer = event.target.checked; render(); };
  $('captions-toggle').onchange = event => { preferences.captions = event.target.checked; render(); };
  $('captions-import').onclick = () => $('caption-picker').click();
  $('caption-picker').onchange = async event => {
    const file = event.target.files[0]; if (!file || !recording) return;
    const captions = R.parseCaptions(await file.text(), recording.durationMs);
    if (!captions.length) { status('No timed captions were found. Choose an SRT or WebVTT file.', true); return; }
    recording.captions = captions; preferences.captions = true; $('captions-toggle').checked = true; render(); status(captions.length + ' captions added. Save edit settings to keep them.');
  };
  $('save-edits').onclick = $('download-edits').onclick = saveEdits; $('export-button').onclick = exportVideo;
  video.addEventListener('loadeddata', () => { cameraReady = true; syncCamera(true); render(); }); video.addEventListener('seeked', render);
  video.addEventListener('error', () => { if (recording?.camera && video.getAttribute('src')) status('The camera video could not be opened. Terminal replay is available.', true); });
  document.addEventListener('keydown', event => {
    if (!recording || /INPUT|SELECT|TEXTAREA|BUTTON/.test(event.target.tagName) || $('export-dialog').open) return;
    if (event.code === 'Space') { event.preventDefault(); play(); }
    if (event.code === 'ArrowLeft' || event.code === 'ArrowRight') { event.preventDefault(); seek(time + (event.code === 'ArrowLeft' ? -1 : 1) * (event.shiftKey ? 1000 : 5000)); }
    if (event.key === 'Escape' && cropping) toggleCrop();
  });
  let dragDepth = 0;
  document.addEventListener('dragenter', event => { if (event.dataTransfer.types.includes('Files')) { event.preventDefault(); dragDepth++; document.body.classList.add('dropping'); } });
  document.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; document.body.classList.remove('dropping'); } });
  document.addEventListener('dragover', event => event.preventDefault());
  async function entryFiles(entry) {
    if (entry.isFile) return [await new Promise((resolve, reject) => entry.file(resolve, reject))];
    const reader = entry.createReader(); let result = [];
    for (;;) { const entries = await new Promise((resolve, reject) => reader.readEntries(resolve, reject)); if (!entries.length) break; for (const child of entries) result = result.concat(await entryFiles(child)); }
    return result;
  }
  document.addEventListener('drop', async event => {
    event.preventDefault(); dragDepth = 0; document.body.classList.remove('dropping');
    try { const entries = Array.from(event.dataTransfer.items).map(item => item.webkitGetAsEntry?.()).filter(Boolean); await openFiles(entries.length ? (await Promise.all(entries.map(entryFiles))).flat() : Array.from(event.dataTransfer.files)); }
    catch (error) { status('The recording folder could not be opened: ' + error.message, true); }
  });
  // Native host may replace a recording without reloading the player.
  window.hardcoreRecordingPlayer = {load, seek, pause, getEdits, applyEdits, get time() { return time; }};
  if (window.HARDCORE_RECORDING) load(window.HARDCORE_RECORDING).then(() => { if (window.HARDCORE_EDITS) applyEdits(window.HARDCORE_EDITS); }).catch(error => status(error.message, true));
})();
