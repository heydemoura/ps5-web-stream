#ifndef PS5HS_STATUS_H
#define PS5HS_STATUS_H
#include <stddef.h>
#include <stdint.h>
void stream_status_set(const char *state, int error);
void stream_status_video(uint32_t ts);
void stream_status_audio(uint32_t ts);
size_t stream_status_json(char *out, size_t cap);
#endif
