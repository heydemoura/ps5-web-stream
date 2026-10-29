# Web-only validation results and remaining work

The current objective is console-served video/audio playback, an automatically
playing fullscreen `/player` page, and dashboard controls for **720p, 1080p,
1440p and 4K capture**. The active implementation has one HTTP listener and no
RTMP source, listener or build arguments.

**720p, 1080p and 1440p have passed real-console video/audio decoding and browser
resolution changes. 4K capture still fails, so the full objective remains
incomplete.** These results distinguish native decoded dimensions from the
size at which a browser displays video.

## Current console results

The web-only payload was tested at `http://192.168.0.190:8081`; its build default
is `8080`. The resolution test selects each mode through `/settings`, waits for
that configuration to become active, records HTTP-FLV, checks stream dimensions
and decodes both tracks completely.

| Mode | Configured fps / kbps | Decoded video | Decoded audio | Result |
|---|---|---|---|---|
| 720p | 60 / 5000 | H.264, 1280×720, 179 frames | AAC, 48 kHz stereo, 139 frames | Passed; 3.035 s, no decode errors |
| 1080p | 60 / 8000 | H.264, 1920×1080, 179 frames | AAC, 48 kHz stereo, 140 frames | Passed; 3.035 s, no decode errors |
| 1440p | 60 / 16000 | H.264, 2560×1440, 178 frames | AAC, 48 kHz stereo, 141 frames | Passed; 3.016 s, no decode errors |
| 4K | 30 / 32000 | No verified frame | No integrated sample | **Pending:** native capture error `0x8195000a` |

The 4K attempt also failed with the earlier 60-fps configuration. The current
30-fps preset is an investigation setting, not a successful 4K result.

The [mode results](evidence/web-only-console-modes.json) retain status, decoded
dimensions, frame counts and the 4K failure. Captured console media remains
outside version control.

### Fullscreen player and dashboard

The real Chrome integration opened `/player` without a click and verified:

- Muted autoplay with decoded video and AAC; Unmute enabled audio playback.
- Video filled desktop and mobile viewports without page scrolling.
- A second tab changed capture using the dashboard's selector and Apply button.
- The player resumed at decoded 1280×720, 2560×1440 and 1920×1080, with audio,
  after the respective 720p, 1440p and 1080p requests.
- Playback continued after each switch; explicit reconnect and Stop worked.
- All assets/media came from the console origin, with no JavaScript page errors.

The [browser log](evidence/web-only-browser.log) records the three working
modes; this run does not establish 4K playback.

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

- Native-call initialization, resource ownership/cleanup, parameter records,
  headers, read statuses and periodic keyframe requests using controlled mocks.
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
The GitHub Actions workflow runs the host suite on Ubuntu; its remote execution
has not been observed in this workspace.

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
ffprobe; currently expected to fail at 4K):

```sh
python3 tests/test_console_resolutions.py http://192.168.0.190:8081 /tmp/ps5hs-mode-check
```

It records separate files, validates native dimensions and audio, then restores
1080p even if a mode fails. The output directory contains captured console media
and should remain outside version control.

For the verified three-mode browser check, provide existing Playwright/Chromium:

```sh
PLAYWRIGHT_MODULE=/path/to/playwright-core CHROME_PATH=/path/to/chrome \
PS5HS_TEST_URL=http://192.168.0.190:8081 PS5HS_TEST_MODES=720p,1440p,1080p \
node tests/test_web_player.cjs
```

Omitting `PS5HS_TEST_MODES` includes 4K and will not pass until 4K works. Set it to
`none` to test autoplay/layout/playback against a fixed stream without changing
capture settings. `tests/test_preview_browser.cjs` separately checks capture
error UI against `build/http_fixture <port>`.

To inspect one live sample without changing settings:

```sh
ffmpeg -i http://192.168.0.190:8081/live.flv -t 15 -map 0:v:0 -map 0:a:0 -c copy sample.flv
ffprobe -v error -count_frames -show_streams -show_format -of json sample.flv
ffmpeg -v error -i sample.flv -map 0:v:0 -map 0:a:0 -f null -
```

Wait for recording to finish before decoding its output. SDK targets
`make probe-audio` / `make probe-video` build finite console probes without
deploying them. Do not run a video probe alongside another capture owner.

## Remaining work and limits

1. **4K is required and unresolved.** Obtain native 3840×2160 H.264 plus AAC,
   verify full decoding, then repeat the dashboard-to-player test at that mode.
   A selectable option, accepted request, resized page or lower-resolution
   fallback does not prove 4K capture.
2. **Record the final web-only artifact and complete run.** Retain the build/
   deployment identity and all-mode hardware/browser results after the 4K fix.
3. **Compatibility and performance remain bounded.** Fresh-boot independence
   from earlier scan-in diagnostics, other firmware versions, HDR and multi-hour
   operation are untested. Mode switches/recovery briefly interrupt viewers.
4. **A/V precision is unmeasured.** First-read audio-clock anchoring preserves
   sample spacing but leaves roughly one PCM block (21.33 ms) of phase uncertainty
   plus scheduling/queue delay. Successful playback does not establish perceptual
   A/V offset or glass-to-glass latency.
5. **Capacity evidence has limits.** Host tests cover eight media slots, slow
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

RTMP is removed from the active code and current usage flow; its historical
success does not substitute for the remaining 4K work.
