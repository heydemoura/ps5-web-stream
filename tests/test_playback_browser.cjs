/* Real browser decode smoke; expects the live transport fixture or PS5 stream.
 * PLAYWRIGHT_MODULE=/path/to/playwright-core CHROME_PATH=/path/to/chrome \
 * PS5HS_TEST_URL=http://127.0.0.1:18080 node tests/test_playback_browser.cjs
 */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright-core');
(async () => {
  const browser = await chromium.launch({headless:true,
    ...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {})});
  try {
    const page = await browser.newPage();
    const errors = [], requests = [];
    page.on('pageerror', error => errors.push(String(error)));
    page.on('request', request => requests.push(request.url()));
    const url = process.env.PS5HS_TEST_URL || 'http://127.0.0.1:18080/';
    const origin = new URL(url).origin;
    await page.route('**/*', route => new URL(route.request().url()).origin === origin ? route.continue() : route.abort());
    await page.goto(url);
    await page.waitForFunction(() => document.getElementById('state').textContent === 'stream available');
    await page.click('#play');
    await page.waitForFunction(() => {
      const v = document.getElementById('v');
      return document.getElementById('state').textContent === 'live' && v.currentTime > 1 && v.videoWidth > 0;
    }, null, {timeout:20000});
    await page.click('#mute');
    const before = await page.evaluate(() => {
      const v = document.getElementById('v');
      return {time:v.currentTime,width:v.videoWidth,height:v.videoHeight,
        frames:v.getVideoPlaybackQuality().totalVideoFrames,audio:v.webkitAudioDecodedByteCount,muted:v.muted};
    });
    assert.equal(before.muted, false);
    assert(before.audio > 0, 'AAC must be decoded by the browser');
    await page.waitForFunction(t => document.getElementById('v').currentTime > t + 2, before.time);
    await page.click('#reload');
    await page.waitForFunction(() => document.getElementById('state').textContent === 'live');
    await page.click('#play');
    await page.waitForFunction(() => document.getElementById('state').textContent === 'stream available');
    assert(requests.every(u => new URL(u).origin === origin), 'All assets and media must be console-local');
    assert.deepEqual(errors, []);
    console.log('Browser playback decoded H.264 and AAC, unmuted, reconnected and stopped:', before);
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
