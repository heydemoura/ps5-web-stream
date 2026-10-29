#include "status.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char capture_state[16] = "starting";
static int capture_error;
static uint64_t video_frames, audio_frames, video_at, audio_at;
static uint32_t video_ts, audio_ts;

static uint64_t monotonic_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

void stream_status_set(const char *state, int error) {
  /* Only known state names enter JSON. */
  if (!state || (strcmp(state, "starting") && strcmp(state, "opening") &&
      strcmp(state, "waiting") && strcmp(state, "streaming") &&
      strcmp(state, "error"))) state = "error";
  pthread_mutex_lock(&lock);
  snprintf(capture_state, sizeof(capture_state), "%s", state);
  capture_error = error;
  pthread_mutex_unlock(&lock);
}
void stream_status_video(uint32_t ts) {
  pthread_mutex_lock(&lock);
  video_frames++; video_ts = ts; video_at = monotonic_ms();
  pthread_mutex_unlock(&lock);
}
void stream_status_audio(uint32_t ts) {
  pthread_mutex_lock(&lock);
  audio_frames++; audio_ts = ts; audio_at = monotonic_ms();
  pthread_mutex_unlock(&lock);
}
size_t stream_status_json(char *out, size_t cap) {
  if (!out || !cap) return 0;
  pthread_mutex_lock(&lock);
  uint64_t now = monotonic_ms();
  int n = snprintf(out, cap,
    "{\"capture_state\":\"%s\",\"error\":%d,\"video_frames\":%llu,"
    "\"audio_frames\":%llu,\"last_video_ms\":%u,\"last_audio_ms\":%u,"
    "\"video_age_ms\":%lld,\"audio_age_ms\":%lld}",
    capture_state, capture_error, (unsigned long long)video_frames,
    (unsigned long long)audio_frames, video_ts, audio_ts,
    video_frames ? (long long)(now - video_at) : -1LL,
    audio_frames ? (long long)(now - audio_at) : -1LL);
  pthread_mutex_unlock(&lock);
  if (n < 0 || (size_t)n >= cap) { out[0] = 0; return 0; }
  return (size_t)n;
}
