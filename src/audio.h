/* Copyright (C) 2026 heydemoura */
#ifndef PS5HS_AUDIO_H
#define PS5HS_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#define AUDIO_SAMPLE_RATE 48000
#define AUDIO_CHANNELS 2
#define AUDIO_FRAME_SAMPLES 1024
#define AUDIO_PCM_FLOATS (AUDIO_FRAME_SAMPLES * AUDIO_CHANNELS)
#define AUDIO_AAC_MAX_BYTES 1536

typedef struct audio_capture audio_capture;
typedef struct audio_encoder audio_encoder;
typedef struct {
  uint8_t data[AUDIO_AAC_MAX_BYTES]; /* one raw AAC-LC access unit, no ADTS */
  size_t len;
  uint64_t pts_us;
} audio_packet;

/* The caller owns sceAvcap2Initialize/Terminate and must keep the service
 * initialized until all video and audio handles have closed. These functions
 * open/start and stop/close only their own handle. resource_id must remain
 * RUNNING until audio closes (production shares avcap_resource_id()). */
audio_capture *audio_capture_open(int resource_id);
void audio_capture_close(audio_capture *capture);
/* 1024 float32 stereo frames, 0 for empty/overrun, negative on failure.
 * pcm must have AUDIO_PCM_FLOATS elements. pts_us is the capture timestamp. */
int audio_capture_read(audio_capture *capture, float *pcm, uint64_t *pts_us);

/* Runtime-resolved libSceM4aacEnc, 48 kHz stereo, native AJM AAC encoder. */
audio_encoder *audio_encoder_open(unsigned bitrate);
void audio_encoder_close(audio_encoder *encoder);
/* Exactly one 1024-frame interleaved float32 block. 1 = AAC packet,
 * 0 = encoder buffering, negative = failure. Timestamps follow buffered PCM. */
int audio_encoder_encode(audio_encoder *encoder, const float *pcm,
                         uint64_t pts_us, audio_packet *packet);
int audio_encoder_flush(audio_encoder *encoder, audio_packet *packet);
/* AAC-LC 48 kHz stereo AudioSpecificConfig: two bytes. */
size_t audio_encoder_config(uint8_t out[2]);

/* Validate one complete ADTS AAC-LC/48 kHz/stereo frame. Returns payload size,
 * zero on invalid input; *payload points into input. Used by native encoder. */
size_t audio_adts_payload(const uint8_t *data, size_t len,
                          const uint8_t **payload);
/* Construct a seven-byte ADTS header for one raw packet (diagnostic files). */
int audio_adts_header(uint8_t out[7], size_t payload_len);

#endif
