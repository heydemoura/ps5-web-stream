/* Behavioral checks for status/playback UI; no browser packages required. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync('web/index.html', 'utf8');
assert(html.includes('<script src="/vendor/flv.min.js"></script>'));
assert(!/<script src="https?:/.test(html), 'Preview must run without a CDN');
const code = html.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
const nodes = new Map();
function element(id) {
  if (!nodes.has(id)) nodes.set(id, {textContent:'', className:'', style:{}, listeners:{},
    addEventListener(event, fn) { this.listeners[event] = fn; }});
  return nodes.get(id);
}
const v = element('v');
Object.assign(v, {paused:true, currentTime:0, muted:true, readyState:0,
  buffered:{length:0}, play(){ this.paused = false; return Promise.resolve(); }});
let current = {capture_state:'error', error:-2120941560, video_frames:0, audio_frames:0,
  video_age_ms:-1, audio_age_ms:-1, video:'1920x1080', fps:60, audio:false, clients:0, rtmp_port:1935};
let offline = false;
const timers = new Map();
let timerID = 0;
const players = [];
const windowListeners = {};
const flvjs = {isSupported:()=>true, Events:{ERROR:'error'}, createPlayer(source) {
  const player = {source, dead:false, events:{},
    attachMediaElement(){}, on(event, callback){ this.events[event] = callback; }, load(){},
    destroy(){ this.dead = true; }};
  players.push(player);
  return player;
}};
const context = {document:{getElementById:element},
  location:{href:'http://192.168.0.190:8080/', hostname:'192.168.0.190'},
  window:{flvjs, addEventListener(event, callback){ windowListeners[event] = callback; }}, flvjs,
  URL, Date, AbortController,
  setTimeout(callback, ms){ const id = ++timerID; timers.set(id,{callback,ms}); return id; },
  clearTimeout(id){ timers.delete(id); },
  fetch:async path => {
    if (offline) throw new Error('offline');
    return {ok:true, json:async()=>({...current}), text:async()=> 'capture log'};
  }};
vm.runInNewContext(code, context);
async function settle(){ await new Promise(resolve=>setImmediate(resolve)); }
async function poll(){
  const entry = [...timers].find(([,value])=>value.ms === 2000);
  assert(entry, 'Status polling must continue');
  timers.delete(entry[0]);
  await entry[1].callback();
}
(async()=>{
  await settle();
  assert.equal(element('state').textContent, 'capture unavailable');
  element('play').onclick();
  assert.equal(players.length, 0, 'Failed capture must not create a player');
  assert(!element('dot').className.includes('live'));
  Object.assign(current, {capture_state:'streaming', error:0, video_frames:10, video_age_ms:0});
  await poll();
  assert.equal(players.length, 1);
  assert.equal(players[0].source.hasAudio, false, 'Video-only source must not wait for missing audio');
  assert.equal(element('state').textContent, 'buffering', 'A successful play() is not proof of live video');
  v.readyState = 2;
  v.currentTime = 0.1;
  v.listeners.timeupdate();
  assert.equal(element('state').textContent, 'live');
  v.listeners.waiting();
  assert.equal(element('state').textContent, 'buffering');
  Object.assign(current, {audio:true, audio_frames:20, audio_age_ms:0});
  await poll();
  assert(players[0].dead);
  assert.equal(players[1].source.hasAudio, true);
  assert.equal(element('mute').disabled, false);
  Object.assign(current, {capture_state:'error', error:-2120941560});
  await poll();
  assert(players[1].dead);
  assert.equal(element('state').textContent, 'capture unavailable');
  offline = true;
  await poll();
  assert.equal(element('state').textContent, 'console unreachable');
  offline = false;
  Object.assign(current, {capture_state:'streaming', error:0});
  flvjs.isSupported = ()=>false;
  await poll();
  assert.equal(element('state').textContent, 'preview unsupported');
  await poll();
  assert.equal(element('state').textContent, 'preview unsupported', 'Polling must preserve browser capability errors');
  windowListeners.pagehide();
  assert.equal(timers.size, 0);
  console.log('Preview behavior: capture failure, startup buffering, observed playback, audio transition, reconnect and offline state passed');
})().catch(error=>{ console.error(error); process.exitCode = 1; });
