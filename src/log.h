/* Copyright (C) 2026 heydemoura
 *
 * Simple logging for the payload: everything goes to stdout (picked up by the
 * ELF loader's klog) and is mirrored to an in-memory ring that the HTTP server
 * exposes at /log, so the console can be debugged without a serial cable.
 */
#ifndef PS5HS_LOG_H
#define PS5HS_LOG_H

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOG_RING_SIZE (64 * 1024)

extern char   g_log_ring[LOG_RING_SIZE];
extern size_t g_log_len;
extern pthread_mutex_t g_log_mtx;

static inline void
log_vprintf(const char *fmt, va_list ap) {
  char line[1024];
  int n = vsnprintf(line, sizeof(line), fmt, ap);
  if (n < 0) return;
  if (n > (int)sizeof(line) - 1) n = sizeof(line) - 1;

  /* klog */
  fwrite(line, 1, n, stdout);
  fputc('\n', stdout);
  fflush(stdout);

  /* ring (newest content kept; oldest dropped) */
  pthread_mutex_lock(&g_log_mtx);
  size_t add = (size_t)n + 1;
  if (add >= LOG_RING_SIZE) add = LOG_RING_SIZE - 1;
  if (g_log_len + add >= LOG_RING_SIZE) {
    size_t drop = g_log_len + add - (LOG_RING_SIZE - 1);
    if (drop > g_log_len) drop = g_log_len;
    memmove(g_log_ring, g_log_ring + drop, g_log_len - drop);
    g_log_len -= drop;
  }
  memcpy(g_log_ring + g_log_len, line, add - 1);
  g_log_len += add - 1;
  g_log_ring[g_log_len++] = '\n';
  pthread_mutex_unlock(&g_log_mtx);
}

static inline void
LOG(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  log_vprintf(fmt, ap);
  va_end(ap);
}

#endif /* PS5HS_LOG_H */
