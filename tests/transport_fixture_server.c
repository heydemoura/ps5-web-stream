/* Host-only fixture source; this does not emulate or verify PS5 capture. */
#include "flv.h"
#include "http.h"
#include "status.h"
#include "settings.h"
#include "log.h"
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint32_t read24(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint64_t now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  FILE *f = fopen(argv[1], "rb"); assert(f);
  assert(!fseek(f, 0, SEEK_END)); long size = ftell(f); assert(size > 13);
  rewind(f); uint8_t *data = malloc((size_t)size); assert(data);
  assert(fread(data, 1, (size_t)size, f) == (size_t)size); fclose(f);
  assert(!memcmp(data, "FLV", 3));
  broadcaster b; broadcaster_init(&b);
  broadcaster_set_video_params(&b, 320, 180, 30, 500);
  b.have_audio = 1;
  assert(!http_server_start(&b, atoi(argv[2])));
  capture_settings settings; capture_settings_get(&settings);
  capture_settings_begin(&settings); capture_settings_ready(settings.revision);
  uint32_t base = 0;
  uint64_t start = now_ms();
  for (;;) {
    uint32_t last = 0;
    for (size_t pos = 13; pos + 15 <= (size_t)size;) {
      uint8_t *tag = data + pos;
      uint32_t len = read24(tag+1);
      assert(pos + len + 15 <= (size_t)size);
      uint32_t ts = read24(tag+4) | ((uint32_t)tag[7] << 24);
      last = ts;
      while (now_ms() < start + base + ts) usleep(1000);
      if (tag[0] == 9 && len >= 5 && tag[12] == 0)
        broadcaster_set_avc_config(&b, tag+16, len-5);
      else if (tag[0] == 8 && len >= 2 && tag[12] == 0)
        broadcaster_set_aac_config(&b, tag+13, len-2);
      else if (tag[0] == 9 || tag[0] == 8) {
        uint8_t saved[4]; memcpy(saved, tag+4, 4);
        uint32_t stamp = base + ts;
        tag[4] = stamp >> 16; tag[5] = stamp >> 8; tag[6] = stamp; tag[7] = stamp >> 24;
        broadcaster_push(&b, tag, len+15, tag[0] == 9 && (tag[11] >> 4) == 1);
        if (tag[0] == 9) stream_status_video(stamp);
        else stream_status_audio(stamp);
        stream_status_set("streaming", 0);
        memcpy(tag+4, saved, 4);
      }
      pos += len + 15;
    }
    base += last + 34;
  }
}
