/* Copyright (C) 2026 heydemoura */
#include "http.h"
#include "log.h"
#include "gen/web_index.h"   /* web_index_html, web_index_html_len */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

static broadcaster *g_b;
static int g_rtmp_port;

static void
write_all(int fd, const void *p, size_t n) {
  size_t o = 0;
  while (o < n) {
    ssize_t r = write(fd, (const char *)p + o, n - o);
    if (r <= 0) { if (r < 0 && errno == EINTR) continue; return; }
    o += (size_t)r;
  }
}

static void
send_simple(int fd, const char *status, const char *ctype, const char *body) {
  char hdr[512];
  int n = snprintf(hdr, sizeof(hdr),
    "HTTP/1.1 %s\r\nContent-Type: %s\r\nConnection: close\r\n"
    "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n",
    status, ctype, strlen(body));
  write_all(fd, hdr, n);
  write_all(fd, body, strlen(body));
}

static void
serve_flv(int fd) {
  const char *hdr =
    "HTTP/1.1 200 OK\r\nContent-Type: video/x-flv\r\n"
    "Cache-Control: no-cache\r\nConnection: close\r\n"
    "Access-Control-Allow-Origin: *\r\n\r\n";
  write_all(fd, hdr, strlen(hdr));

  bc_client *c = broadcaster_add_client(g_b, fd, 1 /* FLV file header */);
  if (!c) { LOG("http: broadcaster full"); return; }
  broadcaster_client_serve(c);
  broadcaster_remove_client(g_b, c);
  broadcaster_client_free(c);
}

static void
serve_status(int fd) {
  char body[256];
  snprintf(body, sizeof(body),
    "{\"video\":\"%dx%d\",\"fps\":%.0f,\"audio\":%s,"
    "\"clients\":%d,\"rtmp_port\":%d}",
    g_b->width, g_b->height, g_b->fps, g_b->have_audio ? "true" : "false",
    g_b->n_clients, g_rtmp_port);
  send_simple(fd, "200 OK", "application/json", body);
}

static void
serve_log(int fd) {
  char hdr[256];
  pthread_mutex_lock(&g_log_mtx);
  int n = snprintf(hdr, sizeof(hdr),
    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n"
    "Access-Control-Allow-Origin: *\r\nContent-Length: %zu\r\n\r\n", g_log_len);
  write_all(fd, hdr, n);
  write_all(fd, g_log_ring, g_log_len);
  pthread_mutex_unlock(&g_log_mtx);
}

static void
serve_index(int fd) {
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
    "Connection: close\r\nContent-Length: %u\r\n\r\n", web_index_html_len);
  write_all(fd, hdr, n);
  write_all(fd, web_index_html, web_index_html_len);
}

static void *
conn_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  char req[1024];
  ssize_t r = read(fd, req, sizeof(req) - 1);
  if (r <= 0) { close(fd); return NULL; }
  req[r] = 0;

  char method[8] = {0}, path[256] = {0};
  sscanf(req, "%7s %255s", method, path);

  if (strcmp(method, "GET") != 0) {
    send_simple(fd, "405 Method Not Allowed", "text/plain", "method not allowed\n");
  } else if (!strcmp(path, "/") || !strncmp(path, "/index", 6)) {
    serve_index(fd);
  } else if (!strncmp(path, "/live.flv", 9)) {
    serve_flv(fd);
  } else if (!strcmp(path, "/status")) {
    serve_status(fd);
  } else if (!strcmp(path, "/log")) {
    serve_log(fd);
  } else {
    send_simple(fd, "404 Not Found", "text/plain", "not found\n");
  }
  close(fd);
  return NULL;
}

typedef struct { int port; } hctx;

static void *
listen_thread(void *arg) {
  hctx *h = arg;
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a; memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(h->port);
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { LOG("http: bind %d failed", h->port); return NULL; }
  listen(srv, 8);
  LOG("http: listening on :%d", h->port);
  for (;;) {
    int fd = accept(srv, NULL, NULL);
    if (fd < 0) continue;
    pthread_t t;
    pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)fd);
    pthread_detach(t);
  }
  return NULL;
}

int
http_server_start(broadcaster *b, int port, int rtmp_port) {
  g_b = b;
  g_rtmp_port = rtmp_port;
  hctx *h = malloc(sizeof(*h));
  h->port = port;
  pthread_t t;
  if (pthread_create(&t, NULL, listen_thread, h) != 0) return -1;
  pthread_detach(t);
  return 0;
}
