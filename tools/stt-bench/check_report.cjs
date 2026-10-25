/* Browser verification for the standalone report, including narrow screens. */
const fs = require('fs');
const path = require('path');
const { chromium } = require('/Users/cristianbianco/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
(async () => {
  const report = path.resolve(process.argv[2]);
  const output = path.resolve(process.argv[3]);
  fs.mkdirSync(output, {recursive: true});
  const browser = await chromium.launch({headless: true, args: ['--disable-gpu']});
  const errors = [];
  const results = [];
  for (const width of [1440, 736, 360]) {
    const page = await browser.newPage({viewport: {width, height: 1000}});
    page.on('pageerror', error => errors.push(String(error)));
    await page.goto('file://' + report);
    await page.waitForFunction(() => document.querySelectorAll('#numbers tr').length > 0);
    await page.waitForTimeout(150);
    const initial = await page.evaluate(() => ({
      rows: document.querySelectorAll('#numbers tr').length,
      charts: document.querySelectorAll('.chart svg').length,
      audio: document.querySelectorAll('audio').length,
      overflow: document.documentElement.scrollWidth > innerWidth,
      chartTextOutside: [...document.querySelectorAll('.chart svg')].flatMap(svg => {
        const bounds = svg.getBoundingClientRect();
        return [...svg.querySelectorAll('text')].filter(text => {
          const r = text.getBoundingClientRect();
          return r.left < bounds.left - 1 || r.right > bounds.right + 1 || r.top < bounds.top - 1 || r.bottom > bounds.bottom + 2;
        }).map(t => t.textContent);
      }),
      nonFiniteGeometry: [...document.querySelectorAll('.chart svg *')].some(e => /NaN|Infinity/.test([...e.attributes].map(a => a.value).join(' ')))
    }));
    await page.screenshot({path: path.join(output, `report-${width}.png`), fullPage: true});
    await page.locator('#conly').check();
    const cRows = await page.locator('#numbers tr').count();
    if (cRows !== 2) errors.push(`C-only rows ${cRows}, expected 2 at ${width}`);
    await page.locator('#scope').selectOption('long');
    if (await page.locator('#numbers tr').count() !== 2) errors.push(`Long/C-only rows mismatch at ${width}`);
    await page.locator('#channel').selectOption('MIC');
    if (await page.locator('#numbers tr').count() !== 0) errors.push(`Empty selection not respected at ${width}`);
    await page.locator('#channel').selectOption('all');
    await page.locator('#scope').selectOption('silence');
    await page.locator('#conly').uncheck();
    await page.locator('#transcripts > details').first().locator(':scope > summary').click();
    const media = await page.locator('audio').first().evaluate(async audio => {
      audio.load();
      await new Promise((resolve, reject) => { audio.onloadedmetadata = resolve; audio.onerror = reject; });
      return {duration: audio.duration, readyState: audio.readyState};
    });
    if (Math.abs(media.duration - 5) > 0.01) errors.push(`Audio duration mismatch: ${media.duration}`);
    if (initial.overflow || initial.nonFiniteGeometry || initial.chartTextOutside.length) errors.push(`Layout problem at ${width}: ${JSON.stringify(initial)}`);
    results.push({width, initial, cRows, media});
    await page.close();
  }
  await browser.close();
  fs.writeFileSync(path.join(output, 'browser-check.json'), JSON.stringify({results, errors}, null, 2));
  console.log(JSON.stringify({viewports: results.length, errors, output}));
  process.exit(errors.length ? 1 : 0);
})().catch(error => { console.error(error); process.exit(1); });
