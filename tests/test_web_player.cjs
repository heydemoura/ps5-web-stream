/* Opt-in browser/hardware integration. Opens /player without a user gesture,
 * changes capture through dashboard controls and verifies decoded dimensions.
 * PLAYWRIGHT_MODULE=/path/to/playwright-core CHROME_PATH=/path/to/chrome
 * PS5HS_TEST_URL=http://PS5:8081 node tests/test_web_player.cjs
 * PS5HS_TEST_MODES=none skips hardware settings for a fixed host fixture.
 */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright-core');
const dimensions = {'720p':[1280,720], '1080p':[1920,1080], '1440p':[2560,1440], '4k':[3840,2160]};
(async () => {
  const browser = await chromium.launch({headless:true,
    ...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {})});
  const base = new URL(process.env.PS5HS_TEST_URL || 'http://127.0.0.1:18080/').origin;
  const modes = process.env.PS5HS_TEST_MODES === 'none' ? [] :
    (process.env.PS5HS_TEST_MODES || '720p,1440p,4k,1080p').split(',');
  const errors = [], requests = [], results = [];
  const context = await browser.newContext({viewport:{width:1366,height:768}});
  context.on('page', page => {
    page.on('pageerror', error => errors.push(String(error)));
    page.on('request', request => requests.push(request.url()));
  });
  await context.route('**/*', route => new URL(route.request().url()).origin === base ?
    route.continue() : route.abort());
  try {
    const player = await context.newPage();
    await player.goto(base + '/player');
    await player.waitForFunction(() => {
      const v = document.getElementById('v');
      return document.getElementById('state').textContent === 'live' && v.currentTime > 1 &&
        v.videoWidth > 0 && v.webkitAudioDecodedByteCount > 0;
    }, null, {timeout:30000});
    assert(await player.locator('#v').evaluate(v => v.muted), 'Autoplay starts muted without a gesture');
    for (const viewport of [{width:1366,height:768}, {width:390,height:844}]) {
      await player.setViewportSize(viewport);
      assert(await player.locator('#v').evaluate(v => {
        const r = v.getBoundingClientRect();
        return r.x === 0 && r.y === 0 && r.width === innerWidth && r.height === innerHeight &&
          document.documentElement.scrollWidth === innerWidth;
      }), 'Player must fill the viewport without scrolling');
    }
    await player.locator('#mute').click();
    assert.equal(await player.locator('#v').evaluate(v => v.muted), false);
    const dashboard = await context.newPage();
    await dashboard.goto(base + '/');
    assert.equal(await dashboard.locator('#resolution option').count(), 4);
    assert.equal(await dashboard.locator('a[href="/player"]').count(), 1);
    for (const mode of modes) {
      await dashboard.locator('#resolution').selectOption(mode);
      await dashboard.locator('#apply').click();
      await dashboard.waitForFunction(mode =>
        document.getElementById('settings-message').textContent.includes('Capturing ' +
          ({'720p':'1280x720','1080p':'1920x1080','1440p':'2560x1440','4k':'3840x2160'})[mode]),
        mode, {timeout:30000});
      await player.waitForFunction(([w,h]) => {
        const v = document.getElementById('v');
        return document.getElementById('state').textContent === 'live' &&
          v.videoWidth === w && v.videoHeight === h && v.webkitAudioDecodedByteCount > 0;
      }, dimensions[mode], {timeout:30000});
      const sample = await player.locator('#v').evaluate(v => ({width:v.videoWidth, height:v.videoHeight,
        frames:v.getVideoPlaybackQuality().totalVideoFrames, audioBytes:v.webkitAudioDecodedByteCount,
        time:v.currentTime, muted:v.muted}));
      await player.waitForFunction(time => document.getElementById('v').currentTime > time + 1, sample.time);
      results.push({mode, ...sample});
      console.log('Dashboard applied capture resolution; fullscreen player resumed:', results.at(-1));
    }
    await player.locator('#reload').click();
    await player.waitForFunction(() => document.getElementById('state').textContent === 'live');
    await player.locator('#play').click();
    await player.waitForFunction(() => document.getElementById('state').textContent === 'stream available');
    assert.deepEqual(errors, []);
    assert(requests.every(url => new URL(url).origin === base));
    console.log('Autoplay without a gesture, viewport coverage, audio, dashboard controls and reconnect passed.');
  } finally {
    if (modes.length) {
      await context.request.post(base + '/settings', {form:{resolution:'1080p'},
        headers:{'X-PS5HS-Request':'1'}}).catch(() => {});
    }
    await browser.close();
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
