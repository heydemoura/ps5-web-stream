/* Copyright (C) 2026 heydemoura
 *
 * FLV muxing + a broadcaster that fans the muxed tag stream out to many
 * HTTP-FLV viewers.
 *
 * The broadcaster keeps the current AVC and (optionally) AAC sequence headers
 * so a client that joins mid-stream is primed and then started on the next
 * video keyframe. Each client owns a bounded byte queue; on overflow the queue
 * is dropped forward to the next keyframe, so a slow consumer degrades to a
 * lower effective frame rate instead of stalling the encoder.
 */
#ifndef PS5HS_FLV_H
#define PS5HS_FLV_H

#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

/* ---- FLV tag construction ------------------------------------------------ */

/* Build the 13-byte FLV file header (signature + flags + first PrevTagSize). */
size_t flv_file_header(uint8_t *out, int have_audio);

/* onMetaData script tag. Returns bytes written. */
size_t flv_metadata_tag(uint8_t *out, int width, int height, double fps,
                        int video_kbps, int have_audio);

/* Video tag. `avc_packet_type`: 0 = AVC sequence header (SPS/PPS as
 * AVCDecoderConfigurationRecord), 1 = NALU access unit, 2 = end of sequence.
 * `data` is the raw payload (config record, or length-prefixed NALUs). */
size_t flv_video_tag(uint8_t **out, const uint8_t *data, size_t len,
                     int keyframe, int avc_packet_type, uint32_t ts_ms,
                     int cts_ms);

/* Audio tag (AAC). `aac_packet_type`: 0 = AudioSpecificConfig, 1 = raw frame. */
size_t flv_audio_tag(uint8_t **out, const uint8_t *data, size_t len,
                     int aac_packet_type, uint32_t ts_ms);

/* Convert an Annex-B byte stream (00 00 01 / 00 00 00 01 start codes) into
 * AVCC length-prefixed form in `out`, bounded by `outcap`. Also reports
 * SPS/PPS pointers if present. Returns AVCC payload length, or 0 on invalid
 * input / insufficient space. */
size_t annexb_to_avcc(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap,
                      const uint8_t **sps, size_t *sps_len,
                      const uint8_t **pps, size_t *pps_len);

/* Build an AVCDecoderConfigurationRecord from one SPS and one PPS.
 * Returns 0 on invalid parameter sets or insufficient output space. */
size_t avc_decoder_config(uint8_t *out, size_t outcap, const uint8_t *sps, size_t sps_len,
                          const uint8_t *pps, size_t pps_len);

/* ---- Broadcaster --------------------------------------------------------- */

#define BC_MAX_CLIENTS 8

typedef struct bc_client bc_client;

typedef struct {
  pthread_mutex_t mtx;
  bc_client      *clients[BC_MAX_CLIENTS];
  int             n_clients;

  uint8_t  avc_cfg[256];
  size_t   avc_cfg_len;
  uint8_t  aac_cfg[64];
  size_t   aac_cfg_len;

  int      width, height, video_kbps;
  double   fps;
  int      have_audio;
} broadcaster;

void broadcaster_init(broadcaster *b);

/* Clear codec state and reprime existing clients after a capture restart. */
void broadcaster_reset(broadcaster *b);
/* End connections before a resolution switch, so each decoder starts fresh. */
void broadcaster_disconnect_clients(broadcaster *b);

/* Set the AVC/AAC sequence headers (raw config-record bytes). */
void broadcaster_set_avc_config(broadcaster *b, const uint8_t *cfg, size_t len);
void broadcaster_set_aac_config(broadcaster *b, const uint8_t *cfg, size_t len);
void broadcaster_set_video_params(broadcaster *b, int w, int h, double fps,
                                  int kbps);

/* Publish a muxed tag (already an FLV tag + trailing PrevTagSize) to all
 * clients. `keyframe` marks a video keyframe tag for resync. */
void broadcaster_push(broadcaster *b, const uint8_t *tag, size_t len,
                      int keyframe);

/* Register/unregister a client socket. Returns NULL on failure (too many). */
bc_client *broadcaster_add_client(broadcaster *b, int sockfd, int want_header);
void       broadcaster_remove_client(broadcaster *b, bc_client *c);

/* A client's write loop: blocks, draining its queue to the socket, until the
 * socket dies or the client is removed. Used by the HTTP-FLV path. */
void broadcaster_client_serve(bc_client *c);

/* Block until `n` bytes are available in the client queue and copy them out.
 * Returns 1 on success, 0 if the client died first. */
int broadcaster_client_read_exact(bc_client *c, uint8_t *dst, size_t n);

/* Free a client after its consumer has stopped and membership was removed. */
void broadcaster_client_free(bc_client *c);

#endif /* PS5HS_FLV_H */
