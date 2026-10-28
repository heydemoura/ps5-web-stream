/* Copyright (C) 2026 heydemoura
 *
 * PS5 HDMI Stream — entry point.
 *
 * Captures the console's composited HDMI output as hardware-encoded H.264 via
 * libSceAvcap2, muxes it to FLV, and broadcasts it:
 *   - as an RTMP server on :1935  (OBS Media Source input)
 *   - as HTTP-FLV + a live preview web page on :8080
 *
 * The capture backend is privileged and firmware-sensitive; the HTTP server
 * (and therefore the /log endpoint) always comes up first, so the payload can
 * be diagnosed from a browser even when capture itself fails to open.
 */
#include "avcap.h"
#include "flv.h"
#include "http.h"
#include "rtmp.h"
#include "log.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- globals required by log.h ------------------------------------------- */
char   g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ---- configuration (overridable by argv: [http_port] [rtmp_port]) -------- */
#define DEF_HTTP_PORT  8080
#define DEF_RTMP_PORT  1935
#define CFG_WIDTH      1920
#define CFG_HEIGHT     1080
#define CFG_FPS        60
#define CFG_BITRATE    8000      /* encoder config field (see docs/AVCAP2.md) */
#define CFG_AUDIO      0         /* AAC encode not wired yet; video-only v1   */

#define VIDBUF_SZ  (4 * 1024 * 1024)
#define AVCCBUF_SZ (4 * 1024 * 1024 + 4096)

static broadcaster g_bc;

/* ---- on-screen toast (best effort) --------------------------------------- */
static void
notify(const char *msg) {
  void *lib = dlopen("libSceNotification.sprx", RTLD_LAZY);
  if (!lib) lib = dlopen("/system/common/lib/libSceNotification.sprx", RTLD_LAZY);
  if (!lib) return;
  int (*send)(int, int, const char *) = dlsym(lib, "sceNotificationSend");
  if (!send) return;
  char json[768];
  snprintf(json, sizeof(json),
    "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\","
    "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\","
    "\"isImmediate\":true,\"priority\":100,\"viewData\":{"
    "\"message\":{\"body\":\"%s\"},"
    "\"subMessage\":{\"body\":\"PS5 HDMI Stream\"}},"
    "\"platformViews\":{\"previewDisabled\":{\"viewData\":{"
    "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
    "\"message\":{\"body\":\"PS5 HDMI Stream\"}}}}}}", msg);
  send(0xFE, 1, json);
}

/* ---- capture loop -------------------------------------------------------- */

static void
capture_loop(void) {
  uint8_t *annexb = malloc(VIDBUF_SZ);
  uint8_t *avcc   = malloc(AVCCBUF_SZ);
  if (!annexb || !avcc) { LOG("main: capture buffers alloc failed"); return; }

  int have_cfg = 0;
  uint32_t frame_no = 0;
  uint32_t base_ms = 0;
  int base_set = 0;

  for (;;) {
    avcap_frame fr;
    int r = avcap_read_video(annexb, VIDBUF_SZ, &fr);
    if (r == 0) { usleep(2000); continue; }
    if (r < 0) {
      LOG("main: ReadVideo fatal 0x%x, stopping capture", avcap_last_error());
      break;
    }

    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    size_t avcc_len = annexb_to_avcc(annexb, fr.len, avcc,
                                     &sps, &sps_len, &pps, &pps_len);
    if (!avcc_len) continue;

    if (!have_cfg && sps && pps) {
      uint8_t cfg[256];
      size_t cfg_len = avc_decoder_config(cfg, sps, sps_len, pps, pps_len);
      broadcaster_set_avc_config(&g_bc, cfg, cfg_len);
      have_cfg = 1;
      LOG("main: AVC config set (sps=%zu pps=%zu), %dx%d",
          sps_len, pps_len, g_bc.width, g_bc.height);
      notify("Streaming started");
    }
    if (!have_cfg) continue;   /* wait for the first keyframe with SPS/PPS */

    uint32_t ts = fr.pts_ms;
    if (!base_set) { base_ms = ts; base_set = 1; }
    ts = (ts >= base_ms) ? ts - base_ms : 0;
    if (ts == 0 && frame_no)                      /* no usable pts: synth */
      ts = (uint32_t)((uint64_t)frame_no * 1000 / CFG_FPS);

    uint8_t *tag;
    size_t tl = flv_video_tag(&tag, avcc, avcc_len, fr.keyframe, 1, ts, 0);
    if (tag) {
      broadcaster_push(&g_bc, tag, tl, fr.keyframe);
      free(tag);
    }
    frame_no++;
    if ((frame_no % 300) == 0)
      LOG("main: %u frames pushed (last ts=%ums, key=%d, viewers=%d)",
          frame_no, ts, fr.keyframe, g_bc.n_clients);
  }
  free(annexb);
  free(avcc);
}

int
main(int argc, char **argv) {
  int http_port = DEF_HTTP_PORT;
  int rtmp_port = DEF_RTMP_PORT;
  if (argc > 1) http_port = atoi(argv[1]);
  if (argc > 2) rtmp_port = atoi(argv[2]);

  setvbuf(stdout, NULL, _IONBF, 0);
  LOG("=== PS5 HDMI Stream starting (http=%d rtmp=%d) ===", http_port, rtmp_port);

  broadcaster_init(&g_bc);
  broadcaster_set_video_params(&g_bc, CFG_WIDTH, CFG_HEIGHT, CFG_FPS, CFG_BITRATE);

  /* Servers first: the web UI and /log must be reachable even if capture
   * never opens, so failures can be read from a browser. */
  if (http_server_start(&g_bc, http_port, rtmp_port) != 0)
    LOG("main: HTTP server failed to start");
  if (rtmp_server_start(&g_bc, rtmp_port) != 0)
    LOG("main: RTMP server failed to start");

  if (avcap_load() != 0) {
    LOG("main: libSceAvcap2 unavailable; serving UI/log only");
    for (;;) pause();
  }
  if (avcap_init() != 0) {
    LOG("main: avcap_init failed (0x%x); serving UI/log only", avcap_last_error());
    for (;;) pause();
  }

  avcap_config cfg = {
    .width = CFG_WIDTH, .height = CFG_HEIGHT,
    .fps = CFG_FPS, .bitrate = CFG_BITRATE, .have_audio = CFG_AUDIO,
  };

  for (;;) {
    if (avcap_open(&cfg) == 0) {
      LOG("main: capture open OK, entering capture loop");
      capture_loop();
      avcap_close();
    } else {
      LOG("main: capture open failed (0x%x); retrying in 5s", avcap_last_error());
    }
    sleep(5);   /* retry: HDMI mode change, HDCP, or transient open failure */
  }
  return 0;
}
