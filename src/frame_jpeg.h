/* Copyright (C) 2026 heydemoura
 *
 * JPEG still image for /frame.jpeg.
 *
 * The capture hardware only produces H.264, so the stored keyframe is decoded
 * in software (FFmpeg's H.264 decoder, statically linked) and re-encoded as
 * a baseline JFIF JPEG. Colours are converted from the stream's signalled
 * matrix and range (BT.709 limited on the tested console) to the BT.601
 * full-range YCbCr that JPEG viewers assume.
 *
 * Conversion runs once per keyframe: the JPEG is cached on the snapshot
 * frame, and conversions are serialised so concurrent requests cannot each
 * allocate a 4K decoder.
 */
#ifndef PS5HS_FRAME_JPEG_H
#define PS5HS_FRAME_JPEG_H

#include "snapshot.h"
#include <stddef.h>
#include <stdint.h>

/* MJPEG quantiser scale: 1 is best, 31 worst. 2 is visually near-lossless. */
#define FRAME_JPEG_QSCALE 2

/* Decode a standalone Annex-B H.264 keyframe and encode it as JPEG. On
 * success returns 0 and a malloc'd buffer the caller frees. */
int frame_jpeg_encode(const uint8_t *h264, size_t len, int qscale,
                      uint8_t **out, size_t *out_len);

/* Ensure `frame->jpeg` is populated (cached per keyframe). The caller holds a
 * reference to `frame`; once set, the JPEG is immutable until the frame is
 * released. Returns 0 on success. */
int frame_jpeg_prepare(snapshot_frame *frame);

#endif /* PS5HS_FRAME_JPEG_H */
