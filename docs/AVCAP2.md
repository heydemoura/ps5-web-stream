# libSceAvcap2 — capture API notes

Reverse-engineered from the console's own modules (`libSceAvcap2.sprx`,
`libSceVideoRecording.sprx`) and `SceShellCore.elf`, the known-good caller.
Symbol names come from the public aerolib NID table. No Sony code is included
here; this is interface documentation to drive the payload.

## Why this API

`libSceAvcap2` is the "AV capture" service behind the system Share / broadcast
features. It taps the composited HDMI scan-out and returns it **already encoded
as H.264 (AVC) Annex-B access units**, plus audio, via `sceAvcap2ReadVideo` /
`sceAvcap2ReadAudio`. That is ideal for RTMP: no capture card, no software
encode. The alternative (reading the raw DCE scan-out ring and encoding
ourselves) needs kernel page-table manipulation; this payload deliberately
avoids that.

## Open sequence (from SceShellCore)

```
sceAvcap2Initialize()                         // no args
sceAvcap2OpenVideo(&handle, params, flag)     // handle written to *arg0
sceAvcap2Start(handle)
loop: sceAvcap2ReadVideo(handle, buf, size, &info, 0)
sceAvcap2Stop(handle); sceAvcap2Close(handle); sceAvcap2Terminate()
```

`OpenVideo(obj, params, flag)` — `obj` is a caller buffer whose first qword
receives the handle; `params = obj + 8`. The library allocates its own encoder
memory when the memory-mode field (`params + 0x1a0`) is 0.

### Video params (offsets within `params`, from SceShellCore @0x9b0f00)

| Offset | Meaning | 1080p value used |
|--------|---------|------------------|
| 0x18 | resolution class code | 4 |
| 0x1c | resolution sub-code | 1 |
| 0x20 | flag A (bit3 of a format byte) | 0 |
| 0x24 | flag B | 2 |
| 0x94 | frame rate | 60 |
| 0xc8 | encoder bitrate field | 8000 |
| 0x2a0 | colour-space A | 0 |
| 0x2a4 | colour-space B | 1 |
| 0x2b4, 0x2b5 | colour-space flags | 1, 1 |
| 0x1a0 | memory mode (0 = library allocates) | 0 |

### ReadVideo info block (offsets within the caller's info struct)

| Offset | Meaning |
|--------|---------|
| 0x08 | presentation timestamp (microseconds) |
| 0x10 | decode timestamp |
| 0x1c | frame type (1 = IDR / keyframe) |

`sceAvcap2ReadVideo` returns the encoded byte count written to `buf`, or
`0x81950001` (try again). `sceAvcap2GetNonVclNalUnitStream` returns SPS/PPS
separately; in practice the Annex-B access units already carry them on
keyframes, so the payload extracts SPS/PPS from the stream.

## Return codes seen

| Code | Meaning (inferred) |
|------|--------------------|
| `0x81950001` | EAGAIN / not ready |
| `0x81950008` | video-out mode mismatch — the daemon has no scan-in context for this caller |
| `0x81950009` | related mode-change code |
| `0x8195000a` | buffer / read state error |

`0x81950008` is **not** a literal inside `libSceAvcap2`; it is the reply from
the `SceSysAvControl` daemon reached over IPMI. `sceAvcap2GetVideoOutMode`
returns `0x81950001` until a scan-in context exists (its singleton member,
fetched via the helper at `0x2c20`, is NULL until then).

## Current blocker and the remaining work

From a plain elfldr payload: `Initialize` succeeds, but `OpenVideo` returns
`0x81950008` and `GetVideoOutMode` returns `0x81950001`. The scan-in tap has
not been allocated for our process.

The system establishes the tap by talking to the `SceSysAvControl` daemon over
IPMI (`libSceIpmi`, service `"SceSysAvControlIpc"`): create a client, connect,
and `invoke` method `0x24` with a scan-in config (tap point, resolution index).
This is a **userspace** IPMI exchange — not a kernel exploit. The documented
risk is that selecting the wrong scan-in mode can disturb the display engine,
so this step must be done carefully and is gated behind an explicit opt-in.

Next step: add an IPMI scan-in setup before `OpenVideo` (resolve `libSceIpmi`
via dlopen; `IPMI::Client::Config` → `estimateClientMemorySize` → `create` →
connect (vtable+0x80) → invoke (vtable+0x60) method `0x24`), then re-check
`GetVideoOutMode` and retry `OpenVideo`.

## Audio

`sceAvcap2OpenAudio` / `sceAvcap2ReadAudio` return PCM. RTMP/FLV and the browser
preview want AAC, so an on-console AAC encoder (`libSceAjm`) is needed to carry
audio. The muxer and FLV audio-tag path are already in place
(`flv_audio_tag`, broadcaster AAC sequence header); only the encoder is
outstanding. Video is unaffected and ships first.
