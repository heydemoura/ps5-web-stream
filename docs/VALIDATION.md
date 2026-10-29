# Web-only validation results and limits

The current objective is console-served video/audio playback, an automatically
playing fullscreen `/player` page, and dashboard controls for **720p, 1080p,
1440p and 4K capture**. The active implementation has one HTTP listener and no
RTMP source, listener or build arguments.

**720p, 1080p, 1440p and 4K passed real-console video/audio decoding and the
all-mode browser test.** Chrome also passed muted autoplay without a gesture,
desktop/mobile viewport coverage, Unmute, reconnect and Stop. These results
distinguish native decoded dimensions from the size at which a browser
displays video.

## Current console results

The web-only payload was tested at `http://192.168.0.190:8081`; its build default
is `8080`. The resolution test selects each mode through `/settings`, waits for
that configuration to become active, records HTTP-FLV, checks stream dimensions
and decodes both tracks completely.

The [build identity](evidence/web-only-build.json) records the deployed ELF,
source hashes, base commit and startup as PID 194 on HTTP 8081. Its SHA-256 is
`cc58369cfd0376f9533a4418acb9f2b9e09ac440eaaa29eac50dac583425f600`.
The working tree had changes on top of `b4da0c6`; the source hashes identify the
tested implementation more precisely than that base commit alone.
The [runtime snapshot](evidence/web-only-runtime.json) and
[payload log](evidence/web-only-runtime.log) record successful mode changes and
fresh 1080p video/audio after the tests. HTTP 8081 was open and the former RTMP
port 1936 was closed. The snapshot also retains sample hashes and non-silent
4K audio measurements: −38.0 dB mean and −26.1 dB peak.

| Mode | Configured fps / kbps | Decoded video | Decoded audio | Result |
|---|---|---|---|---|
| 720p | 60 / 5000 | H.264, 1280×720, 179 frames | AAC, 48 kHz stereo, 139 frames | Passed; 3.035 s, no decode errors |
| 1080p | 60 / 8000 | H.264, 1920×1080, 177 frames | AAC, 48 kHz stereo, 139 frames | Passed; 3.035 s, no decode errors |
| 1440p | 30 / 16000 | H.264, 2560×1440, 91 frames | AAC, 48 kHz stereo, 140 frames | Passed; 3.069 s, no decode errors |
| 4K | 30 / 32000 | H.264, 3840×2160, 91 frames | AAC, 48 kHz stereo, 141 frames | Passed; 3.069 s, no decode errors |

Both tracks have strictly increasing decode timestamps in each sample. All
samples passed a full FFmpeg decode. Higher modes use ResourceArbitrator 182
and a matching per-client audio device; 1440p and 4K use the verified 30-fps
configuration. Higher-resolution 60-fps capture is not validated.

The [mode results](evidence/web-only-console-modes.json) retain status, decoded
dimensions, frame counts and decode checks. Captured console media remains
outside version control.

### Fullscreen player and dashboard

The real Chrome integration opened `/player` without a click and verified:

- Muted autoplay with decoded video and AAC; Unmute enabled audio playback.
- Video filled desktop and mobile viewports without page scrolling.
- A second tab changed capture using the dashboard's selector and Apply button.
- The player resumed at decoded 1280×720, 2560×1440, 3840×2160 and 1920×1080,
  with AAC, after the respective 720p, 1440p, 4K and 1080p requests. It retained
  the unmuted selection across those changes.
- Playback continued after each switch; explicit reconnect and Stop worked.
- All assets/media came from the console origin, with no JavaScript page errors.

The [browser log](evidence/web-only-browser.log) records decoded dimensions,
video frames, audio bytes and mute state for all four modes. These browser
and media checks used the real console; the host transport fixture uses
synthetic H.264/AAC.

### Settings semantics

Settings apply globally to the current payload session; a restart selects
1080p. `POST /settings` accepts form data and requires `X-PS5HS-Request: 1`.
`202 Accepted` confirms receipt, not capture success. `/status` includes:

| Field | Meaning |
|---|---|
| `requested_resolution` | Latest requested preset |
| `active_resolution` | Ready mode, or `null` while unavailable/applying |
| `settings_revision` | Version of requested settings |
| `capture_revision` | Settings version currently being attempted |
| `stream_revision` | Capture-session generation used for viewer reconnect |
| `applying` | Requested mode is not yet active |

The mode test matches a capture failure to `capture_revision`, so an error from
a previous request cannot falsely fail the new mode before it is attempted.

## Current host checks

`make test` builds with AddressSanitizer, UndefinedBehaviorSanitizer and
`-Wall -Wextra -Werror`. The current suite exercises:

- Native-call initialization, resource selection/ownership/cleanup, correlated
  callbacks, parameter records, headers, read statuses, timestamp rebasing and
  periodic keyframe requests using controlled mocks.
- PCM validation, AAC framing, capture clocks, encoder delay and actual capture
  loop logic with deterministic sources.
- Bounded muxing/queues, partially sent tags during overflow, concurrent client
  removal, codec-header replay and idle disconnect cleanup.
- HTTP routing/fragments/deadlines/capacity; settings validation, request headers
  and bodies; requested-versus-active state and revision changes.
- Twelve browser-independent dashboard/player scenarios: no-gesture autoplay,
  blocked-play fallback, capture failure, dirty selections, POST payloads,
  duplicate prevention, choices changed during a pending POST, persistent errors,
  timeouts/retry, capture generations, Stop intent and reconnect backoff.
- Concurrent and late-joining HTTP-FLV clients decoding synthetic H.264/AAC.

The [current full-suite log](evidence/web-only-host-tests.log) records all checks
passing, including the 12 UI scenarios and three HTTP-FLV viewers. Earlier logs
with RTMP tests belong to the previous implementation and are not evidence for
this web-only revision.
The SDK payload and finite audio/video probes also built successfully.
The [GitHub Actions workflow](../.github/workflows/test.yml) is configured for
the host suite on Ubuntu. The `b4da0c6` job did not start because of the account's
billing/spending limit, so there is no CI execution result. The local suite
above passed.

## Reproduce

Host checks need Clang with sanitizers, Python 3, Node.js and FFmpeg with
`libx264`/AAC. No SDK or console is needed, and `xxd` is no longer required:

```sh
make test
```

With the SDK configured, build and upload using the tested HTTP port. Stop an
existing capture copy before replacing it:

```sh
make HTTP_PORT=8081
python3 tools/deploy.py 192.168.0.190 9021 ps5-hdmi-stream.elf
curl http://192.168.0.190:8081/status
```

The explicit upload preserves the compiled ELF. `make deploy` rebuilds it;
repeat `HTTP_PORT=8081` on that command or the default becomes `8080`.

To run the opt-in test of all four native capture modes (requires FFmpeg and
ffprobe):

```sh
python3 tests/test_console_resolutions.py http://192.168.0.190:8081 /tmp/ps5hs-mode-check
```

It records separate files, validates native dimensions and audio, then restores
1080p even if a mode fails. The output directory contains captured console media
and should remain outside version control.

For the all-mode browser check, provide existing Playwright/Chromium:

```sh
PLAYWRIGHT_MODULE=/path/to/playwright-core CHROME_PATH=/path/to/chrome \
PS5HS_TEST_URL=http://192.168.0.190:8081 \
node tests/test_web_player.cjs
```

The default mode order is 720p, 1440p, 4K, 1080p. Set `PS5HS_TEST_MODES=none` to test
autoplay/layout/playback against a fixed stream without changing
capture settings. `tests/test_preview_browser.cjs` separately checks capture
error UI against `build/http_fixture <port>`.

To inspect one live sample without changing settings:

```sh
ffmpeg -i http://192.168.0.190:8081/live.flv -t 15 -map 0:v:0 -map 0:a:0 -c copy sample.flv
ffprobe -v error -count_frames -show_streams -show_format -of json sample.flv
ffmpeg -v error -i sample.flv -map 0:v:0 -map 0:a:0 -enc_time_base:v 1:1000 -fps_mode passthrough -f null -
```

Wait for recording to finish before decoding its output. SDK targets
`make probe-audio` / `make probe-video` build finite console probes without
deploying them. Both probes acquire a video resource; do not run them alongside
another capture owner.

## Limits

- **The higher modes use 30 fps.** The verified 1440p and 4K presets do not
   establish 60-fps operation at those resolutions.
- **Compatibility and performance remain bounded.** Cold-boot operation, other
   firmware versions, HDR and multi-hour operation are untested. Native open
   negotiates the capture tap; no separate scan-in setup is needed. Mode
   switches/recovery briefly interrupt viewers.
- **A/V precision is unmeasured.** First-read video/audio anchoring preserves
   native timestamp differences, but video delivery has unmeasured capture and
   encode delay. Audio adds roughly one PCM block (21.33 ms) of phase uncertainty
   plus scheduling/queue delay. Successful playback does not establish perceptual
   A/V offset or glass-to-glass latency.
- **Capacity evidence has limits.** Host tests cover eight media slots, slow
   viewers and disconnect races; hardware tests do not establish eight sustained
   simultaneous viewers at every resolution.

## Historical evidence — previous RTMP/OBS implementation

The following retained records describe the earlier 1080p implementation. They
remain useful capture/transport history, but do not validate the new controls,
fullscreen autoplay, web-only binary, or four-resolution requirement:

- [Native video probe](evidence/video-probe-2026-10-28.log) and
  [audio probe](evidence/audio-probe-2026-10-28.log): working native interfaces.
- [Concurrent HTTP/RTMP capture](evidence/console-streams-2026-10-28.json):
  identical 90.039-second captures with 5,351 video / 4,218 audio frames and no
  decode errors; 45 keyframes, maximum interval 2.036 seconds.
- [OBS playback](evidence/obs-console-playback-2026-10-28.json): actual 1080p
  source and nonzero audio levels; the 12.044-second OBS recording decoded
  cleanly. Original OBS configuration was unchanged.
- [Resident runtime](evidence/console-runtime-2026-10-28.json) and
  [recovery log](evidence/console-runtime-2026-10-28.log): about 24.5 minutes,
  including one automatic recovery after `0x81950009`.
- [Previous build](evidence/payload-build-2026-10-28.log),
  [deployment](evidence/console-final-deploy-2026-10-28.json),
  [browser check](evidence/console-browser-final-2026-10-28.log), and
  [old host suite](evidence/host-tests-2026-10-28.log) identify that prior revision.

RTMP is removed from the active code and current usage flow. Current resolution
and browser claims rely on the web-only evidence above.
