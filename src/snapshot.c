/* Copyright (C) 2026 heydemoura */
#include "snapshot.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static snapshot_frame *latest;
static uint64_t next_seq = 1;
static int keyframe_wanted;

uint64_t snapshot_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

/* Caller holds the lock. */
static void unref(snapshot_frame *frame) {
  if (frame && !--frame->refs) free(frame);
}

static const uint8_t start_code[4] = {0, 0, 0, 1};

int snapshot_store(const uint8_t *sps, size_t sps_len,
                   const uint8_t *pps, size_t pps_len,
                   const uint8_t *avcc, size_t avcc_len,
                   int width, int height, uint32_t stream_ts_ms) {
  if (!sps || !sps_len || !pps || !pps_len || !avcc || !avcc_len) return -1;
  /* Validate the length-prefixed units and size the Annex-B output. */
  size_t total = 8 + sps_len + pps_len;
  int idr = 0;
  for (size_t i = 0; i < avcc_len;) {
    if (avcc_len - i < 4) return -1;
    size_t n = ((size_t)avcc[i] << 24) | ((size_t)avcc[i+1] << 16) |
               ((size_t)avcc[i+2] << 8) | avcc[i+3];
    i += 4;
    if (!n || n > avcc_len - i) return -1;
    int type = avcc[i] & 31;
    if (type == 5) idr = 1;
    if (type != 7 && type != 8 && type != 9) total += 4 + n;
    i += n;
  }
  if (!idr) return -1;
  snapshot_frame *frame = malloc(sizeof(*frame) + total);
  if (!frame) return -1;
  uint8_t *p = frame->data;
  memcpy(p, start_code, 4); memcpy(p + 4, sps, sps_len); p += 4 + sps_len;
  memcpy(p, start_code, 4); memcpy(p + 4, pps, pps_len); p += 4 + pps_len;
  for (size_t i = 0; i < avcc_len;) {
    size_t n = ((size_t)avcc[i] << 24) | ((size_t)avcc[i+1] << 16) |
               ((size_t)avcc[i+2] << 8) | avcc[i+3];
    i += 4;
    int type = avcc[i] & 31;
    if (type != 7 && type != 8 && type != 9) {
      memcpy(p, start_code, 4); memcpy(p + 4, avcc + i, n); p += 4 + n;
    }
    i += n;
  }
  frame->len = total;
  frame->width = width; frame->height = height;
  frame->stream_ts_ms = stream_ts_ms;
  frame->captured_ms = snapshot_now_ms();
  frame->refs = 1;
  pthread_mutex_lock(&lock);
  frame->seq = next_seq++;
  snapshot_frame *old = latest;
  latest = frame;
  keyframe_wanted = 0; /* Any new IDR satisfies outstanding requests. */
  unref(old);
  pthread_mutex_unlock(&lock);
  return 0;
}

void snapshot_clear(void) {
  pthread_mutex_lock(&lock);
  unref(latest);
  latest = NULL;
  keyframe_wanted = 0;
  pthread_mutex_unlock(&lock);
}

int snapshot_take_keyframe_request(void) {
  pthread_mutex_lock(&lock);
  int wanted = keyframe_wanted;
  keyframe_wanted = 0;
  pthread_mutex_unlock(&lock);
  return wanted;
}

snapshot_frame *snapshot_acquire(unsigned fresh_ms, unsigned wait_ms) {
  uint64_t start = snapshot_now_ms();
  pthread_mutex_lock(&lock);
  uint64_t seq = latest ? latest->seq : 0;
  if (!latest || start - latest->captured_ms > fresh_ms) {
    /* Never serve a stale frame: drop it (readers holding a reference keep
     * theirs) and wait for the IDR requested below. */
    unref(latest);
    latest = NULL;
    keyframe_wanted = 1;
    /* Poll rather than use a condition variable: it needs no monotonic
     * condattr support and waits at most `wait_ms`. */
    while (snapshot_now_ms() - start < wait_ms) {
      pthread_mutex_unlock(&lock);
      usleep(10000);
      pthread_mutex_lock(&lock);
      if (latest && latest->seq != seq) break;
    }
  }
  snapshot_frame *frame = latest;
  if (frame) frame->refs++;
  pthread_mutex_unlock(&lock);
  return frame;
}

void snapshot_release(snapshot_frame *frame) {
  if (!frame) return;
  pthread_mutex_lock(&lock);
  unref(frame);
  pthread_mutex_unlock(&lock);
}
