/* Copyright (C) 2026 heydemoura
 *
 * The deployed payload: launches the embedded capture service through the
 * console's ELF loader and launches a fresh one whenever the service asks
 * (see control.h for why a new process is needed). It stops when the
 * service exits without asking, and the service stops when this process
 * goes away, so killing either one ends both.
 */
#include "control.h"
#include "gen/service_elf.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef DEF_HTTP_PORT
#define DEF_HTTP_PORT 8080
#endif
#ifndef ELFLDR_PORT
#define ELFLDR_PORT 9021
#endif
#define SAY(...) do { printf("supervisor: " __VA_ARGS__); putchar('\n'); fflush(stdout); } while (0)

static int loopback_socket(int port, struct sockaddr_in *a) {
  memset(a, 0, sizeof(*a));
  a->sin_family = AF_INET;
  a->sin_port = htons((uint16_t)port);
  a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return socket(AF_INET, SOCK_STREAM, 0);
}

/* Hand the service ELF to the ELF loader, which starts it as a new process. */
static int launch(void) {
  struct sockaddr_in a;
  int fd = loopback_socket(ELFLDR_PORT, &a);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&a, sizeof(a))) { close(fd); return -1; }
  size_t sent = 0;
  while (sent < service_elf_len) {
    ssize_t n = send(fd, service_elf + sent, service_elf_len - sent, MSG_NOSIGNAL);
    if (n <= 0) { close(fd); return -1; }
    sent += (size_t)n;
  }
  shutdown(fd, SHUT_WR);
  close(fd);
  return 0;
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  setvbuf(stdout, NULL, _IONBF, 0);
  struct sockaddr_in a;
  int listener = loopback_socket(DEF_HTTP_PORT + CONTROL_PORT_OFFSET, &a);
  int one = 1;
  setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (listener < 0 || bind(listener, (struct sockaddr *)&a, sizeof(a)) || listen(listener, 1)) {
    SAY("control port %d unavailable (already running?); exiting", DEF_HTTP_PORT + CONTROL_PORT_OFFSET);
    return 1;
  }
  SAY("PS5 Web Stream %s: PID %d, service %u bytes", PS5HS_VERSION, (int)getpid(), service_elf_len);
  int preset = -1;
  for (unsigned generation = 1;; generation++) {
    int launched = -1;
    for (int attempt = 0; attempt < 5 && launched; attempt++) {
      if (attempt) sleep(2);
      launched = launch();
    }
    if (launched) { SAY("ELF loader on port %d unavailable; exiting", ELFLDR_PORT); return 1; }
    struct pollfd p = {.fd = listener, .events = POLLIN};
    int fd = poll(&p, 1, 30000) == 1 ? accept(listener, NULL, NULL) : -1;
    if (fd < 0) { SAY("service #%u did not connect; exiting", generation); return 1; }
    char line[64];
    int n = snprintf(line, sizeof(line), "G%u P%d\n", generation, preset);
    send(fd, line, (size_t)n, MSG_NOSIGNAL);
    SAY("service #%u running", generation);
    /* Wait for the service to exit; note a restart request on the way. */
    int restart = 0;
    size_t len = 0;
    for (;;) {
      char c;
      ssize_t r = recv(fd, &c, 1, 0);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) break;
      if (len + 1 < sizeof(line)) line[len++] = c;
      if (c == '\n') {
        line[len] = 0; len = 0;
        if (!control_parse_restart(line, &preset)) restart = 1;
      }
    }
    close(fd);
    if (!restart) { SAY("service #%u exited; stopping", generation); return 0; }
    SAY("service #%u asked for a fresh process (preset %d)", generation, preset);
    sleep(1); /* let the old process release its HTTP port and capture */
  }
}
