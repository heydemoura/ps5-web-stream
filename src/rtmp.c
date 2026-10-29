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
#include <sys/time.h>
#include <stdatomic.h>

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
#ifdef MSG_NOSIGNAL
    ssize_t r = send(fd, p + o, n - o, MSG_NOSIGNAL);
#else
    ssize_t r = send(fd, p + o, n - o, 0);
#endif
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
  if (len > 0xffffff || csid < 2 || csid > 63) return -1;
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
      uint8_t c[5];
      uint8_t *q = c;
      *q++ = (3 << 6) | (csid & 0x3f);        /* fmt 3 continuation */
      if (ext) { *q++ = ts >> 24; *q++ = ts >> 16; *q++ = ts >> 8; *q++ = ts; }
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

#define IN_MESSAGE_MAX 16384
#define IN_STREAMS_MAX 64

typedef struct {
  uint32_t id, ts, delta, len, msid, received;
  int type, initialized, extended;
  uint8_t *data;
} cs_state;

static uint32_t be24(const uint8_t *p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
static uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | be24(p + 1);
}

static void free_chunks(cs_state *cs, int ncs) {
  for (int i = 0; i < ncs; i++) free(cs[i].data);
}

/* Each chunk-stream ID has separate reassembly state. Control messages may
 * arrive between fragments of a command; never interpret them as payload. */
static int
read_msg(int fd, uint32_t in_chunk, cs_state *cs, int ncs,
         uint8_t *buf, size_t cap, int *type) {
  if (!in_chunk || (in_chunk & 0x80000000)) return -1;
  for (;;) {
    uint8_t bh, h[11];
    int r = read_all(fd, &bh, 1);
    if (r <= 0) return r;
    int fmt = bh >> 6;
    uint32_t id = bh & 63;
    if (id == 0) {
      if (read_all(fd, h, 1) <= 0) return -1;
      id = 64 + h[0];
    } else if (id == 1) {
      if (read_all(fd, h, 2) <= 0) return -1;
      id = 64 + h[0] + ((uint32_t)h[1] << 8);
    }
    cs_state *st = NULL, *unused = NULL;
    for (int i = 0; i < ncs; i++) {
      if (cs[i].initialized && cs[i].id == id) { st = &cs[i]; break; }
      if (!cs[i].initialized && !unused) unused = &cs[i];
    }
    if (!st) {
      if (fmt != 0 || !unused) return -1;
      st = unused; st->id = id;
    }
    int continuing = st->received != 0;
    if (continuing && fmt != 3) return -1;
    if (fmt != 3) {
      int nh = fmt == 0 ? 11 : fmt == 1 ? 7 : 3;
      if (read_all(fd, h, (size_t)nh) <= 0) return -1;
      uint32_t stamp = be24(h);
      st->extended = stamp == 0xffffff;
      if (fmt <= 1) { st->len = be24(h + 3); st->type = h[6]; }
      if (fmt == 0)
        st->msid = h[7] | ((uint32_t)h[8] << 8) |
                   ((uint32_t)h[9] << 16) | ((uint32_t)h[10] << 24);
      if (st->extended) {
        if (read_all(fd, h, 4) <= 0) return -1;
        stamp = be32(h);
      }
      st->delta = stamp;
      if (fmt == 0) st->ts = stamp;
      else st->ts += stamp;
    } else {
      if (st->extended && read_all(fd, h, 4) <= 0) return -1;
      if (!continuing) st->ts += st->delta;
    }
    if (!st->len || st->len > cap || st->len > IN_MESSAGE_MAX) return -1;
    if (!st->data) {
      st->data = malloc(IN_MESSAGE_MAX);
      if (!st->data) return -1;
    }
    st->initialized = 1;
    uint32_t take = st->len - st->received;
    if (take > in_chunk) take = in_chunk;
    if (read_all(fd, st->data + st->received, take) <= 0) return -1;
    st->received += take;
    if (st->received == st->len) {
      memcpy(buf, st->data, st->len);
      st->received = 0;
      *type = st->type;
      return (int)st->len;
    }
  }
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
  if (len < 3 || m[0] != 0x02) return -1;
  int sl = (m[1] << 8) | m[2];
  if (3 + sl > len || sl >= ncap) return -1;
  memcpy(name, m + 3, sl); name[sl] = 0;
  int o = 3 + sl;
  *txn = 0;
  if (o + 9 > len || m[o] != 0x00) return -1;
  {
    union { double d; uint64_t u; } x; x.u = 0;
    for (int i = 0; i < 8; i++) x.u = (x.u << 8) | m[o + 1 + i];
    *txn = x.d;
  }
  return 0;
}

/* Bounded AMF0 traversal for the documented /live/stream endpoint. */
static const uint8_t *amf_skip(const uint8_t *p, const uint8_t *end, int depth) {
  if (p >= end || depth > 16) return NULL;
  unsigned type = *p++;
  size_t n;
  switch (type) {
    case 0: n = 8; break;
    case 1: n = 1; break;
    case 5: case 6: return p;
    case 2:
      if (end - p < 2) return NULL;
      n = ((size_t)p[0] << 8) | p[1]; p += 2; break;
    case 12:
      if (end - p < 4) return NULL;
      n = be32(p); p += 4; break;
    case 11: n = 10; break;
    case 8:
      if (end - p < 4) return NULL;
      p += 4;
      /* fall through */
    case 3:
      for (;;) {
        if (end - p < 3) return NULL;
        n = ((size_t)p[0] << 8) | p[1]; p += 2;
        if (!n && *p == 9) return p + 1;
        if ((size_t)(end - p) < n) return NULL;
        p = amf_skip(p + n, end, depth + 1);
        if (!p) return NULL;
      }
    case 10: {
      if (end - p < 4) return NULL;
      uint32_t count = be32(p); p += 4;
      if (count > (size_t)(end - p)) return NULL;
      while (count--) { p = amf_skip(p, end, depth + 1); if (!p) return NULL; }
      return p;
    }
    default: return NULL;
  }
  return (size_t)(end - p) < n ? NULL : p + n;
}

static int amf_string_is(const uint8_t *p, const uint8_t *end, const char *value) {
  size_t n = strlen(value);
  return p && end - p >= 3 && p[0] == 2 &&
    (((size_t)p[1] << 8) | p[2]) == n && (size_t)(end - p) >= n + 3 &&
    memcmp(p + 3, value, n) == 0;
}

static int connect_app_is_live(const uint8_t *m, size_t n) {
  const uint8_t *end = m + n;
  const uint8_t *p = amf_skip(m, end, 0);
  if (p) p = amf_skip(p, end, 0); /* transaction number */
  if (!p || p == end || *p++ != 3) return 0;
  while (end - p >= 3) {
    size_t len = ((size_t)p[0] << 8) | p[1]; p += 2;
    if (!len && *p == 9) return 0;
    if ((size_t)(end - p) < len) return 0;
    int app = len == 3 && !memcmp(p, "app", 3);
    p += len;
    if (app) return amf_string_is(p, end, "live");
    p = amf_skip(p, end, 0);
    if (!p) return 0;
  }
  return 0;
}

static int play_stream_is_valid(const uint8_t *m, size_t n) {
  const uint8_t *end = m + n;
  const uint8_t *p = m;
  for (int i = 0; i < 3 && p; i++) p = amf_skip(p, end, 0);
  return amf_string_is(p, end, "stream");
}

/* ---- input drain and media pump ----------------------------------------- */

typedef struct {
  int fd;
  atomic_int dead;
  uint32_t in_chunk;
  cs_state *cs;
  pthread_mutex_t write_mtx;
} session;

static int control(session *s, int type, const uint8_t *buf, int n) {
  if (type == 1) {
    if (n != 4 || !be32(buf) || (be32(buf) & 0x80000000)) return -1;
    s->in_chunk = be32(buf);
  } else if (type == 2) {
    if (n != 4) return -1;
    for (int i = 0; i < IN_STREAMS_MAX; i++)
      if (s->cs[i].initialized && s->cs[i].id == be32(buf)) s->cs[i].received = 0;
  } else if (type == 4 && n >= 2 && buf[0] == 0 && buf[1] == 6) {
    if (n != 6) return -1;
    uint8_t reply[6] = {0, 7, buf[2], buf[3], buf[4], buf[5]};
    pthread_mutex_lock(&s->write_mtx);
    int r = send_msg(s->fd, 2, 0, 4, 0, reply, sizeof(reply));
    pthread_mutex_unlock(&s->write_mtx);
    return r;
  }
  return 0;
}

static void *drain_thread(void *arg) {
  session *s = arg;
  uint8_t buf[IN_MESSAGE_MAX];
  while (!atomic_load(&s->dead)) {
    int type;
    int n = read_msg(s->fd, s->in_chunk, s->cs, IN_STREAMS_MAX,
                     buf, sizeof(buf), &type);
    if (n <= 0 || control(s, type, buf, n) < 0) break;
    if (type == 20 || type == 17) {
      const uint8_t *m = buf; int len = n;
      if (type == 17) { m++; len--; }
      char name[64]; double txn;
      if (parse_cmd(m, len, name, sizeof(name), &txn) < 0) break;
      if (!strcmp(name, "deleteStream") || !strcmp(name, "closeStream")) break;
    }
  }
  atomic_store(&s->dead, 1);
  /* Wake a media writer and broadcaster's idle EOF detector. */
  shutdown(s->fd, SHUT_RDWR);
  return NULL;
}

static void pump(session *s, broadcaster *b) {
  bc_client *c = broadcaster_add_client(b, s->fd, 0);
  if (!c) { LOG("rtmp: broadcaster full"); return; }
  uint8_t hdr[11], trailer[4], *body = NULL;
  size_t capacity = 0;
  while (!atomic_load(&s->dead)) {
    if (!broadcaster_client_read_exact(c, hdr, sizeof(hdr))) break;
    int type = hdr[0];
    uint32_t len = be24(hdr + 1);
    uint32_t ts = be24(hdr + 4) | ((uint32_t)hdr[7] << 24);
    if ((type != 8 && type != 9 && type != 18) || len > 6 * 1024 * 1024) break;
    if (len > capacity) {
      uint8_t *next = realloc(body, len);
      if (!next) break;
      body = next; capacity = len;
    }
    if (!broadcaster_client_read_exact(c, body, len) ||
        !broadcaster_client_read_exact(c, trailer, sizeof(trailer))) break;
    if (be32(trailer) != len + 11) break;
    int csid = type == 8 ? 6 : type == 9 ? 4 : 5;
    pthread_mutex_lock(&s->write_mtx);
    int r = send_msg(s->fd, csid, ts, type, MSID, body, len);
    pthread_mutex_unlock(&s->write_mtx);
    if (r < 0) break;
  }
  free(body);
  broadcaster_remove_client(b, c);
  broadcaster_client_free(c);
}

/* ---- connection lifecycle ------------------------------------------------ */

void rtmp_serve(int fd, broadcaster *b) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  struct timeval timeout = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  timeout.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (handshake(fd) < 0) { close(fd); return; }
  cs_state cs[IN_STREAMS_MAX]; memset(cs, 0, sizeof(cs));
  session s = {.fd = fd, .in_chunk = 128, .cs = cs};
  atomic_init(&s.dead, 0);
  pthread_mutex_init(&s.write_mtx, NULL);
  uint8_t msg[IN_MESSAGE_MAX];
  int played = 0, connected = 0, created = 0;
  while (!played) {
    int type;
    int n = read_msg(fd, s.in_chunk, cs, IN_STREAMS_MAX, msg, sizeof(msg), &type);
    if (n <= 0 || control(&s, type, msg, n) < 0) goto done;
    if (type != 20 && type != 17) continue;
    const uint8_t *m = msg; int ml = n;
    if (type == 17) { m++; ml--; }
    char name[64]; double txn;
    if (parse_cmd(m, ml, name, sizeof(name), &txn) < 0) goto done;
    LOG("rtmp: cmd '%s' txn=%.0f", name, txn);
    if (!strcmp(name, "connect")) {
      if (!connect_app_is_live(m, (size_t)ml)) goto done;
      if (connected || send_win_ack(fd, 5000000) < 0 ||
          send_peer_bw(fd, 5000000) < 0 || send_set_chunk(fd, OUT_CHUNK) < 0 ||
          reply_connect(fd, txn) < 0) goto done;
      connected = 1;
    } else if (!strcmp(name, "createStream")) {
      if (!connected || reply_create_stream(fd, txn) < 0) goto done;
      created = 1;
    } else if (!strcmp(name, "play")) {
      if (!play_stream_is_valid(m, (size_t)ml)) {
        reply_on_status(fd, "NetStream.Play.StreamNotFound", "Use /live/stream.");
        goto done;
      }
      if (!created || send_stream_begin(fd, MSID) < 0 ||
          reply_on_status(fd, "NetStream.Play.Reset", "Playing and resetting stream.") < 0 ||
          reply_on_status(fd, "NetStream.Play.Start", "Started playing stream.") < 0) goto done;
      played = 1;
    } else if (!strcmp(name, "FCUnpublish") || !strcmp(name, "deleteStream") ||
               !strcmp(name, "closeStream")) goto done;
  }
  timeout.tv_sec = timeout.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  pthread_t reader;
  if (pthread_create(&reader, NULL, drain_thread, &s) != 0) goto done;
  pump(&s, b);
  atomic_store(&s.dead, 1);
  shutdown(fd, SHUT_RDWR);
  pthread_join(reader, NULL);
done:
  free_chunks(cs, IN_STREAMS_MAX);
  pthread_mutex_destroy(&s.write_mtx);
  close(fd);
}

#define RTMP_MAX_CONNECTIONS 24
typedef struct {
  broadcaster *b;
  int fd, active;
  pthread_mutex_t mtx;
} listen_ctx;
typedef struct { int fd; listen_ctx *listener; } conn_ctx;

static void *conn_thread(void *arg) {
  conn_ctx *c = arg;
  rtmp_serve(c->fd, c->listener->b);
  pthread_mutex_lock(&c->listener->mtx);
  c->listener->active--;
  pthread_mutex_unlock(&c->listener->mtx);
  free(c);
  return NULL;
}

static void *listen_thread(void *arg) {
  listen_ctx *lc = arg;
  int srv = lc->fd;
  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      usleep(100000);
      continue;
    }
    pthread_mutex_lock(&lc->mtx);
    int full = lc->active >= RTMP_MAX_CONNECTIONS;
    if (!full) lc->active++;
    pthread_mutex_unlock(&lc->mtx);
    if (full) { close(fd); continue; }
    conn_ctx *cc = malloc(sizeof(*cc));
    if (cc) { cc->fd = fd; cc->listener = lc; }
    pthread_t thread;
    if (!cc || pthread_create(&thread, NULL, conn_thread, cc) != 0) {
      close(fd); free(cc);
      pthread_mutex_lock(&lc->mtx);
      lc->active--;
      pthread_mutex_unlock(&lc->mtx);
      continue;
    }
    pthread_detach(thread);
  }
  close(srv);
  return NULL;
}

int rtmp_server_start(broadcaster *b, int port) {
  if (!b || port < 1 || port > 65535) return -1;
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0) return -1;
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(port);
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(srv, 8) < 0) {
    LOG("rtmp: listen on :%d failed: %s", port, strerror(errno));
    close(srv); return -1;
  }
  listen_ctx *lc = calloc(1, sizeof(*lc));
  if (!lc) { close(srv); return -1; }
  lc->b = b; lc->fd = srv;
  if (pthread_mutex_init(&lc->mtx, NULL) != 0) {
    close(srv); free(lc); return -1;
  }
  pthread_t thread;
  if (pthread_create(&thread, NULL, listen_thread, lc) != 0) {
    pthread_mutex_destroy(&lc->mtx); close(srv); free(lc); return -1;
  }
  pthread_detach(thread);
  LOG("rtmp: listening on :%d", port);
  return 0;
}
