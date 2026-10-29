/* Regression tests for stream framing, queue ownership and capture switches. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "flv.h"
#include "log.h"
static uint32_t be24(const uint8_t *p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|be24(p+1); }

char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

static const uint8_t sps[] = {0x67, 0x64, 0, 0x28, 0xaa};
static const uint8_t pps[] = {0x68, 0xee};

static void configure(broadcaster *b) {
  uint8_t cfg[64];
  size_t n = avc_decoder_config(cfg, sizeof(cfg), sps, sizeof(sps), pps, sizeof(pps));
  assert(n);
  broadcaster_set_avc_config(b, cfg, n);
  const uint8_t aac[] = {0x11, 0x90};
  broadcaster_set_aac_config(b, aac, sizeof(aac));
}

static void test_codecs(void) {
  uint8_t in[256], out[320];
  for (size_t i = 0; i < sizeof(in); i += 4) {
    in[i] = in[i+1] = 0; in[i+2] = 1; in[i+3] = 0x65;
  }
  assert(annexb_to_avcc(in, sizeof(in), out, sizeof(out), NULL, NULL, NULL, NULL) == 320);
  memset(out, 0xa5, sizeof(out));
  assert(annexb_to_avcc(in, sizeof(in), out, 256, NULL, NULL, NULL, NULL) == 0);
  for (int i = 256; i < 320; i++) assert(out[i] == 0xa5);
  uint8_t param[] = {0,0,0,1,0x67,0x64,0,0x28,0xaa,0,0,1,0x68,0xee,0,0};
  const uint8_t *ps = NULL;
  assert(annexb_to_avcc(param, sizeof(param), out, sizeof(out), &ps, NULL, NULL, NULL) == 15);
  assert(ps == param + 4);
  assert(avc_decoder_config(out, sizeof(out), sps, 3, pps, 2) == 0);
  assert(avc_decoder_config(out, 17, sps, 5, pps, 2) == 0);
  assert(avc_decoder_config(out, 18, sps, 5, pps, 2) == 18);
  uint8_t *tag = (uint8_t *)1;
  assert(flv_video_tag(&tag, in, SIZE_MAX, 1, 1, 0, 0) == 0 && !tag);
  assert(flv_audio_tag(&tag, in, SIZE_MAX, 1, 0) == 0 && !tag);
  size_t n = flv_video_tag(&tag, in, 3, 0, 1, 0x12345678, -25);
  assert(n == 23 && tag[7] == 0x12 && be24(tag+4) == 0x345678);
  assert(be24(tag+13) == 0xffffe7 && be32(tag+n-4) == n-4);
  free(tag);
}

static uint8_t *read_tag(bc_client *c, size_t *size) {
  uint8_t h[11];
  assert(broadcaster_client_read_exact(c, h, sizeof(h)));
  size_t n = be24(h+1) + 15;
  assert(n < 7*1024*1024);
  uint8_t *p = malloc(n); assert(p);
  memcpy(p, h, sizeof(h));
  assert(broadcaster_client_read_exact(c, p+11, n-11));
  assert(be32(p+n-4) == n-4);
  *size = n;
  return p;
}

static void consume_headers(bc_client *c) {
  size_t n;
  uint8_t *p = read_tag(c, &n); assert(p[0] == 18); free(p);
  p = read_tag(c, &n); assert(p[0] == 9 && p[12] == 0); free(p);
  p = read_tag(c, &n); assert(p[0] == 8 && p[12] == 0); free(p);
}

static void test_queue(void) {
  broadcaster b; broadcaster_init(&b);
  b.have_audio = 1;
  int fd[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
  bc_client *c = broadcaster_add_client(&b, fd[0], 1); assert(c);
  /* A viewer joining before the encoder config must still get all headers. */
  configure(&b);
  uint8_t hdr[13];
  assert(broadcaster_client_read_exact(c, hdr, sizeof(hdr)));
  assert(!memcmp(hdr, "FLV", 3) && hdr[4] == 5);
  size_t payload_len = 1024 * 1024;
  uint8_t *payload = malloc(payload_len); assert(payload);
  memset(payload, 0x55, payload_len);
  uint8_t *tag; size_t len = flv_video_tag(&tag, payload, payload_len, 1, 1, 100, 0);
  broadcaster_push(&b, tag, len, 1);
  consume_headers(c);
  /* Consume a partial frame, then overflow and replace pending frames. */
  uint8_t first[31];
  assert(broadcaster_client_read_exact(c, first, sizeof(first)));
  assert(!memcmp(first, tag, sizeof(first)));
  for (int i = 0; i < 20; i++) broadcaster_push(&b, tag, len, 0);
  broadcaster_push(&b, tag, len, 1);
  uint8_t *rest = malloc(len); assert(rest);
  assert(broadcaster_client_read_exact(c, rest, len-sizeof(first)));
  assert(!memcmp(rest, tag+sizeof(first), len-sizeof(first)));
  consume_headers(c);
  size_t got;
  uint8_t *p = read_tag(c, &got);
  assert(got == len && !memcmp(p, tag, len)); free(p);
  /* Changed encoder config is sent before the next random-access frame. */
  broadcaster_reset(&b); configure(&b);
  broadcaster_push(&b, tag, len, 1); consume_headers(c);
  p = read_tag(c, &got); assert(got == len); free(p);
  free(rest); free(tag); free(payload);
  close(fd[1]);
  assert(!broadcaster_client_read_exact(c, first, 1));
  broadcaster_remove_client(&b, c); broadcaster_client_free(c); close(fd[0]);
  pthread_mutex_destroy(&b.mtx);
}

typedef struct { broadcaster *b; uint8_t *tag; size_t len; } push_ctx;
static void *push_many(void *arg) {
  push_ctx *c = arg;
  for (int i = 0; i < 20000; i++) broadcaster_push(c->b, c->tag, c->len, 1);
  return NULL;
}
static void test_lifetime(void) {
  broadcaster b; broadcaster_init(&b); configure(&b);
  uint8_t data[4] = {0, 0, 0, 1}, *tag;
  size_t len = flv_video_tag(&tag, data, sizeof(data), 1, 1, 0, 0);
  push_ctx ctx = {&b, tag, len};
  pthread_t producer; assert(!pthread_create(&producer, NULL, push_many, &ctx));
  for (int i = 0; i < 1000; i++) {
    int fd[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    bc_client *c = broadcaster_add_client(&b, fd[0], 1); assert(c);
    broadcaster_remove_client(&b, c); broadcaster_client_free(c);
    close(fd[0]); close(fd[1]);
  }
  pthread_join(producer, NULL); free(tag); pthread_mutex_destroy(&b.mtx);
}

static void test_switch_disconnect(void) {
  broadcaster b; broadcaster_init(&b);
  int fd[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
  bc_client *c = broadcaster_add_client(&b, fd[0], 1); assert(c);
  broadcaster_disconnect_clients(&b);
  uint8_t byte;
  assert(!broadcaster_client_read_exact(c, &byte, 1));
  assert(read(fd[1], &byte, 1) == 0);
  broadcaster_remove_client(&b, c); broadcaster_client_free(c);
  close(fd[0]); close(fd[1]); pthread_mutex_destroy(&b.mtx);
}

int main(void) {
  test_codecs(); test_queue(); test_lifetime(); test_switch_disconnect();
  puts("transport regressions passed");
  return 0;
}
