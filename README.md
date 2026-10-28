# PS5 HDMI Stream

A PlayStation 5 homebrew payload that captures the console's composited HDMI
output, muxes it to FLV, and broadcasts it over the local network so you can:

- add it to **OBS** as an **RTMP source** (`rtmp://<ps5-ip>:1935/live/stream`), and
- watch a **live preview in a browser**, served from the console itself
  (`http://<ps5-ip>:8080/`).

It is built with the [ps5-payload-dev SDK][sdk] and loaded through an ELF loader
(elfldr, port 9021). It uses the console's own hardware H.264 encoder through
`libSceAvcap2` — the same AV-capture service the system's Share / broadcast
features use — so no capture card is required and nothing is re-encoded in
software.

> Status: the network stack (RTMP server, HTTP-FLV, web preview, FLV muxer) is
> complete and runs on-console. The capture backend loads and initialises, but
> opening the capture tap currently needs the scan-in session to be negotiated
> with the AV-control daemon first — see **[docs/AVCAP2.md](docs/AVCAP2.md)** for
> the full reverse-engineering notes and the remaining work.

## Architecture

```
 sceAvcap2 (HW H.264 + audio)
        │  avcap.c       capture backend, runtime-resolved via dlopen
        ▼
   FLV muxer            flv.c   AVCC packaging, AVCDecoderConfigurationRecord
        │
        ▼
   broadcaster          flv.c   one encoder -> many clients, keyframe-aware
        ├────────────► RTMP server   rtmp.c   OBS Media Source pulls this
        └────────────► HTTP server   http.c   /live.flv (flv.js) + preview page
```

A single capture thread pushes muxed FLV tags into a broadcaster; each
connected client (RTMP `play` session or HTTP-FLV reader) owns a bounded queue
and is resynchronised on the next keyframe if it falls behind, so one slow
viewer never stalls the encoder.

## Build

Install the SDK and point the environment at it (on macOS also at a modern LLVM):

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk          # or ~/opt/ps5-payload-sdk
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config   # macOS only
make
```

This produces `ps5-hdmi-stream.elf`.

## Run

Send the payload to a running ELF loader on the console:

```sh
make deploy PS5_HOST=192.168.0.190        # nc -q0 <host> 9021 < elf
```

Then open `http://192.168.0.190:8080/` for the preview, or in OBS add a
**Media Source**, uncheck *Local File*, and set the input to
`rtmp://192.168.0.190:1935/live/stream`.

### Endpoints

| Port | Purpose |
|------|---------|
| 8080 | web preview page, `/live.flv` (HTTP-FLV), `/status`, `/log` |
| 1935 | RTMP server (`/live/stream`) for OBS |

Ports can be overridden: the loader can pass `argv` as `[http_port] [rtmp_port]`.

## Layout

| Path | What |
|------|------|
| `src/main.c`  | entry point, config, capture loop |
| `src/avcap.*` | `libSceAvcap2` capture backend (runtime-resolved) |
| `src/flv.*`   | FLV muxing + the broadcaster / client fan-out |
| `src/rtmp.*`  | minimal RTMP server (connect/createStream/play) |
| `src/http.*`  | HTTP server: preview page, HTTP-FLV, status, log |
| `web/index.html` | the preview page (embedded into the ELF at build) |
| `docs/AVCAP2.md` | reverse-engineering notes for the capture API |

## Legal

For use on a console you own. This project contains no Sony code; the capture
library is resolved from the console at runtime. GPLv3+ (see `LICENSE`).

[sdk]: https://github.com/ps5-payload-dev/sdk
