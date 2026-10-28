/* Copyright (C) 2026 heydemoura
 *
 * Minimal RTMP server. It accepts a client that issues connect / createStream
 * / play (this is what OBS's Media Source — libavformat — does when you give
 * it rtmp://<ps5>:1935/live/stream as an input) and then pushes the broadcast
 * to it, re-framing the shared FLV tag stream into RTMP messages.
 */
#ifndef PS5HS_RTMP_H
#define PS5HS_RTMP_H

#include "flv.h"

/* Serve one accepted RTMP connection to completion. Takes ownership of `fd`. */
void rtmp_serve(int fd, broadcaster *b);

/* Start the RTMP listener thread on `port` (blocks accept in its own thread).
 * Returns 0 on success. */
int rtmp_server_start(broadcaster *b, int port);

#endif /* PS5HS_RTMP_H */
