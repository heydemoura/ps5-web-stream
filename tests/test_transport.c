/* Regression tests for stream framing, queue ownership and RTMP parsing. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/rtmp.c"

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

static void test_rtmp_chunks(void) {
  int fd[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
  /* A fragmented extended-timestamp message interrupted by another CSID. */
  uint8_t wire[] = {
    3, 255,255,255, 0,0,5, 20, 1,0,0,0, 1,2,3,4, 'h','e',
    0,6, 0,0,0, 0,0,2, 4, 0,0,0,0, 'O','K', /* csid 70 */
    0xc3, 1,2,3,4, 'l','l',
    0xc3, 1,2,3,4, 'o'
  };
  assert(write_all(fd[0], wire, sizeof(wire)) == 1);
  cs_state cs[IN_STREAMS_MAX] = {{0}};
  uint8_t buf[32]; int type;
  assert(read_msg(fd[1], 2, cs, IN_STREAMS_MAX, buf, sizeof(buf), &type) == 2);
  assert(type == 4 && !memcmp(buf, "OK", 2));
  assert(read_msg(fd[1], 2, cs, IN_STREAMS_MAX, buf, sizeof(buf), &type) == 5);
  assert(type == 20 && !memcmp(buf, "hello", 5));
  assert(cs[0].ts == 0x01020304 && cs[1].id == 70);
  assert(read_msg(fd[1], 0, cs, IN_STREAMS_MAX, buf, sizeof(buf), &type) == -1);
  free_chunks(cs, IN_STREAMS_MAX); close(fd[0]); close(fd[1]);
  char name[64]; double txn;
  uint8_t cmd[20] = {2,0,4,'p','l','a','y',0};
  for (int n = 0; n < 16; n++) assert(parse_cmd(cmd, n, name, sizeof(name), &txn) == -1);
  assert(parse_cmd(cmd, 16, name, sizeof(name), &txn) == 0 && !strcmp(name, "play"));
  uint8_t command[256], *p = command;
  p = amf_str(p, "connect"); p = amf_num(p, 1); *p++ = 3;
  p = amf_key(p, "app"); p = amf_str(p, "live"); p = amf_obj_end(p);
  assert(connect_app_is_live(command, (size_t)(p-command)));
  for (size_t n = 0; n < (size_t)(p-command)-3; n++)
    assert(!connect_app_is_live(command, n));
  p = command; p = amf_str(p, "play"); p = amf_num(p, 0); *p++ = 5;
  p = amf_str(p, "stream");
  assert(play_stream_is_valid(command, (size_t)(p-command)));
  for (size_t n = 0; n < (size_t)(p-command); n++)
    assert(!play_stream_is_valid(command, n));
  p[-1] = 'x'; assert(!play_stream_is_valid(command, (size_t)(p-command)));

}

typedef struct { int fd; uint8_t *body; size_t len; } send_ctx;
static void *send_extended(void *arg) {
  send_ctx *c = arg;
  assert(send_msg(c->fd, 4, 0x12345678, 9, 1, c->body, c->len) == 0);
  return NULL;
}
static void test_rtmp_output(void) {
  int fd[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
  uint8_t *body = malloc(OUT_CHUNK + 7); assert(body);
  memset(body, 0x77, OUT_CHUNK + 7);
  send_ctx ctx = {fd[0], body, OUT_CHUNK + 7};
  pthread_t writer; assert(!pthread_create(&writer, NULL, send_extended, &ctx));
  size_t wire_len = 16 + OUT_CHUNK + 5 + 7;
  uint8_t *wire = malloc(wire_len); assert(wire);
  assert(read_all(fd[1], wire, wire_len) == 1);
  assert(wire[16+OUT_CHUNK] == 0xc4);
  assert(be32(wire+17+OUT_CHUNK) == 0x12345678);
  assert(!memcmp(wire+21+OUT_CHUNK, body, 7));
  pthread_join(writer, NULL); free(wire); free(body); close(fd[0]); close(fd[1]);
}

int main(void) {
  test_codecs(); test_queue(); test_lifetime(); test_rtmp_chunks(); test_rtmp_output();
  puts("transport regressions passed");
  return 0;
}
