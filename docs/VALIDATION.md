# Validation results and limits

Live PS5 video **and** audio have been verified in the console-served browser
preview and an actual OBS RTMP Media Source, without a capture card. The results
below distinguish real-console playback from host fixtures and identify the
bounds of that verification.

The tested resident instance serves HTTP on `192.168.0.190:8081` and RTMP on
`192.168.0.190:1936`. Source build defaults remain `8080` / `1935`.

## Real-console results — 2026-10-28

| Requirement | Evidence and result |
|---|---|
| Final built ELF and fresh deployment | [Build record](evidence/payload-build-2026-10-28.log) and [final deployment](evidence/console-final-deploy-2026-10-28.json) match ELF SHA-256 `508c991d447d52a9aea074b2ce4becbe65aec022b21eae074bde877bdb899047`. After a graceful replacement, PID 158 served ports 8081/1936; a fresh 10.042-second RTMP sample decoded 596 video / 469 audio frames without errors, and [Chrome playback/unmute/reconnect passed again](evidence/console-browser-final-2026-10-28.log). |
| Native H.264 capture | [Production probe](evidence/video-probe-2026-10-28.log) and [ffprobe result](evidence/video-probe-ffprobe.json): 177 H.264 Main 1920×1080 frames. All frames decoded cleanly. The working ResourceArbitrator/capture ABI is documented in [AVCAP2.md](AVCAP2.md). |
| Native audio capture and AAC encoding | [Audio probe](evidence/audio-probe-2026-10-28.log) produced 141 PCM blocks and 141 AAC packets, exit 0. [ffprobe](evidence/audio-probe-ffprobe.json) identified 48 kHz stereo AAC and 3.008 seconds of audio. Integrated tests below also received both tracks. |
| Concurrent HTTP-FLV and RTMP delivery | [90-second analysis](evidence/console-streams-2026-10-28.json): both transports delivered 5,351 H.264 and 4,218 AAC frames in 90.039 seconds. Both complete captures decoded with no errors and had the same SHA-256, `d4b294bec31d9da4025170ff0a64afc01e1587c6f26fd8c2404949d199a256a4`. |
| Timestamps and periodic keyframes | The same captures had monotonic DTS on both tracks, 45 video keyframes and a maximum keyframe interval of 2.036 seconds. Twenty status samples were all streaming; maximum reported frame ages were 18 ms video / 21 ms audio. These are producer freshness measurements, not viewing latency. |
| Console-served browser playback | [Chrome result](evidence/console-browser-2026-10-28.log): decoded 1920×1080 video and AAC, unmuted, advanced playback, reconnected and stopped. The observed sample contained 69 video frames and 22,991 decoded audio bytes at media time 1.044025 seconds. The test permits only same-origin assets/media and rejects JavaScript page errors. |
| Actual OBS Media Source | [OBS 32.0.4 result](evidence/obs-console-playback-2026-10-28.json): `OBS_MEDIA_STATE_PLAYING`, 1920×1080 source, cursor advanced 12.011 seconds, 272 audio-meter events and nonzero peak 0.04464. A source screenshot showed the PS5 Media home screen. |
| OBS recording of received media | [Recording metadata](evidence/obs-console-recording-ffprobe.json): 12.044 seconds, 360 H.264 frames and 562 stereo AAC frames at 48 kHz. Full FFmpeg decode passed. OBS re-encoded at 30 fps; this verifies OBS reception/decoding, not preservation of the native input cadence. |
| Capture recovery | The [retained runtime log](evidence/console-runtime-2026-10-28.log) records a video read returning `0x81950009`. The payload released its resource, waited five seconds, reopened video and audio, and resumed both tracks. Subsequent OBS playback passed. The [runtime snapshot](evidence/console-runtime-2026-10-28.json) was streaming at about 24.5 minutes, with 87,037 video / 68,623 audio frames and one recorded retry. |

The OBS test used a fresh isolated profile with no desktop, camera or microphone
inputs. All 167 original OBS configuration files remained hash-identical after
a clean exit. Console media/screenshots are retained privately; their analysis
and relevant hashes are recorded here instead of committing captured media.

## Host regression results

The [complete host test log](evidence/host-tests-2026-10-28.log) records a passing
`make test` run with AddressSanitizer, UndefinedBehaviorSanitizer and
`-Wall -Wextra -Werror`:

- Capture initialization order, resource ownership/cleanup, native headers,
  nonblocking reads and periodic keyframe requests.
- Audio framing, finite PCM values, capture-clock conversion and encoder delay.
- Actual capture-loop behavior with mock sources: DTS/CTS, parameter sets,
  timestamp continuity after restart, stalled audio and cleanup.
- Muxer bounds, partially consumed tags during queue overflow, concurrent client
  removal, codec-header replay and idle disconnect cleanup.
- RTMP interleaved chunks, extended timestamps, command validation and playback.
- Fragmented HTTP requests, exact routes, errors, request deadlines, 24-worker
  limit, eight shared media slots and slot reclamation.
- Preview playback gating, audio transitions, reconnect, unsupported-browser and
  offline states. Headless Chrome also checked the error UI and mobile layout.

A generated H.264/AAC fixture independently passed simultaneous HTTP/RTMP decode
and late join: 60 video / 94 audio frames for each initial client, 59 / 94 for
the late RTMP client. [Synthetic Chrome playback](evidence/browser-playback-2026-10-28.log)
also passed. These fixture results supplement the real-console results above.
The GitHub Actions workflow runs the host suite on Ubuntu; its remote execution
has not been observed in this workspace.

## Reproduce

Host checks require Clang with sanitizers, Python 3, Node.js, `xxd`, and FFmpeg
with `libx264` and AAC support. They require neither a console nor the PS5 SDK:

```sh
make test
```

With the SDK configured, build and upload the resident payload using the tested
ports. Stop an existing capture copy before replacing it:

```sh
make HTTP_PORT=8081 RTMP_PORT=1936
python3 tools/deploy.py 192.168.0.190 9021 ps5-hdmi-stream.elf
curl http://192.168.0.190:8081/status
```

The explicit Python upload preserves the built ELF. `make deploy` always
rebuilds, so include `HTTP_PORT=8081 RTMP_PORT=1936` on that command too if those
ports are desired. Upload success alone does not prove capture started.

Record a sample from either endpoint, then inspect and decode both tracks:

```sh
ffmpeg -i rtmp://192.168.0.190:1936/live/stream -t 15 -map 0:v:0 -map 0:a:0 -c copy sample.flv
ffprobe -v error -count_frames -show_streams -show_format -of json sample.flv
ffmpeg -v error -i sample.flv -map 0:v:0 -map 0:a:0 -f null -
```

Use `http://192.168.0.190:8081/live.flv` as the input for the HTTP variant. Use
separate output filenames for concurrent runs and wait for each recorder to
exit before decoding its output.

For automated browser playback, provide an installed Playwright/Chromium:

```sh
PLAYWRIGHT_MODULE=/path/to/playwright-core CHROME_PATH=/path/to/chrome \
PS5HS_TEST_URL=http://192.168.0.190:8081/ node tests/test_playback_browser.cjs
```

The test starts playback, checks video dimensions and decoded audio, unmutes,
waits for progress, reconnects and stops. `tests/test_preview_browser.cjs` is a
separate error-state check against `build/http_fixture <port>`. The host
transport fixture accepts `<fixture.flv> <rtmp_port> <http_port>`; see
`tests/test_streaming.py` for synthetic media generation.

`make probe-audio` and `make probe-video` build finite diagnostic payloads under
`build/`. Deploy them explicitly with `tools/deploy.py`; they write
`/data/ps5hs-{audio,video}-probe.*` and exit. Run a video probe only after stopping
other clients of the same capture resource.

## Remaining limits

- **Cold boot:** the working production backend did not call scan-in setup,
  but an earlier diagnostic enabled it during the same console session. Its
  persistence or necessity after a fresh boot has not been established.
- **Compatibility:** validation covers the tested console's 1080p SDR,
  nominal 60-fps / 8000-kbps configuration. Other firmware, HDR and video modes
  are untested; native observed cadence is approximately 59.94 Hz.
- **A/V precision:** both tracks play and have monotonic timelines. The audio
  source's stale clock epoch is re-anchored while preserving sample spacing.
  A simultaneous audiovisual reference is still needed to measure perceptual
  offset; first-read anchoring has roughly one PCM block (21.33 ms) of phase
  uncertainty plus unmeasured scheduling/queue delay. No millisecond sync
  guarantee or glass-to-glass latency result is claimed.
- **Duration and recovery:** the retained resident snapshot covers about 24.5 minutes
  with one automatic recovery before the final rebuild/redeployment, and the retained concurrent decode test covers
  90 seconds. Multi-hour sessions, repeated forced mode changes and exhaustive
  game/content coverage remain untested.
- **Capacity scope:** bounded queues, slow-viewer recovery and disconnect races
  have host regression coverage. The hardware run validates concurrent HTTP,
  RTMP and subsequent OBS use, not eight sustained hardware viewers.

These limits qualify compatibility and performance claims; live console
video/audio delivery to the browser and OBS is established by the recorded
end-to-end evidence.
