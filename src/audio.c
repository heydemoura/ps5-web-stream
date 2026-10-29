/* Copyright (C) 2026 heydemoura
 *
 * Capture layout corroborated by SoundBridge-PS5 commit
 * 050fe4fd32389312aa4865bce4c9d7fbdc7dd7aa, src/avcap2.c.
 * Native AAC ABI recovered from libSceM4aacEnc.sprx SHA256
 * 828a040bcaaaec585aa937eb3b90ae0e0249135e9fcc07f778cce03c6d457660:
 * Initialize 0x1e0, CreateEncoderEx 0x250, EncodeEx 0x600,
 * FlushEx 0x780, DeleteEncoderEx 0x580, Terminate 0x200.
 * These are symbol-resolved interfaces, never fixed runtime addresses.
 */
#include "audio.h"
#include "log.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

struct audio_capture {
  void *lib;
  void *kernel;
  int64_t handle;
  int (*read)(int64_t, void *, size_t, void *, int);
  int (*stop)(int64_t);
  int (*close)(int64_t);
  uint64_t (*read_tsc)(void);
  uint64_t tsc_frequency;
  uint64_t source_first_ticks, source_last_ticks, target_first_ticks;
  int clock_set;
};

static void *load_library(const char *name) {
  void *lib = dlopen(name, RTLD_LAZY | RTLD_GLOBAL);
  if (!lib) {
    char path[128];
    snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    lib = dlopen(path, RTLD_LAZY | RTLD_GLOBAL);
  }
  if (!lib) LOG("audio: load %s failed: %s", name, dlerror());
  return lib;
}

audio_capture *audio_capture_open(int resource_id) {
  if (resource_id < 0) return NULL;
  audio_capture *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->lib = load_library("libSceAvcap2.sprx");
  if (!s->lib) { free(s); return NULL; }
  int (*open_audio)(int64_t *, void *, int) = dlsym(s->lib, "sceAvcap2OpenAudio");
  int (*start)(int64_t) = dlsym(s->lib, "sceAvcap2Start");
  s->read = dlsym(s->lib, "sceAvcap2ReadAudio");
  s->stop = dlsym(s->lib, "sceAvcap2Stop");
  s->close = dlsym(s->lib, "sceAvcap2Close");
  if (!open_audio || !start || !s->read || !s->stop || !s->close) {
    LOG("audio: required capture symbols unavailable");
    audio_capture_close(s);
    return NULL;
  }
  s->kernel = load_library("libkernel.sprx");
  if (!s->kernel) { audio_capture_close(s); return NULL; }
  s->read_tsc = dlsym(s->kernel, "sceKernelReadTsc");
  uint64_t (*frequency)(void) = dlsym(s->kernel, "sceKernelGetTscFrequency");
  if (!s->read_tsc || !frequency || !(s->tsc_frequency = frequency())) {
    LOG("audio: native capture clock unavailable");
    audio_capture_close(s);
    return NULL;
  }
  /* uint64 size, uint64 format-structure size, uint32 fields. */
  uint32_t params[0x38 / 4] = {0};
  params[0] = 0x38;
  params[2] = 0x18;
  /* The second audio-device class has a per-client key while reading the same
   * system mix. Type 0 can reuse another client's stale resource binding. */
  params[4] = 2;
  params[5] = 2; /* Remote Play system mix */
  /* The producer checks this ResourceArbitrator ID on every PCM read.
   * It must match the RUNNING video resource, including the 4K allocation. */
  params[7] = (uint32_t)resource_id;
  int r = open_audio(&s->handle, params, 0);
  LOG("audio: OpenAudio -> 0x%x handle=0x%llx", r,
      (unsigned long long)s->handle);
  if (r != 0 || !s->handle) {
    s->handle = 0;
    audio_capture_close(s);
    return NULL;
  }
  r = start(s->handle);
  LOG("audio: Start -> 0x%x", r);
  if (r != 0) { audio_capture_close(s); return NULL; }
  return s;
}

void audio_capture_close(audio_capture *s) {
  if (!s) return;
  if (s->handle) {
    s->stop(s->handle);
    s->close(s->handle);
  }
  if (s->lib) dlclose(s->lib);
  if (s->kernel) dlclose(s->kernel);
  free(s);
}

static int audio_rebase_timestamp(audio_capture *s, uint64_t source, uint64_t *us) {
  uint64_t first = s->source_first_ticks, target = s->target_first_ticks;
  if (!s->clock_set) {
    if (!s->read_tsc || !s->tsc_frequency) return -1;
    /* AvCaptureManager+0x11440 initializes a fresh device to TSC*240000/freq
     * plus one PCM block (5120 ticks), then advances by sample count. Its
     * persistent device can retain a stale counter across capture gaps. Rebase
     * once to the same native TSC epoch as video and preserve subsequent deltas.
     * First-read arrival leaves approximately one block (21 ms) of phase
     * uncertainty, plus unmeasured scheduling/queue latency. */
    __uint128_t native = (__uint128_t)s->read_tsc()*240000u/s->tsc_frequency;
    native += AUDIO_FRAME_SAMPLES*5u;
    if (native > UINT64_MAX) return -1;
    first = source; target = (uint64_t)native;
  } else if (source < s->source_last_ticks) {
    LOG("audio: capture sample clock regressed");
    return -1;
  }
  uint64_t delta = source-first;
  if (delta > UINT64_MAX-target) return -1;
  __uint128_t converted = (__uint128_t)(target+delta)*1000000u/240000u;
  if (converted > UINT64_MAX) return -1;
  *us = (uint64_t)converted;
  s->source_first_ticks = first; s->target_first_ticks = target;
  s->source_last_ticks = source; s->clock_set = 1;
  return 0;
}

int audio_capture_read(audio_capture *s, float *pcm, uint64_t *pts_us) {
  if (!s || !pcm || !pts_us) return -1;
  uint64_t meta[2] = {0x10, 0};
  int r = s->read(s->handle, pcm, AUDIO_PCM_FLOATS * sizeof(float), meta, 1);
  if (r == 0 || (uint32_t)r == 0x81950002u || (uint32_t)r == 0x81950004u)
    return 0;
  if (r < 0) return r;
  if (r != (int)(AUDIO_PCM_FLOATS * sizeof(float))) {
    LOG("audio: unsupported PCM block size %d (expected 8192)", r);
    return -1;
  }
  if (audio_rebase_timestamp(s, meta[1], pts_us) < 0) return -1;
  for (size_t i = 0; i < AUDIO_PCM_FLOATS; i++) {
    uint32_t bits;
    memcpy(&bits, &pcm[i], sizeof(bits));
    if ((bits & 0x7f800000u) == 0x7f800000u) pcm[i] = 0;
    else if (pcm[i] > 1.0f) pcm[i] = 1.0f;
    else if (pcm[i] < -1.0f) pcm[i] = -1.0f;
  }
  return AUDIO_FRAME_SAMPLES;
}

typedef struct { int context, instance; } aac_context;
struct audio_encoder {
  void *lib;
  void *ajmi;
  aac_context ctx;
  int initialized, created, flushing;
  int (*encode)(aac_context *, const void *, unsigned, void *, int *, int *);
  int (*flush)(aac_context *, void *, int *, int *);
  int (*destroy)(aac_context *, int *);
  int (*terminate)(aac_context *);
  uint64_t timestamps[16];
  unsigned head, count;
};

size_t audio_adts_payload(const uint8_t *data, size_t len, const uint8_t **payload) {
  if (payload) *payload = NULL;
  if (!data || !payload || len < 7 || data[0] != 0xff ||
      (data[1] & 0xf6) != 0xf0) return 0; /* sync, layer zero */
  size_t header = (data[1] & 1) ? 7 : 9;
  unsigned profile = data[2] >> 6;
  unsigned rate = (data[2] >> 2) & 15;
  unsigned channels = ((data[2] & 1) << 2) | (data[3] >> 6);
  size_t frame = ((size_t)(data[3] & 3) << 11) |
                 ((size_t)data[4] << 3) | (data[5] >> 5);
  if (profile != 1 || rate != 3 || channels != 2 || (data[6] & 3) ||
      frame != len || len <= header) return 0;
  *payload = data + header;
  return len - header;
}

int audio_adts_header(uint8_t out[7], size_t payload_len) {
  if (!out || !payload_len || payload_len > 8191 - 7) return -1;
  size_t n = payload_len + 7;
  out[0] = 0xff; out[1] = 0xf1; out[2] = 0x4c;
  out[3] = (uint8_t)(0x80 | (n >> 11));
  out[4] = (uint8_t)(n >> 3);
  out[5] = (uint8_t)((n << 5) | 0x1f);
  out[6] = 0xfc;
  return 0;
}

size_t audio_encoder_config(uint8_t out[2]) {
  if (!out) return 0;
  out[0] = 0x11; out[1] = 0x90; /* AOT=2, frequencyIndex=3, channels=2 */
  return 2;
}

audio_encoder *audio_encoder_open(unsigned bitrate) {
  if (bitrate < 28000 || bitrate > 320000) return NULL;
  audio_encoder *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->ctx.instance = -1;
  /* The payload dlopen shim does not recursively load imported SPRXs. */
  s->ajmi = load_library("libSceAjmi.sprx");
  if (!s->ajmi) { free(s); return NULL; }
  s->lib = load_library("libSceM4aacEnc.sprx");
  if (!s->lib) { audio_encoder_close(s); return NULL; }
  int (*init)(aac_context *) = dlsym(s->lib, "sceM4aacEncInitialize");
  int (*create)(aac_context *, int, int, int, int, int, int *) =
      dlsym(s->lib, "sceM4aacEncCreateEncoderEx");
  s->encode = dlsym(s->lib, "sceM4aacEncEncodeEx");
  s->flush = dlsym(s->lib, "sceM4aacEncFlushEx");
  s->destroy = dlsym(s->lib, "sceM4aacEncDeleteEncoderEx");
  s->terminate = dlsym(s->lib, "sceM4aacEncTerminate");
  if (!init || !create || !s->encode || !s->flush || !s->destroy || !s->terminate) {
    LOG("audio: required AAC encoder symbols unavailable");
    audio_encoder_close(s);
    return NULL;
  }
  int r = init(&s->ctx);
  LOG("audio: AAC Initialize -> 0x%x", r);
  if (r != 0) { audio_encoder_close(s); return NULL; }
  s->initialized = 1;
  int detail = 0;
  /* float input, ADTS output; parsing the returned header also checks that
   * the firmware's output agrees with our declared AudioSpecificConfig. */
  r = create(&s->ctx, 2, 48000, (int)bitrate, 1, 1, &detail);
  LOG("audio: AAC Create -> 0x%x detail=0x%x", r, detail);
  if (r != 0) { audio_encoder_close(s); return NULL; }
  s->created = 1;
  return s;
}

static int finish_packet(audio_encoder *s, const uint8_t *data, int written,
                         int r, int detail, audio_packet *packet) {
  if (r != 0) { LOG("audio: AAC encode -> 0x%x detail=0x%x", r, detail); return r < 0 ? r : -1; }
  if (!written) return 0;
  if (written < 0 || written > AUDIO_AAC_MAX_BYTES || !s->count) return -1;
  const uint8_t *payload;
  size_t n = audio_adts_payload(data, (size_t)written, &payload);
  if (!n) {
    LOG("audio: invalid native AAC frame (%d bytes, prefix %02x %02x %02x %02x)",
        written, data[0], data[1], data[2], data[3]);
    return -1;
  }
  memcpy(packet->data, payload, n);
  packet->len = n;
  packet->pts_us = s->timestamps[s->head];
  s->head = (s->head + 1) % 16;
  s->count--;
  return 1;
}

int audio_encoder_encode(audio_encoder *s, const float *pcm, uint64_t pts_us,
                         audio_packet *packet) {
  if (!s || !pcm || !packet || s->flushing || s->count == 16) return -1;
  packet->len = 0;
  s->timestamps[(s->head + s->count) % 16] = pts_us;
  s->count++;
  uint8_t out[AUDIO_AAC_MAX_BYTES] = {0};
  int written = 0, detail = 0;
  int r = s->encode(&s->ctx, pcm, AUDIO_PCM_FLOATS * sizeof(float), out, &written, &detail);
  return finish_packet(s, out, written, r, detail, packet);
}

int audio_encoder_flush(audio_encoder *s, audio_packet *packet) {
  if (!s || !packet) return -1;
  packet->len = 0;
  s->flushing = 1;
  if (!s->count) return 0;
  uint8_t out[AUDIO_AAC_MAX_BYTES] = {0};
  int written = 0, detail = 0;
  int r = s->flush(&s->ctx, out, &written, &detail);
  return finish_packet(s, out, written, r, detail, packet);
}

void audio_encoder_close(audio_encoder *s) {
  if (!s) return;
  if (s->created) { int detail = 0; s->destroy(&s->ctx, &detail); }
  if (s->initialized) s->terminate(&s->ctx);
  if (s->lib) dlclose(s->lib);
  if (s->ajmi) dlclose(s->ajmi);
  free(s);
}
