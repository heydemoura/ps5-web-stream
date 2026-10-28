/* Copyright (C) 2026 heydemoura */
#include "rtmp.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define OUT_CHUNK   65536
#define MSID        1          /* media stream id we hand out */

/* ---- socket helpers ------------------------------------------------------ */

static int
read_all(int fd, uint8_t *p, size_t n) {
  size_t o = 0;
  while (o < n) {
    ssize_t r = read(fd, p + o, n - o);
    if (r == 0) return 0;
    if (r < 0) { if (errno == EINTR) continue; return -1; }
    o += (size_t)r;
  }
  return 1;
}

static int
write_all(int fd, const uint8_t *p, size_t n) {
  size_t o = 0;
  while (o < n) {
    ssize_t r = write(fd, p + o, n - o);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
    o += (size_t)r;
  }
  return 1;
}

/* ---- AMF0 writers --------------------------------------------------------- */

static uint8_t *amf_num(uint8_t *p, double v) {
  union { double d; uint64_t u; } x; x.d = v;
  *p++ = 0x00;
  for (int i = 7; i >= 0; i--) *p++ = (uint8_t)(x.u >> (i * 8));
  return p;
}
static uint8_t *amf_str(uint8_t *p, const char *s) {
  size_t n = strlen(s); *p++ = 0x02; *p++ = n >> 8; *p++ = n & 0xff;
  memcpy(p, s, n); return p + n;
}
static uint8_t *amf_key(uint8_t *p, const char *s) {
  size_t n = strlen(s); *p++ = n >> 8; *p++ = n & 0xff;
  memcpy(p, s, n); return p + n;
}
static uint8_t *amf_obj_end(uint8_t *p) { *p++ = 0; *p++ = 0; *p++ = 0x09; return p; }

/* ---- RTMP chunk output --------------------------------------------------- */

/* Write one RTMP message with a fmt0 basic+message header, splitting the
 * payload across OUT_CHUNK-sized chunks with fmt3 continuations. */
static int
send_msg(int fd, int csid, uint32_t ts, int type, uint32_t msid,
         const uint8_t *body, size_t len) {
  uint8_t h[20];
  int ext = (ts >= 0xffffff);
  uint8_t *p = h;
  *p++ = (0 << 6) | (csid & 0x3f);          /* fmt 0 */
  uint32_t t = ext ? 0xffffff : ts;
  *p++ = t >> 16; *p++ = t >> 8; *p++ = t;   /* timestamp */
  *p++ = len >> 16; *p++ = len >> 8; *p++ = len; /* message length */
  *p++ = (uint8_t)type;                      /* type id */
  *p++ = msid & 0xff; *p++ = (msid >> 8) & 0xff;
  *p++ = (msid >> 16) & 0xff; *p++ = (msid >> 24) & 0xff; /* LE stream id */
  if (ext) { *p++ = ts >> 24; *p++ = ts >> 16; *p++ = ts >> 8; *p++ = ts; }
  if (write_all(fd, h, (size_t)(p - h)) < 0) return -1;

  size_t off = 0;
  while (off < len) {
    size_t n = len - off;
    if (n > OUT_CHUNK) n = OUT_CHUNK;
    if (write_all(fd, body + off, n) < 0) return -1;
    off += n;
    if (off < len) {
      uint8_t c[4];
      uint8_t *q = c;
      *q++ = (3 << 6) | (csid & 0x3f);        /* fmt 3 continuation */
      if (ext) { *q++ = ts >> 24; *q++ = ts >> 16; *q++ = ts >> 8; } /* (ext ts echoed) */
      if (write_all(fd, c, (size_t)(q - c)) < 0) return -1;
    }
  }
  return 0;
}

static int send_set_chunk(int fd, uint32_t sz) {
  uint8_t b[4] = { sz>>24, sz>>16, sz>>8, sz };
  return send_msg(fd, 2, 0, 1, 0, b, 4);
}
static int send_win_ack(int fd, uint32_t sz) {
  uint8_t b[4] = { sz>>24, sz>>16, sz>>8, sz };
  return send_msg(fd, 2, 0, 5, 0, b, 4);
}
static int send_peer_bw(int fd, uint32_t sz) {
  uint8_t b[5] = { sz>>24, sz>>16, sz>>8, sz, 2 };
  return send_msg(fd, 2, 0, 6, 0, b, 5);
}
static int send_stream_begin(int fd, uint32_t sid) {
  uint8_t b[6] = { 0,0, sid>>24, sid>>16, sid>>8, sid };
  return send_msg(fd, 2, 0, 4, 0, b, 6);
}

/* ---- RTMP chunk input (setup phase) -------------------------------------- */

typedef struct { uint32_t ts, len; int type; uint32_t msid; } cs_state;

/* Read one full RTMP message (reassembling chunks) into buf. Returns message
 * length, 0 on EOF, -1 on error. Fills *type. Handles csid 2..63 (1-byte). */
static int
read_msg(int fd, uint32_t in_chunk, cs_state *cs, int ncs,
         uint8_t *buf, size_t cap, int *type) {
  uint8_t bh;
  int r = read_all(fd, &bh, 1);
  if (r <= 0) return r;
  int fmt = bh >> 6;
  int csid = bh & 0x3f;
  if (csid == 0)  { uint8_t x; if (read_all(fd,&x,1)<=0) return -1; csid = 64 + x; }
  else if (csid == 1) { uint8_t x[2]; if (read_all(fd,x,2)<=0) return -1; csid = 64 + x[0] + (x[1]<<8); }
  if (csid >= ncs) csid = ncs - 1;
  cs_state *st = &cs[csid];

  uint8_t mh[11];
  if (fmt <= 2) { if (read_all(fd, mh, 3) <= 0) return -1;
                  st->ts = (mh[0]<<16)|(mh[1]<<8)|mh[2]; }
  if (fmt <= 1) { if (read_all(fd, mh, 4) <= 0) return -1;
                  st->len = (mh[0]<<16)|(mh[1]<<8)|mh[2]; st->type = mh[3]; }
  if (fmt == 0) { if (read_all(fd, mh, 4) <= 0) return -1;
                  st->msid = mh[0]|(mh[1]<<8)|(mh[2]<<16)|((uint32_t)mh[3]<<24); }
  if (st->ts == 0xffffff) { uint8_t e[4]; if (read_all(fd,e,4)<=0) return -1;
                  st->ts = (e[0]<<24)|(e[1]<<16)|(e[2]<<8)|e[3]; }

  uint32_t len = st->len;
  if (len > cap) return -1;
  uint32_t off = 0;
  while (off < len) {
    uint32_t n = len - off;
    if (n > in_chunk) n = in_chunk;
    if (read_all(fd, buf + off, n) <= 0) return -1;
    off += n;
    if (off < len) {
      uint8_t cbh; if (read_all(fd, &cbh, 1) <= 0) return -1; /* fmt3 */
      int cc = cbh & 0x3f;
      if (cc == 0) { uint8_t x; read_all(fd,&x,1); }
      else if (cc == 1) { uint8_t x[2]; read_all(fd,x,2); }
      if (st->ts == 0xffffff) { uint8_t e[4]; read_all(fd,e,4); }
    }
  }
  *type = st->type;
  return (int)len;
}

/* ---- handshake ----------------------------------------------------------- */

static int
handshake(int fd) {
  uint8_t c0c1[1 + 1536];
  if (read_all(fd, c0c1, sizeof(c0c1)) <= 0) return -1;
  if (c0c1[0] != 3) { LOG("rtmp: bad version %d", c0c1[0]); return -1; }

  uint8_t s0s1s2[1 + 1536 + 1536];
  s0s1s2[0] = 3;
  uint8_t *s1 = s0s1s2 + 1;
  memset(s1, 0, 1536);                 /* time=0, zero=0 */
  for (int i = 8; i < 1536; i++) s1[i] = (uint8_t)(i * 7 + 13);
  memcpy(s0s1s2 + 1 + 1536, c0c1 + 1, 1536);  /* S2 echoes C1 */
  if (write_all(fd, s0s1s2, sizeof(s0s1s2)) < 0) return -1;

  uint8_t c2[1536];
  if (read_all(fd, c2, sizeof(c2)) <= 0) return -1;
  return 0;
}

/* ---- command replies ----------------------------------------------------- */

static int
reply_connect(int fd, double txn) {
  uint8_t b[512]; uint8_t *p = b;
  p = amf_str(p, "_result");
  p = amf_num(p, txn);
  *p++ = 0x03;                                    /* properties object */
  p = amf_key(p, "fmsVer");       p = amf_str(p, "FMS/3,5,7,7009");
  p = amf_key(p, "capabilities"); p = amf_num(p, 31);
  p = amf_obj_end(p);
  *p++ = 0x03;                                    /* information object */
  p = amf_key(p, "level");        p = amf_str(p, "status");
  p = amf_key(p, "code");         p = amf_str(p, "NetConnection.Connect.Success");
  p = amf_key(p, "description");  p = amf_str(p, "Connection succeeded.");
  p = amf_key(p, "objectEncoding"); p = amf_num(p, 0);
  p = amf_obj_end(p);
  return send_msg(fd, 3, 0, 20, 0, b, (size_t)(p - b));
}

static int
reply_create_stream(int fd, double txn) {
  uint8_t b[64]; uint8_t *p = b;
  p = amf_str(p, "_result");
  p = amf_num(p, txn);
  *p++ = 0x05;                      /* null */
  p = amf_num(p, MSID);
  return send_msg(fd, 3, 0, 20, 0, b, (size_t)(p - b));
}

static int
reply_on_status(int fd, const char *code, const char *desc) {
  uint8_t b[256]; uint8_t *p = b;
  p = amf_str(p, "onStatus");
  p = amf_num(p, 0);
  *p++ = 0x05;                      /* null */
  *p++ = 0x03;
  p = amf_key(p, "level");       p = amf_str(p, "status");
  p = amf_key(p, "code");        p = amf_str(p, code);
  p = amf_key(p, "description"); p = amf_str(p, desc);
  p = amf_obj_end(p);
  return send_msg(fd, 5, 0, 20, MSID, b, (size_t)(p - b));
}

/* parse an AMF0 command name + transaction id from a message buffer */
static int
parse_cmd(const uint8_t *m, int len, char *name, int ncap, double *txn) {
  if (len < 1 || m[0] != 0x02) return -1;
  int sl = (m[1] << 8) | m[2];
  if (3 + sl > len || sl >= ncap) return -1;
  memcpy(name, m + 3, sl); name[sl] = 0;
  int o = 3 + sl;
  *txn = 0;
  if (o < len && m[o] == 0x00) {
    union { double d; uint64_t u; } x; x.u = 0;
    for (int i = 0; i < 8; i++) x.u = (x.u << 8) | m[o + 1 + i];
    *txn = x.d;
  }
  return 0;
}

/* ---- input drain thread (post-play) -------------------------------------- */

typedef struct { int fd; volatile int *dead; uint32_t in_chunk; } drain_ctx;

static void *
drain_thread(void *arg) {
  drain_ctx *d = arg;
  cs_state cs[64]; memset(cs, 0, sizeof(cs));
  static __thread uint8_t buf[16384];
  int type;
  while (!*d->dead) {
    int r = read_msg(d->fd, d->in_chunk, cs, 64, buf, sizeof(buf), &type);
    if (r <= 0) { *d->dead = 1; break; }
    if (type == 1 && r >= 4)           /* peer changed its chunk size */
      d->in_chunk = (buf[0]<<24)|(buf[1]<<16)|(buf[2]<<8)|buf[3];
  }
  return NULL;
}

/* ---- the FLV-to-RTMP media pump ------------------------------------------ */

static void
pump(int fd, broadcaster *b, volatile int *dead) {
  bc_client *c = broadcaster_add_client(b, fd, 0 /* no FLV file header */);
  if (!c) { LOG("rtmp: broadcaster full"); return; }

  uint8_t hdr[11], *body = malloc(2 * 1024 * 1024);
  uint8_t trailer[4];
  if (!body) { broadcaster_remove_client(b, c); broadcaster_client_free(c); return; }

  while (!*dead) {
    if (!broadcaster_client_read_exact(c, hdr, 11)) break;
    int type = hdr[0];
    uint32_t len = (hdr[1]<<16)|(hdr[2]<<8)|hdr[3];
    uint32_t ts  = (hdr[4]<<16)|(hdr[5]<<8)|hdr[6] | ((uint32_t)hdr[7] << 24);
    if (len > 2 * 1024 * 1024) break;
    if (!broadcaster_client_read_exact(c, body, len)) break;
    if (!broadcaster_client_read_exact(c, trailer, 4)) break;  /* PrevTagSize */

    int csid = (type == 8) ? 6 : (type == 9) ? 4 : 5;
    if (send_msg(fd, csid, ts, type, MSID, body, len) < 0) { *dead = 1; break; }
  }
  free(body);
  broadcaster_remove_client(b, c);
  broadcaster_client_free(c);
}

/* ---- connection lifecycle ------------------------------------------------ */

void
rtmp_serve(int fd, broadcaster *b) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  if (handshake(fd) < 0) { LOG("rtmp: handshake failed"); close(fd); return; }
  LOG("rtmp: handshake ok");

  cs_state cs[64]; memset(cs, 0, sizeof(cs));
  uint32_t in_chunk = 128;
  uint8_t msg[8192];
  int type;
  int played = 0;

  while (!played) {
    int r = read_msg(fd, in_chunk, cs, 64, msg, sizeof(msg), &type);
    if (r <= 0) { close(fd); return; }
    if (type == 1 && r >= 4) {
      in_chunk = (msg[0]<<24)|(msg[1]<<16)|(msg[2]<<8)|msg[3];
      continue;
    }
    if (type != 20 && type != 17) continue;   /* AMF0 / AMF3 command */
    const uint8_t *m = msg; int ml = r;
    if (type == 17) { m++; ml--; }             /* AMF3: skip leading byte */
    char name[64]; double txn;
    if (parse_cmd(m, ml, name, sizeof(name), &txn) < 0) continue;
    LOG("rtmp: cmd '%s' txn=%.0f", name, txn);

    if (!strcmp(name, "connect")) {
      send_win_ack(fd, 5000000);
      send_peer_bw(fd, 5000000);
      send_set_chunk(fd, OUT_CHUNK);
      reply_connect(fd, txn);
    } else if (!strcmp(name, "createStream")) {
      reply_create_stream(fd, txn);
    } else if (!strcmp(name, "play")) {
      send_stream_begin(fd, MSID);
      reply_on_status(fd, "NetStream.Play.Reset", "Playing and resetting stream.");
      reply_on_status(fd, "NetStream.Play.Start", "Started playing stream.");
      played = 1;
    } else if (!strcmp(name, "FCUnpublish") || !strcmp(name, "deleteStream") ||
               !strcmp(name, "closeStream")) {
      close(fd); return;
    }
  }

  LOG("rtmp: play started");
  volatile int dead = 0;
  drain_ctx dc = { fd, &dead, in_chunk };
  pthread_t rt;
  pthread_create(&rt, NULL, drain_thread, &dc);

  pump(fd, b, &dead);

  dead = 1;
  shutdown(fd, SHUT_RDWR);
  pthread_join(rt, NULL);
  close(fd);
  LOG("rtmp: client closed");
}

typedef struct { broadcaster *b; int port; } listen_ctx;
typedef struct { int fd; broadcaster *b; } conn_ctx;

static void *conn_thread(void *arg) {
  conn_ctx *c = arg;
  rtmp_serve(c->fd, c->b);
  free(c);
  return NULL;
}

static void *listen_thread(void *arg) {
  listen_ctx *lc = arg;
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(lc->port);
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { LOG("rtmp: bind %d failed", lc->port); return NULL; }
  listen(srv, 4);
  LOG("rtmp: listening on :%d", lc->port);
  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) continue;
    conn_ctx *cc = malloc(sizeof(*cc));
    cc->fd = fd; cc->b = lc->b;
    pthread_t t; pthread_create(&t, NULL, conn_thread, cc); pthread_detach(t);
  }
  return NULL;
}

int
rtmp_server_start(broadcaster *b, int port) {
  listen_ctx *lc = malloc(sizeof(*lc));
  lc->b = b; lc->port = port;
  pthread_t t;
  if (pthread_create(&t, NULL, listen_thread, lc) != 0) return -1;
  pthread_detach(t);
  return 0;
}
