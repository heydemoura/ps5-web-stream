/* Copyright (C) 2026 heydemoura */
#include "flv.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

/* ---- byte writers -------------------------------------------------------- */

static void w8(uint8_t **p, uint32_t v)  { *(*p)++ = v & 0xff; }
static void w16(uint8_t **p, uint32_t v) { w8(p, v >> 8); w8(p, v); }
static void w24(uint8_t **p, uint32_t v) { w8(p, v >> 16); w8(p, v >> 8); w8(p, v); }
static void w32(uint8_t **p, uint32_t v) { w8(p, v >> 24); w8(p, v >> 16); w8(p, v >> 8); w8(p, v); }
static void wd(uint8_t **p, double v) {
  union { double d; uint64_t u; } x; x.d = v;
  for (int i = 7; i >= 0; i--) w8(p, (uint32_t)(x.u >> (i * 8)));
}

size_t
flv_file_header(uint8_t *out, int have_audio) {
  uint8_t *p = out;
  w8(&p, 'F'); w8(&p, 'L'); w8(&p, 'V');
  w8(&p, 1);                          /* version */
  w8(&p, (have_audio ? 0x04 : 0x00) | 0x01); /* flags: audio|video */
  w32(&p, 9);                         /* header size */
  w32(&p, 0);                         /* PrevTagSize0 */
  return (size_t)(p - out);
}

/* amf helpers */
static void amf_str(uint8_t **p, const char *s) {
  size_t n = strlen(s);
  w16(p, (uint32_t)n);
  memcpy(*p, s, n); *p += n;
}
static void amf_named_num(uint8_t **p, const char *k, double v) {
  amf_str(p, k); w8(p, 0x00); wd(p, v);
}
static void amf_named_bool(uint8_t **p, const char *k, int v) {
  amf_str(p, k); w8(p, 0x01); w8(p, v ? 1 : 0);
}

static size_t
wrap_tag(uint8_t **out, int tag_type, const uint8_t *body, size_t body_len,
         uint32_t ts_ms) {
  if (!out) return 0;
  *out = NULL;
  if (!body || body_len > 0xffffff) return 0;
  size_t total = 11 + body_len + 4;
  uint8_t *buf = malloc(total);
  if (!buf) { *out = NULL; return 0; }
  uint8_t *p = buf;
  w8(&p, tag_type);
  w24(&p, (uint32_t)body_len);
  w24(&p, ts_ms & 0xffffff);
  w8(&p, (ts_ms >> 24) & 0xff);       /* timestamp extended */
  w24(&p, 0);                         /* stream id */
  memcpy(p, body, body_len); p += body_len;
  w32(&p, (uint32_t)(11 + body_len)); /* PrevTagSize */
  *out = buf;
  return total;
}

size_t
flv_metadata_tag(uint8_t *out, int width, int height, double fps,
                 int video_kbps, int have_audio) {
  uint8_t body[512];
  uint8_t *p = body;
  w8(&p, 0x02); amf_str(&p, "onMetaData");  /* SCRIPTDATASTRING */
  w8(&p, 0x08);                              /* ECMA array */
  uint8_t *count = p; w32(&p, 0);
  int n = 0;
  amf_named_num(&p, "width", width); n++;
  amf_named_num(&p, "height", height); n++;
  amf_named_num(&p, "framerate", fps); n++;
  amf_named_num(&p, "videocodecid", 7); n++;   /* AVC */
  amf_named_num(&p, "videodatarate", video_kbps); n++;
  if (have_audio) {
    amf_named_num(&p, "audiocodecid", 10); n++; /* AAC */
    amf_named_bool(&p, "stereo", 1); n++;
    amf_named_num(&p, "audiosamplerate", 48000); n++;
  }
  w32(&count, n);
  w8(&p, 0x00); w8(&p, 0x00); w8(&p, 0x09);    /* object end */

  uint8_t *tag;
  size_t tl = wrap_tag(&tag, 0x12, body, (size_t)(p - body), 0);
  if (!tag) return 0;
  memcpy(out, tag, tl);
  free(tag);
  return tl;
}

size_t
flv_video_tag(uint8_t **out, const uint8_t *data, size_t len, int keyframe,
              int avc_packet_type, uint32_t ts_ms, int cts_ms) {
  if (!out) return 0;
  *out = NULL;
  if ((!data && len) || len > 0xffffff - 5 || avc_packet_type < 0 ||
      avc_packet_type > 2 || cts_ms < -8388608 || cts_ms > 8388607) return 0;
  size_t blen = 5 + len;
  uint8_t *body = malloc(blen);
  if (!body) { *out = NULL; return 0; }
  uint8_t *p = body;
  w8(&p, (keyframe ? 0x10 : 0x20) | 0x07); /* frametype<<4 | codecid(AVC) */
  w8(&p, avc_packet_type);                 /* AVC packet type */
  w24(&p, (uint32_t)(cts_ms & 0xffffff));  /* composition time */
  if (len) memcpy(p, data, len);
  size_t tl = wrap_tag(out, 0x09, body, blen, ts_ms);
  free(body);
  return tl;
}

size_t
flv_audio_tag(uint8_t **out, const uint8_t *data, size_t len,
              int aac_packet_type, uint32_t ts_ms) {
  if (!out) return 0;
  *out = NULL;
  if ((!data && len) || len > 0xffffff - 2 ||
      (aac_packet_type != 0 && aac_packet_type != 1)) return 0;
  size_t blen = 2 + len;
  uint8_t *body = malloc(blen);
  if (!body) { *out = NULL; return 0; }
  uint8_t *p = body;
  w8(&p, 0xaf);               /* AAC, 44/48kHz, 16-bit, stereo */
  w8(&p, aac_packet_type);    /* 0 = seq header, 1 = raw */
  if (len) memcpy(p, data, len);
  size_t tl = wrap_tag(out, 0x08, body, blen, ts_ms);
  free(body);
  return tl;
}

/* ---- H.264 helpers ------------------------------------------------------- */

/* A three-byte start code expands by one byte per NAL, so inlen+16 is
 * not a valid general bound. Every output write is checked instead. */
static size_t start_code(const uint8_t *in, size_t len, size_t i) {
  if (len - i >= 3 && in[i] == 0 && in[i+1] == 0) {
    if (in[i+2] == 1) return 3;
    if (len - i >= 4 && in[i+2] == 0 && in[i+3] == 1) return 4;
  }
  return 0;
}

size_t
annexb_to_avcc(const uint8_t *in, size_t inlen, uint8_t *out, size_t outcap,
               const uint8_t **sps, size_t *sps_len,
               const uint8_t **pps, size_t *pps_len) {
  if (sps) *sps = NULL;
  if (pps) *pps = NULL;
  if (sps_len) *sps_len = 0;
  if (pps_len) *pps_len = 0;
  if (!in || !out) return 0;
  size_t i = 0, o = 0;
  while (i < inlen && !start_code(in, inlen, i)) {
    if (in[i] != 0) return 0; /* Annex-B permits leading zero bytes only. */
    i++;
  }
  while (i < inlen) {
    size_t sc = start_code(in, inlen, i);
    if (!sc) return 0;
    size_t begin = i + sc;
    i = begin;
    while (i < inlen && !start_code(in, inlen, i)) i++;
    size_t end = i;
    while (end > begin && in[end-1] == 0) end--;
    size_t n = end - begin;
    if (!n) continue;
    if ((in[begin] & 0x80) || n > UINT32_MAX ||
        outcap - o < 4 || n > outcap - o - 4) return 0;
    int type = in[begin] & 31;
    if (type == 7) {
      if (sps) *sps = in + begin;
      if (sps_len) *sps_len = n;
    } else if (type == 8) {
      if (pps) *pps = in + begin;
      if (pps_len) *pps_len = n;
    }
    uint8_t *p = out + o;
    w32(&p, (uint32_t)n);
    memcpy(p, in + begin, n);
    o += 4 + n;
  }
  return o;
}

size_t
avc_decoder_config(uint8_t *out, size_t outcap,
                   const uint8_t *sps, size_t sps_len,
                   const uint8_t *pps, size_t pps_len) {
  if (!out || !sps || !pps || sps_len < 4 || !pps_len ||
      sps_len > UINT16_MAX || pps_len > UINT16_MAX ||
      (sps[0] & 31) != 7 || (pps[0] & 31) != 8 ||
      outcap < 11 + sps_len + pps_len) return 0;
  uint8_t *p = out;
  w8(&p, 1);
  w8(&p, sps[1]); w8(&p, sps[2]); w8(&p, sps[3]);
  w8(&p, 0xff); w8(&p, 0xe1);
  w16(&p, (uint32_t)sps_len);
  memcpy(p, sps, sps_len); p += sps_len;
  w8(&p, 1);
  w16(&p, (uint32_t)pps_len);
  memcpy(p, pps, pps_len); p += pps_len;
  return (size_t)(p - out);
}

/* ---- broadcaster --------------------------------------------------------- */

#ifndef BC_QUEUE_BYTES
#define BC_QUEUE_BYTES (6 * 1024 * 1024)
#endif
#define BC_QUEUE_PACKETS 1024

typedef struct bc_packet {
  struct bc_packet *next;
  size_t len;
  uint8_t data[];
} bc_packet;

struct bc_client {
  int fd;
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bc_packet *head, *tail;
  /* The consumer owns the current packet until its LAST byte has been read.
   * Queue overflow may discard only packets that have not been started. */
  bc_packet *current;
  size_t offset, queued_bytes, queued_packets;
  int dead, priming;
};

static void discard_pending(bc_client *c) {
  while (c->head) {
    bc_packet *p = c->head;
    c->head = p->next;
    free(p);
  }
  c->tail = NULL;
  c->queued_bytes = c->queued_packets = 0;
  c->priming = 1;
}

static int enqueue(bc_client *c, const uint8_t *data, size_t len) {
  size_t active = c->current ? c->current->len : 0;
  if (!len || active > BC_QUEUE_BYTES ||
      len > BC_QUEUE_BYTES - active - c->queued_bytes ||
      c->queued_packets >= BC_QUEUE_PACKETS) return 0;
  bc_packet *p = malloc(sizeof(*p) + len);
  if (!p) return 0;
  p->next = NULL; p->len = len;
  memcpy(p->data, data, len);
  if (c->tail) c->tail->next = p;
  else c->head = p;
  c->tail = p;
  c->queued_bytes += len; c->queued_packets++;
  return 1;
}

void broadcaster_init(broadcaster *b) {
  memset(b, 0, sizeof(*b));
  pthread_mutex_init(&b->mtx, NULL);
  b->fps = 60.0;
}

void broadcaster_set_video_params(broadcaster *b, int w, int h, double fps, int kbps) {
  pthread_mutex_lock(&b->mtx);
  b->width = w; b->height = h; b->fps = fps; b->video_kbps = kbps;
  pthread_mutex_unlock(&b->mtx);
}

/* Lock order is always broadcaster then client. Membership stays locked
 * through enqueue, preventing a disconnect/free racing with a producer. */
static void reprime(broadcaster *b) {
  for (int i = 0; i < b->n_clients; i++) {
    bc_client *c = b->clients[i];
    pthread_mutex_lock(&c->mtx);
    discard_pending(c);
    pthread_mutex_unlock(&c->mtx);
  }
}

void broadcaster_reset(broadcaster *b) {
  pthread_mutex_lock(&b->mtx);
  b->avc_cfg_len = b->aac_cfg_len = 0;
  reprime(b);
  pthread_mutex_unlock(&b->mtx);
}

void broadcaster_set_avc_config(broadcaster *b, const uint8_t *cfg, size_t len) {
  if (!cfg || !len || len > sizeof(b->avc_cfg)) return;
  pthread_mutex_lock(&b->mtx);
  if (b->avc_cfg_len != len || memcmp(b->avc_cfg, cfg, len)) {
    memcpy(b->avc_cfg, cfg, len); b->avc_cfg_len = len;
    reprime(b);
  }
  pthread_mutex_unlock(&b->mtx);
}

void broadcaster_set_aac_config(broadcaster *b, const uint8_t *cfg, size_t len) {
  if (!cfg || !len || len > sizeof(b->aac_cfg)) return;
  pthread_mutex_lock(&b->mtx);
  if (b->aac_cfg_len != len || memcmp(b->aac_cfg, cfg, len)) {
    memcpy(b->aac_cfg, cfg, len); b->aac_cfg_len = len; b->have_audio = 1;
    reprime(b);
  }
  pthread_mutex_unlock(&b->mtx);
}

static int enqueue_headers(broadcaster *b, bc_client *c, uint32_t ts) {
  uint8_t meta[512], *tag = NULL;
  size_t len = flv_metadata_tag(meta, b->width, b->height, b->fps,
                               b->video_kbps, b->have_audio);
  if (!len || !enqueue(c, meta, len)) return 0;
  len = flv_video_tag(&tag, b->avc_cfg, b->avc_cfg_len, 1, 0, ts, 0);
  int ok = tag && enqueue(c, tag, len);
  free(tag);
  if (!ok) return 0;
  if (b->aac_cfg_len) {
    len = flv_audio_tag(&tag, b->aac_cfg, b->aac_cfg_len, 0, ts);
    ok = tag && enqueue(c, tag, len);
    free(tag);
  }
  return ok;
}

void broadcaster_push(broadcaster *b, const uint8_t *tag, size_t len, int keyframe) {
  if (!tag || len < 15) return;
  uint32_t body = ((uint32_t)tag[1] << 16) | ((uint32_t)tag[2] << 8) | tag[3];
  if (body + 15 != len) return;
  uint32_t ts = ((uint32_t)tag[7] << 24) | ((uint32_t)tag[4] << 16) |
                ((uint32_t)tag[5] << 8) | tag[6];
  keyframe = keyframe && tag[0] == 9 && body >= 5 && tag[12] == 1;
  pthread_mutex_lock(&b->mtx);
  for (int i = 0; i < b->n_clients; i++) {
    bc_client *c = b->clients[i];
    pthread_mutex_lock(&c->mtx);
    if (!c->dead && (!c->priming || (keyframe && b->avc_cfg_len))) {
      int ok = !c->priming || enqueue_headers(b, c, ts);
      if (ok) ok = enqueue(c, tag, len);
      if (!ok) {
        discard_pending(c);
        /* A large IDR may itself have caused overflow; retain it if it fits. */
        if (keyframe && b->avc_cfg_len && enqueue_headers(b, c, ts) &&
            enqueue(c, tag, len)) c->priming = 0;
        else discard_pending(c);
      } else c->priming = 0;
      pthread_cond_signal(&c->cond);
    }
    pthread_mutex_unlock(&c->mtx);
  }
  pthread_mutex_unlock(&b->mtx);
}

bc_client *broadcaster_add_client(broadcaster *b, int fd, int want_header) {
  bc_client *c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->fd = fd; c->priming = 1;
  pthread_mutex_init(&c->mtx, NULL);
  pthread_cond_init(&c->cond, NULL);
  struct timeval timeout = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  pthread_mutex_lock(&b->mtx);
  if (b->n_clients >= BC_MAX_CLIENTS) {
    pthread_mutex_unlock(&b->mtx);
    broadcaster_client_free(c);
    return NULL;
  }
  if (want_header) {
    uint8_t hdr[13];
    flv_file_header(hdr, b->have_audio);
    if (!enqueue(c, hdr, sizeof(hdr))) {
      pthread_mutex_unlock(&b->mtx);
      broadcaster_client_free(c);
      return NULL;
    }
    /* Protect the one-time file header even if config arrives before reads. */
    c->current = c->head;
    c->head = c->tail = NULL;
    c->queued_bytes = c->queued_packets = 0;
  }
  b->clients[b->n_clients++] = c;
  pthread_mutex_unlock(&b->mtx);
  return c;
}

void broadcaster_remove_client(broadcaster *b, bc_client *c) {
  pthread_mutex_lock(&b->mtx);
  for (int i = 0; i < b->n_clients; i++) {
    if (b->clients[i] == c) {
      b->clients[i] = b->clients[--b->n_clients];
      b->clients[b->n_clients] = NULL;
      break;
    }
  }
  pthread_mutex_lock(&c->mtx);
  c->dead = 1;
  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->mtx);
  pthread_mutex_unlock(&b->mtx);
}

/* A client can vanish while capture is stalled. Periodically check EOF so
 * abandoned viewers cannot occupy all slots indefinitely. */
static int wait_packet(bc_client *c) {
  while (!c->dead && !c->current && !c->head) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until); until.tv_sec++;
    pthread_cond_timedwait(&c->cond, &c->mtx, &until);
    uint8_t byte;
    ssize_t n = recv(c->fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      c->dead = 1;
  }
  if (c->dead) return 0;
  if (!c->current) {
    c->current = c->head;
    c->head = c->head->next;
    if (!c->head) c->tail = NULL;
    c->queued_bytes -= c->current->len; c->queued_packets--;
    c->current->next = NULL; c->offset = 0;
  }
  return 1;
}

/* Only the owning connection thread consumes a client. */
int broadcaster_client_read_exact(bc_client *c, uint8_t *dst, size_t n) {
  size_t got = 0;
  pthread_mutex_lock(&c->mtx);
  while (got < n) {
    if (!wait_packet(c)) { pthread_mutex_unlock(&c->mtx); return 0; }
    size_t take = c->current->len - c->offset;
    if (take > n - got) take = n - got;
    memcpy(dst + got, c->current->data + c->offset, take);
    c->offset += take; got += take;
    if (c->offset == c->current->len) {
      free(c->current); c->current = NULL; c->offset = 0;
    }
  }
  pthread_mutex_unlock(&c->mtx);
  return 1;
}

void broadcaster_client_serve(bc_client *c) {
  for (;;) {
    pthread_mutex_lock(&c->mtx);
    if (!wait_packet(c)) { pthread_mutex_unlock(&c->mtx); return; }
    bc_packet *packet = c->current;
    size_t off = c->offset;
    pthread_mutex_unlock(&c->mtx);
    /* current stays protected from queue flushes while send may block. */
    while (off < packet->len) {
#ifdef MSG_NOSIGNAL
      ssize_t n = send(c->fd, packet->data + off, packet->len - off, MSG_NOSIGNAL);
#else
      ssize_t n = send(c->fd, packet->data + off, packet->len - off, 0);
#endif
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return;
      off += (size_t)n;
    }
    pthread_mutex_lock(&c->mtx);
    free(c->current); c->current = NULL; c->offset = 0;
    pthread_mutex_unlock(&c->mtx);
  }
}

void broadcaster_client_free(bc_client *c) {
  if (!c) return;
  discard_pending(c);
  free(c->current);
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
  free(c);
}
