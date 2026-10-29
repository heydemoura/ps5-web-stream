# PS5 HDMI Stream

A PlayStation 5 homebrew payload that streams the console's video and audio
over a local network, without a capture card. It serves an H.264/AAC browser
preview from the console and an RTMP feed for OBS.

**Verified on the test console:** native 1920×1080 H.264 at approximately
59.94 fps, 48 kHz stereo AAC, live Chrome playback with audio, and an actual OBS
Media Source. Concurrent HTTP-FLV and RTMP captures decoded cleanly for 90
seconds and contained identical media. See [validation results and limits](docs/VALIDATION.md)
and [capture API findings](docs/AVCAP2.md).

The verified instance uses **[http://192.168.0.190:8081/](http://192.168.0.190:8081/)**
and **`rtmp://192.168.0.190:1936/live/stream`**. Normal build defaults remain
HTTP `8080` and RTMP `1935`.

## Architecture

```
libSceAvcap2 video ── H.264 ──────────────────────┐
libSceAvcap2 audio ── float PCM ── libSceM4aacEnc ─┤
                                                ▼
                                           FLV muxer
                                                ▼
                                          broadcaster
                                   ┌────────────┴────────────┐
                                 RTMP                      HTTP
                               OBS pull               browser + FLV
```

Firmware libraries are resolved at runtime. Video uses the console's encoded
capture output; audio uses its native AJM AAC encoder. ResourceArbitrator
negotiation, complete versioned capture records, native SPS/PPS and periodic
keyframe requests establish a decodable video feed. Capture clocks are mapped
to a continuous audio/video timeline, including after a capture restart.

One capture loop feeds up to **eight viewers shared across both transports**.
Each viewer has a bounded queue; a slow viewer resumes at a keyframe with codec
headers, without blocking capture or corrupting an in-flight tag. The preview
page and its pinned flv.js dependency are embedded in the ELF, with no CDN
requirement. `/status` reports errors, media counts and frame age; the preview
shows “live” only after playback advances.

## Build

Install the [ps5-payload-dev SDK][sdk], a recent LLVM, `make`, and `xxd`:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk  # or ~/opt/ps5-payload-sdk
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config  # macOS
make
```

Output: `ps5-hdmi-stream.elf`, using ports `8080` / `1935`. To build with the
ports used for the verified instance:

```sh
make HTTP_PORT=8081 RTMP_PORT=1936
```

## Run

Send the compiled ELF unchanged to a running ELF loader (Python 3 required):

```sh
python3 tools/deploy.py 192.168.0.190 9021 ps5-hdmi-stream.elf
```

This preserves the ports compiled into that file. Alternatively, build and
upload together:

```sh
make deploy PS5_HOST=192.168.0.190 HTTP_PORT=8081 RTMP_PORT=1936
```

**`make deploy` rebuilds the ELF.** Repeat `HTTP_PORT` / `RTMP_PORT` overrides on
that command; omitting them rebuilds with `8080` / `1935`. The loader port is
`9021` by default (`PS5_PORT` overrides it). Stop an existing copy before
replacing it: the capture resource is shared with other capture clients. Upload
success confirms transfer; check `/status` and `/log` for capture results.

For the verified ports, open `http://192.168.0.190:8081/` and select **Play
preview**. Audio starts muted; select **Unmute** once audio is available. The
browser needs H.264/AAC MediaSource playback support.

In OBS, add a **Media Source**, uncheck **Local File**, and use:

```text
rtmp://192.168.0.190:1936/live/stream
```

`http://192.168.0.190:8081/live.flv` carries the same feed. Substitute your
console address and chosen ports as needed.

| Default port | Endpoint | Purpose |
|---|---|---|
| 8080 | `/` | Preview and diagnostics |
| 8080 | `/live.flv` | HTTP-FLV stream |
| 8080 | `/status` | Capture state, errors, media counters, viewers |
| 8080 | `/log` | Recent payload log |
| 1935 | `/live/stream` | RTMP playback for OBS/FFmpeg |

A loader supporting arguments can pass `[http_port] [rtmp_port] [--scanin]`.
Ports must differ and be between 1 and 65535. Scan-in negotiation is an
experimental opt-in, disabled by default. The working backend does not require
Share initialization or permission sweeps; the interface is documented in
[AVCAP2.md](docs/AVCAP2.md).

## Tests

Host tests need Clang with AddressSanitizer/UndefinedBehaviorSanitizer,
Python 3, Node.js, `xxd`, and FFmpeg with H.264 (`libx264`) and AAC support:

```sh
make test
```

This runs without the SDK or a console deployment. It covers capture lifecycle,
audio framing and clocks, muxing, bounded fan-out, RTMP framing, HTTP behavior,
preview states, and simultaneous/late-join decoding with synthetic media.

`make probe-audio` and `make probe-video` build finite console probes with the
SDK; building them does not deploy them. Optional browser tests cover both
capture-error UI and actual media playback. Reproduction commands and retained
hardware results are in [docs/VALIDATION.md](docs/VALIDATION.md).

## Limits

- Validated output is 1080p SDR with the nominal 60-fps / 8000-kbps configuration;
  actual cadence is approximately 59.94 Hz and bitrate is variable.
- Eight active media viewers total; HTTP has 24 connection workers. Pending
  media per viewer is limited to 6 MiB / 1,024 tags, plus the current tag.
- HTTP headers are limited to 8 KiB with a five-second complete-request
  deadline; stalled writes time out and idle disconnected viewers release slots.
- The console recovered automatically from an observed capture read failure.
  A brief interruption during a reopen is possible.
- Cold-boot behavior, other firmware versions, HDR, other video modes and
  multi-hour operation are untested. A prior diagnostic enabled scan-in in the
  tested console session; its effect after a fresh boot remains to be checked.
- Perceptual A/V offset and glass-to-glass latency have not been measured;
  monotonic timestamps and successful playback do not establish their precision.
- This is a LAN playback server without authentication, TLS, RTMP publishing,
  recording controls, or internet relay/NAT traversal.

## Layout

| Path | Purpose |
|---|---|
| `src/main.c` | Capture lifecycle and audio/video timeline |
| `src/avcap.*`, `src/scanin.*` | Firmware capture and optional scan-in interfaces |
| `src/audio.*` | PCM capture, native AAC encoding and audio framing |
| `src/flv.*` | H.264/AAC FLV muxing and bounded broadcaster |
| `src/rtmp.*`, `src/http.*` | Playback servers |
| `src/status.*` | Thread-safe capture diagnostics |
| `web/` | Embedded preview and vendored player/license |
| `tests/` | Host regressions, integration fixtures and console probes |
| `tools/deploy.py` | Cross-platform ELF upload |
| `docs/` | Capture findings and validation evidence |

## License

For use on a console you own. No Sony code is included; firmware libraries are
resolved from the console at runtime. Project code is GPLv3+ (see `LICENSE`).
The vendored flv.js player is Apache-2.0; its license and provenance are in
[`web/vendor/`](web/vendor/README.md).

[sdk]: https://github.com/ps5-payload-dev/sdk
