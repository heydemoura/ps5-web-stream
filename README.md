# PS5 Web Stream

A web-only PlayStation 5 homebrew payload that streams console video and audio
over your local network, without a capture card. The console serves the
settings dashboard, a fullscreen player and the H.264/AAC media stream.

**Current verification:** native 720p, 1080p, 1440p and 4K H.264 video with 48 kHz
stereo AAC decode correctly on the test console. Chrome verified automatic
muted playback, desktop/mobile viewport coverage, audio, and all four resolution
changes from a separate dashboard tab. See
[validation results](docs/VALIDATION.md).

The test instance uses:

- Dashboard: **[http://192.168.0.190:8081/](http://192.168.0.190:8081/)**
- Fullscreen player: **[http://192.168.0.190:8081/player](http://192.168.0.190:8081/player)**

The normal build default is HTTP port `8080`.

## Watch and configure

Open `/player` to fill the browser viewport with live video. Both the dashboard
and fullscreen player start playback muted automatically, without pressing Play. Select **Unmute** for audio. If the
browser blocks playback, use its video Play control. A browser with H.264/AAC
MediaSource support is required.

For a clean fullscreen view, use **`/player?controls=0`** (or
`/player?controls=false`). This hides the glass control bar and status header
from the first paint; hover, tap and keyboard focus do not reveal them. Autoplay
and reconnection continue, with audio muted by default. Omit the parameter or
use `controls=1` to show controls again. The option applies only to `/player`.
Non-interactive connection/error messages remain visible when needed.

Player URL options can be combined:

| Parameter | Meaning |
|---|---|
| `mute=0` | Start unmuted; preserve this state across reconnects |
| `mute=1` | Start muted (also the default) |
| `resolution=1` | 720p |
| `resolution=2` | 1080p |
| `resolution=3` | 1440p |
| `resolution=4` | 4K |

Example: `/player?controls=0&mute=0&resolution=4`.
Resolution is applied once on page load and changes capture for all viewers.
Invalid values are ignored. If the browser blocks autoplay with sound, tap the
video to start; controls stay hidden and audio is not silently muted.

### Use the fullscreen player in OBS

1. In OBS, add a **Browser** source to the scene.
2. Set its URL to the player URL, for example:
   `http://192.168.0.190:8081/player?controls=0&mute=0&resolution=4`.
   Replace the address with the console’s reachable IP and port.
3. Set the source dimensions to match the output you want (for example,
   3840 × 2160 for 4K, 2560 × 1440 for 1440p, or 1920 × 1080 for 1080p), then
   confirm the source is visible in the scene.
4. Keep `controls=0` for a clean feed. Remove it or set `controls=1` when you
   want the in-player settings and playback controls. Choose `mute=1` if you
   want OBS to receive no player audio; `mute=0` enables audio when the browser
   allows unmuted autoplay.

The `resolution` option changes the console’s shared capture mode, so it also
changes the stream seen by other viewers. OBS browser audio is controlled by
`mute`; use the Browser source’s audio settings in OBS to route or monitor it.



Both `/` and `/player` use Video.js 10 with controls inside the video: play/pause,
mute, volume, reconnect, picture-in-picture and fullscreen. Open **Settings**
inside the player to choose a resolution, select **Apply resolution**, or Stop
the stream. The dashboard at `/` also shows capture diagnostics. This changes the
console capture for **all viewers**; connected players reconnect automatically.
Changing the selector alone does not apply it. Requested settings remain in
memory for the current payload session; restarting the payload selects 1080p.

| Selection | Capture dimensions | Configured fps / kbps | Console verification |
|---|---|---|---|
| 720p | 1280 × 720 | 60 / 5000 | Video + audio passed |
| 1080p | 1920 × 1080 | 60 / 8000 | Video + audio passed; default |
| 1440p | 2560 × 1440 | 60 / 16000 | Video + audio passed |
| 4K | 3840 × 2160 | 60 / 32000 | Video + audio passed |

Rates are encoder configuration values; actual cadence and bitrate can vary.
The selected resolution is a capture request, not a browser scaling option.
`/status` distinguishes the requested mode from the active mode. A mode change
can take several seconds while native resources are reallocated.

## Architecture

```
libSceAvcap2 video ── H.264 ──────────────────────┐
libSceAvcap2 audio ── float PCM ── libSceM4aacEnc ─┤
                                                ▼
                                           FLV muxer
                                                ▼
                                      bounded broadcaster
                                                ▼
                                           HTTP server
                                  dashboard / player / live.flv
```

Firmware libraries are resolved at runtime. Native H.264 capture uses
ResourceArbitrator negotiation, complete capture records, SPS/PPS and periodic
keyframe requests. Native AJM encoding supplies AAC. Source clocks are mapped
to a continuous audio/video timeline, including capture restarts.

One capture loop serves up to **eight media viewers**. Each has a bounded queue;
slow viewers resume at a keyframe with codec headers without blocking capture.
The HTML, CSS, application JavaScript, pinned Video.js components and flv.js
transport are embedded in the
ELF, so no CDN is needed. Capture failures and pending changes remain visible;
the player shows “live” only after playback advances.

## Build and deploy

Install the [ps5-payload-dev SDK][sdk], a recent LLVM, `make`, and Python 3:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk  # or ~/opt/ps5-payload-sdk
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config  # macOS
make HTTP_PORT=8081
```

This builds `ps5-web-stream.elf`. Omit `HTTP_PORT` to use `8080`. Web assets are
embedded by `tools/embed.py`; `xxd` is not required.

Stop an existing capture copy, then send the compiled ELF unchanged to a running
ELF loader:

```sh
python3 tools/deploy.py 192.168.0.190 9021 ps5-web-stream.elf
```

Alternatively, build and upload together:

```sh
make deploy PS5_HOST=192.168.0.190 HTTP_PORT=8081
```

**`make deploy` rebuilds the ELF.** Repeat the `HTTP_PORT` override or it rebuilds
with port `8080`. The loader port defaults to `9021` (`PS5_PORT` overrides it).
Upload success confirms transfer; check `/status` and `/log` for capture results.
A loader supporting arguments can pass `[http_port]`; the HTTP port must be
between 1 and 65535. Native capture open negotiates its capture tap; no separate
scan-in helper or manual tap setup is required. See
[capture API notes](docs/AVCAP2.md).

## HTTP API

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/` | Dashboard and capture controls |
| GET | `/player` | Fullscreen page with muted autoplay |
| GET | `/live.flv` | Live H.264/AAC HTTP-FLV stream |
| GET | `/status` | Capture errors, counters, frame age and settings state |
| GET | `/settings` | Requested/active resolution and revision state |
| POST | `/settings` | Request a shared capture resolution |
| GET | `/log` | Recent payload log |

To request a resolution:

```sh
curl -X POST http://192.168.0.190:8081/settings \
  -H 'Content-Type: application/x-www-form-urlencoded' \
  -H 'X-PS5HS-Request: 1' --data 'resolution=1440p'
```

Valid values are `720p`, `1080p`, `1440p`, and `4k`. A `202 Accepted` response
acknowledges the request; it does not mean native capture succeeded. Check
`capture_state`, `requested_resolution`, `active_resolution`, and `applying` in
`/status`. `settings_revision` identifies requests, `capture_revision` identifies
the configuration being attempted, and `stream_revision` identifies capture
sessions so viewers can reconnect when the source changes.

## Tests

Host tests need Clang with AddressSanitizer/UndefinedBehaviorSanitizer,
Python 3, Node.js, and FFmpeg with `libx264` and AAC support:

```sh
make test
```

The suite runs without the SDK or console deployment. It covers firmware-call
lifecycle using mocks, clocks, muxing, bounded queues, HTTP/settings behavior,
autoplay, settings races, reconnect, and concurrent/late-join HTTP-FLV decoding.

Opt-in hardware tests decode every requested mode and exercise the dashboard
with a real browser. They change shared capture settings and restore 1080p on
exit. Commands, mode results and current limitations are in
[docs/VALIDATION.md](docs/VALIDATION.md).

## Limits

- All four presets use nominal 60 fps, verified at fixed 59.94/60-Hz SDR output.
  HDR, VRR and 120-Hz output are not covered by this validation.
- Old experimental capture clients can retain a failed native device. If native
  Start repeatedly fails after experimentation, restart the console and relaunch
  the jailbreak/loader before running this payload.
- Eight media viewers; 24 HTTP workers; up to 6 MiB / 1,024 pending tags per
  viewer, plus the tag currently being sent. HTTP headers are limited to 8 KiB
  with a five-second request deadline; stalled/disconnected clients are released.
- Resolution changes and capture recovery briefly interrupt playback. Settings
  are shared by viewers and reset to 1080p when the payload restarts.
- Cold-boot behavior, other firmware versions, HDR and multi-hour operation
  remain untested.
- Perceptual A/V offset and glass-to-glass latency have not been measured.
- This is a LAN server without authentication, TLS, internet relay or NAT traversal.

## Layout and license

`src/main.c` manages capture; `avcap.*` and `audio.*` implement native
media interfaces; `flv.*` muxes and broadcasts; `http.*`, `settings.*` and
`status.*` provide the web server and controls. `web/` contains the dashboard,
fullscreen player and embedded dependencies; `tests/` contains regressions,
fixtures and console probes.

For use on a console you own. No Sony code is included; firmware libraries are
resolved at runtime. Project code is GPLv3+ (`LICENSE`). The vendored flv.js
player is Apache-2.0; see its [license and provenance](web/vendor/README.md).

[sdk]: https://github.com/ps5-payload-dev/sdk

### Rebuild the Video.js bundle

The checked-in bundle keeps payload builds independent of Node.js and CDNs.
To update player imports, run `npm ci` then `npm run build:player`; commit the
lockfile and generated `web/vendor/videojs.min.js` together. The payload serves
all assets locally, including `/player.css` and `/vendor/videojs.min.js`.
