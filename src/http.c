/* Copyright (C) 2026 heydemoura */
#include "http.h"
#include "log.h"
#include "status.h"
#include "settings.h"
#include "gen/web_index.h"
#include "gen/web_player.h"
#include "gen/web_app.h"
#include "gen/web_flv.h"
#include "gen/web_flv_license.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>

#define HTTP_MAX_CONNECTIONS 24
#define HTTP_HEADER_LIMIT 8192
#define HTTP_REQUEST_TIMEOUT_MS 5000

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
  broadcaster *b;
  int fd;
  unsigned active;
  pthread_mutex_t mtx;
} http_server;

typedef struct { http_server *server; int fd; } http_connection;

static int
write_all(int fd, const void *p, size_t n) {
  size_t o = 0;
  while (o < n) {
    ssize_t r = send(fd, (const char *)p + o, n - o, MSG_NOSIGNAL);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
    o += (size_t)r;
  }
  return 0;
}

static void
send_bytes(int fd, const char *status, const char *ctype,
           const void *body, size_t len) {
  char hdr[512];
  int n = snprintf(hdr, sizeof(hdr),
    "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n"
    "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
    "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n%s\r\n",
    status, ctype, len, !strncmp(status, "405", 3) ? "Allow: GET\r\n" : "");
  if (n > 0 && (size_t)n < sizeof(hdr) && write_all(fd, hdr, (size_t)n) == 0)
    write_all(fd, body, len);
}

static void
send_simple(int fd, const char *status, const char *body) {
  send_bytes(fd, status, "text/plain; charset=utf-8", body, strlen(body));
}

static void
serve_flv(http_server *s, int fd) {
  /* Reserve the client before sending success, so a full broadcaster can
   * produce a useful HTTP error instead of a truncated FLV response. */
  bc_client *c = broadcaster_add_client(s->b, fd, 1);
  if (!c) {
    send_simple(fd, "503 Service Unavailable", "All stream slots are busy. Try again shortly.\n");
    return;
  }
  const char *hdr =
    "HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\n"
    "Cache-Control: no-store\r\nConnection: close\r\n"
    "X-Content-Type-Options: nosniff\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
  if (write_all(fd, hdr, strlen(hdr)) == 0) broadcaster_client_serve(c);
  broadcaster_remove_client(s->b, c);
  broadcaster_client_free(c);
}

static void
serve_status(http_server *s, int fd) {
  char body[2048], settings[512];
  size_t used = stream_status_json(body, sizeof(body));
  if (!used || used >= sizeof(body) || body[used - 1] != '}') {
    send_simple(fd, "500 Internal Server Error", "Status unavailable.\n");
    return;
  }
  /* Snapshot broadcaster metadata under its lock, without holding a lock
   * across network I/O. */
  pthread_mutex_lock(&s->b->mtx);
  int width = s->b->width, height = s->b->height;
  int audio = s->b->have_audio, clients = s->b->n_clients;
  double fps = s->b->fps;
  pthread_mutex_unlock(&s->b->mtx);
  used--;
  if (!capture_settings_json(settings, sizeof(settings))) {
    send_simple(fd, "500 Internal Server Error", "Settings unavailable.\n"); return;
  }
  int n = snprintf(body + used, sizeof(body) - used,
    ",\"video\":\"%dx%d\",\"fps\":%.2f,\"audio\":%s,"
    "\"clients\":%d,%s",
    width, height, fps, audio ? "true" : "false", clients, settings + 1);
  if (n < 0 || (size_t)n >= sizeof(body) - used) {
    send_simple(fd, "500 Internal Server Error", "Status unavailable.\n");
    return;
  }
  send_bytes(fd, "200 OK", "application/json", body, used + (size_t)n);
}

static void
serve_log(int fd) {
  char *snapshot = malloc(LOG_RING_SIZE);
  if (!snapshot) { send_simple(fd, "503 Service Unavailable", "Log unavailable.\n"); return; }
  pthread_mutex_lock(&g_log_mtx);
  size_t len = g_log_len;
  if (len > LOG_RING_SIZE) len = LOG_RING_SIZE;
  memcpy(snapshot, g_log_ring, len);
  pthread_mutex_unlock(&g_log_mtx);
  send_bytes(fd, "200 OK", "text/plain; charset=utf-8", snapshot, len);
  free(snapshot);
}

static int64_t
monotonic_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* TCP does not preserve request boundaries. Read a bounded complete header
 * with a total deadline, rather than a timeout that a trickle can renew. */
static int
read_request(int fd, char *buf, size_t cap, size_t *length) {
  size_t used = 0;
  int64_t deadline = monotonic_ms() + HTTP_REQUEST_TIMEOUT_MS;
  while (used < cap - 1) {
    int64_t left = deadline - monotonic_ms();
    if (left <= 0) return 408;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, (int)left);
    if (r < 0) { if (errno == EINTR) continue; return 400; }
    if (r == 0) return 408;
    ssize_t n = recv(fd, buf + used, cap - 1 - used, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return 400;
    if (memchr(buf + used, '\0', (size_t)n)) return 400;
    used += (size_t)n;
    buf[used] = '\0';
    if (strstr(buf, "\r\n\r\n")) { *length = used; return 0; }
  }
  return 431;
}

static void serve_settings(int fd, const char *status) {
  char body[512];
  size_t n = capture_settings_json(body, sizeof(body));
  send_bytes(fd, status, "application/json", body, n);
}

/* Small, explicit control API. A custom header prevents an unrelated web page
 * from submitting a simple cross-origin form; no preflight route is exposed. */
static void update_settings(int fd, char *headers, char *body, size_t available) {
  size_t length = 0;
  int have_length = 0, form = 0, control = 0;
  for (char *line = headers; *line;) {
    char *end = strstr(line, "\r\n");
    if (!end) goto malformed;
    if (end == line) break;
    *end = '\0';
    char *colon = strchr(line, ':');
    if (!colon) goto malformed;
    *colon++ = '\0';
    while (*colon == ' ' || *colon == '\t') colon++;
    if (!strcasecmp(line, "Content-Length")) {
      if (have_length++ || !*colon) goto malformed;
      for (const char *p = colon; *p; p++) {
        if (*p < '0' || *p > '9') goto malformed;
        length = length * 10 + (unsigned)(*p - '0');
        if (length > 64) {
          send_simple(fd, "413 Content Too Large", "Settings body is too large.\n"); return;
        }
      }
    } else if (!strcasecmp(line, "Content-Type")) {
      form = !strcmp(colon, "application/x-www-form-urlencoded");
    } else if (!strcasecmp(line, "X-PS5HS-Request")) {
      control = !strcmp(colon, "1");
    } else if (!strcasecmp(line, "Transfer-Encoding")) goto malformed;
    line = end + 2;
  }
  if (!control) { send_simple(fd, "403 Forbidden", "Missing control header.\n"); return; }
  if (!form) { send_simple(fd, "415 Unsupported Media Type", "Use form-encoded settings.\n"); return; }
  if (!have_length || !length) goto malformed;
  char value[65];
  size_t used = available < length ? available : length;
  memcpy(value, body, used);
  int64_t deadline = monotonic_ms() + HTTP_REQUEST_TIMEOUT_MS;
  while (used < length) {
    int64_t left = deadline - monotonic_ms();
    if (left <= 0) { send_simple(fd, "408 Request Timeout", "Request timed out.\n"); return; }
    struct pollfd pollfd = {.fd = fd, .events = POLLIN};
    int r = poll(&pollfd, 1, (int)left);
    if (r < 0 && errno == EINTR) continue;
    if (r == 0) { send_simple(fd, "408 Request Timeout", "Request timed out.\n"); return; }
    if (r < 0) goto malformed;
    ssize_t n = recv(fd, value + used, length - used, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) goto malformed;
    used += (size_t)n;
  }
  if (memchr(value, '\0', length)) goto malformed;
  value[length] = '\0';
  if (strncmp(value, "resolution=", 11) || capture_settings_request(value + 11) < 0) {
    send_simple(fd, "400 Bad Request", "Choose 4k, 1440p, 1080p or 720p.\n"); return;
  }
  serve_settings(fd, "202 Accepted");
  return;
malformed:
  send_simple(fd, "400 Bad Request", "Malformed settings request.\n");
}

static void
handle_request(http_server *s, int fd) {
  char req[HTTP_HEADER_LIMIT + 1];
  size_t length = 0;
  int error = read_request(fd, req, sizeof(req), &length);
  if (error) {
    if (error == 408) send_simple(fd, "408 Request Timeout", "Request timed out.\n");
    else if (error == 431) send_simple(fd, "431 Request Header Fields Too Large", "Request headers too large.\n");
    else send_simple(fd, "400 Bad Request", "Malformed request.\n");
    return;
  }
  char *end = strstr(req, "\r\n");
  if (!end) { send_simple(fd, "400 Bad Request", "Malformed request.\n"); return; }
  char *body = strstr(req, "\r\n\r\n") + 4;
  *end = '\0';
  char method[16], path[1024], version[16], extra;
  if (sscanf(req, "%15s %1023s %15s %c", method, path, version, &extra) != 3 ||
      path[0] != '/' || (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) {
    send_simple(fd, "400 Bad Request", "Malformed request.\n");
    return;
  }
  for (const unsigned char *p = (const unsigned char *)req; *p; p++) {
    if (*p < 0x20 || *p == 0x7f) { send_simple(fd, "400 Bad Request", "Malformed request.\n"); return; }
  }
  char *query = strchr(path, '?');
  if (query) *query = '\0';
  if (!strcmp(path, "/settings") && !strcmp(method, "POST")) {
    update_settings(fd, end + 2, body, length - (size_t)(body - req));
    return;
  }
  if (strcmp(method, "GET")) {
    send_simple(fd, "405 Method Not Allowed", "Only GET is supported.\n");
    return;
  }
  if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
    send_bytes(fd, "200 OK", "text/html; charset=utf-8", web_index_html, web_index_html_len);
  } else if (!strcmp(path, "/player") || !strcmp(path, "/player/")) {
    send_bytes(fd, "200 OK", "text/html; charset=utf-8", web_player_html, web_player_html_len);
  } else if (!strcmp(path, "/app.js")) {
    send_bytes(fd, "200 OK", "application/javascript", web_app_js, web_app_js_len);
  } else if (!strcmp(path, "/settings")) {
    serve_settings(fd, "200 OK");
  } else if (!strcmp(path, "/vendor/flv.min.js")) {
    send_bytes(fd, "200 OK", "application/javascript", web_flv_js, web_flv_js_len);
  } else if (!strcmp(path, "/vendor/flv.LICENSE")) {
    send_bytes(fd, "200 OK", "text/plain; charset=utf-8", web_flv_license, web_flv_license_len);
  } else if (!strcmp(path, "/live.flv")) {
    serve_flv(s, fd);
  } else if (!strcmp(path, "/status")) {
    serve_status(s, fd);
  } else if (!strcmp(path, "/log")) {
    serve_log(fd);
  } else {
    send_simple(fd, "404 Not Found", "Not found.\n");
  }
}

static void *
conn_thread(void *arg) {
  http_connection *c = arg;
  http_server *s = c->server;
  int fd = c->fd;
  free(c);
  handle_request(s, fd);
  close(fd);
  pthread_mutex_lock(&s->mtx);
  s->active--;
  pthread_mutex_unlock(&s->mtx);
  return NULL;
}

static void *
listen_thread(void *arg) {
  http_server *s = arg;
  for (;;) {
    int fd = accept(s->fd, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) continue;
      /* Avoid a tight loop if the process temporarily exhausts descriptors. */
      usleep(100000);
      continue;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    pthread_mutex_lock(&s->mtx);
    int full = s->active >= HTTP_MAX_CONNECTIONS;
    if (!full) s->active++;
    pthread_mutex_unlock(&s->mtx);
    if (full) {
      send_simple(fd, "503 Service Unavailable", "Too many connections. Try again shortly.\n");
      close(fd);
      continue;
    }
    http_connection *c = malloc(sizeof(*c));
    pthread_t t;
    if (c) { c->server = s; c->fd = fd; }
    if (!c || pthread_create(&t, NULL, conn_thread, c) != 0) {
      free(c);
      close(fd);
      pthread_mutex_lock(&s->mtx);
      s->active--;
      pthread_mutex_unlock(&s->mtx);
    } else pthread_detach(t);
  }
  return NULL;
}

int
http_server_start(broadcaster *b, int port) {
  if (!b || port < 1 || port > 65535) return -1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = INADDR_ANY;
  a.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) {
    LOG("http: listen on :%d failed: %s", port, strerror(errno));
    close(fd);
    return -1;
  }
  http_server *s = calloc(1, sizeof(*s));
  if (!s) { close(fd); return -1; }
  s->b = b; s->fd = fd;
  if (pthread_mutex_init(&s->mtx, NULL) != 0) { close(fd); free(s); return -1; }
  pthread_t t;
  if (pthread_create(&t, NULL, listen_thread, s) != 0) {
    pthread_mutex_destroy(&s->mtx); close(fd); free(s); return -1;
  }
  pthread_detach(t);
  LOG("http: listening on :%d", port);
  return 0;
}
