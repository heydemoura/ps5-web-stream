/* Copyright (C) 2026 heydemoura */
#include "avcap.h"
#include "audio.h"
#include "flv.h"
#include "http.h"
#include "rtmp.h"
#include "status.h"
#include "log.h"
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
#ifndef DEF_HTTP_PORT
#define DEF_HTTP_PORT 8080
#endif
#ifndef DEF_RTMP_PORT
#define DEF_RTMP_PORT 1935
#endif
#define VIDBUF_SZ (4 * 1024 * 1024)
#define AVCCBUF_SZ (VIDBUF_SZ * 2)
static broadcaster g_bc;
static volatile sig_atomic_t stopping;
static uint64_t next_epoch_ms;
static void on_signal(int sig) { (void)sig; stopping = 1; }
static uint64_t now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* One capture epoch uses one common video/audio clock. Its offset is anchored
 * to payload uptime and the last emitted timestamp, even if a buffered source
 * ran ahead of wall time. Reopened capture never rewinds connected players. */
static int capture_loop(uint64_t started) {
  uint8_t *annexb = malloc(VIDBUF_SZ), *avcc = malloc(AVCCBUF_SZ);
  audio_capture *audio = NULL;
  audio_encoder *encoder = NULL;
  int result = -1;
  if (!annexb || !avcc) goto done;
  audio = audio_capture_open();
  encoder = audio_encoder_open(128000);
  if (!audio || !encoder) { LOG("main: audio capture/encoder unavailable"); goto done; }
  uint8_t asc[2];
  broadcaster_set_aac_config(&g_bc, asc, audio_encoder_config(asc));
  uint8_t saved_sps[256], saved_pps[256], config[256], previous[256];
  size_t saved_sps_len = 0, saved_pps_len = 0, previous_len = 0;
  uint64_t origin_us = 0, epoch_ms = 0;
  int clock_set = 0, audio_sent = 0;
  unsigned audio_reads = 0;
  uint64_t last_dts_us = 0, last_audio_us = 0;
  stream_status_set("waiting", 0);
  uint64_t last_video = now_ms(), last_audio = last_video;
  while (!stopping) {
    int activity = 0;
    avcap_frame fr;
    int r = avcap_read_video(annexb, VIDBUF_SZ, &fr);
    if (r < 0) { result = r; LOG("main: video read failed 0x%x", r); break; }
    if (r > 0) {
      activity = 1;
      const uint8_t *sps = NULL, *pps = NULL;
      size_t sps_len = 0, pps_len = 0;
      size_t n = annexb_to_avcc(annexb, fr.len, avcc, AVCCBUF_SZ,
                                &sps, &sps_len, &pps, &pps_len);
      if (!n) { LOG("main: invalid/oversized H.264 access unit"); result = -1; break; }
      if (sps_len > sizeof(saved_sps) || pps_len > sizeof(saved_pps)) {
        LOG("main: oversized H.264 parameter set"); result = -1; break;
      }
      if (sps) { memcpy(saved_sps, sps, sps_len); saved_sps_len = sps_len; }
      if (pps) { memcpy(saved_pps, pps, pps_len); saved_pps_len = pps_len; }
      if (saved_sps_len && saved_pps_len) {
        size_t cn = avc_decoder_config(config, sizeof(config), saved_sps,
                          saved_sps_len, saved_pps, saved_pps_len);
        if (!cn) { LOG("main: invalid AVC configuration"); result = -1; break; }
        if (cn != previous_len || memcmp(previous, config, cn)) {
          broadcaster_set_avc_config(&g_bc, config, cn);
          memcpy(previous, config, cn); previous_len = cn;
        }
      }
      /* Keyframe information is corroborated from the encoded NAL units. */
      int key = 0, picture = 0;
      for (size_t i = 0; i + 4 < n;) {
        size_t len = ((size_t)avcc[i] << 24) | ((size_t)avcc[i+1] << 16) |
                     ((size_t)avcc[i+2] << 8) | avcc[i+3];
        i += 4;
        if (!len || len > n-i) break;
        int nal_type = avcc[i] & 31;
        if (nal_type >= 1 && nal_type <= 5) picture = 1;
        if (nal_type == 5) key = 1;
        i += len;
      }
      if (picture && previous_len && (clock_set || key)) {
        if (!clock_set) {
          origin_us = fr.dts_us; epoch_ms = now_ms() - started;
          if (epoch_ms < next_epoch_ms) epoch_ms = next_epoch_ms;
          last_audio = now_ms(); clock_set = 1;
          LOG("main: video clock pts=%llu dts=%llu us monotonic_ms=%llu", (unsigned long long)fr.pts_us,
              (unsigned long long)fr.dts_us, (unsigned long long)now_ms());
        }
        if (fr.dts_us < origin_us || fr.dts_us < last_dts_us) { result = -1; break; }
        uint64_t full_ts = epoch_ms + (fr.dts_us - origin_us) / 1000;
        uint32_t ts = (uint32_t)full_ts;
        uint64_t distance = fr.pts_us >= fr.dts_us ? fr.pts_us-fr.dts_us : fr.dts_us-fr.pts_us;
        if (distance / 1000 > 8388608) { result = -1; break; }
        int64_t cts = (int64_t)(distance / 1000);
        if (fr.pts_us < fr.dts_us) cts = -cts;
        if (cts < -8388608 || cts > 8388607) { result = -1; break; }
        uint8_t *tag = NULL;
        size_t tl = flv_video_tag(&tag, avcc, n, key, 1, ts, (int)cts);
        if (!tl) { result = -1; break; }
        broadcaster_push(&g_bc, tag, tl, key); free(tag);
        last_dts_us = fr.dts_us;
        if (next_epoch_ms <= full_ts) next_epoch_ms = full_ts + 1;
        last_video = now_ms();
        stream_status_video(ts);
        if (audio_sent) stream_status_set("streaming", 0);
      }
    }
    /* Drain bounded batches so neither source can starve the other. */
    for (unsigned i = 0; i < 4; i++) {
      float pcm[AUDIO_PCM_FLOATS]; uint64_t pts;
      r = audio_capture_read(audio, pcm, &pts);
      if (r < 0) { result = r; LOG("main: audio read failed 0x%x", r); goto done; }
      if (!r) break;
      activity = 1;
      if (audio_reads++ < 3) LOG("main: PCM clock pts=%llu us monotonic_ms=%llu", (unsigned long long)pts,
          (unsigned long long)now_ms());
      audio_packet packet;
      r = audio_encoder_encode(encoder, pcm, pts, &packet);
      if (r < 0) { result = r; goto done; }
      if (!r || !clock_set || packet.pts_us < origin_us) continue;
      if (audio_sent && packet.pts_us < last_audio_us) { result = -1; goto done; }
      uint64_t full_ts = epoch_ms + (packet.pts_us - origin_us) / 1000;
      uint32_t ts = (uint32_t)full_ts;
      uint8_t *tag = NULL;
      size_t tl = flv_audio_tag(&tag, packet.data, packet.len, 1, ts);
      if (!tl) { result = -1; goto done; }
      broadcaster_push(&g_bc, tag, tl, 0); free(tag);
      last_audio_us = packet.pts_us; last_audio = now_ms(); audio_sent = 1;
      if (next_epoch_ms <= full_ts) next_epoch_ms = full_ts + 1;
      stream_status_audio(ts); stream_status_set("streaming", 0);
    }
    if (now_ms() - last_video > 10000) {
      LOG("main: no usable video for 10 seconds; reopening capture");
      result = -1; break;
    }
    if (clock_set && now_ms() - last_audio > 10000) {
      LOG("main: no usable audio for 10 seconds; reopening capture");
      result = -1; break;
    }
    if (!activity) usleep(2000);
  }
  if (stopping) result = 0;
done:
  audio_encoder_close(encoder); audio_capture_close(audio);
  free(annexb); free(avcc);
  return result;
}

static int parse_port(const char *s, int *port) {
  char *end; errno = 0;
  long n = strtol(s, &end, 10);
  if (errno || end == s || *end || n < 1 || n > 65535) return -1;
  *port = (int)n; return 0;
}
int main(int argc, char **argv) {
  int http_port = DEF_HTTP_PORT, rtmp_port = DEF_RTMP_PORT;
  int ports = 0, use_scanin = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--noscanin") || !strcmp(argv[i], "noscanin")) use_scanin = 0;
    else if (!strcmp(argv[i], "--scanin")) use_scanin = 1;
    else if (ports >= 2 || parse_port(argv[i], ports++ ? &rtmp_port : &http_port)) {
      fprintf(stderr, "Usage: %s [http_port] [rtmp_port] [--scanin]\n", argv[0]); return 1;
    }
  }
  if (http_port == rtmp_port) { fprintf(stderr, "Ports must differ\n"); return 1; }
  signal(SIGPIPE, SIG_IGN); signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
  setvbuf(stdout, NULL, _IONBF, 0);
  LOG("PS5 HDMI Stream %s: PID %d HTTP %d / RTMP %d", PS5HS_VERSION,
      (int)getpid(), http_port, rtmp_port);
  broadcaster_init(&g_bc);
  broadcaster_set_video_params(&g_bc, 1920, 1080, 60, 8000);
  /* FLV's track flags are fixed for the lifetime of an HTTP connection. */
  g_bc.have_audio = 1;
  if (http_server_start(&g_bc, http_port, rtmp_port) || rtmp_server_start(&g_bc, rtmp_port)) {
    LOG("main: listener startup failed; exiting"); return 1;
  }
  avcap_set_scanin(use_scanin);
  avcap_config cfg = {.width=1920,.height=1080,.fps=60,.bitrate=8000};
  uint64_t started = now_ms();
  int initialized = 0;
  while (!stopping) {
    stream_status_set("opening", 0);
    int r = 0;
    if (!initialized) {
      r = avcap_load();
      if (!r) r = avcap_init();
      if (!r) initialized = 1;
    }
    if (!r) r = avcap_open(&cfg);
    if (!r) r = capture_loop(started);
    avcap_close();
    broadcaster_reset(&g_bc);
    if (stopping) break;
    LOG("main: capture unavailable (0x%x); retrying in 5s", r);
    stream_status_set("error", r);
    for (int i=0; i<50 && !stopping; i++) usleep(100000);
  }
  avcap_terminate();
  return 0;
}
