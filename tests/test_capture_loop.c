/* Run the actual payload capture loop with deterministic capture/encoder input.
 * These tests validate scheduling and mux timing, not the console capture ABI. */
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "avcap.h"
#include "audio.h"
#include "flv.h"
#include "status.h"

static uint64_t fake_now = 1100;
static int mocked_clock_gettime(clockid_t id, struct timespec *ts) {
  (void)id; ts->tv_sec = fake_now / 1000; ts->tv_nsec = (fake_now % 1000) * 1000000;
  return 0;
}
static int mocked_usleep(unsigned usec) { fake_now += usec / 1000; return 0; }
static void record_push(broadcaster *, const uint8_t *, size_t, int);
#define main payload_entry
#define clock_gettime mocked_clock_gettime
#define usleep mocked_usleep
#define broadcaster_push record_push
#define PS5HS_VERSION "test"
#include "../src/main.c"
#undef main
#undef clock_gettime
#undef usleep
#undef broadcaster_push

struct audio_capture { int unused; };
struct audio_encoder { int unused; };
static struct audio_capture mock_capture;
static struct audio_encoder mock_encoder;
static int opened_audio, closed_audio, opened_encoder, closed_encoder;
static int audio_open_fails, video_closes, terminations, init_count;

static const uint8_t sps_data[] = {0,0,0,1,0x67,0x64,0,0x28,0xaa};
static const uint8_t pps_data[] = {0,0,1,0x68,0xee};
static const uint8_t idr_data[] = {0,0,1,0x65,0x88};
static const uint8_t p_data[] = {0,0,1,0x41,0x88};
static const uint8_t full_idr[] = {0,0,0,1,0x67,0x64,0,0x28,0xaa,
                                  0,0,1,0x68,0xee,0,0,1,0x65,0x88};
typedef struct {
  const uint8_t *data;
  size_t len;
  uint64_t dts, pts;
  int audio_count;
  uint64_t audio_pts[4];
  uint64_t advance;
  int stop;
  const char *resolution;
} source_step;
static source_step steps[16];
static size_t step_count, step_index;
static source_step *current;
static int audio_index;

typedef struct { int type, key, cts; uint32_t ts; } recorded_tag;
static recorded_tag records[64];
static size_t record_count;
static int video_status_count, audio_status_count;
static char last_state[32];

static void record_push(broadcaster *b, const uint8_t *tag, size_t len, int key) {
  assert(record_count < 64 && len >= 17);
  recorded_tag *r = &records[record_count++];
  r->type = tag[0]; r->key = key;
  r->ts = ((uint32_t)tag[7] << 24) | ((uint32_t)tag[4] << 16) |
          ((uint32_t)tag[5] << 8) | tag[6];
  r->cts = 0;
  if (r->type == 9) {
    r->cts = (tag[13] << 16) | (tag[14] << 8) | tag[15];
    if (r->cts & 0x800000) r->cts -= 0x1000000;
    assert(b->avc_cfg_len && b->aac_cfg_len);
  }
  broadcaster_push(b, tag, len, key);
}
void stream_status_set(const char *state, int error) {
  (void)error; snprintf(last_state, sizeof(last_state), "%s", state);
}
void stream_status_video(uint32_t ts) { (void)ts; video_status_count++; }
void stream_status_audio(uint32_t ts) { (void)ts; audio_status_count++; }

int avcap_load(void) { return 0; }
int avcap_init(void) { init_count++; return 0; }
int avcap_open(const avcap_config *cfg) { assert(cfg->fps == 60); return 0; }
int avcap_resource_id(void) { return 0xb6; }
void avcap_close(void) { video_closes++; }
void avcap_terminate(void) { terminations++; }
int avcap_read_video(uint8_t *buf, size_t size, avcap_frame *fr) {
  assert(step_index < step_count);
  current = &steps[step_index++]; audio_index = 0;
  fake_now += current->advance;
  if (current->stop) on_signal(SIGTERM);
  if (current->resolution) assert(capture_settings_request(current->resolution) >= 0);
  if (!current->len) return 0;
  assert(size >= current->len); memcpy(buf, current->data, current->len);
  fr->len = current->len; fr->dts_us = current->dts; fr->pts_us = current->pts;
  fr->keyframe = 0; /* IDR must be corroborated from NALs. */
  return 1;
}
audio_capture *audio_capture_open(int resource_id) {
  assert(resource_id==0xb6); /* the acquired resource is shared with audio */
  opened_audio++; return audio_open_fails ? NULL : &mock_capture;
}
void audio_capture_close(audio_capture *capture) {
  if (capture) { assert(capture == &mock_capture); closed_audio++; }
}
int audio_capture_read(audio_capture *capture, float *pcm, uint64_t *pts) {
  assert(capture == &mock_capture);
  if (audio_index >= current->audio_count) return 0;
  memset(pcm, 0, AUDIO_PCM_FLOATS * sizeof(*pcm));
  *pts = current->audio_pts[audio_index++]; return AUDIO_FRAME_SAMPLES;
}
audio_encoder *audio_encoder_open(unsigned bitrate) {
  assert(bitrate == 128000); opened_encoder++; return &mock_encoder;
}
void audio_encoder_close(audio_encoder *encoder) {
  if (encoder) { assert(encoder == &mock_encoder); closed_encoder++; }
}
int audio_encoder_encode(audio_encoder *encoder, const float *pcm,
                          uint64_t pts, audio_packet *packet) {
  assert(encoder == &mock_encoder && pcm);
  packet->data[0] = 0xaa; packet->len = 1; packet->pts_us = pts; return 1;
}
size_t audio_encoder_config(uint8_t out[2]) { out[0] = 0x11; out[1] = 0x90; return 2; }
int http_server_start(broadcaster *b, int port) { assert(b && port == 18080); return 0; }

static void reset_test(void) {
  broadcaster_reset(&g_bc);
  assert(g_bc.have_audio == 1);
  stopping = 0; fake_now = 1100;
  capture_settings_request("1080p");
  opened_audio = closed_audio = opened_encoder = closed_encoder = 0;
  audio_open_fails = 0;
  step_count = step_index = record_count = 0;
  video_status_count = audio_status_count = 0;
  memset(steps, 0, sizeof(steps));
  memset(records, 0, sizeof(records));
}
static void add_video(const uint8_t *data, size_t len, uint64_t dts, uint64_t pts) {
  steps[step_count++] = (source_step){.data=data, .len=len, .dts=dts, .pts=pts};
}
static void stop_source(void) { steps[step_count++].stop = 1; }
static void check_cleanup(void) {
  assert(opened_audio == 1 && closed_audio == 1);
  assert(opened_encoder == 1 && closed_encoder == 1);
}

static int run_capture(void) {
  capture_settings settings; capture_settings_get(&settings);
  capture_settings_begin(&settings);
  return capture_loop(1000, settings.revision);
}
static void test_resolution_change(void) {
  reset_test();
  add_video(full_idr, sizeof(full_idr), 1000000, 1000000);
  steps[0].resolution = "4k";
  assert(run_capture() == 1); check_cleanup();
  capture_settings settings; capture_settings_get(&settings);
  assert(settings.width == 3840 && settings.height == 2160);
  assert(step_index == 1); /* interrupted promptly, without waiting for watchdog */
}
static void test_shared_clock_and_restart(void) {
  reset_test(); next_epoch_ms = 0;
  add_video(sps_data, sizeof(sps_data), 1000000, 1000000);
  steps[0].audio_count = 1; steps[0].audio_pts[0] = 950000;
  add_video(pps_data, sizeof(pps_data), 1000000, 1000000);
  add_video(idr_data, sizeof(idr_data), 1000000, 1033000);
  steps[2].audio_count = 2;
  steps[2].audio_pts[0] = 990000; steps[2].audio_pts[1] = 1010000;
  add_video(pps_data, sizeof(pps_data), 1020000, 1020000); /* no VCL */
  add_video(p_data, sizeof(p_data), 1033333, 1000000);
  steps[4].audio_count = 1; steps[4].audio_pts[0] = 1042666;
  stop_source();
  assert(run_capture() == 0); check_cleanup();
  assert(record_count == 4 && video_status_count == 2 && audio_status_count == 2);
  assert(records[0].type == 9 && records[0].key && records[0].ts == 100 && records[0].cts == 33);
  assert(records[1].type == 8 && records[1].ts == 110);
  assert(records[2].type == 9 && !records[2].key && records[2].ts == 133 && records[2].cts == -33);
  assert(records[3].type == 8 && records[3].ts == 142);
  assert(!strcmp(last_state, "streaming") && next_epoch_ms == 143);
  reset_test(); /* Wall clock has not caught up to the source's last timestamp. */
  add_video(full_idr, sizeof(full_idr), 600000, 600000);
  steps[0].audio_count = 1; steps[0].audio_pts[0] = 610000;
  stop_source(); assert(run_capture() == 0); check_cleanup();
  assert(record_count == 2 && records[0].ts == 143 && records[1].ts == 153);
}
static void test_config_change(void) {
  reset_test();
  uint8_t changed_sps[sizeof(sps_data)];
  memcpy(changed_sps, sps_data, sizeof(changed_sps)); changed_sps[7] = 0x29;
  add_video(full_idr, sizeof(full_idr), 1000000, 1000000);
  add_video(changed_sps, sizeof(changed_sps), 1010000, 1010000);
  add_video(pps_data, sizeof(pps_data), 1020000, 1020000);
  add_video(idr_data, sizeof(idr_data), 1033333, 1033333);
  stop_source(); assert(run_capture() == 0); check_cleanup();
  assert(record_count == 2 && records[0].key && records[1].key);
  assert(g_bc.avc_cfg[3] == 0x29); /* Latest SPS level is published. */
}
static void test_regressions_and_stalls(void) {
  reset_test();
  add_video(full_idr, sizeof(full_idr), 1000000, 1000000);
  add_video(p_data, sizeof(p_data), 999999, 1000000);
  assert(run_capture() < 0); check_cleanup(); assert(record_count == 1);
  reset_test();
  add_video(full_idr, sizeof(full_idr), 1000000, 1000000);
  steps[0].audio_count = 1; steps[0].audio_pts[0] = 1100000;
  add_video(p_data, sizeof(p_data), 1033333, 1033333);
  steps[1].audio_count = 1; steps[1].audio_pts[0] = 1090000;
  assert(run_capture() < 0); check_cleanup(); assert(record_count == 3);
  reset_test();
  add_video(full_idr, sizeof(full_idr), 1000000, 1000000);
  add_video(p_data, sizeof(p_data), 1033333, 1033333); steps[1].advance = 10001;
  assert(run_capture() < 0); check_cleanup(); assert(audio_status_count == 0);
  assert(!strcmp(last_state, "waiting"));
}
static void test_failures_and_signal(void) {
  reset_test(); stop_source();
  assert(run_capture() == 0); check_cleanup(); assert(record_count == 0);
  reset_test(); audio_open_fails = 1;
  assert(run_capture() < 0);
  assert(!closed_audio && closed_encoder == 1);
  reset_test();
  add_video(full_idr, sizeof(full_idr), 1, UINT64_MAX);
  assert(run_capture() < 0); check_cleanup(); assert(record_count == 0);
}
static void test_entry_cleanup(void) {
  reset_test(); stop_source();
  pthread_mutex_destroy(&g_bc.mtx); /* entry point initializes its broadcaster */
  char *args[] = {"payload", "18080", NULL};
  assert(payload_entry(2, args) == 0);
  check_cleanup();
  assert(video_closes == 1 && terminations == 1 && init_count == 1);
  assert(g_bc.have_audio == 1 && !g_bc.avc_cfg_len && !g_bc.aac_cfg_len);
  char settings_json[512];
  assert(capture_settings_json(settings_json,sizeof(settings_json)));
  assert(strstr(settings_json,"\"active_resolution\":null"));
}
int main(void) {
  broadcaster_init(&g_bc); g_bc.have_audio = 1;
  test_shared_clock_and_restart(); test_config_change(); test_regressions_and_stalls();
  test_failures_and_signal(); test_resolution_change(); test_entry_cleanup();
  pthread_mutex_destroy(&g_bc.mtx);
  puts("capture-loop timing, configuration, restart and cleanup passed");
  return 0;
}
