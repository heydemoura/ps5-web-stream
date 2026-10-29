/* Short-lived on-console probe. Emits only /data/ps5hs-audio-probe.{log,aac}.
 * Holds a video session to acquire the same resource as production audio. */
#include "audio.h"
#include "avcap.h"
#include "log.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int write_packet(FILE *f, const audio_packet *p) {
  uint8_t adts[7];
  if (audio_adts_header(adts, p->len) != 0) return -1;
  return fwrite(adts, 1, 7, f) == 7 &&
         fwrite(p->data, 1, p->len, f) == p->len ? 0 : -1;
}

int main(void) {
  FILE *log = fopen("/data/ps5hs-audio-probe.log", "w");
  FILE *aac = fopen("/data/ps5hs-audio-probe.aac", "wb");
  if (!log || !aac) return 1;
  LOG("audio probe start");
  void *ipmi = dlopen("/system/common/lib/libSceIpmi.sprx", RTLD_LAZY | RTLD_GLOBAL);
  void *ajmi = dlopen("/system/common/lib/libSceAjmi.sprx", RTLD_LAZY | RTLD_GLOBAL);
  LOG("dependencies: ipmi=%p ajmi=%p", ipmi, ajmi);
  audio_capture *capture = NULL;
  audio_encoder *encoder = NULL;
  int result = 1;
  unsigned reads = 0, packets = 0, empty = 0;
  int r = avcap_load();
  if (!r) r = avcap_init();
  LOG("avcap Initialize -> 0x%x", r);
  if (r) goto done;
  avcap_config cfg = {1920,1080,60,8000,0};
  if (avcap_open(&cfg)) goto done;
  capture = audio_capture_open(avcap_resource_id());
  encoder = audio_encoder_open(128000);
  if (!capture || !encoder) goto done;
  uint64_t begin = now_us(), first_pts = 0, last_pts = 0;
  while (now_us() - begin < 3000000) {
    float pcm[AUDIO_PCM_FLOATS];
    uint64_t pts;
    r = audio_capture_read(capture, pcm, &pts);
    if (r < 0) { LOG("capture read -> 0x%x", r); goto done; }
    if (!r) { empty++; usleep(2000); continue; }
    if (!reads) first_pts = pts;
    last_pts = pts;
    reads++;
    audio_packet packet;
    r = audio_encoder_encode(encoder, pcm, pts, &packet);
    if (r < 0) goto done;
    if (r > 0) {
      if (write_packet(aac, &packet) != 0) goto done;
      packets++;
      if (packets <= 3) LOG("AAC packet %u: %zu bytes pts=%llu", packets,
          packet.len, (unsigned long long)packet.pts_us);
    }
  }
  for (unsigned i = 0; i < 16; i++) {
    audio_packet packet;
    int flushed = audio_encoder_flush(encoder, &packet);
    if (flushed <= 0) break;
    if (write_packet(aac, &packet) != 0) goto done;
    packets++;
  }
  LOG("captured=%u encoded=%u empty=%u timestamp range=%llu..%llu", reads,
      packets, empty, (unsigned long long)first_pts, (unsigned long long)last_pts);
  result = packets > 0 ? 0 : 1;
done:
  audio_capture_close(capture);
  audio_encoder_close(encoder);
  avcap_terminate();
  LOG("audio probe exit=%d", result);
  fwrite(g_log_ring, 1, g_log_len, log);
  fclose(log);
  fclose(aac);
  return result;
}
