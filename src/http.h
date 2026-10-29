/* Copyright (C) 2026 heydemoura
 *
 * Tiny HTTP server served from the console:
 *   GET /           -> dashboard, preview and capture resolution controls
 *   GET /player     -> viewport-filling autoplay player
 *   GET /live.flv   -> HTTP-FLV media for both browser pages
 *   GET /settings   -> requested and active capture configuration
 *   POST /settings  -> resolution=4k|1440p|1080p|720p (form encoded)
 *   GET /status     -> JSON status
 *   GET /log        -> the payload log ring (plain text)
 *   GET /vendor/flv.min.js -> embedded browser player (no CDN required)
 *   GET /vendor/flv.LICENSE -> player license
 */
#ifndef PS5HS_HTTP_H
#define PS5HS_HTTP_H

#include "flv.h"

/* Returns 0 only after bind/listen and listener thread creation succeed. */
int http_server_start(broadcaster *b, int port);

#endif /* PS5HS_HTTP_H */
