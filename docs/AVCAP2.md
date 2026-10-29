# libSceAvcap2 capture ABI and validation

The current payload serves native H.264 video and 48 kHz stereo AAC through
HTTP-FLV, with a console-hosted dashboard and autoplay `/player` page. Its
capture presets are 720p/1080p at nominal 60 fps and 1440p/3840×2160 at nominal
30 fps. All four passed native H.264/AAC decoding on the test console. The
[validation ledger](VALIDATION.md) distinguishes decoded console
results from host mocks and records the current all-mode verification status.
RTMP has been removed from the active implementation.

Capture does not require a ShellCore Share permit or a separate scan-in helper.
The earlier permit diagnosis came from querying `GetVideoOutMode` before
`Initialize` and omitting required record sizes and an active ResourceArbitrator
ID. Native `OpenVideo` negotiates the capture tap itself. The removed helper's
IPMI method-24 transport acknowledgement did not establish a separate enable
operation. No manual tap setup or kernel patch is part of the current flow.
Cold-boot operation remains unverified.

The earlier 1080p implementation also passed a 90-second simultaneous HTTP/RTMP
decode and OBS playback. Those results are
[historical evidence](VALIDATION.md#historical-evidence--previous-rtmpobs-implementation),
not verification of the current web-only binary or every capture preset.
Console media and proprietary binaries are not committed. Successful decoding
does not measure glass-to-glass latency or perceptual A/V alignment.

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
3. Select ResourceArbitrator resource `0xb4` (180) for 720p/1080p or `0xb6`
   (182) for 1440p/4K. Register its callback, require
   `sceResourceArbitratorIsExecutable(resource, "RUNNING") == 1`, then request
   `sceResourceArbitratorChangeState(resource, "RUNNING", NULL, getpid())`.
   Wait up to 15 seconds for the matching successful completion. Return value
   2 from `IsExecutable` is ambiguous and is rejected; it does not prove that
   this client already owns the resource. The PID belongs to the state request;
   it is **not** the capture record's resource ID.
4. Call `sceAvcap2OpenVideo(&handle, params, 0)` with the complete versioned
   record, source 2, the selected resource, and the current output mode.
5. Obtain SPS/PPS using `sceAvcap2GetNonVclNalUnitStream(handle, &headers)`.
6. Start, then read with `sceAvcap2ReadVideo(handle, buf, capacity, info, 1)`.
   Flag 1 makes the read nonblocking.
7. Close audio before releasing its video resource. Stop and close video, then
   request `"STOP"` only for an allocation this client acquired. Retain ownership
   until a successful STOP completion; do not replace an unreleased allocation.
8. Keep callback registrations across capture switches, because repeated
   clear/register cycles can block in firmware. Clear them on final termination,
   and terminate Avcap only after video **and audio** close.

The callback ABI is `callback(int resource, int event, void *info)`. For event 1,
the completion has a success byte at `info+0`, a state count at `+4`, and up to
five 32-byte state names starting at `+0x18`. The backend matches both resource
and requested state. A late RUNNING completion cannot satisfy a STOP wait, and
a completion for the other allocation cannot unblock capture.

The 180 allocation guarantees the smaller capture tap. The native higher-mode
path uses allocation 182 with `scaninType=3`; both 1440p and 4K request it. This
resource negotiation is part of native open, not a separate user action.

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
| `0x18` | Resolution class: 3 = 720p, 4 = 1080p, 5 = 1440p, 6 = 3840×2160 |
| `0x1c` | Cadence selector: 1 = nominal 60, 0 = nominal 30 |
| `0x24` | Codec: 0 = AVC, 1 = HEVC; only AVC tested |
| `0x28` | Active ResourceArbitrator ID: `0xb4` for 720p/1080p, `0xb6` for 1440p/4K |
| `0x40` / `0x44` / `0x48` | AVC packetization controls: 1 / `245760/fps/60` / 1 |
| `0x4c` | Packet-size budget in bits: `1400*8` |
| `0x58` / `0x5c` / `0x68` | Native controls: 1 / reference-picture budget / 1; budget is 4 for 4K, otherwise 9 |
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
Four reference pictures keep 4K AVC within the intended level budget; the
previous value of nine forced level 6.0. The dashboard currently requests 4K
at 30 fps / 32000 kbps, 1440p at 30 / 16000, 1080p at 60 / 8000, and 720p at
60 / 5000. These are encoder configuration values, not measured constant
output rates. The 1440p 60-fps attempt failed at native Start with `0x81950008`;
the final 30-fps preset passed. HDR color paths and higher-resolution 60-fps
capture remain unverified.

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
handling and reopening are covered by host tests. The historical 90-second
1080p console capture confirmed **45 IDRs**, with a maximum interval of **2.036
seconds**, and actual browser reconnect succeeded. That measurement does not
establish the interval at every current resolution.

The video info record is `0x70` bytes (the backend reserves `0x80`):

| Offset | Observed meaning |
| --- | --- |
| `0x08` | 64-bit presentation time, **240,000 ticks per second** |
| `0x10` | 64-bit decode time, same clock |
| `0x1c` | Observed 1 on initial IDR, 3 on following P pictures |
| `0x3c` / `0x40` | Observed encoded width / height |

Consecutive normal 60-fps video frames advanced by 4004 ticks, or 16,683⅓ µs.
Audio advanced by 5120 ticks per 1024 samples at 48 kHz, independently
confirming the same clock **rate**, but its absolute value is not directly
interchangeable with the video clock. The backend uses wide integer arithmetic
to convert rebased values to 64-bit microseconds. Timestamps are not
microseconds in the native record; dividing them by 1000 as the previous
implementation did was wrong.

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

Consequently, both backends resolve `sceKernelReadTsc` and
`sceKernelGetTscFrequency` from `libkernel.sprx` and anchor each capture session
to the native TSC epoch. Some encoder modes also retain a media counter while
idle; treating their raw timestamps as current time left video seconds behind
audio after resolution changes.

For video, the first successful read records native PTS `V0` and
`anchorV = floor(TSC*240000/frequency)`. Every subsequent native PTS or DTS `Vi`
maps to `floor((anchorV + Vi - V0)*1000000/240000)` microseconds. Using the same
offset for both preserves the native cadence and PTS/DTS separation. Signed
wide intermediates allow a DTS before `V0` while rejecting negative mapped
times and overflow. Close clears the anchor before the next session.

For audio, the first successful PCM read records source counter `S0` and
`anchorA = floor(TSC*240000/frequency) + 5120`. Counter `Si` maps to
`floor((anchorA + Si - S0)*1000000/240000)` microseconds. The 5120-tick addition
matches the fresh native producer's one-block offset. This preserves sample
spacing, rejects regressing counters/overflow, and resets on reopen. The
historical 90-second captures then contained 4218 decoded AAC frames each,
alongside video, with monotonic timestamps on both tracks. The capture loop
maps each session onto a continuous payload timeline and releases new stream
headers to viewers after a restart or resolution change.
RemoteplayDaemon's audio consumer at `0x5a390` obtains the native timestamp but
does not use it when returning the PCM frame. Exact perceptual A/V alignment
still needs a simultaneous audiovisual reference; packet arrival alone does
not establish capture or encoder latency. Video's first delivery includes
unmeasured capture/encode delay. Audio's first-read anchor leaves roughly one
PCM block (21.33 ms) of phase uncertainty plus scheduling/queue latency. These
anchors prevent stale epochs from breaking playback; they are not a measured
A/V synchronization calibration.

Native reads return the positive encoded byte count. `0x81950002` means no
packet ready; `0x81950004` is an overrun/retry condition. `0x81950001` is an
invalid-argument/state error and must not be swallowed as EAGAIN. Error
`0x81950008` has multiple daemon branches, including mismatched current mode
and inactive resource ID; its value alone does not identify the cause.
`0x819b....` errors belong to ResourceArbitrator, while Venc uses `0x80c7....`.

The historical [console runtime log](evidence/console-runtime-2026-10-28.log)
also records one native video error `0x81950009`. The payload closed capture,
returned its resource state to STOP, waited five seconds, reopened successfully,
and resumed both tracks. The native error's cause is unknown; this observed
recovery was not a deliberately injected fault.

## Audio

`audio.c` opens stereo interleaved float PCM at 48 kHz from Avcap2 using the
resource already held by video. Its `0x38`-byte open record contains:

| Offset | Value / meaning |
| --- | --- |
| `0x00` | 64-bit record size `0x38` |
| `0x08` | 64-bit nested format size `0x18` |
| `0x10` | Device type 2: native AudioSecond, with a per-client device key |
| `0x14` | Source 2: Remote Play system mix |
| `0x1c` | The selected video ResourceArbitrator ID, 180 or 182 |

AudioSecond still reads the full system mix; it does not select a different
channel or synthesize audio. The previous type-0 device could reuse another
client's stale resource binding. The producer checks its resource on every
PCM read, so the audio record must follow the video allocation across mode
changes. Audio closes before the owning video allocation is returned to STOP.

Each read supplies 1024 stereo float samples (8192 bytes). The backend encodes
these blocks with `libSceM4aacEnc` / `libSceAjmi`.
The native encoder emits ADTS AAC-LC; the backend validates/removes ADTS and
supplies raw AAC plus AudioSpecificConfig `11 90` to FLV. It preserves delayed
encoder timestamps through a FIFO. It never substitutes silence.

The historical audio probe produced 141 frames in approximately 3 seconds.
FFprobe found AAC-LC, stereo, 48 kHz; FFmpeg decoded 144,384 finite, non-silent
samples. See
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

`tests/test_avcap.c` exercises initialization order, resolution/resource mapping,
callback matching, cleanup after open/header/start failures, nonblocking read
statuses, native header insertion, timestamp rebasing and periodic keyframe
requests. Capture-loop tests check the resource passed to audio; audio unit
tests cover PCM validation, rebasing and AAC framing. The opt-in four-mode console and browser commands are in
[VALIDATION.md](VALIDATION.md#reproduce).
This host test supplements the console probe; mocks alone do not establish ABI
correctness or on-console playback.
