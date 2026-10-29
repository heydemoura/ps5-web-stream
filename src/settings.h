#ifndef PS5HS_SETTINGS_H
#define PS5HS_SETTINGS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *resolution;
  int width, height, fps, bitrate;
  uint32_t revision;
} capture_settings;

/* Requested settings are shared by HTTP workers and the capture thread.
 * Only the capture thread opens/closes the firmware handles. */
void capture_settings_get(capture_settings *out);
int capture_settings_request(const char *resolution); /* -1 invalid, 0 same, 1 changed */
int capture_settings_current(uint32_t revision);
void capture_settings_begin(const capture_settings *settings);
void capture_settings_ready(uint32_t revision);
void capture_settings_end(uint32_t revision);
size_t capture_settings_json(char *out, size_t cap);
#endif
