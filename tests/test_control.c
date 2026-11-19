/* Host test for the service <-> supervisor control link (src/control.c). */
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "control.h"

static int listen_on(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(!bind(fd, (struct sockaddr *)&a, sizeof(a)) && !listen(fd, 1));
  return fd;
}
int main(void) {
  control_hello h; int preset;
  assert(!control_parse_hello("G1 P-1\n", &h) && h.generation == 1 && h.preset == -1);
  assert(!control_parse_hello("G7 P3\n", &h) && h.generation == 7 && h.preset == 3);
  assert(control_parse_hello("G0 P1\n", &h) && control_parse_hello("G1 P1", &h));
  assert(control_parse_hello("X1 P1\n", &h) && control_parse_hello("G1 P99\n", &h));
  assert(!control_parse_restart("R2\n", &preset) && preset == 2);
  assert(control_parse_restart("R2", &preset) && control_parse_restart("Q2\n", &preset));

  const int http = 18181;
  /* No supervisor: unsupervised, nothing is alive and restarts fail. */
  assert(control_connect(http, &h) == -1 && h.preset == -1 && !control_alive());
  assert(control_request_restart(1) == -1);

  int listener = listen_on(http + CONTROL_PORT_OFFSET);
  /* A supervisor that never greets is not accepted (3 s timeout). */
  assert(control_connect(http, &h) == -1);
  int stale = accept(listener, NULL, NULL); close(stale);

  /* Greeting, liveness, restart request, then supervisor exit. */
  int peer = -1;
  pid_t pid = fork();
  if (!pid) {
    int fd = accept(listener, NULL, NULL);
    const char *hello = "G4 P2\n";
    send(fd, hello, strlen(hello), 0);
    char buf[16] = {0}; size_t n = 0;
    while (n < sizeof(buf) - 1 && recv(fd, buf + n, 1, 0) == 1 && buf[n++] != '\n') {}
    _exit(control_parse_restart(buf, &preset) || preset != 1 ? 1 : 0);
  }
  (void)peer;
  assert(!control_connect(http, &h) && h.generation == 4 && h.preset == 2);
  assert(control_alive());
  assert(!control_request_restart(1));
  int status; waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  usleep(50000);
  assert(!control_alive()); /* supervisor gone */
  close(listener);
  puts("control link: greeting, liveness, restart request and supervisor loss passed");
  return 0;
}
