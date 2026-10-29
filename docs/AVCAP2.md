# libSceAvcap2 capture ABI and validation

The production payload delivered **live 1920×1080 H.264 Main video and stereo
48 kHz AAC-LC audio** from the test console on 2026-10-28. A 90.039-second
simultaneous HTTP-FLV/RTMP capture contained 5351 video frames and 4218 AAC
frames on each transport. Both files had identical SHA-256 hashes, monotonic
decode timestamps, and clean full FFmpeg decodes. The stream reports
`60000/1001` fps. See the [console stream results](evidence/console-streams-2026-10-28.json).

Actual Chrome decoded video and audio, unmuted, reconnected and stopped
successfully ([browser result](evidence/console-browser-2026-10-28.log)).
An actual OBS Media Source played the RTMP stream with active audio meters;
its 12.044-second recording decoded without errors. OBS re-encoded that
recording at 30 fps, so this confirms reception and playback rather than
native frame-rate preservation ([OBS result](evidence/obs-console-playback-2026-10-28.json)).
These tests do not measure glass-to-glass latency or perceptual A/V alignment.

The earlier finite production-backend probe captured and decoded 177 video
frames. Both runs requested nominal 60 fps and 8000 kbps. The encoded output is
variable bitrate, so 8000 kbps is an encoder limit, not a measured constant
output rate. Earlier evidence: [probe log](evidence/video-probe-2026-10-28.log),
[FFprobe result](evidence/video-probe-ffprobe.json). The captured file SHA-256 is
`f0c4485e9f8eae17d084b60b0babcb588f5d472eec047c40125b769870880f7c`.
Console screenshots/video and proprietary binaries are not committed.

This replaces the earlier diagnosis that capture required a ShellCore Share
permit. That diagnosis was incorrect. `GetVideoOutMode` had been called before
`Initialize`, and the video record omitted mandatory sizes and the active
ResourceArbitrator ID. The working backend does not initialize Share or sweep
permits. Successful production capture did not call `scanin_enable`.
A prior diagnostic had enabled the read tap in the same console session.
**Cold-boot capture remains unverified:** the successful tests do not establish
whether that earlier action has a persistent effect. No scan-in mode change
or kernel patch was used.

## Source and evidence scope

The interface was reconstructed from the console's own `RemoteplayDaemon`,
`AvCaptureManager`, `libSceAvcap2`, and `libSceResourceArbitrator`, then verified
with short-lived probes. No proprietary implementation is included here.

| Artifact | SHA-256 |
| --- | --- |
| RemoteplayDaemon.elf | `d0bb7b0e3c6c8d80f2d73972d08720190c138b0849764669973c79a6fd828aa3` |
| AvCaptureManager.elf | `1b0e2d4134c19449cf42da6242720e76dc558e893c4ea023e3d1baf2b4b28bae` |
| libSceResourceArbitrator.sprx | `7577d305d47e418cc3be33ab66e079f25a4a881923a11eff776df800afa6896a` |

The cited addresses below are image-relative addresses in those artifacts.
REA/Hopper analysis was attempted; its bridge timed out. The final ABI findings
come from local Capstone disassembly plus the console's runtime results, not a
successful Hopper decompilation. Different firmware layouts remain untested.

## Working video sequence

1. Load `libSceIpmi.sprx`, then `libSceAvcap2.sprx`.
2. Call `sceAvcap2Initialize()` before querying output mode. The tested mode was
   `0x13c`; querying it after initialization succeeds.
3. Register a ResourceArbitrator callback for resource `0xb4` (180), check
   `sceResourceArbitratorIsExecutable(0xb4, "RUNNING")`, and request
   `sceResourceArbitratorChangeState(0xb4, "RUNNING", NULL, getpid())` when needed.
   Wait for event 1 with a successful result. The PID belongs to this state
   request; it is **not** the capture record's resource ID.
4. Call `sceAvcap2OpenVideo(&handle, params, 0)` with the complete versioned
   record, source 2, resource `0xb4`, and the current output mode.
5. Obtain SPS/PPS using `sceAvcap2GetNonVclNalUnitStream(handle, &headers)`.
6. Start, then read with `sceAvcap2ReadVideo(handle, buf, capacity, info, 1)`.
   Flag 1 makes the read nonblocking.
7. Stop and close the video handle. Return resource `0xb4` to `"STOP"` only if
   this client acquired that state, and clear its callback.
8. Terminate the shared Avcap service only after video **and audio** close.

`RemoteplayDaemon`'s resource lifecycle is at `0x530f0` / `0x53450` / `0x534e0`.
Its parameter initializer is at `0x57770`, and rate updates are at `0x580b0`.
`AvCaptureManager`'s normal open path at `0x15450` calls `0x39a0`; the resource
getter at `0x14550` reads `params+0x28`. Its `0x2750` helper compares that value
against the 32 active ResourceArbitrator IDs. A missing match returns
`0x81950008`. Runtime confirmed this: a PID or user ID failed, while `0xb4`
with an active RUNNING resource opened and encoded successfully.

## Parameter record

The record is `0x2e0` bytes. Each nested versioned record requires its size at
the following offset; unused bytes are zero unless specified:

```
000:2e0  008:028  030:058  038:018  050:010  060:018
088:030  0b8:0e0  0c0:020  0e0:020  100:020  120:018
188:010  198:050  1e8:0f8  1f0:028  218:088  2a8:010  2b8:010
```

| Offset | Meaning / tested value |
| --- | --- |
| `0x14` | Source: 2 (Remote Play path) |
| `0x18` | Resolution class: 4 = 1920×1080 |
| `0x1c` | Cadence selector: 1 = nominal 60, 0 = nominal 30 |
| `0x24` | Codec: 0 = AVC, 1 = HEVC; only AVC tested |
| `0x28` | Active ResourceArbitrator ID, `0xb4` |
| `0x40` / `0x44` / `0x48` | AVC packetization controls: 1 / 68 / 1 for 60 fps |
| `0x4c` | Packet-size budget in bits: `1400*8` |
| `0x58` / `0x5c` / `0x68` | Native initialization values: 1 / 9 / 1 |
| `0xc8` | Native AVC target rate: `(float)(kbps*1000)/1.12f` |
| `0xcc` | Rate limit in bits/s: `kbps*1000` |
| `0xd0` | Frame bit budget: `bps*1001/(fps*1000)` |
| `0xd4` / `0xd8` | Native rate-control values 35 / 51 |
| `0x108` / `0x10c` | Enable frame-byte budget / `frame_bits/8` |
| `0x128` / `0x12c` | Native initialization values: 1 / 25 |
| `0x1a0` | Encoder-memory mode: 0 lets the library allocate |
| `0x220..0x29f` | Native initialization: all `0xff` |
| `0x2a0` | Current video-output mode from the initialized service |
| `0x2a4` | Output color selector: 2 for tested SDR mode |
| `0x2b0` / bytes `0x2b4,0x2b5` | Native initialization values: 1 / 1,1 |
| byte `0x2c0` / `0x2c4` | Native initialization values: 1 / 110 |

The native divisor is read from `RemoteplayDaemon` rodata `0x4f0db0`.
Setting both `0xc8` and `0xcc` to the full rate with an incomplete set of
associated controls produced a `Start` error `0x80c78000`. The native formula
above and the production configuration successfully started and decoded.
Only 1080p with the nominal 60-fps selector and 8000-kbps setting has been
validated on this console; other exposed resolutions/rates are ABI mappings,
not tested playback guarantees. HDR color paths are also unverified.

## Video headers, samples, and clock

The `0x1a8`-byte non-VCL output starts with its size. AVC SPS length is a
64-bit integer at `0x10`, followed by up to 128 SPS bytes at `0x18`. PPS length
is at `0x98`, followed by up to 128 PPS bytes at `0xa0`. The tested output held
a 29-byte SPS and 4-byte PPS. These NAL payloads do not include start codes.

The video reads returned Annex-B slice NALs without SPS/PPS. A dump of slices
alone failed to decode with missing-PPS errors. Prefixing the actual native
SPS/PPS with Annex-B start codes fixed decoding. The backend prefixes those
headers at each IDR. It identifies IDRs from NAL type 5 instead of relying on
an undocumented frame-type enumeration.

The default encoder emitted one initial IDR followed by P pictures throughout
the three-second probe. The backend now calls the symbol-resolved
`sceAvcap2RequestKeyFrame(handle)` after two seconds of captured media without
an IDR, so late viewers can obtain another decoding entry point. The exported
one-argument ABI is confirmed at `libSceAvcap2+0x17c0`; request timing, error
handling and reopening are covered by host tests. The integrated 90-second
console capture confirmed **45 IDRs**, with a maximum interval of **2.036
seconds**, and actual browser reconnect succeeded.

The video info record is `0x70` bytes (the backend reserves `0x80`):

| Offset | Observed meaning |
| --- | --- |
| `0x08` | 64-bit presentation time, **240,000 ticks per second** |
| `0x10` | 64-bit decode time, same clock |
| `0x1c` | Observed 1 on initial IDR, 3 on following P pictures |
| `0x3c` / `0x40` | Observed encoded width / height |

Consecutive normal video frames advanced by 4004 ticks, or 16,683⅓ µs.
Audio advanced by 5120 ticks per 1024 samples at 48 kHz, independently
confirming the same clock **rate**, but its absolute value is not directly
interchangeable with the video clock. The backend converts to 64-bit microseconds using
quotient/remainder arithmetic. Timestamps are not microseconds in the native
record; dividing them by 1000 as the previous implementation did was wrong.

Before audio clock rebasing, an integrated probe found audio more than 3300
seconds behind video and discarded all AAC packets. During
repeated ten-second capture sessions separated by five-second retry delays,
audio advanced by about ten seconds and video by about fifteen. Static analysis
explains this: `AvCaptureManager+0x11440` initializes the audio device's counter
from `sceKernelReadTsc()*240000/sceKernelGetTscFrequency()` only once, then adds
five ticks per sample to its stored counter. The initialization flag is set by
the base constructor at `0x2403`; capture restarts need not recreate the shared
audio device. Video also uses the TSC formula (`0x50a9`), while `CLOCK_MONOTONIC`
is a separate host clock. A simultaneous probe measured a roughly 1.6-second
offset between video time and monotonic time.

Consequently, source clocks require explicit anchoring when assembling an A/V
timeline. The audio counter's rate remains useful for sample spacing; its stale
absolute epoch must not cause otherwise valid AAC packets to be discarded.
The audio backend now resolves `sceKernelReadTsc` and
`sceKernelGetTscFrequency` from `libkernel.sprx`. At its first successful PCM
read, it records source counter `S0` and sets
`anchor = floor(TSC*240000/frequency) + 5120`. Subsequent source counter `Si`
maps to `floor((anchor + Si - S0)*1000000/240000)` microseconds. The 5120-tick
addition reproduces the native fresh-device producer's explicit one-block
offset; it is not a guessed correction for the observed multi-second mismatch.
The mapping preserves all subsequent sample-counter differences, rejects
regressions/overflow, and establishes a fresh anchor on capture reopen. It
resolved the integrated audio failure: the subsequent 90-second captures
contained 4218 decoded AAC frames each, alongside video, with monotonic
timestamps on both tracks.
RemoteplayDaemon's audio consumer at `0x5a390` obtains the native timestamp but
does not use it when returning the PCM frame. Exact perceptual A/V alignment
still needs a simultaneous audiovisual reference; packet arrival alone does
not establish capture or encoder latency. Anchoring at the first read instead
of the producer's internal timestamp instant leaves roughly one PCM block
(21.33 ms) of phase uncertainty plus unmeasured scheduling/queue latency.

Native reads return the positive encoded byte count. `0x81950002` means no
packet ready; `0x81950004` is an overrun/retry condition. `0x81950001` is an
invalid-argument/state error and must not be swallowed as EAGAIN. Error
`0x81950008` has multiple daemon branches, including mismatched current mode
and inactive resource ID; its value alone does not identify the cause.
`0x819b....` errors belong to ResourceArbitrator, while Venc uses `0x80c7....`.

The retained [console runtime log](evidence/console-runtime-2026-10-28.log)
also records one native video error `0x81950009`. The payload closed capture,
returned its resource state to STOP, waited five seconds, reopened successfully,
and resumed both tracks. The native error's cause is unknown; this observed
recovery was not a deliberately injected fault.

## Audio

`audio.c` independently opens stereo interleaved float PCM at 48 kHz from
Avcap2 and encodes 1024-sample frames with `libSceM4aacEnc` / `libSceAjmi`.
The native encoder emits ADTS AAC-LC; the backend validates/removes ADTS and
supplies raw AAC plus AudioSpecificConfig `11 90` to FLV. It preserves delayed
encoder timestamps through a FIFO. It never substitutes silence.

The audio probe produced 141 frames in approximately 3 seconds. FFprobe found
AAC-LC, stereo, 48 kHz; FFmpeg decoded 144,384 finite, non-silent samples. See
[audio probe](evidence/audio-probe-2026-10-28.log) and
[audio FFprobe result](evidence/audio-probe-ffprobe.json).

## Reproducing validation

`make probe-video` builds the finite probe from the production backend. It
writes only `/data/ps5hs-video-probe.log` and `.h264`, captures about 3 seconds,
and closes its video/resource session. Do not run it concurrently with another
capture client using the same resource. Decode the downloaded output with:

```
ffprobe -v error -count_frames -show_entries stream=codec_name,width,height,profile,pix_fmt,nb_read_frames -of json capture.h264
ffmpeg -v error -i capture.h264 -f null -
```

`tests/test_avcap.c` exercises initialization order, resource cleanup after
open/header/start failures, nonblocking read statuses, native header insertion,
large timestamps, periodic keyframe request timing, and preserving a resource
state this client did not acquire.
This host test supplements the console probe; mocks alone do not establish ABI
correctness or on-console playback.
