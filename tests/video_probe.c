/* Finite probe of the production backend; only writes its diagnostic outputs. */
#include "avcap.h"
#include "log.h"
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;
int main(void) {
  FILE *log = fopen("/data/ps5hs-video-probe.log", "w");
  if (!log) return 1;
  setvbuf(log, NULL, _IONBF, 0);
  avcap_config cfg = {1920,1080,60,8000,0};
  int r = avcap_load();
  if (!r) r = avcap_init();
  if (!r) r = avcap_open(&cfg);
  fprintf(log, "backend open=0x%x\n", r);
  if (!r) {
    FILE *video = fopen("/data/ps5hs-video-probe.h264", "wb");
    size_t capacity = 4*1024*1024;
    uint8_t *bytes = malloc(capacity);
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    time_t end = now.tv_sec+3;
    unsigned count = 0;
    while (bytes && video) {
      clock_gettime(CLOCK_MONOTONIC, &now); if (now.tv_sec >= end) break;
      avcap_frame frame;
      r = avcap_read_video(bytes, capacity, &frame);
      if (r > 0) {
        fwrite(bytes,1,frame.len,video);
        if (count++ < 3) fprintf(log,"frame bytes=%zu pts_us=%llu dts_us=%llu idr=%d\n",
          frame.len,(unsigned long long)frame.pts_us,(unsigned long long)frame.dts_us,frame.keyframe);
      } else if (!r) usleep(2000);
      else { fprintf(log,"read=0x%x\n",r); break; }
    }
    fprintf(log,"frames=%u\n",count);
    free(bytes); if(video)fclose(video);
  }
  avcap_close(); avcap_terminate();
  pthread_mutex_lock(&g_log_mtx);
  fwrite(g_log_ring,1,g_log_len,log);
  pthread_mutex_unlock(&g_log_mtx);
  fprintf(log,"done\n"); fclose(log); return r<0;
}
