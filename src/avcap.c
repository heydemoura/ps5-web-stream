/* Copyright (C) 2026 heydemoura */
#include "avcap.h"
#include "scanin.h"
#include "log.h"
#include <dlfcn.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#define AVCAP_INVALID ((int)0x81950001)
#define AVCAP_EMPTY ((int)0x81950002)
#define AVCAP_OVERRUN ((int)0x81950004)

static void *g_lib, *g_ipmi, *g_video;
static int g_initialized, g_err, g_use_scanin;
static int (*p_Initialize)(void), (*p_Terminate)(void);
static int (*p_OpenVideo)(void **, void *, int);
static int (*p_Start)(void *), (*p_Stop)(void *), (*p_Close)(void *);
static int (*p_ReadVideo)(void *, void *, uint64_t, void *, int);
static int (*p_GetVideoOutMode)(int *);
static int (*p_GetHeaders)(void *, void *);
static int (*p_RequestKeyFrame)(void *);
static void *g_ra_lib;
static int (*p_RaSet)(int, void (*)(int,int,void *), void *);
static int (*p_RaCheck)(int, const char *);
static int (*p_RaState)(int, const char *, void *, int);
static int (*p_RaClear)(int);
static int g_ra_registered, g_ra_owned;
static atomic_int g_ra_event;
static uint8_t g_headers[264];
static size_t g_headers_len;
static uint64_t g_keyframe_clock_us;
static int g_have_keyframe_clock;
#define KEYFRAME_INTERVAL_US UINT64_C(2000000)
#define VIDEO_RESOURCE 0xb4

static void resource_callback(int id, int event, void *info) {
  if (id == VIDEO_RESOURCE && event == 1 && info)
    atomic_store(&g_ra_event, *(const uint8_t *)info ? 1 : -1);
}
static int resource_wait(void) {
  for (unsigned i=0; i<200; ++i) {
    int event = atomic_load(&g_ra_event);
    if (event) return event > 0 ? 0 : (int)0x81950008;
    usleep(5000);
  }
  return (int)0x81950008;
}
static void resource_release(void) {
  if (g_ra_owned) {
    atomic_store(&g_ra_event, 0);
    int r = p_RaState(VIDEO_RESOURCE, "STOP", NULL, getpid());
    if (r >= 0) r = resource_wait();
    LOG("avcap: ResourceArbitrator STOP -> 0x%x", r);
    g_ra_owned = 0;
  }
  if (g_ra_registered) { p_RaClear(VIDEO_RESOURCE); g_ra_registered = 0; }
}
static int resource_acquire(void) {
  if (!g_ra_lib) {
    g_ra_lib = dlopen("libSceResourceArbitrator.sprx", RTLD_NOW | RTLD_GLOBAL);
    if (!g_ra_lib) return -1;
    p_RaSet = dlsym(g_ra_lib, "sceResourceArbitratorSetCallback");
    p_RaCheck = dlsym(g_ra_lib, "sceResourceArbitratorIsExecutable");
    p_RaState = dlsym(g_ra_lib, "sceResourceArbitratorChangeState");
    p_RaClear = dlsym(g_ra_lib, "sceResourceArbitratorClearCallback");
  }
  if (!p_RaSet || !p_RaCheck || !p_RaState || !p_RaClear) return -1;
  int r = p_RaSet(VIDEO_RESOURCE, resource_callback, NULL);
  if (r < 0) return r;
  g_ra_registered = 1;
  r = p_RaCheck(VIDEO_RESOURCE, "RUNNING");
  if (r <= 0) { resource_release(); return r < 0 ? r : (int)0x81950008; }
  if (r != 2) {
    atomic_store(&g_ra_event, 0);
    r = p_RaState(VIDEO_RESOURCE, "RUNNING", NULL, getpid());
    if (r < 0) { resource_release(); return r; }
    g_ra_owned = 1;
    r = resource_wait();
    if (r < 0) { resource_release(); return r; }
  }
  LOG("avcap: ResourceArbitrator 0xb4 RUNNING");
  return 0;
}

int avcap_last_error(void) { return g_err; }
void avcap_set_scanin(int on) { g_use_scanin = !!on; }

static void *resolve(const char *name) {
  void *p = dlsym(g_lib, name);
  if (!p) LOG("avcap: missing %s: %s", name, dlerror());
  return p;
}

int avcap_load(void) {
  if (g_lib) return 0;
  /* Avcap2 imports Ipmi; load its dependency before resolving the wrapper. */
  g_ipmi = dlopen("libSceIpmi.sprx", RTLD_NOW | RTLD_GLOBAL);
  if (!g_ipmi) { LOG("avcap: Ipmi load failed: %s", dlerror()); return g_err = -1; }
  g_lib = dlopen("libSceAvcap2.sprx", RTLD_NOW | RTLD_GLOBAL);
  if (!g_lib) { LOG("avcap: load failed: %s", dlerror()); return g_err = -1; }
  p_Initialize = resolve("sceAvcap2Initialize");
  p_Terminate = resolve("sceAvcap2Terminate");
  p_OpenVideo = resolve("sceAvcap2OpenVideo");
  p_Start = resolve("sceAvcap2Start");
  p_Stop = resolve("sceAvcap2Stop");
  p_Close = resolve("sceAvcap2Close");
  p_ReadVideo = resolve("sceAvcap2ReadVideo");
  p_GetVideoOutMode = resolve("sceAvcap2GetVideoOutMode");
  p_GetHeaders = resolve("sceAvcap2GetNonVclNalUnitStream");
  p_RequestKeyFrame = resolve("sceAvcap2RequestKeyFrame");
  if (!p_Initialize || !p_Terminate || !p_OpenVideo || !p_Start ||
      !p_Stop || !p_Close || !p_ReadVideo || !p_GetVideoOutMode || !p_GetHeaders ||
      !p_RequestKeyFrame) {
    dlclose(g_lib); g_lib = NULL; return g_err = -1;
  }
  return g_err = 0;
}

int avcap_init(void) {
  if (g_initialized) return 0;
  if (!g_lib && avcap_load() < 0) return g_err;
  int r = p_Initialize();
  LOG("avcap: Initialize -> 0x%x", r);
  if (r >= 0) g_initialized = 1;
  g_err = r;
  return r < 0 ? r : 0;
}

static void set32(uint8_t *p, size_t off, uint32_t value) {
  memcpy(p + off, &value, sizeof(value));
}

/* Full versioned record from RemoteplayDaemon 0x57770. These sizes are
 * mandatory. Output mode is queried after Initialize. The source resource
 * must be RUNNING before OpenVideo; see docs/AVCAP2.md for runtime evidence. */
static void video_params(uint8_t p[0x2e0], const avcap_config *cfg, int mode,
                         unsigned resolution) {
  static const unsigned sizes[][2] = {
    {0,0x2e0},{8,0x28},{0x30,0x58},{0x38,0x18},{0x50,0x10},
    {0x60,0x18},{0x88,0x30},{0xb8,0xe0},{0xc0,0x20},{0xe0,0x20},
    {0x100,0x20},{0x120,0x18},{0x188,0x10},{0x198,0x50},
    {0x1e8,0xf8},{0x1f0,0x28},{0x218,0x88},{0x2a8,0x10},{0x2b8,0x10}
  };
  memset(p, 0, 0x2e0);
  for (size_t i=0; i<sizeof(sizes)/sizeof(sizes[0]); ++i)
    set32(p, sizes[i][0], sizes[i][1]);
  set32(p, 0x14, 2); /* Remote Play source */
  set32(p, 0x18, resolution);
  set32(p, 0x1c, cfg->fps == 60);
  set32(p, 0x24, 0); /* AVC */
  set32(p, 0x28, VIDEO_RESOURCE); /* active ResourceArbitrator ID */
  set32(p, 0x40, 1); set32(p, 0x44, 245760u/(unsigned)cfg->fps/60u);
  set32(p, 0x48, 1); set32(p, 0x4c, 1400*8);
  set32(p, 0x58, 1); set32(p, 0x5c, 9); set32(p, 0x68, 1);
  uint32_t bps = (uint32_t)cfg->bitrate * 1000u;
  set32(p, 0xc8, (uint32_t)((float)bps/1.12f)); set32(p, 0xcc, bps);
  uint32_t frame_bits = (uint32_t)((uint64_t)bps*1001u/((unsigned)cfg->fps*1000u));
  set32(p, 0xd0, frame_bits); set32(p, 0xd4, 35);
  set32(p, 0xd8, 0x33);
  set32(p, 0x108, 1); set32(p, 0x10c, frame_bits/8); set32(p, 0x128, 1); set32(p, 0x12c, 25);
  memset(p+0x220, 0xff, 0x80);
  set32(p, 0x2a0, (uint32_t)mode);
  set32(p, 0x2a4, (mode&1) ? 5 : (mode&2) ? 7 : 2);
  set32(p, 0x2b0, 1); p[0x2b4] = 1; p[0x2b5] = 1;
  p[0x2c0] = 1; set32(p, 0x2c4, 110);
}

int avcap_open(const avcap_config *cfg) {
  static const struct { int w,h; unsigned code; } resolutions[] = {
    {640,360,1},{960,540,2},{1280,720,3},{1920,1080,4},
    {2560,1440,5},{3840,2160,6}
  };
  if (!cfg || !g_initialized || g_video ||
      (cfg->fps != 30 && cfg->fps != 60) || cfg->bitrate <= 0 ||
      (unsigned)cfg->bitrate > UINT32_MAX/1000u) return g_err = AVCAP_INVALID;
  unsigned resolution = 0;
  for (size_t i=0; i<sizeof(resolutions)/sizeof(resolutions[0]); ++i)
    if (cfg->width == resolutions[i].w && cfg->height == resolutions[i].h)
      resolution = resolutions[i].code;
  if (!resolution) return g_err = AVCAP_INVALID;
  int mode = -1;
  int r = p_GetVideoOutMode(&mode);
  LOG("avcap: GetVideoOutMode -> 0x%x mode=0x%x", r, mode);
  if (r < 0) return g_err = r;
  if (g_use_scanin) {
    r = scanin_enable(11);
    LOG("avcap: scanin_enable -> 0x%x", r);
    if (r < 0) return g_err = r;
  }
  r = resource_acquire();
  if (r < 0) return g_err = r;
  uint8_t params[0x2e0];
  video_params(params, cfg, mode, resolution);
  void *handle = NULL;
  r = p_OpenVideo(&handle, params, 0);
  LOG("avcap: OpenVideo %dx%d@%d %d kbps mode=0x%x -> 0x%x", cfg->width,
      cfg->height, cfg->fps, cfg->bitrate, mode, r);
  if (r < 0) { resource_release(); return g_err = r; }
  if (!handle) { resource_release(); return g_err = -1; }
  uint8_t headers[0x1a8] = {0};
  set32(headers, 0, sizeof(headers));
  r = p_GetHeaders(handle, headers);
  g_headers_len = 0;
  if (r >= 0) {
    const size_t offsets[] = {0x10, 0x98};
    for (unsigned i=0; i<2; ++i) {
      uint64_t len;
      memcpy(&len, headers+offsets[i], sizeof(len));
      if (!len || len > 128 || (headers[offsets[i]+8]&31) != 7+i) {
        r = AVCAP_INVALID; break;
      }
      memcpy(g_headers+g_headers_len, "\0\0\0\1", 4);
      g_headers_len += 4;
      memcpy(g_headers+g_headers_len, headers+offsets[i]+8, (size_t)len);
      g_headers_len += (size_t)len;
    }
  }
  LOG("avcap: NonVcl headers -> 0x%x (%zu bytes)", r, g_headers_len);
  if (r < 0) { p_Close(handle); resource_release(); g_headers_len=0; return g_err=r; }
  r = p_Start(handle);
  LOG("avcap: Start -> 0x%x", r);
  if (r < 0) { p_Close(handle); resource_release(); return g_err = r; }
  g_video = handle;
  return g_err = 0;
}

static uint64_t ticks_to_us(uint64_t ticks) {
  /* Avcap service uses the 240 kHz media clock; audio verified by sample count. */
  return (ticks/240000u)*1000000u + (ticks%240000u)*1000000u/240000u;
}
static int has_idr(const uint8_t *p, size_t n) {
  for (size_t i=0; i+3<n; ++i)
    if (p[i]==0 && p[i+1]==0 && p[i+2]==1 && (p[i+3]&31)==5) return 1;
  return 0;
}

int avcap_read_video(uint8_t *buf, size_t size, avcap_frame *fr) {
  if (!g_video || !buf || !size || !fr) return g_err = AVCAP_INVALID;
  uint8_t info[0x80] = {0};
  int r = p_ReadVideo(g_video, buf, size, info, 1); /* nonblocking */
  if (r == 0 || r == AVCAP_EMPTY || r == AVCAP_OVERRUN) return 0;
  if (r < 0) return g_err = r;
  if ((size_t)r > size) return g_err = AVCAP_INVALID;
  uint64_t pts, dts;
  memcpy(&pts, info+8, 8); memcpy(&dts, info+0x10, 8);
  fr->len = (size_t)r; fr->pts_us = ticks_to_us(pts); fr->dts_us = ticks_to_us(dts);
  fr->keyframe = has_idr(buf, fr->len);
  /* Remote Play's default encoder configuration emits only an initial IDR.
   * Request another every two seconds of captured media so new viewers can
   * start. RequestKeyFrame(handle) is the exported one-argument native API. */
  if (!g_have_keyframe_clock || fr->keyframe || fr->pts_us < g_keyframe_clock_us) {
    g_keyframe_clock_us = fr->pts_us;
    g_have_keyframe_clock = 1;
  } else if (fr->pts_us-g_keyframe_clock_us >= KEYFRAME_INTERVAL_US) {
    int kr = p_RequestKeyFrame(g_video);
    if (kr < 0) {
      LOG("avcap: RequestKeyFrame -> 0x%x", kr);
      return g_err = kr;
    }
    g_keyframe_clock_us = fr->pts_us;
  }
  /* The encoder emits slice NALs only. Repeat native SPS/PPS at each IDR so
   * the transport can initialize and late viewers can decode independently. */
  if (fr->keyframe && g_headers_len) {
    if (size-fr->len < g_headers_len) return g_err = AVCAP_INVALID;
    memmove(buf+g_headers_len, buf, fr->len);
    memcpy(buf, g_headers, g_headers_len);
    fr->len += g_headers_len;
  }
  g_err = 0;
  return 1;
}

void avcap_close(void) {
  if (g_video) { p_Stop(g_video); p_Close(g_video); g_video = NULL; }
  g_headers_len = 0;
  g_have_keyframe_clock = 0;
  resource_release();
}
void avcap_terminate(void) {
  avcap_close();
  if (g_initialized) { p_Terminate(); g_initialized = 0; }
}
