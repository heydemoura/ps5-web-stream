/* Copyright (C) 2026 heydemoura
 *
 * Latest decodable still frame from the live stream.
 *
 * The console's capture service only produces encoded H.264, so a "frame" is
 * the most recent IDR access unit, rebuilt as a standalone Annex-B
 * elementary stream (SPS, PPS, then the IDR slices). Any H.264 decoder can
 * turn it into an image without the rest of the stream, for example:
 *
 *   ffmpeg -i http://<console>:<port>/frame.h264 -frames:v 1 frame.png
 *
 * The capture thread stores keyframes; HTTP workers take a reference to the
 * newest one. When the stored keyframe is stale, a reader discards it, asks
 * the capture thread to request an extra IDR and briefly waits for it.
 * Requests are coalesced and rate-limited so pollers cannot flood viewers
 * with IDRs.
 */
#ifndef PS5HS_SNAPSHOT_H
#define PS5HS_SNAPSHOT_H

#include <stddef.h>
#include <stdint.h>

/* Keyframes newer than this are served without requesting another IDR; older
 * ones are discarded when a frame is requested. This also bounds forced
 * keyframes to roughly two per second. */
#define SNAPSHOT_FRESH_MS 500
/* How long a request waits for a requested IDR before giving up. */
#define SNAPSHOT_WAIT_MS 1000

typedef struct snapshot_frame {
  int      width, height;
  uint32_t stream_ts_ms;   /* FLV timeline timestamp of this frame */
  uint64_t captured_ms;    /* CLOCK_MONOTONIC milliseconds at store time */
  uint64_t seq;
  unsigned refs;           /* guarded by the snapshot lock */
  size_t   len;
  uint8_t  data[];         /* Annex-B: SPS, PPS, IDR slice NAL units */
} snapshot_frame;

/* Capture thread: store an IDR access unit. `avcc` holds 4-byte
 * length-prefixed NAL units as produced by annexb_to_avcc(); in-band SPS/PPS
 * and access-unit delimiters are dropped in favour of the supplied parameter
 * sets. Returns 0 on success, -1 on invalid input or allocation failure. */
int  snapshot_store(const uint8_t *sps, size_t sps_len,
                    const uint8_t *pps, size_t pps_len,
                    const uint8_t *avcc, size_t avcc_len,
                    int width, int height, uint32_t stream_ts_ms);

/* Drop the stored frame (capture restart or resolution change). */
void snapshot_clear(void);

/* Capture thread: returns 1 once per pending reader request for a new IDR. */
int  snapshot_take_keyframe_request(void);

/* HTTP worker: return a referenced frame, or NULL if none is available. When
 * the newest frame is older than `fresh_ms`, it is cleared, a new IDR is
 * requested, and the call waits up to `wait_ms` for it; NULL on timeout.
 * Release with snapshot_release(). */
snapshot_frame *snapshot_acquire(unsigned fresh_ms, unsigned wait_ms);
void snapshot_release(snapshot_frame *frame);

/* CLOCK_MONOTONIC milliseconds, the clock used for `captured_ms`. */
uint64_t snapshot_now_ms(void);

#endif /* PS5HS_SNAPSHOT_H */
