/* Copyright (C) 2026 heydemoura */
#include "control.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int g_fd = -1;

int control_parse_hello(const char *line, control_hello *hello) {
  unsigned generation; int preset; char end;
  if (sscanf(line, "G%u P%d%c", &generation, &preset, &end) != 3 || end != '\n' ||
      !generation || preset < -1 || preset > 15) return -1;
  hello->generation = generation; hello->preset = preset;
  return 0;
}
int control_parse_restart(const char *line, int *preset) {
  int value; char end;
  if (sscanf(line, "R%d%c", &value, &end) != 2 || end != '\n' ||
      value < -1 || value > 15) return -1;
  *preset = value;
  return 0;
}

/* Read one newline-terminated line within timeout_ms. */
static int read_line(int fd, char *out, size_t cap, int timeout_ms) {
  size_t len = 0;
  while (len + 1 < cap) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    int n = poll(&p, 1, timeout_ms);
    if (n <= 0) return -1;
    ssize_t r = recv(fd, out + len, 1, 0);
    if (r <= 0) return -1;
    if (out[len++] == '\n') { out[len] = 0; return 0; }
  }
  return -1;
}

int control_connect(int http_port, control_hello *hello) {
  hello->generation = 0; hello->preset = -1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)(http_port + CONTROL_PORT_OFFSET));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  char line[64];
  if (connect(fd, (struct sockaddr *)&a, sizeof(a)) || read_line(fd, line, sizeof(line), 3000) ||
      control_parse_hello(line, hello)) {
    close(fd); hello->generation = 0; hello->preset = -1;
    return -1;
  }
  g_fd = fd;
  return 0;
}

int control_alive(void) {
  if (g_fd < 0) return 0;
  struct pollfd p = {.fd = g_fd, .events = POLLIN};
  if (poll(&p, 1, 0) <= 0) return 1;
  char c;
  ssize_t r = recv(g_fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
  if (r > 0) return 1;
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 1;
  return 0;
}

int control_request_restart(int preset) {
  if (g_fd < 0) return -1;
  char line[32];
  int n = snprintf(line, sizeof(line), "R%d\n", preset);
  if (send(g_fd, line, (size_t)n, MSG_NOSIGNAL) != n) return -1;
  /* The connection stays open until this process exits, so the supervisor
   * only relaunches once the HTTP port and capture handles are released. */
  return 0;
}
