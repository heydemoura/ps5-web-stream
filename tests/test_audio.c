/* Host regression tests for media validation and delayed encoder timestamps.
 * The actual encoder is additionally verified by tests/audio_probe.c on PS5. */
#include <assert.h>
#include <stdio.h>
#include "../src/audio.c"

char g_log_ring[LOG_RING_SIZE];
size_t g_log_len;
pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

static void test_adts(void) {
  uint8_t frame[64] = {0};
  const uint8_t *payload = NULL;
  assert(audio_adts_header(frame, sizeof(frame) - 7) == 0);
  assert(audio_adts_payload(frame, sizeof(frame), &payload) == sizeof(frame) - 7);
  assert(payload == frame + 7);
  for (size_t n = 0; n < sizeof(frame); n++)
    assert(audio_adts_payload(frame, n, &payload) == 0);
  assert(audio_adts_header(frame, 8192) < 0);
  assert(audio_adts_header(NULL, 7) < 0);
  assert(audio_adts_payload(NULL, 7, &payload) == 0);
  assert(audio_adts_payload(frame, sizeof(frame), NULL) == 0);
  frame[2] = 0x50; /* 44.1 kHz instead of declared 48 kHz */
  assert(audio_adts_payload(frame, sizeof(frame), &payload) == 0);
  audio_adts_header(frame, sizeof(frame) - 7);
  frame[6] |= 1; /* multiple AAC frames cannot share one FLV timestamp */
  assert(audio_adts_payload(frame, sizeof(frame), &payload) == 0);
  audio_adts_header(frame, sizeof(frame) - 7);
  frame[1] &= ~1u; /* CRC-protected variant has a nine-byte header */
  assert(audio_adts_payload(frame, sizeof(frame), &payload) == sizeof(frame) - 9);
  assert(payload == frame + 9);
  uint8_t asc[2];
  assert(audio_encoder_config(asc) == 2);
  assert(asc[0] == 0x11 && asc[1] == 0x90);
}

static int mock_read_status = 8192;
static uint64_t mock_source_ticks = UINT64_C(240000)*1000+5120;
static uint64_t mock_tsc = UINT64_C(1000000000);
static unsigned tsc_reads;
static uint64_t read_tsc(void) { ++tsc_reads; return mock_tsc; }
static int mock_read(int64_t handle, void *buf, size_t n, void *meta, int flag) {
  assert(handle == 1 && n == 8192 && flag == 1);
  assert(((uint64_t *)meta)[0] == 16);
  memset(buf, 0, n);
  uint32_t nan = 0x7fc00000;
  memcpy(buf, &nan, 4);
  ((float *)buf)[1] = 2;
  ((float *)buf)[2] = -2;
  ((uint64_t *)meta)[1] = mock_source_ticks;
  return mock_read_status;
}

static void test_capture(void) {
  audio_capture s = { .handle = 1, .read = mock_read, .read_tsc = read_tsc,
                      .tsc_frequency = 1000000000 };
  float pcm[AUDIO_PCM_FLOATS];
  uint64_t pts;
  mock_read_status = (int)0x81950002u;
  assert(audio_capture_read(&s, pcm, &pts) == 0);
  assert(!s.clock_set && tsc_reads == 0); /* Empty reads cannot establish an epoch. */
  mock_read_status = 8192;
  assert(audio_capture_read(&s, pcm, &pts) == 1024);
  assert(pts == 1021333); /* TSC epoch plus native first-block offset; raw epoch ignored. */
  assert(pcm[0] == 0 && pcm[1] == 1 && pcm[2] == -1);
  mock_source_ticks += 5120; mock_tsc += 5000000000;
  assert(audio_capture_read(&s, pcm, &pts) == 1024 && pts == 1042666);
  assert(tsc_reads == 1); /* Delivery jitter cannot change the sample timeline. */
  mock_source_ticks += 5120;
  assert(audio_capture_read(&s, pcm, &pts) == 1024 && pts == 1064000);
  uint64_t accepted = s.source_last_ticks;
  --mock_source_ticks;
  assert(audio_capture_read(&s, pcm, &pts) < 0 && s.source_last_ticks == accepted);
  mock_source_ticks = accepted+5120;
  assert(audio_capture_read(&s, pcm, &pts) == 1024 && pts == 1085333);
  mock_read_status = (int)0x81950002u;
  assert(audio_capture_read(&s, pcm, &pts) == 0);
  mock_read_status = (int)0x81950004u;
  assert(audio_capture_read(&s, pcm, &pts) == 0);
  mock_read_status = (int)0x81950001u;
  assert(audio_capture_read(&s, pcm, &pts) < 0);
  mock_read_status = 4096;
  assert(audio_capture_read(&s, pcm, &pts) < 0);

  audio_capture reopened = { .handle = 1, .read = mock_read, .read_tsc = read_tsc,
                             .tsc_frequency = 1000000000 };
  mock_read_status = 8192;
  assert(audio_capture_read(&reopened, pcm, &pts) == 1024 && pts == 6021333);
  assert(tsc_reads == 2); /* Capture gaps get a new epoch, not a stale source counter. */
}

static void test_capture_clock_limits(void) {
  audio_capture s = { .read_tsc = read_tsc, .tsc_frequency = UINT64_MAX };
  uint64_t pts = 123;
  mock_tsc = UINT64_MAX;
  assert(audio_rebase_timestamp(&s, UINT64_MAX, &pts) == 0 && pts == 1021333);
  assert(audio_rebase_timestamp(&s, 0, &pts) < 0); /* Counter wrap/regression rejected. */
  s.clock_set = 0; s.tsc_frequency = 0;
  assert(audio_rebase_timestamp(&s, 0, &pts) < 0);
  s.tsc_frequency = 1;
  assert(audio_rebase_timestamp(&s, 0, &pts) < 0); /* TSC scaling overflow. */
  s.tsc_frequency = 240000;
  assert(audio_rebase_timestamp(&s, 0, &pts) < 0); /* One-block offset overflow. */
  s.clock_set = 1; s.source_first_ticks = s.source_last_ticks = 0;
  s.target_first_ticks = UINT64_MAX-1;
  assert(audio_rebase_timestamp(&s, 2, &pts) < 0); /* Rebased tick addition overflow. */
  s.target_first_ticks = UINT64_MAX/2;
  assert(audio_rebase_timestamp(&s, 0, &pts) < 0); /* Microsecond conversion overflow. */
  s.target_first_ticks = UINT64_C(240000)*20000000;
  assert(audio_rebase_timestamp(&s, 5120, &pts) == 0 && pts == UINT64_C(20000000021333));
}

static unsigned encode_calls;
static int mock_encode(aac_context *ctx, const void *pcm, unsigned n,
                        void *out, int *written, int *detail) {
  (void)ctx; (void)pcm;
  assert(n == 8192);
  *detail = 0;
  *written = 0;
  if (++encode_calls == 1) return 0; /* encoder priming */
  audio_adts_header(out, 4);
  memcpy((uint8_t *)out + 7, "AAC!", 4);
  *written = 11;
  return 0;
}
static int mock_flush(aac_context *ctx, void *out, int *written, int *detail) {
  return mock_encode(ctx, NULL, 8192, out, written, detail);
}
static void test_timestamps(void) {
  audio_encoder s = { .encode = mock_encode, .flush = mock_flush };
  audio_packet packet;
  float pcm[AUDIO_PCM_FLOATS] = {0};
  assert(audio_encoder_encode(&s, pcm, 1000000, &packet) == 0);
  assert(audio_encoder_encode(&s, pcm, 1021333, &packet) == 1);
  assert(packet.pts_us == 1000000 && packet.len == 4);
  assert(memcmp(packet.data, "AAC!", 4) == 0);
  assert(audio_encoder_flush(&s, &packet) == 1);
  assert(packet.pts_us == 1021333);
  assert(audio_encoder_flush(&s, &packet) == 0);
  assert(audio_encoder_encode(&s, pcm, 0, &packet) < 0); /* closed input */
}

static void verify_file(const char *path) {
  FILE *f = fopen(path, "rb");
  assert(f);
  uint8_t data[8192];
  unsigned frames = 0;
  while (fread(data, 1, 7, f) == 7) {
    size_t n = ((size_t)(data[3] & 3) << 11) |
                ((size_t)data[4] << 3) | (data[5] >> 5);
    assert(n >= 7 && n <= sizeof(data));
    assert(fread(data + 7, 1, n - 7, f) == n - 7);
    const uint8_t *payload;
    assert(audio_adts_payload(data, n, &payload) != 0);
    frames++;
  }
  assert(feof(f) && frames > 0);
  fclose(f);
  printf("validated %u captured ADTS frames\n", frames);
}

int main(int argc, char **argv) {
  test_adts(); test_capture(); test_capture_clock_limits(); test_timestamps();
  if (argc == 2) verify_file(argv[1]);
  puts("audio tests passed");
  return 0;
}
