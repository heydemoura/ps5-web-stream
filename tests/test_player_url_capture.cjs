/* Opt-in console test: URL-selected resolution and unmuted playback, including
 * the hidden-controls gesture fallback. Restores 1080p after testing. */
const assert = require('node:assert/strict');
const {chromium} = require(process.env.PLAYWRIGHT_MODULE || 'playwright-core');
(async () => {
  const requireGesture = process.env.PS5HS_TEST_BLOCK_AUTOPLAY === '1';
  const browser = await chromium.launch({headless:true,
    ...(process.env.CHROME_PATH ? {executablePath:process.env.CHROME_PATH} : {})});
  const base = process.env.PS5HS_TEST_URL || 'http://192.168.0.190:8081';
  const context = await browser.newContext();
  try {
    const modes = requireGesture ? [[2,'1080p',1920,1080]] : [[1,'720p',1280,720],[2,'1080p',1920,1080],[3,'1440p',2560,1440],[4,'4k',3840,2160]];
    for (const [number,mode,w,h] of modes) {
      const page = await context.newPage(), posts = [], errors = [];
      if (requireGesture) await page.addInitScript(() => {
        // Headless Chromium may permit audible autoplay regardless of flags.
        // Simulate a policy rejection, then allow the real user click to play.
        let allowed = false;
        document.addEventListener('pointerdown', () => { allowed = true; }, true);
        document.addEventListener('play', event => {
          if (!allowed && !event.target.muted) event.target.pause();
        }, true);
        const play = HTMLMediaElement.prototype.play;
        HTMLMediaElement.prototype.play = function() {
          if (!this.muted && !allowed)
            return Promise.reject(new DOMException('Autoplay blocked', 'NotAllowedError'));
          return play.call(this);
        };
        document.addEventListener('DOMContentLoaded', () => {
          document.getElementById('v').autoplay = false;
        });
      });
      page.on('request',r=>{if(r.method()==='POST' && r.url().endsWith('/settings')) posts.push(r.postData());});
      page.on('pageerror',e=>errors.push(String(e)));
      await page.goto(base+'/player?controls=0&mute=0&resolution='+number);
      await page.waitForFunction(() => {
        const state=document.getElementById('state').textContent;
        return state==='live' || state==='press video Play';
      },null,{timeout:30000});
      const blocked=await page.locator('#v').evaluate(v=>v.paused);
      if(requireGesture) assert(blocked, 'Simulated autoplay rejection must exercise the tap fallback');
      if(blocked) await page.locator('#v').click();
      await page.waitForFunction(([w,h])=>{
        const v=document.getElementById('v');
        return !v.muted && !v.paused && v.currentTime>1 && v.videoWidth===w && v.videoHeight===h && v.webkitAudioDecodedByteCount>0;
      },[w,h],{timeout:30000});
      assert.equal(await page.locator('.player-controls').isVisible(),false);
      await page.waitForTimeout(2000);
      assert.deepEqual(posts,['resolution='+mode]);
      assert.deepEqual(errors,[]);
      console.log(JSON.stringify({resolution:number,mode,width:w,height:h,muted:false,controls:false,gestureRequired:blocked,passed:true}));
      await page.close();
    }
    const muted=await context.newPage();
    await muted.goto(base+'/player?controls=0&mute=1');
    await muted.waitForFunction(()=>document.getElementById('v').currentTime>1,null,{timeout:30000});
    assert(await muted.locator('#v').evaluate(v=>v.muted));
    console.log('Explicit mute=1 autoplay passed');
  } finally {
    await context.request.post(base+'/settings',{form:{resolution:'1080p'},headers:{'X-PS5HS-Request':'1'}}).catch(()=>{});
    await browser.close();
  }
})().catch(e=>{console.error(e);process.exitCode=1});
