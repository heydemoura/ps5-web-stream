/* Browser-independent behavior regressions for the shared dashboard/player.
 * Run with node tests/test_preview.cjs; no network or PS5 settings are touched. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const dashboardHTML = fs.readFileSync('web/index.html', 'utf8');
const playerHTML = fs.readFileSync('web/player.html', 'utf8');
const code = fs.readFileSync('web/app.js', 'utf8');
for (const html of [dashboardHTML, playerHTML]) {
  assert(html.includes('<script src="/vendor/flv.min.js"></script>'));
  assert(html.includes('<script src="/app.js"></script>'));
  assert(!/<script src="https?:/.test(html), 'Both pages must run without a CDN');
}
assert(/<video[^>]*autoplay[^>]*muted[^>]*playsinline/.test(playerHTML),
  'The dedicated player must declare muted inline autoplay');
assert.deepEqual([...dashboardHTML.matchAll(/<option value="([^"]+)"/g)].map(match => match[1]),
  ['4k', '1440p', '1080p', '720p']);

const streaming = (overrides = {}) => ({capture_state:'streaming', error:0,
  video_frames:10, audio_frames:20, video_age_ms:0, audio_age_ms:0,
  video:'1920x1080', fps:60, audio:true, clients:0, applying:false,
  requested_resolution:'1080p', active_resolution:'1080p',
  settings_revision:1, stream_revision:1, ...overrides});
const settle = () => new Promise(resolve => setImmediate(resolve));
function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return {promise, resolve, reject};
}
function response(body, status = 200) {
  return {ok:status >= 200 && status < 300, status,
    json:async () => ({...body}), text:async () => String(body)};
}
function harness({fullscreen = false, initial = streaming(), blocked = false} = {}) {
  const html = fullscreen ? playerHTML : dashboardHTML;
  const nodes = new Map();
  for (const [,id] of html.matchAll(/\bid="([^"]+)"/g)) {
    nodes.set(id, {textContent:'', className:'', style:{}, listeners:{}, disabled:false,
      value:id === 'resolution' ? '1080p' : '',
      addEventListener(event, callback) { this.listeners[event] = callback; }});
  }
  const node = id => nodes.get(id) || null;
  let now = 1000, clockID = 0, current = {...initial};
  let offline = false, support = true, playBlocked = blocked, playCalls = 0;
  let postHandler = () => response({requested_resolution:node('resolution').value}, 202);
  const timers = new Map(), players = [], requests = [], windowListeners = {};
  const video = node('v');
  Object.assign(video, {paused:true, currentTime:0, muted:false, readyState:0,
    buffered:{length:0}, play() {
      playCalls++;
      if (playBlocked) return Promise.reject(new Error('NotAllowedError'));
      this.paused = false;
      return Promise.resolve();
    }});
  const flvjs = {isSupported:() => support, Events:{ERROR:'error', LOADING_COMPLETE:'complete'},
    createPlayer(source, options) {
      const player = {source, options, dead:false, events:{},
        attachMediaElement() {}, on(event, callback) { this.events[event] = callback; }, load() {},
        destroy() { this.dead = true; video.paused = true; video.readyState = 0; video.currentTime = 0; }};
      players.push(player);
      return player;
    }};
  const context = {
    document:{body:{dataset:fullscreen ? {player:'fullscreen'} : {}}, getElementById:node},
    location:{href:'http://192.168.0.190:8081/' + (fullscreen ? 'player' : ''), hostname:'192.168.0.190'},
    window:{flvjs, addEventListener(event, callback) { windowListeners[event] = callback; }}, flvjs,
    URL, AbortController, Date:{now:() => now},
    setTimeout(callback, ms) { const id = ++clockID; timers.set(id, {callback,ms,due:now+ms}); return id; },
    clearTimeout(id) { timers.delete(id); },
    async fetch(path, options = {}) {
      requests.push({path, options});
      if (offline) throw new Error('offline');
      if (path === '/status') return response(current);
      if (path === '/log') return response('capture log');
      if (path !== '/settings') throw new Error('Unexpected URL ' + path);
      const result = Promise.resolve(postHandler(options));
      return new Promise((resolve, reject) => {
        const abort = () => reject(new Error('AbortError'));
        options.signal.addEventListener('abort', abort, {once:true});
        result.then(resolve, reject).finally(() => options.signal.removeEventListener('abort', abort));
      });
    }
  };
  vm.runInNewContext(code, context);
  return {
    node, video, players, requests, timers,
    get playCalls() { return playCalls; },
    status(value) { current = {...current, ...value}; },
    offline(value) { offline = value; },
    supported(value) { support = value; },
    blockPlay(value) { playBlocked = value; },
    post(handler) { postHandler = handler; },
    advance(ms) { now += ms; },
    async poll() {
      const next = [...timers].find(([,task]) => task.ms === 1000);
      assert(next, 'Status polling must continue');
      timers.delete(next[0]); now = Math.max(now, next[1].due);
      await next[1].callback();
    },
    async fireTimeout(ms) {
      const next = [...timers].find(([,task]) => task.ms === ms);
      assert(next, 'Expected timeout of ' + ms + 'ms');
      timers.delete(next[0]); now = Math.max(now, next[1].due);
      await next[1].callback(); await settle();
    },
    choose(value) { node('resolution').value = value; node('resolution').onchange(); },
    submit() {
      let prevented = false;
      const result = node('settings-form').onsubmit({preventDefault() { prevented = true; }});
      assert(prevented, 'Settings submit must not navigate away');
      return result;
    },
    progress(time = 1) {
      video.paused = false; video.readyState = 2; video.currentTime = time;
      video.listeners.timeupdate();
    },
    close() { windowListeners.pagehide(); }
  };
}

const cases = [];
function test(name, run) { cases.push({name, run}); }

test('fullscreen player autoplays muted without a gesture and skips dashboard-only requests', async () => {
  const h = harness({fullscreen:true}); await settle();
  assert.equal(h.node('settings-form'), null, 'Fullscreen DOM must not invent dashboard controls');
  assert.equal(h.players.length, 1);
  assert.equal(h.playCalls, 1);
  assert.equal(h.video.muted, true);
  assert.equal(h.players[0].source.url, 'http://192.168.0.190:8081/live.flv');
  assert.equal(h.players[0].source.hasAudio, true);
  assert.equal(h.node('state').textContent, 'buffering', 'play() alone cannot prove live media');
  h.progress();
  assert.equal(h.node('state').textContent, 'live');
  await h.poll();
  assert(!h.requests.some(request => request.path === '/log' || request.path === '/settings'));
  h.close(); assert(h.players[0].dead); assert.equal(h.timers.size, 0);
});

test('dashboard waits for Play; capture failures and stale media never claim live', async () => {
  const h = harness({initial:streaming({capture_state:'error', error:-2120941560,
    video_frames:0, video_age_ms:-1})}); await settle();
  assert.equal(h.players.length, 0);
  assert.equal(h.node('state').textContent, 'capture unavailable');
  h.node('play').onclick();
  assert.equal(h.players.length, 0);
  h.status({capture_state:'streaming', error:0, video_frames:10, video_age_ms:0});
  await h.poll(); assert.equal(h.players.length, 1);
  h.progress(); assert.equal(h.node('state').textContent, 'live');
  h.status({video_age_ms:3000}); await h.poll();
  assert(h.players[0].dead);
  assert.equal(h.node('state').textContent, 'waiting for capture');
  h.close();
  const idle = harness(); await settle();
  assert.equal(idle.players.length, 0, 'A dashboard status poll is not a request to play');
  assert.equal(idle.node('state').textContent, 'stream available');
  idle.close();
});

test('fullscreen player waits through applying, then autoplays the ready source', async () => {
  const h = harness({fullscreen:true, initial:streaming({applying:true, active_resolution:null})});
  await settle(); assert.equal(h.players.length, 0);
  h.status({applying:false, active_resolution:'1080p'}); await h.poll();
  assert.equal(h.players.length, 1); assert.equal(h.playCalls, 1);
  h.close();
});

test('blocked autoplay offers a persistent native-control fallback and recovers on playback', async () => {
  const h = harness({fullscreen:true, blocked:true}); await settle();
  assert.equal(h.players.length, 1); assert.equal(h.playCalls, 1);
  assert.equal(h.node('state').textContent, 'press video Play');
  assert.match(h.node('ov').textContent, /video controls/);
  await h.poll();
  assert.equal(h.node('state').textContent, 'press video Play');
  assert.equal(h.players.length, 1, 'Autoplay rejection must not cause a reconnect loop');
  h.blockPlay(false); await h.video.play(); h.progress();
  assert.equal(h.node('state').textContent, 'live');
  h.close();
});

test('unsupported browser and offline errors remain truthful across polls', async () => {
  const h = harness({fullscreen:true, initial:streaming({applying:true})}); await settle();
  h.supported(false); h.status({applying:false}); await h.poll();
  assert.equal(h.node('state').textContent, 'preview unsupported');
  await h.poll(); assert.equal(h.node('state').textContent, 'preview unsupported');
  assert.equal(h.players.length, 0);
  h.offline(true); await h.poll();
  assert.equal(h.node('state').textContent, 'console unreachable');
  h.close();
});

test('polling preserves an unapplied capture-resolution selection', async () => {
  const h = harness(); await settle();
  assert.equal(h.node('resolution').value, '1080p');
  h.choose('1440p');
  for (let i = 0; i < 3; i++) await h.poll();
  assert.equal(h.node('resolution').value, '1440p');
  assert.equal(h.requests.filter(request => request.path === '/settings').length, 0,
    'Changing the selector alone must not restart capture');
  h.close();
});

test('settings POST sends the chosen preset once, protects pending work, then follows accepted state', async () => {
  const h = harness(); await settle();
  const post = deferred(); h.post(() => post.promise); h.choose('720p');
  const first = h.submit();
  assert.equal(h.node('apply').disabled, true);
  await h.submit();
  const calls = h.requests.filter(request => request.path === '/settings');
  assert.equal(calls.length, 1, 'Duplicate submission must not issue a second mutation');
  assert.equal(calls[0].options.method, 'POST');
  assert.equal(calls[0].options.headers['Content-Type'], 'application/x-www-form-urlencoded');
  assert.equal(calls[0].options.headers['X-PS5HS-Request'], '1');
  assert.equal(calls[0].options.body, 'resolution=720p');
  assert(calls[0].options.signal instanceof AbortSignal);
  await h.poll(); assert.equal(h.node('resolution').value, '720p');
  assert.match(h.node('settings-message').textContent, /Requesting 720p/);
  h.status({requested_resolution:'720p', applying:true, active_resolution:null, settings_revision:2});
  post.resolve(response({requested_resolution:'720p'}, 202)); await first;
  assert.equal(h.node('apply').disabled, false);
  await h.poll(); assert.match(h.node('settings-message').textContent, /Applying 720p/);
  h.status({applying:false, active_resolution:'720p', video:'1280x720', stream_revision:2});
  await h.poll(); assert.match(h.node('settings-message').textContent, /Capturing 1280x720/);
  assert.equal(h.node('resolution').value, '720p');
  h.close();
});

test('a newer selection made during an older POST is preserved after its response', async () => {
  const h = harness(); await settle();
  const post = deferred(); h.post(() => post.promise);
  h.choose('1440p'); const pending = h.submit();
  if (!h.node('resolution').disabled) h.choose('4k');
  const expected = h.node('resolution').value;
  h.status({requested_resolution:'1440p', settings_revision:2, applying:true});
  post.resolve(response({requested_resolution:'1440p'}, 202)); await pending; await h.poll();
  assert.equal(h.node('resolution').value, expected,
    'Acknowledging an older request must not mark a newer local selection clean');
  h.close();
});

test('settings failure stays visible and preserves the choice until the user retries or edits', async () => {
  const h = harness(); await settle();
  h.choose('4k'); h.post(() => response({}, 503)); await h.submit();
  assert.equal(h.node('apply').disabled, false);
  assert.match(h.node('settings-message').textContent, /Could not change resolution/);
  await h.poll();
  assert.equal(h.node('resolution').value, '4k');
  assert.match(h.node('settings-message').textContent, /Could not change resolution/,
    'An unrelated successful status poll must not erase the failed mutation');
  h.choose('720p'); h.post(() => response({requested_resolution:'720p'}, 202));
  await h.submit();
  assert.match(h.node('settings-message').textContent, /Applying 720p/);
  h.close();
});

test('settings timeout aborts the request and permits a retry', async () => {
  const h = harness(); await settle();
  const stalled = deferred(); h.post(() => stalled.promise); h.choose('1440p');
  const pending = h.submit();
  await h.fireTimeout(5000); await pending;
  const mutation = h.requests.find(request => request.path === '/settings');
  assert.equal(mutation.options.signal.aborted, true);
  assert.equal(h.node('apply').disabled, false);
  assert.match(h.node('settings-message').textContent, /Could not change resolution/);
  h.post(() => response({requested_resolution:'1440p'}, 202)); await h.submit();
  assert.equal(h.requests.filter(request => request.path === '/settings').length, 2);
  h.close();
});

test('capture generations and resolution changes replace the player while preserving user audio choice', async () => {
  const h = harness({fullscreen:true}); await settle(); h.progress();
  h.node('mute').onclick(); assert.equal(h.video.muted, false);
  h.status({stream_revision:2}); await h.poll();
  assert(h.players[0].dead); assert.equal(h.players.length, 2);
  assert.equal(h.video.muted, false);
  h.status({settings_revision:2, requested_resolution:'720p', active_resolution:null, applying:true});
  await h.poll(); assert(h.players[1].dead); assert.equal(h.players.length, 2);
  h.status({active_resolution:'720p', video:'1280x720', stream_revision:3, applying:false});
  await h.poll(); assert.equal(h.players.length, 3);
  assert.equal(h.playCalls, 3);
  h.progress(); assert.equal(h.node('state').textContent, 'live');
  h.node('play').onclick(); assert(h.players[2].dead);
  h.status({stream_revision:4}); await h.poll();
  assert.equal(h.players.length, 3, 'Capture restart must not undo an explicit Stop');
  h.node('reload').onclick(); assert.equal(h.players.length, 4);
  h.close();
});

test('stream completion and errors reconnect with backoff; old callbacks cannot destroy a replacement', async () => {
  const h = harness({fullscreen:true}); await settle();
  h.players[0].events.complete(); assert(h.players[0].dead);
  await h.poll(); assert.equal(h.players.length, 2);
  h.players[0].events.error('network', 'stale event');
  assert.equal(h.players[1].dead, false);
  h.players[1].events.error('network', 'interrupted');
  assert(h.players[1].dead);
  await h.poll(); assert.equal(h.players.length, 2, 'Retry must respect backoff');
  h.advance(3000); await h.poll(); assert.equal(h.players.length, 3);
  h.close();
});

(async () => {
  let failures = 0;
  for (const {name, run} of cases) {
    try { await run(); console.log('PASS:', name); }
    catch (error) { failures++; console.error('FAIL:', name); console.error(error); }
  }
  if (failures) process.exitCode = 1;
  else console.log('Dashboard/player behavior:', cases.length, 'scenarios passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
