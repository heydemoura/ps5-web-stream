/* Copyright (C) 2026 heydemoura */
#include "flv.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

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
  size_t blen = 5 + len;
  uint8_t *body = malloc(blen);
  if (!body) { *out = NULL; return 0; }
  uint8_t *p = body;
  w8(&p, (keyframe ? 0x10 : 0x20) | 0x07); /* frametype<<4 | codecid(AVC) */
  w8(&p, avc_packet_type);                 /* AVC packet type */
  w24(&p, (uint32_t)(cts_ms & 0xffffff));  /* composition time */
  memcpy(p, data, len);
  size_t tl = wrap_tag(out, 0x09, body, blen, ts_ms);
  free(body);
  return tl;
}

size_t
flv_audio_tag(uint8_t **out, const uint8_t *data, size_t len,
              int aac_packet_type, uint32_t ts_ms) {
  size_t blen = 2 + len;
  uint8_t *body = malloc(blen);
  if (!body) { *out = NULL; return 0; }
  uint8_t *p = body;
  w8(&p, 0xaf);               /* AAC, 44/48kHz, 16-bit, stereo */
  w8(&p, aac_packet_type);    /* 0 = seq header, 1 = raw */
  memcpy(p, data, len);
  size_t tl = wrap_tag(out, 0x08, body, blen, ts_ms);
  free(body);
  return tl;
}

/* ---- H.264 helpers ------------------------------------------------------- */

size_t
annexb_to_avcc(const uint8_t *in, size_t inlen, uint8_t *out,
               const uint8_t **sps, size_t *sps_len,
               const uint8_t **pps, size_t *pps_len) {
  if (sps) *sps = NULL;
  if (pps) *pps = NULL;
  if (sps_len) *sps_len = 0;
  if (pps_len) *pps_len = 0;

  size_t i = 0, o = 0;
  /* collect NAL unit boundaries */
  const uint8_t *nal = NULL; size_t cur = 0;
  while (i + 3 <= inlen) {
    int sc = 0, sclen = 0;
    if (in[i] == 0 && in[i+1] == 0 && in[i+2] == 1) { sc = 1; sclen = 3; }
    else if (i + 4 <= inlen && in[i]==0 && in[i+1]==0 && in[i+2]==0 && in[i+3]==1) { sc = 1; sclen = 4; }
    if (sc) {
      if (nal) {
        size_t nlen = (in + i) - nal;
        /* strip trailing zero bytes that belong to the next start code */
        while (nlen > 0 && nal[nlen-1] == 0) nlen--;
        if (nlen) {
          int t = nal[0] & 0x1f;
          if (t == 7 && sps) { *sps = nal; *sps_len = nlen; }
          else if (t == 8 && pps) { *pps = nal; *pps_len = nlen; }
          out[o++]=nlen>>24; out[o++]=nlen>>16; out[o++]=nlen>>8; out[o++]=nlen;
          memcpy(out + o, nal, nlen); o += nlen;
        }
      }
      i += sclen; nal = in + i; cur = 0; (void)cur;
      continue;
    }
    i++;
  }
  if (nal) {
    size_t nlen = (in + inlen) - nal;
    while (nlen > 0 && nal[nlen-1] == 0) nlen--;
    if (nlen) {
      int t = nal[0] & 0x1f;
      if (t == 7 && sps) { *sps = nal; *sps_len = nlen; }
      else if (t == 8 && pps) { *pps = nal; *pps_len = nlen; }
      out[o++]=nlen>>24; out[o++]=nlen>>16; out[o++]=nlen>>8; out[o++]=nlen;
      memcpy(out + o, nal, nlen); o += nlen;
    }
  }
  return o;
}

size_t
avc_decoder_config(uint8_t *out, const uint8_t *sps, size_t sps_len,
                   const uint8_t *pps, size_t pps_len) {
  uint8_t *p = out;
  w8(&p, 1);                 /* configurationVersion */
  w8(&p, sps[1]);            /* AVCProfileIndication */
  w8(&p, sps[2]);            /* profile_compatibility */
  w8(&p, sps[3]);            /* AVCLevelIndication */
  w8(&p, 0xff);              /* 6 bits reserved + lengthSizeMinusOne = 3 */
  w8(&p, 0xe1);              /* 3 bits reserved + numOfSPS = 1 */
  w16(&p, (uint32_t)sps_len);
  memcpy(p, sps, sps_len); p += sps_len;
  w8(&p, 1);                 /* numOfPPS */
  w16(&p, (uint32_t)pps_len);
  memcpy(p, pps, pps_len); p += pps_len;
  return (size_t)(p - out);
}

/* ---- broadcaster --------------------------------------------------------- */

#define CQ_SIZE (6 * 1024 * 1024)

struct bc_client {
  int             fd;
  pthread_mutex_t mtx;
  pthread_cond_t  cond;
  uint8_t        *buf;
  size_t          head, tail, used;   /* circular */
  int             dead;
  int             priming;            /* waiting for first/next keyframe */
  int             want_header;        /* send FLV file header on join */
  broadcaster    *b;
};

void
broadcaster_init(broadcaster *b) {
  memset(b, 0, sizeof(*b));
  pthread_mutex_init(&b->mtx, NULL);
  b->fps = 60.0;
}

void broadcaster_set_video_params(broadcaster *b, int w, int h, double fps, int kbps) {
  b->width = w; b->height = h; b->fps = fps; b->video_kbps = kbps;
}
void broadcaster_set_avc_config(broadcaster *b, const uint8_t *cfg, size_t len) {
  pthread_mutex_lock(&b->mtx);
  if (len <= sizeof(b->avc_cfg)) { memcpy(b->avc_cfg, cfg, len); b->avc_cfg_len = len; }
  pthread_mutex_unlock(&b->mtx);
}
void broadcaster_set_aac_config(broadcaster *b, const uint8_t *cfg, size_t len) {
  pthread_mutex_lock(&b->mtx);
  if (len <= sizeof(b->aac_cfg)) { memcpy(b->aac_cfg, cfg, len); b->aac_cfg_len = len; b->have_audio = 1; }
  pthread_mutex_unlock(&b->mtx);
}

/* append bytes to a client's queue; caller holds c->mtx. Returns 0 if it does
 * not fit (caller then enters priming / drop-to-keyframe mode). */
static int
cq_put(bc_client *c, const uint8_t *data, size_t len) {
  if (len > CQ_SIZE - c->used) return 0;
  size_t first = CQ_SIZE - c->tail;
  if (first > len) first = len;
  memcpy(c->buf + c->tail, data, first);
  if (len > first) memcpy(c->buf, data + first, len - first);
  c->tail = (c->tail + len) % CQ_SIZE;
  c->used += len;
  return 1;
}

static void
client_enqueue(bc_client *c, const uint8_t *tag, size_t len, int keyframe) {
  pthread_mutex_lock(&c->mtx);
  if (c->dead) { pthread_mutex_unlock(&c->mtx); return; }

  if (c->priming) {
    if (!keyframe) { pthread_mutex_unlock(&c->mtx); return; }
    c->priming = 0;                   /* keyframe: resume from here */
  }
  if (!cq_put(c, tag, len)) {
    /* overflow: drop everything and wait for the next keyframe */
    c->head = c->tail = c->used = 0;
    c->priming = 1;
    pthread_mutex_unlock(&c->mtx);
    return;
  }
  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

void
broadcaster_push(broadcaster *b, const uint8_t *tag, size_t len, int keyframe) {
  pthread_mutex_lock(&b->mtx);
  bc_client *snap[BC_MAX_CLIENTS];
  int n = b->n_clients;
  memcpy(snap, b->clients, sizeof(bc_client *) * n);
  pthread_mutex_unlock(&b->mtx);
  for (int i = 0; i < n; i++)
    if (snap[i]) client_enqueue(snap[i], tag, len, keyframe);
}

bc_client *
broadcaster_add_client(broadcaster *b, int sockfd, int want_header) {
  bc_client *c = calloc(1, sizeof(*c));
  if (!c) return NULL;
  c->buf = malloc(CQ_SIZE);
  if (!c->buf) { free(c); return NULL; }
  c->fd = sockfd;
  c->b = b;
  c->priming = 1;
  c->want_header = want_header;
  pthread_mutex_init(&c->mtx, NULL);
  pthread_cond_init(&c->cond, NULL);

  pthread_mutex_lock(&b->mtx);
  if (b->n_clients >= BC_MAX_CLIENTS) {
    pthread_mutex_unlock(&b->mtx);
    free(c->buf); free(c); return NULL;
  }
  b->clients[b->n_clients++] = c;
  pthread_mutex_unlock(&b->mtx);

  /* prime: file header (HTTP-FLV only), metadata, and sequence headers */
  uint8_t hdr[1024]; size_t hl = 0;
  if (want_header) hl += flv_file_header(hdr + hl, b->have_audio);
  hl += flv_metadata_tag(hdr + hl, b->width, b->height, b->fps, b->video_kbps, b->have_audio);
  pthread_mutex_lock(&c->mtx);
  cq_put(c, hdr, hl);
  if (b->avc_cfg_len) {
    uint8_t *vt; size_t vl = flv_video_tag(&vt, b->avc_cfg, b->avc_cfg_len, 1, 0, 0, 0);
    if (vt) { cq_put(c, vt, vl); free(vt); }
  }
  if (b->have_audio && b->aac_cfg_len) {
    uint8_t *at; size_t al = flv_audio_tag(&at, b->aac_cfg, b->aac_cfg_len, 0, 0);
    if (at) { cq_put(c, at, al); free(at); }
  }
  pthread_mutex_unlock(&c->mtx);
  return c;
}

void
broadcaster_remove_client(broadcaster *b, bc_client *c) {
  pthread_mutex_lock(&b->mtx);
  for (int i = 0; i < b->n_clients; i++) {
    if (b->clients[i] == c) {
      b->clients[i] = b->clients[--b->n_clients];
      b->clients[b->n_clients] = NULL;
      break;
    }
  }
  pthread_mutex_unlock(&b->mtx);
  pthread_mutex_lock(&c->mtx);
  c->dead = 1;
  pthread_cond_signal(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

void
broadcaster_client_serve(bc_client *c) {
  uint8_t chunk[128 * 1024];
  for (;;) {
    pthread_mutex_lock(&c->mtx);
    while (!c->dead && c->used == 0)
      pthread_cond_wait(&c->cond, &c->mtx);
    if (c->dead && c->used == 0) { pthread_mutex_unlock(&c->mtx); break; }
    size_t take = c->used < sizeof(chunk) ? c->used : sizeof(chunk);
    size_t first = CQ_SIZE - c->head;
    if (first > take) first = take;
    memcpy(chunk, c->buf + c->head, first);
    if (take > first) memcpy(chunk + first, c->buf, take - first);
    c->head = (c->head + take) % CQ_SIZE;
    c->used -= take;
    pthread_mutex_unlock(&c->mtx);

    size_t off = 0;
    while (off < take) {
      ssize_t n = write(c->fd, chunk + off, take - off);
      if (n <= 0) {
        if (n < 0 && (errno == EINTR)) continue;
        pthread_mutex_lock(&c->mtx); c->dead = 1; pthread_mutex_unlock(&c->mtx);
        off = take; break;
      }
      off += (size_t)n;
    }
    pthread_mutex_lock(&c->mtx);
    int dead = c->dead;
    pthread_mutex_unlock(&c->mtx);
    if (dead) break;
  }
}

int
broadcaster_client_read_exact(bc_client *c, uint8_t *dst, size_t n) {
  size_t got = 0;
  while (got < n) {
    pthread_mutex_lock(&c->mtx);
    while (!c->dead && c->used == 0)
      pthread_cond_wait(&c->cond, &c->mtx);
    if (c->used == 0 && c->dead) { pthread_mutex_unlock(&c->mtx); return 0; }
    size_t take = n - got;
    if (take > c->used) take = c->used;
    size_t first = CQ_SIZE - c->head;
    if (first > take) first = take;
    memcpy(dst + got, c->buf + c->head, first);
    if (take > first) memcpy(dst + got + first, c->buf, take - first);
    c->head = (c->head + take) % CQ_SIZE;
    c->used -= take;
    pthread_mutex_unlock(&c->mtx);
    got += take;
  }
  return 1;
}

void
broadcaster_client_free(bc_client *c) {
  free(c->buf);
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
  free(c);
}
