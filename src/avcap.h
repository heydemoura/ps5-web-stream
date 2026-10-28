/* Copyright (C) 2026 heydemoura
 *
 * Capture backend built on libSceAvcap2 — the console's "AV capture" service
 * that the system's own Share / broadcast code uses. It taps the composited
 * HDMI scan-out and returns it already encoded as an H.264 (AVC) Annex-B
 * access unit per call, plus the matching audio, without a capture card.
 *
 * libSceAvcap2 is not part of the payload SDK, so every entry point is
 * resolved at runtime with dlopen/dlsym against the on-console module. The
 * open sequence and the parameter-block layout were modelled on SceShellCore,
 * the known-good caller of this API.
 */
#ifndef PS5HS_AVCAP_H
#define PS5HS_AVCAP_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
  int      width, height;
  int      fps;
  int      bitrate;      /* value written to the encoder config field */
  int      have_audio;
} avcap_config;

typedef struct {
  size_t   len;          /* encoded bytes in the caller buffer */
  uint32_t pts_ms;       /* presentation timestamp, milliseconds */
  uint32_t dts_ms;
  int      keyframe;     /* 1 if this access unit is an IDR */
} avcap_frame;

/* Load and resolve libSceAvcap2; returns 0 on success. */
int  avcap_load(void);

/* Initialize the capture service (sceAvcap2Initialize). */
int  avcap_init(void);

/* Open the video (and, if cfg->have_audio, audio) capture handles. */
int  avcap_open(const avcap_config *cfg);

/* Read one encoded video access unit into `buf`. Returns:
 *   1  frame produced (fills `fr`)
 *   0  nothing ready yet, try again
 *  <0  fatal error (the SCE return code) */
int  avcap_read_video(uint8_t *buf, size_t bufsize, avcap_frame *fr);

/* Read one audio chunk (PCM) into `buf`. Same return convention. */
int  avcap_read_audio(uint8_t *buf, size_t bufsize, avcap_frame *fr);

void avcap_close(void);

/* Last raw SCE return code seen, for diagnostics. */
int  avcap_last_error(void);

#endif /* PS5HS_AVCAP_H */
