/* Copyright (C) 2026 heydemoura
 *
 * Link between the capture service and its supervisor (src/supervisor.c).
 *
 * The firmware keeps capture state per process: after a native session
 * error, OpenVideo can fail for the rest of that process while a new
 * process opens at once. The deployed payload is therefore a small
 * supervisor that embeds the service ELF and launches it through the ELF
 * loader. The service connects back on 127.0.0.1:(HTTP port + 1000); when
 * it is stuck it sends a restart request and exits, and the supervisor
 * launches a fresh service with the same resolution. Either side exits when
 * the other's connection closes.
 *
 * Wire format, one line each way:
 *   supervisor -> service  "G<generation> P<preset>\n"
 *   service -> supervisor  "R<preset>\n"   (restart request; then exit)
 */
#ifndef PS5HS_CONTROL_H
#define PS5HS_CONTROL_H

#define CONTROL_PORT_OFFSET 1000

typedef struct {
  unsigned generation; /* 1 for the first service a supervisor launched */
  int preset;          /* settings preset index to apply, or -1 */
} control_hello;

/* Connect to a supervisor and read its greeting. Returns 0 when supervised,
 * -1 when no supervisor answers (the service then runs on its own). */
int control_connect(int http_port, control_hello *hello);
/* 1 while the supervisor connection is open (non-blocking check). */
int control_alive(void);
/* Ask for a fresh process with this preset. 0 when the request was sent. */
int control_request_restart(int preset);

/* Parse/format helpers shared with the supervisor; parse returns 0 on success. */
int control_parse_hello(const char *line, control_hello *hello);
int control_parse_restart(const char *line, int *preset);
#endif
