/* Real-browser checks for autoplay, forced-hidden controls, and glass layouts.
 * Uses the same PLAYWRIGHT_MODULE, CHROME_PATH and PS5HS_TEST_URL as test_web_player.
 * Does not change console capture settings. */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright-core');
(async () => {
  const browser = await chromium.launch({headless:true,
    ...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {})});
  const base = process.env.PS5HS_TEST_URL || 'http://127.0.0.1:18080';
  const errors = [];
  try {
    const early = await browser.newPage();
    await early.route('**/app.js', route => route.abort());
    await early.goto(base + '/player?controls=0');
    assert.equal(await early.locator('.player-controls').isVisible(), false,
      'Hidden mode must apply before application initialization');
    await early.close();
    for (const route of ['/', '/player', '/player?controls=0', '/player/?controls=false', '/player?controls=1', '/player?controls=invalid', '/?controls=0']) {
      const page = await browser.newPage();
      page.on('pageerror', error => errors.push(String(error)));
      const hidden = route.includes('/player') && /controls=(0|false)/.test(route);
      await page.goto(base + route);
      await page.waitForFunction(() => {
        const v = document.getElementById('v');
        return !v.paused && v.muted && v.currentTime > 1 && v.videoWidth > 0;
      }, null, {timeout:30000});
      for (const viewport of [{width:1280,height:720},{width:390,height:844}]) {
        await page.setViewportSize(viewport);
        const bar = page.locator('.player-controls');
        if (hidden) {
          assert.equal(await bar.isVisible(), false);
          assert.equal(await page.locator('.player-heading').isVisible(), false);
          await page.locator('video').hover();
          await page.locator('video').click();
          for (let i=0;i<8;i++) await page.keyboard.press('Tab');
          assert.equal(await bar.isVisible(), false, 'Hover, click and focus must not reveal controls');
          assert(await page.evaluate(() => !document.activeElement.closest('.player-controls')));
          assert.equal(await page.locator('video').evaluate(v => v.controls), false);
          const time = await page.locator('video').evaluate(v => v.currentTime);
          await page.waitForFunction(t => document.getElementById('v').currentTime > t + .5, time);
        } else {
          assert(await bar.isVisible());
          assert(await bar.evaluate(el => getComputedStyle(el).backdropFilter.includes('blur')));
          assert(await bar.evaluate(el => {
            const r = el.getBoundingClientRect();
            return r.left >= 0 && r.right <= innerWidth && r.bottom <= document.documentElement.scrollHeight;
          }));
          await page.locator('#quality-menu summary').click();
          assert(await page.locator('.settings-popover').evaluate(el => {
            const r=el.getBoundingClientRect(), player=el.closest('media-container').getBoundingClientRect();
            return r.left >= player.left && r.right <= player.right && r.top >= player.top && r.bottom <= player.bottom;
          }), 'Settings stay within the player');
          await page.locator('#quality-menu summary').press('Escape');
        }
      }
      console.log(route + ': muted autoplay, ' + (hidden ? 'controls remain hidden' : 'glass controls visible') + ', desktop/mobile passed');
      await page.close();
    }
    assert.deepEqual(errors, []);
  } finally { await browser.close(); }
})().catch(error => {console.error(error); process.exitCode=1;});
