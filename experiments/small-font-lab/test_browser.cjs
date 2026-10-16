#!/usr/bin/env node
/* Isolated browser regression tests; never attach to any existing browser.
 * Build the demo first: python3 experiments/small-font-lab/build.py
 * Run: node experiments/small-font-lab/test_browser.cjs
 * Optional: HARDCORE_PLAYWRIGHT_CORE and HARDCORE_TEST_CHROMIUM paths.
 */
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const assert = require('node:assert/strict');
const {pathToFileURL} = require('node:url');
const {findPlaywright} = require('./verify-windows.cjs');

function findChromium() {
  if (process.env.HARDCORE_TEST_CHROMIUM) return process.env.HARDCORE_TEST_CHROMIUM;
  const cache = path.join(os.homedir(), '.cache', 'ms-playwright');
  let releases = [];
  try { releases = fs.readdirSync(cache).filter(name => /^chromium-\d+$/.test(name)).sort().reverse(); } catch {}
  for (const release of releases) {
    for (const executable of ['chrome-linux/chrome', 'chrome-linux64/chrome']) {
      const candidate = path.join(cache, release, executable);
      if (fs.existsSync(candidate)) return candidate;
    }
  }
  return undefined; // Use Playwright's normal installed-browser lookup.
}

async function settle(page) {
  await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
}

async function validate(page, label, dpr, scale, checkPixels = true) {
  await settle(page);
  const report = await page.evaluate(checkPixels => {
    const dpr = devicePixelRatio;
    const close = (a, b) => Math.abs(a - b) <= 0.02;
    const canvases = Array.from(document.querySelectorAll('canvas.screen')).map(canvas => {
      const rect = canvas.getBoundingClientRect();
      const f = smallFontLab.data.fonts.find(font => font.id === canvas.dataset.font);
      return {font: f.id, width: canvas.width, height: canvas.height,
        physicalLeft: rect.x * dpr, physicalTop: rect.y * dpr,
        native: close(rect.width * dpr, canvas.width) && close(rect.height * dpr, canvas.height),
        aligned: close(rect.x * dpr, Math.round(rect.x * dpr)) && close(rect.y * dpr, Math.round(rect.y * dpr)),
        correctWidth: canvas.width === 48 * f.width * smallFontLab.state.scale,
        correctHeight: canvas.height % (f.height * smallFontLab.state.scale) === 0};
    });
    return {dpr, scale: smallFontLab.state.scale, canvases,
      documentWidth: document.documentElement.scrollWidth, viewportWidth: innerWidth,
      pixels: checkPixels ? smallFontLab.verifyPixels() : null};
  }, checkPixels);
  assert.equal(report.dpr, dpr, `${label}: DPR`);
  assert.equal(report.scale, scale, `${label}: bitmap zoom`);
  assert.ok(report.canvases.length, `${label}: no preview canvases`);
  assert.ok(report.documentWidth <= report.viewportWidth + 1, `${label}: page overflow`);
  for (const canvas of report.canvases)
    for (const key of ['native', 'aligned', 'correctWidth', 'correctHeight'])
      assert.ok(canvas[key], `${label}, DPR ${dpr}: ${canvas.font} ${key}; ${JSON.stringify(canvas)}`);
  if (checkPixels) assert.ok(report.pixels.ok, `${label}: bitmap mismatch ${JSON.stringify(report.pixels.errors)}`);
  return {label, dpr, scale, canvases: report.canvases.length, checkedPixels: report.pixels?.checked || 0};
}

async function main() {
  const source = path.join(__dirname, 'small-font-lab.html');
  assert.ok(fs.existsSync(source), 'Build small-font-lab.html before running browser tests.');
  const {api} = findPlaywright();
  const browser = await api.chromium.launch({headless: true, executablePath: findChromium(), args: ['--no-sandbox']});
  const results = [], jsErrors = [];
  try {
    for (const dpr of [1, 1.25, 1.5, 2]) {
      const context = await browser.newContext({viewport: {width: 1440, height: 1000}, deviceScaleFactor: dpr});
      const page = await context.newPage();
      page.on('pageerror', error => jsErrors.push(`${dpr}: ${error.message}`));
      try {
        await page.goto(pathToFileURL(source).href);
        await page.waitForFunction(() => !!window.smallFontLab);
        results.push(await validate(page, 'wide native focus', dpr, 1));

        await page.evaluate(() => {smallFontLab.setFilter('all'); smallFontLab.setSample('boxes');});
        results.push(await validate(page, 'all fonts native box joins', dpr, 1));
        assert.equal(await page.locator('canvas.screen').count(), await page.evaluate(() => smallFontLab.data.fonts.length));

        await page.evaluate(() => {smallFontLab.setBoxes(false); smallFontLab.setTheme('light');});
        results.push(await validate(page, 'light source boxes and fallback', dpr, 1));
        assert.ok(await page.locator('body').evaluate(body => body.classList.contains('light')));

        await page.evaluate(() => {smallFontLab.setBoxes(true); smallFontLab.setScale(2);});
        results.push(await validate(page, 'enlarged completed box joins', dpr, 2));

        await page.evaluate(() => {
          smallFontLab.setScale(1); smallFontLab.setFilter('focus');
          smallFontLab.setTheme('dark'); smallFontLab.setSample('text');
        });
        await page.setViewportSize({width: 360, height: 800});
        await page.waitForTimeout(100); // The demo deliberately debounces resize by 60ms.
        results.push(await validate(page, 'narrow native code sample', dpr, 1));

        await page.evaluate(() => {
          const font = smallFontLab.data.fonts.find(f => f.generated.length);
          smallFontLab.inspect(font.id, font.generated[0]);
          smallFontLab.setFilter('one'); smallFontLab.setSample('boxes');
        });
        results.push(await validate(page, 'single font generated glyph', dpr, 1));
        assert.equal(await page.locator('canvas.screen').count(), 1);
        assert.match(await page.locator('#inspect-text').innerText(), /Completed by Hardcore/);
        await page.evaluate(() => smallFontLab.setBoxes(false));
        assert.match(await page.locator('#inspect-text').innerText(), /Unifont fallback/);
        results.push(await validate(page, 'single font fallback glyph', dpr, 1));

        await page.evaluate(() => {
          const wrapper = document.querySelector('.screen-wrap');
          wrapper.scrollLeft = 37;
          window.scrollTo(0, 173);
        });
        results.push(await validate(page, 'scrolled fractional DPR alignment', dpr, 1, false));
      } finally { await context.close(); }
    }
    assert.deepEqual(jsErrors, [], 'No browser JavaScript errors');
  } finally { await browser.close(); }
  const report = {ok: true, scenarios: results.length,
    checkedPixels: results.reduce((total, result) => total + result.checkedPixels, 0), jsErrors, results};
  const output = path.resolve(__dirname, '../../build/small-font-lab');
  fs.mkdirSync(output, {recursive: true});
  fs.writeFileSync(path.join(output, 'browser-tests.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(`${report.scenarios} browser scenarios passed; ${report.checkedPixels.toLocaleString()} pixels checked; DPR 1, 1.25, 1.5, 2.`);
}

main().catch(error => {
  const output = path.resolve(__dirname, '../../build/small-font-lab');
  fs.mkdirSync(output, {recursive: true});
  fs.writeFileSync(path.join(output, 'browser-tests.json'), JSON.stringify({ok: false, error: error.stack}, null, 2) + '\n');
  process.stderr.write(error.stack + '\n', () => process.exit(1));
});
