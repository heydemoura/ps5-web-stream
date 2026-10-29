/* Optional browser smoke test. Start tests/http_fixture on a local port, then:
 * PLAYWRIGHT_MODULE=/path/to/playwright-core PS5HS_TEST_URL=http://127.0.0.1:18080 \
 *   node tests/test_preview_browser.cjs
 * CHROME_PATH overrides the local Chromium executable when needed.
 */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright-core');
(async()=>{
  const browser = await chromium.launch({headless:true,
    ...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {})});
  try {
    const page = await browser.newPage({viewport:{width:1280,height:900}});
    const errors = [];
    const requests = [];
    page.on('pageerror', error=>errors.push(String(error)));
    page.on('request', request=>requests.push(request.url()));
    const url = process.env.PS5HS_TEST_URL || 'http://127.0.0.1:18080/';
    const origin = new URL(url).origin;
    await page.route('**/*', route=> new URL(route.request().url()).origin === origin ? route.continue() : route.abort());
    await page.goto(url);
    await page.waitForFunction(()=>document.getElementById('state').textContent === 'capture unavailable');
    await page.click('#play');
    assert.equal(await page.locator('#state').textContent(), 'capture unavailable');
    assert.equal(await page.evaluate(()=>typeof flvjs.createPlayer), 'function');
    assert(!requests.some(url=>url.endsWith('/live.flv')), 'Failed capture must not start loading a nonexistent stream');
    assert(requests.some(url=>url.endsWith('/vendor/flv.min.js')));
    assert(requests.every(url=>new URL(url).origin === origin), 'Preview must be self-contained');
    assert.deepEqual(errors, []);
    await page.setViewportSize({width:390,height:844});
    await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
    assert(await page.evaluate(()=>document.documentElement.scrollWidth <= innerWidth), 'Mobile preview must not overflow horizontally');
    if (process.env.PS5HS_SCREENSHOT) await page.screenshot({path:process.env.PS5HS_SCREENSHOT, fullPage:true});
    console.log('Browser smoke: local player loads, capture errors remain truthful, preview refuses nonexistent stream, mobile layout fits');
  } finally { await browser.close(); }
})().catch(error=>{ console.error(error); process.exitCode = 1; });
