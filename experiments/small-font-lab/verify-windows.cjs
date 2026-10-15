#!/usr/bin/env node
/* Attach to an independently launched Windows Chrome/Edge preview.
 *
 *   node verify-windows.cjs http://127.0.0.1:9222
 *
 * Requires playwright-core, either installed normally, supplied through
 * HARDCORE_PLAYWRIGHT_CORE, or bundled with the local VS Code installation.
 * This never launches a browser, emulates a display, closes a browser/tab,
 * or accesses a production Hardcore window. Exiting disconnects CDP.
 */
'use strict';

const fs = require('node:fs');
const path = require('node:path');

const root = path.resolve(__dirname, '../..');
const output = path.join(root, 'build', 'small-font-lab');
const reportPath = path.join(output, 'windows-verification.json');
const screenshotPath = path.join(output, 'windows-native.png');
const firstCanvasPath = path.join(output, 'windows-first-canvas.png');

function findPlaywright() {
  const candidates = [];
  if (process.env.HARDCORE_PLAYWRIGHT_CORE)
    candidates.push(process.env.HARDCORE_PLAYWRIGHT_CORE);
  try { candidates.push(require.resolve('playwright-core')); } catch {}
  const bases = [
    process.env.LOCALAPPDATA && path.join(process.env.LOCALAPPDATA, 'Programs', 'Microsoft VS Code'),
    process.env.ProgramFiles && path.join(process.env.ProgramFiles, 'Microsoft VS Code'),
    process.env['ProgramFiles(x86)'] && path.join(process.env['ProgramFiles(x86)'], 'Microsoft VS Code'),
  ].filter(Boolean);
  // WSL can use the same JavaScript package for isolated Linux browser tests.
  if (process.platform !== 'win32') {
    try {
      for (const entry of fs.readdirSync('/mnt/c/Users', {withFileTypes: true}))
        if (entry.isDirectory()) bases.push(path.join('/mnt/c/Users', entry.name,
          'AppData', 'Local', 'Programs', 'Microsoft VS Code'));
    } catch {}
  }
  for (const base of bases) {
    const installs = [base];
    try {
      for (const entry of fs.readdirSync(base, {withFileTypes: true}))
        if (entry.isDirectory()) installs.push(path.join(base, entry.name));
    } catch {}
    for (const install of installs)
      candidates.push(path.join(install, 'resources', 'app', 'node_modules', 'playwright-core'));
  }
  const failures = [];
  for (const candidate of candidates) {
    if (!fs.existsSync(candidate)) continue;
    try {
      const api = require(candidate);
      if (!api.chromium?.connectOverCDP) throw new Error('chromium.connectOverCDP is missing');
      return {api, source: candidate};
    } catch (error) { failures.push(`${candidate}: ${error.message}`); }
  }
  throw new Error('Cannot locate playwright-core; set HARDCORE_PLAYWRIGHT_CORE to its directory.' +
    (failures.length ? '\n' + failures.join('\n') : ''));
}

function pixelResultPassed(result) {
  if (typeof result === 'boolean') return result;
  if (!result || typeof result !== 'object') return false;
  if (typeof result.ok === 'boolean') return result.ok;
  if (typeof result.passed === 'boolean') return result.passed;
  return false; // A missing or unknown verifier result must not look like a pass.
}

async function main() {
  fs.mkdirSync(output, {recursive: true});
  const report = {
    checkedAt: new Date().toISOString(),
    platform: process.platform,
    actualDPR: null,
    screen: null,
    canvases: [],
    jsErrors: [],
    failures: [],
    screenshot: screenshotPath,
    scope: 'Existing Windows browser, native viewport; no device metrics override. Screenshot is the browser compositor output.',
  };
  try {
    if (process.platform !== 'win32')
      throw new Error('Run with a Windows Node runtime to check the Windows display.');
    const endpoint = process.argv[2];
    if (!endpoint) throw new Error('Usage: node verify-windows.cjs http://127.0.0.1:9222');
    const parsed = new URL(endpoint);
    if (!['http:', 'https:', 'ws:', 'wss:'].includes(parsed.protocol))
      throw new Error('Expected an HTTP or WebSocket CDP endpoint.');
    const {api, source} = findPlaywright();
    report.playwrightSource = source;
    const browser = await api.chromium.connectOverCDP(endpoint, {timeout: 15000});
    const pages = browser.contexts().flatMap(context => context.pages());
    const page = pages.find(candidate => {
      try { return new URL(candidate.url()).pathname.endsWith('/small-font-lab.html'); }
      catch { return false; }
    });
    if (!page) throw new Error('No existing small-font-lab.html tab found in the supplied browser.');
    page.on('pageerror', error => report.jsErrors.push(error.message));
    // Reload only this dedicated demo, so rebuilt bitmap data is tested.
    await page.reload({waitUntil: 'load'});
    if (typeof page.pageErrors === 'function') {
      const previous = await page.pageErrors();
      report.jsErrors.push(...previous.map(error => error.message));
    }
    await page.waitForFunction(() => window.smallFontLab &&
      typeof window.smallFontLab.verifyPixels === 'function', undefined, {timeout: 15000});
    await page.evaluate(async () => {
      window.smallFontLab.setScale(1);
      await window.smallFontLab.render();
      window.scrollTo(0, 0);
      await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    });
    Object.assign(report, await page.evaluate(async () => {
      const dpr = window.devicePixelRatio;
      const near = (a, b) => Math.abs(a - b) <= 0.02;
      const canvases = Array.from(document.querySelectorAll('canvas.screen'))
        .filter(canvas => canvas.getBoundingClientRect().width > 0 && canvas.getBoundingClientRect().height > 0)
        .map((canvas, index) => {
          const rect = canvas.getBoundingClientRect();
          const physicalLeft = rect.left * dpr, physicalTop = rect.top * dpr;
          return {
            index, id: canvas.id, font: canvas.dataset.font || canvas.closest('[data-font]')?.dataset.font || null,
            width: canvas.width, height: canvas.height,
            cssWidth: rect.width, cssHeight: rect.height, physicalLeft, physicalTop,
            nativeWidth: near(rect.width * dpr, canvas.width),
            nativeHeight: near(rect.height * dpr, canvas.height),
            alignedX: near(physicalLeft, Math.round(physicalLeft)),
            alignedY: near(physicalTop, Math.round(physicalTop)),
          };
        });
      return {
        actualDPR: dpr, url: location.href, title: document.title, userAgent: navigator.userAgent,
        screen: {width: screen.width, height: screen.height, availWidth: screen.availWidth,
          availHeight: screen.availHeight, colorDepth: screen.colorDepth, pixelDepth: screen.pixelDepth},
        window: {innerWidth, innerHeight, outerWidth, outerHeight, screenX, screenY,
          visualViewportScale: visualViewport?.scale ?? null},
        canvases,
        pixelVerification: await window.smallFontLab.verifyPixels(),
      };
    }));
    if (!report.canvases.length) report.failures.push('No visible canvas.screen elements found.');
    for (const canvas of report.canvases) {
      for (const check of ['nativeWidth', 'nativeHeight', 'alignedX', 'alignedY'])
        if (!canvas[check]) report.failures.push(`Canvas ${canvas.id || canvas.index}: ${check}`);
    }
    if (!pixelResultPassed(report.pixelVerification))
      report.failures.push('Pixel verification did not return a passing result.');
    // No fullPage/viewport resize: preserve the actual monitor's native DPR.
    await page.screenshot({path: screenshotPath, fullPage: false, scale: 'device'});
    const firstCanvas = await page.locator('canvas.screen').first().evaluate(canvas => canvas.toDataURL('image/png'));
    fs.writeFileSync(firstCanvasPath, Buffer.from(firstCanvas.split(',')[1], 'base64'));
    report.firstCanvasBackingImage = firstCanvasPath;
    report.jsErrors = [...new Set(report.jsErrors)];
    if (report.jsErrors.length) report.failures.push('JavaScript errors were recorded.');
  } catch (error) {
    report.failures.push(error.stack || error.message);
  }
  report.ok = report.failures.length === 0;
  fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
  // Close only this automation runtime; do not send Browser.close or close tabs.
  process.stdout.write(JSON.stringify(report, null, 2) + '\n', () => process.exit(report.ok ? 0 : 1));
}

module.exports = {findPlaywright, pixelResultPassed};
if (require.main === module) {
  main().catch(error => {
    process.stderr.write(error.stack + '\n', () => process.exit(1));
  });
}
