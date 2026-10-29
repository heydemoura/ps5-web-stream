#include "http.h"
#include "status.h"
#include "log.h"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  int port = atoi(argv[1]);
  broadcaster b;
  broadcaster_init(&b);
  broadcaster_set_video_params(&b, 1920, 1080, 60, 8000);
  stream_status_set("error", (int)0x81950008);
  assert(http_server_start(&b, 0) == -1);
  assert(http_server_start(&b, 65536) == -1);
  assert(http_server_start(&b, port) == 0);
  assert(http_server_start(&b, port) == -1);
  LOG("http fixture ready");
  for (;;) pause();
}
