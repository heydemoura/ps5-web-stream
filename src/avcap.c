/* Copyright (C) 2026 heydemoura */
#include "avcap.h"
#include "log.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- parameter-block layout (relative to the params pointer) -------------
 * These offsets were taken from SceShellCore's video-open routine. The video
 * object we allocate is [ u64 handle ][ params... ]; params = object + 8.
 * See docs/AVCAP2.md for the full derivation.                               */
#define P_RESCODE   0x18   /* resolution class code   */
#define P_RESSUB    0x1c
#define P_FLAGA     0x20
#define P_FLAGB     0x24
#define P_FPS       0x94
#define P_BITRATE   0xc8
#define P_CS0       0x2a0
#define P_CS1       0x2a4   /* colour space            */
#define P_CS2       0x2b4
#define P_CS3       0x2b5
#define P_MEMMODE   0x1a0   /* 0 => library allocates encoder memory itself */

#define VOBJ_SIZE   0x400
#define AOBJ_SIZE   0x400

/* Return codes observed from the service. */
#define AVCAP_EAGAIN 0x81950001

/* ---- resolved entry points ---------------------------------------------- */
static void *g_lib;
static int (*p_Initialize)(void);
static int (*p_Terminate)(void);
static int (*p_OpenVideo)(void *handle_out, void *params, int flag);
static int (*p_OpenAudio)(void *handle_out, void *params, int flag);
static int (*p_Start)(void *handle);
static int (*p_Stop)(void *handle);
static int (*p_Close)(void *handle);
static int (*p_ReadVideo)(void *handle, void *buf, uint64_t size, void *info, int flag);
static int (*p_ReadAudio)(void *handle, void *buf, uint64_t size, void *info, int flag);
static int (*p_GetVideoOutMode)(int *out);

static void *g_vobj, *g_aobj;
static int   g_err;

int avcap_last_error(void) { return g_err; }

static void *RES(const char *n) {
  void *f = dlsym(g_lib, n);
  if (!f) LOG("avcap: dlsym(%s) failed: %s", n, dlerror());
  return f;
}

int
avcap_load(void) {
  if (g_lib) return 0;
  const char *paths[] = {
    "libSceAvcap2.sprx",
    "/system/common/lib/libSceAvcap2.sprx",
    NULL
  };
  for (int i = 0; paths[i]; i++) {
    g_lib = dlopen(paths[i], RTLD_LAZY | RTLD_GLOBAL);
    if (g_lib) { LOG("avcap: loaded %s (%p)", paths[i], g_lib); break; }
    LOG("avcap: dlopen(%s): %s", paths[i], dlerror());
  }
  if (!g_lib) return -1;

  p_Initialize      = RES("sceAvcap2Initialize");
  p_Terminate       = RES("sceAvcap2Terminate");
  p_OpenVideo       = RES("sceAvcap2OpenVideo");
  p_OpenAudio       = RES("sceAvcap2OpenAudio");
  p_Start           = RES("sceAvcap2Start");
  p_Stop            = RES("sceAvcap2Stop");
  p_Close           = RES("sceAvcap2Close");
  p_ReadVideo       = RES("sceAvcap2ReadVideo");
  p_ReadAudio       = RES("sceAvcap2ReadAudio");
  p_GetVideoOutMode = RES("sceAvcap2GetVideoOutMode");

  if (!p_Initialize || !p_OpenVideo || !p_Start || !p_ReadVideo) {
    LOG("avcap: required symbols missing");
    return -1;
  }
  return 0;
}

int
avcap_init(void) {
  if (p_GetVideoOutMode) {
    int mode = -1;
    int r = p_GetVideoOutMode(&mode);
    LOG("avcap: GetVideoOutMode -> r=0x%x mode=%d", r, mode);
  }
  int r = p_Initialize();
  LOG("avcap: Initialize -> 0x%x", r);
  g_err = r;
  return r < 0 ? r : 0;
}

static void set32(uint8_t *base, int off, uint32_t v) { *(uint32_t *)(base + off) = v; }
static void set8 (uint8_t *base, int off, uint8_t  v) { *(uint8_t  *)(base + off) = v; }

int
avcap_open(const avcap_config *cfg) {
  g_vobj = aligned_alloc(16, VOBJ_SIZE);
  if (!g_vobj) return -1;
  memset(g_vobj, 0, VOBJ_SIZE);

  uint8_t *params = (uint8_t *)g_vobj + 8;
  set32(params, P_RESCODE, 4);
  set32(params, P_RESSUB,  1);
  set32(params, P_FLAGA,   0);
  set32(params, P_FLAGB,   2);
  set32(params, P_FPS,     (uint32_t)cfg->fps);
  set32(params, P_BITRATE, (uint32_t)cfg->bitrate);
  set32(params, P_CS0,     0);
  set32(params, P_CS1,     1);
  set8 (params, P_CS2,     1);
  set8 (params, P_CS3,     1);
  set32(params, P_MEMMODE, 0);

  int r = p_OpenVideo(g_vobj, params, 0);
  LOG("avcap: OpenVideo -> 0x%x (handle=%p)", r, *(void **)g_vobj);
  g_err = r;
  if (r < 0) { free(g_vobj); g_vobj = NULL; return r; }

  r = p_Start(*(void **)g_vobj);
  LOG("avcap: Start -> 0x%x", r);
  g_err = r;
  if (r < 0) { p_Close(*(void **)g_vobj); free(g_vobj); g_vobj = NULL; return r; }

  if (cfg->have_audio && p_OpenAudio) {
    g_aobj = aligned_alloc(16, AOBJ_SIZE);
    if (g_aobj) {
      memset(g_aobj, 0, AOBJ_SIZE);
      uint8_t *ap = (uint8_t *)g_aobj + 8;
      /* audio params: mode 1, two 0x10-byte planes, sample rate hint */
      set32(ap, 0x18, 1);
      set32(ap, 0x20, 0x10);
      set32(ap, 0x30, 48);
      int ar = p_OpenAudio(g_aobj, ap, 0);
      LOG("avcap: OpenAudio -> 0x%x (handle=%p)", ar, *(void **)g_aobj);
      if (ar < 0) { free(g_aobj); g_aobj = NULL; }
    }
  }
  return 0;
}

/* The info block the service fills. Only a few fields are known; it is sized
 * generously (the library writes up to 0x60 bytes). */
typedef struct {
  uint8_t raw[0x80];
} avcap_info;

int
avcap_read_video(uint8_t *buf, size_t bufsize, avcap_frame *fr) {
  if (!g_vobj) return -1;
  avcap_info info;
  memset(&info, 0, sizeof(info));
  void *handle = *(void **)g_vobj;

  int r = p_ReadVideo(handle, buf, bufsize, &info, 0);
  if (r == (int)AVCAP_EAGAIN) return 0;
  if (r <= 0) { g_err = r; return r < 0 ? r : 0; }

  uint64_t pts = *(uint64_t *)(info.raw + 0x08);
  uint64_t dts = *(uint64_t *)(info.raw + 0x10);
  uint32_t ftype = *(uint32_t *)(info.raw + 0x1c);

  fr->len = (size_t)r;
  fr->pts_ms = (uint32_t)(pts / 1000);   /* service reports microseconds */
  fr->dts_ms = (uint32_t)(dts / 1000);
  fr->keyframe = (ftype == 1);
  return 1;
}

int
avcap_read_audio(uint8_t *buf, size_t bufsize, avcap_frame *fr) {
  if (!g_aobj || !p_ReadAudio) return -1;
  avcap_info info;
  memset(&info, 0, sizeof(info));
  void *handle = *(void **)g_aobj;

  int r = p_ReadAudio(handle, buf, bufsize, &info, 0);
  if (r == (int)AVCAP_EAGAIN) return 0;
  if (r <= 0) { g_err = r; return r < 0 ? r : 0; }

  uint64_t pts = *(uint64_t *)(info.raw + 0x08);
  fr->len = (size_t)r;
  fr->pts_ms = (uint32_t)(pts / 1000);
  fr->dts_ms = fr->pts_ms;
  fr->keyframe = 0;
  return 1;
}

void
avcap_close(void) {
  if (g_vobj) {
    void *h = *(void **)g_vobj;
    if (h) { if (p_Stop) p_Stop(h); if (p_Close) p_Close(h); }
    free(g_vobj); g_vobj = NULL;
  }
  if (g_aobj) {
    void *h = *(void **)g_aobj;
    if (h && p_Close) p_Close(h);
    free(g_aobj); g_aobj = NULL;
  }
  if (p_Terminate) p_Terminate();
}
