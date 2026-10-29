(function(){
  'use strict';
  var v = document.getElementById('v');
  var ov = document.getElementById('ov');
  var dot = document.getElementById('dot');
  var state = document.getElementById('state');
  var playButton = document.getElementById('play');
  var muteButton = document.getElementById('mute');
  var fullscreenPage = document.body.dataset.player === 'fullscreen';
  var player = null, wanted = fullscreenPage, status = null, timer = null;
  var settingsBusy = false, settingsDirty = false, settingsError = '';
  var resolution = document.getElementById('resolution');
  var settingsForm = document.getElementById('settings-form');
  var settingsMessage = document.getElementById('settings-message');
  var previewIssue = null, playBlocked = false;
  var lastTime = -1, lastProgress = 0, retryAfter = 0, stopped = false;
  var flvUrl = new URL('/live.flv', location.href).href;
  var playerLink = document.getElementById('player-link');
  if (playerLink) playerLink.textContent = new URL('/player', location.href).href;
  v.muted = true;

  function setState(text, cls){
    state.textContent = text;
    dot.className = 'dot ' + (cls || '');
  }
  function overlay(text){ ov.textContent = text; ov.style.display = 'flex'; }
  function sourceReady(){
    return status && !status.applying && status.capture_state === 'streaming' &&
      status.video_frames > 0 && status.video_age_ms >= 0 && status.video_age_ms < 3000;
  }
  function destroyPlayer(){
    var old = player;
    player = null;
    lastTime = -1; lastProgress = 0; playBlocked = false;
    if (old) { try { old.destroy(); } catch (e) {} }
  }
  function captureMessage(){
    if (!status) return 'Connecting to console…';
    if (status.capture_state === 'error') {
      var code = '0x' + (status.error >>> 0).toString(16).padStart(8, '0');
      return 'Capture unavailable (' + code + '). See the payload log.';
    }
    if (status.capture_state === 'streaming') return 'Waiting for new video from the console…';
    return 'Waiting for console capture…';
  }
  function render(){
    playButton.textContent = wanted ? 'Stop' : 'Play';
    playButton.hidden = !wanted;
    if (!sourceReady()) {
      setState(status && status.capture_state === 'error' ? 'capture unavailable' : 'waiting for capture',
        status && status.capture_state === 'error' ? 'err' : '');
      overlay(captureMessage());
    } else if (previewIssue) {
      setState('preview unsupported', 'err');
      overlay(previewIssue);
    } else if (playBlocked) {
      setState('press video Play', '');
      overlay('Press Play in the video controls to begin playback.');
    } else if (!wanted) {
      setState('stream available', '');
      overlay('Press Play to start the live preview.');
    } else if (lastProgress && Date.now() - lastProgress < 3000 && !v.paused) {
      setState('live', 'live');
      ov.style.display = 'none';
    } else if (player && v.paused && v.currentTime > 0) {
      setState('preview paused', '');
      overlay('Press Play in the video controls to resume.');
    } else {
      setState('buffering', '');
      overlay('Waiting for video playback…');
    }
  }
  function startPlayer(){
    if (!wanted || player || !sourceReady() || Date.now() < retryAfter) return;
    if (!window.flvjs || !flvjs.isSupported()) {
      wanted = false;
      previewIssue = 'This browser cannot play the preview. Try a browser with H.264 and AAC MediaSource support.';
      render();
      return;
    }
    try {
      var created = flvjs.createPlayer(
        {type:'flv', url:flvUrl, isLive:true, hasAudio:!!status.audio, hasVideo:true},
        {enableStashBuffer:false, lazyLoad:false, autoCleanupSourceBuffer:true,
         autoCleanupMaxBackwardDuration:15, autoCleanupMinBackwardDuration:5});
      player = created;
      created.attachMediaElement(v);
      created.on(flvjs.Events.ERROR, function(type, detail){
        if (player !== created) return;
        destroyPlayer();
        retryAfter = Date.now() + 3000;
        setState('playback error', 'err');
        overlay('Playback interrupted: ' + detail + '. Reconnecting…');
      });
      created.on(flvjs.Events.LOADING_COMPLETE, function(){
        if (player !== created) return;
        destroyPlayer(); retryAfter = Date.now() + 500;
      });
      created.load();
      var promise = v.play();
      if (promise && promise.catch) promise.catch(function(){
        if (player !== created) return;
        playBlocked = true;
        render();
      });
      render();
    } catch (e) {
      destroyPlayer();
      retryAfter = Date.now() + 3000;
      setState('playback error', 'err');
      overlay('Could not start playback. Reconnecting…');
    }
  }
  v.addEventListener('timeupdate', function(){
    if (!player) return;
    if (v.currentTime > lastTime && !v.paused && v.readyState >= 2) {
      playBlocked = false;
      lastTime = v.currentTime;
      lastProgress = Date.now();
    }
    // Keep live viewing close to the newest buffered frames after a stall.
    if (!v.paused && v.buffered.length) {
      var edge = v.buffered.end(v.buffered.length - 1);
      if (edge - v.currentTime > 3) v.currentTime = Math.max(0, edge - 0.5);
    }
    render();
  });
  v.addEventListener('pause', render);
  v.addEventListener('waiting', function(){
    lastProgress = 0;
    render();
  });
  v.addEventListener('volumechange', function(){
    muteButton.textContent = v.muted ? '🔇 Unmute' : '🔊 Mute';
  });
  playButton.onclick = function(){
    wanted = !wanted;
    previewIssue = null; playBlocked = false;
    retryAfter = 0;
    if (!wanted) destroyPlayer();
    render();
    startPlayer();
  };
  var playbackControl = document.getElementById('playback');
  function requestPlayback(){
    if (!wanted) {
      wanted = true; previewIssue = null; playBlocked = false; retryAfter = 0;
      startPlayer(); render();
    }
  }
  if (playbackControl) {
    playbackControl.addEventListener('click', requestPlayback, true);
    playbackControl.addEventListener('keydown', function(event){
      if (event.key === ' ' || event.key === 'Enter') requestPlayback();
    }, true);
  }
  var qualityMenu = document.getElementById('quality-menu');
  if (qualityMenu) qualityMenu.addEventListener('keydown', function(event){
    if (event.key === 'Escape') {
      qualityMenu.open = false;
      qualityMenu.querySelector('summary').focus();
    }
  });
  document.getElementById('reload').onclick = function(){
    wanted = true;
    previewIssue = null; playBlocked = false;
    retryAfter = 0;
    destroyPlayer();
    render();
    startPlayer();
  };
  muteButton.onclick = function(){ v.muted = !v.muted; };

  async function fetchWithTimeout(path, kind){
    var controller = new AbortController();
    var timeout = setTimeout(function(){ controller.abort(); }, 4000);
    try {
      var response = await fetch(path, {cache:'no-store', signal:controller.signal});
      if (!response.ok) throw new Error('HTTP ' + response.status);
      return await response[kind]();
    } finally { clearTimeout(timeout); }
  }
  async function poll(){
    try {
      var next = await fetchWithTimeout('/status', 'json');
      if (stopped) return;
      var sourceChanged = status && (status.audio !== next.audio ||
        status.stream_revision !== next.stream_revision ||
        status.settings_revision !== next.settings_revision);
      status = next;
      var res = document.getElementById('res');
      if (res) res.textContent = status.active_resolution ? status.video : 'Switching…';
      var fps = document.getElementById('fps');
      if (fps) fps.textContent = status.fps ? status.fps + ' fps' : '—';
      var audioActive = status.audio && status.audio_frames > 0 &&
        status.audio_age_ms >= 0 && status.audio_age_ms < 3000;
      var aud = document.getElementById('aud');
      if (aud) aud.textContent = audioActive ? '48 kHz stereo' : 'Waiting for audio';
      muteButton.disabled = !audioActive;
      var cli = document.getElementById('cli');
      if (cli) cli.textContent = status.clients;
      if (resolution && !settingsDirty && !settingsBusy) resolution.value = status.requested_resolution;
      if (settingsMessage && !settingsBusy) {
        settingsMessage.textContent = settingsError || (status.capture_state === 'error' ? captureMessage() :
          status.applying ? 'Applying ' + status.requested_resolution + '… Viewers reconnect automatically.' :
          'Capturing ' + status.video + '. This setting applies to all viewers.');
      }
      if (!sourceReady() || sourceChanged) destroyPlayer();
      render();
      startPlayer();
    } catch (e) {
      if (stopped) return;
      status = null;
      destroyPlayer();
      setState('console unreachable', 'err');
      overlay('Cannot reach the console. Check the payload and network connection.');
    }
    try {
      if (!fullscreenPage) {
      var log = await fetchWithTimeout('/log', 'text');
      if (stopped) return;
      var el = document.getElementById('log');
      el.textContent = log.slice(-4000);
      el.scrollTop = el.scrollHeight;
      }
    } catch (e) {}
    if (!stopped) timer = setTimeout(poll, 1000);
  }
  if (settingsForm) {
    resolution.onchange = function(){ settingsDirty = true; settingsError = ''; };
    settingsForm.onsubmit = async function(event){
      event.preventDefault();
      if (settingsBusy) return;
      settingsBusy = true;
      settingsError = '';
      var submittedResolution = resolution.value;
      var apply = document.getElementById('apply');
      apply.disabled = true;
      settingsMessage.textContent = 'Requesting ' + resolution.value + '…';
      var controller = new AbortController();
      var deadline = setTimeout(function(){ controller.abort(); }, 5000);
      try {
        var response = await fetch('/settings', {method:'POST', signal:controller.signal,
          headers:{'Content-Type':'application/x-www-form-urlencoded', 'X-PS5HS-Request':'1'},
          body:'resolution=' + encodeURIComponent(submittedResolution)});
        if (!response.ok) throw new Error('HTTP ' + response.status);
        var accepted = await response.json();
        settingsDirty = resolution.value !== submittedResolution;
        settingsMessage.textContent = 'Applying ' + accepted.requested_resolution + '…';
      } catch (error) {
        settingsError = 'Could not change resolution. Please try again.';
        settingsMessage.textContent = settingsError;
      } finally { clearTimeout(deadline); settingsBusy = false; apply.disabled = false; }
    };
  }
  window.addEventListener('pagehide', function(){
    stopped = true;
    wanted = false;
    clearTimeout(timer);
    destroyPlayer();
  });
  poll();
})();
