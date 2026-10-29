/* Copyright (C) 2026 heydemoura
 *
 * Tiny HTTP server served from the console:
 *   GET /           -> the live-preview web page
 *   GET /live.flv   -> HTTP-FLV of the broadcast (flv.js in the browser; also
 *                      usable directly as an OBS Media Source input URL)
 *   GET /status     -> JSON status
 *   GET /log        -> the payload log ring (plain text)
 *   GET /vendor/flv.min.js -> embedded browser player (no CDN required)
 *   GET /vendor/flv.LICENSE -> player license
 */
#ifndef PS5HS_HTTP_H
#define PS5HS_HTTP_H

#include "flv.h"

/* Returns 0 only after bind/listen and listener thread creation succeed. */
int http_server_start(broadcaster *b, int port, int rtmp_port);

#endif /* PS5HS_HTTP_H */
