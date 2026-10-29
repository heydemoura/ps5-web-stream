#include "settings.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static const capture_settings presets[] = {
  {"4k", 3840, 2160, 30, 32000, 0},
  {"1440p", 2560, 1440, 30, 16000, 0},
  {"1080p", 1920, 1080, 60, 8000, 0},
  {"720p", 1280, 720, 60, 5000, 0}
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static capture_settings requested = {"1080p", 1920, 1080, 60, 8000, 1};
static capture_settings capturing;
static uint32_t stream_revision;
static int ready;

void capture_settings_get(capture_settings *out) {
  pthread_mutex_lock(&lock); *out = requested; pthread_mutex_unlock(&lock);
}
int capture_settings_request(const char *resolution) {
  const capture_settings *preset = NULL;
  if (!resolution) return -1;
  for (size_t i = 0; i < sizeof(presets)/sizeof(presets[0]); i++)
    if (!strcmp(resolution, presets[i].resolution)) preset = &presets[i];
  if (!preset) return -1;
  pthread_mutex_lock(&lock);
  int changed = strcmp(requested.resolution, resolution) != 0;
  if (changed) {
    uint32_t revision = requested.revision + 1;
    requested = *preset; requested.revision = revision;
  }
  pthread_mutex_unlock(&lock);
  return changed;
}
int capture_settings_current(uint32_t revision) {
  pthread_mutex_lock(&lock);
  int current = requested.revision == revision;
  pthread_mutex_unlock(&lock); return current;
}
void capture_settings_begin(const capture_settings *settings) {
  pthread_mutex_lock(&lock);
  capturing = *settings; ready = 0; stream_revision++;
  pthread_mutex_unlock(&lock);
}
void capture_settings_ready(uint32_t revision) {
  pthread_mutex_lock(&lock);
  if (capturing.revision == revision && requested.revision == revision) ready = 1;
  pthread_mutex_unlock(&lock);
}
void capture_settings_end(uint32_t revision) {
  pthread_mutex_lock(&lock);
  if (capturing.revision == revision) ready = 0;
  pthread_mutex_unlock(&lock);
}
size_t capture_settings_json(char *out, size_t cap) {
  pthread_mutex_lock(&lock);
  int active = ready && capturing.revision == requested.revision;
  char active_name[16] = "null";
  if (active) snprintf(active_name, sizeof(active_name), "\"%s\"", capturing.resolution);
  int n = snprintf(out, cap,
    "{\"requested_resolution\":\"%s\",\"active_resolution\":%s,"
    "\"settings_revision\":%u,\"capture_revision\":%u,\"stream_revision\":%u,\"applying\":%s,"
    "\"resolutions\":[\"4k\",\"1440p\",\"1080p\",\"720p\"]}",
    requested.resolution, active_name, requested.revision, capturing.revision, stream_revision,
    active ? "false" : "true");
  pthread_mutex_unlock(&lock);
  return n < 0 || (size_t)n >= cap ? 0 : (size_t)n;
}
