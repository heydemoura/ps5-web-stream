/* Copyright (C) 2026 heydemoura */
#include "frame_jpeg.h"
#include "log.h"
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t convert_lock = PTHREAD_MUTEX_INITIALIZER;

static AVFrame *decode_keyframe(const uint8_t *h264, size_t len) {
  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  int ok = 0;
  if (!ctx || !packet || !frame || len > INT_MAX) goto done;
  ctx->thread_count = 1; /* one picture: frame threading would only add delay */
  if (avcodec_open2(ctx, codec, NULL) < 0) goto done;
  /* av_new_packet supplies the zeroed input padding the decoder requires. */
  if (av_new_packet(packet, (int)len) < 0) goto done;
  memcpy(packet->data, h264, len);
  packet->flags |= AV_PKT_FLAG_KEY;
  if (avcodec_send_packet(ctx, packet) < 0 || avcodec_send_packet(ctx, NULL) < 0) goto done;
  ok = avcodec_receive_frame(ctx, frame) == 0 && frame->width > 0 && frame->height > 0;
done:
  if (!ok) av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&ctx);
  return frame;
}

/* Signalled matrix, defaulting by size as decoders and players do. */
static int source_matrix(const AVFrame *f) {
  switch (f->colorspace) {
  case AVCOL_SPC_BT709: return SWS_CS_ITU709;
  case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
  case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
  default: return f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
  }
}

/* swscale ignores matrix changes for YUV-to-YUV scaling, so convert through
 * RGB: source matrix/range -> RGB -> BT.601 full range (JFIF). */
static AVFrame *to_jfif(const AVFrame *src) {
  int w = src->width, h = src->height;
  AVFrame *rgb = av_frame_alloc(), *out = av_frame_alloc();
  struct SwsContext *to_rgb = NULL, *to_yuv = NULL;
  int ok = 0;
  if (!rgb || !out) goto done;
  rgb->format = AV_PIX_FMT_RGB24; rgb->width = w; rgb->height = h;
  out->format = AV_PIX_FMT_YUV420P; out->width = w; out->height = h;
  if (av_frame_get_buffer(rgb, 0) < 0 || av_frame_get_buffer(out, 0) < 0) goto done;
  int flags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT | SWS_FULL_CHR_H_INP;
  to_rgb = sws_getContext(w, h, src->format, w, h, AV_PIX_FMT_RGB24, flags, NULL, NULL, NULL);
  to_yuv = sws_getContext(w, h, AV_PIX_FMT_RGB24, w, h, AV_PIX_FMT_YUV420P, flags, NULL, NULL, NULL);
  if (!to_rgb || !to_yuv) goto done;
  const int *rgb_table = sws_getCoefficients(SWS_CS_DEFAULT);
  if (sws_setColorspaceDetails(to_rgb, sws_getCoefficients(source_matrix(src)),
        src->color_range == AVCOL_RANGE_JPEG, rgb_table, 1, 0, 1 << 16, 1 << 16) < 0 ||
      sws_setColorspaceDetails(to_yuv, rgb_table, 1,
        sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16) < 0) goto done;
  if (sws_scale(to_rgb, (const uint8_t *const *)src->data, src->linesize, 0, h,
                rgb->data, rgb->linesize) != h ||
      sws_scale(to_yuv, (const uint8_t *const *)rgb->data, rgb->linesize, 0, h,
                out->data, out->linesize) != h) goto done;
  out->color_range = AVCOL_RANGE_JPEG;
  out->colorspace = AVCOL_SPC_BT470BG;
  ok = 1;
done:
  sws_freeContext(to_rgb); sws_freeContext(to_yuv);
  av_frame_free(&rgb);
  if (!ok) av_frame_free(&out);
  return out;
}

static int encode_jpeg(AVFrame *picture, int qscale, uint8_t **out, size_t *out_len) {
  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
  AVPacket *packet = av_packet_alloc();
  int result = -1;
  if (!ctx || !packet) goto done;
  ctx->width = picture->width; ctx->height = picture->height;
  ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  ctx->color_range = AVCOL_RANGE_JPEG;
  ctx->colorspace = AVCOL_SPC_BT470BG;
  ctx->time_base = (AVRational){1, 1};
  ctx->flags |= AV_CODEC_FLAG_QSCALE;
  ctx->global_quality = FF_QP2LAMBDA * qscale;
  ctx->thread_count = 1;
  if (avcodec_open2(ctx, codec, NULL) < 0) goto done;
  picture->quality = ctx->global_quality;
  picture->pts = 0;
  if (avcodec_send_frame(ctx, picture) < 0 || avcodec_receive_packet(ctx, packet) < 0 ||
      packet->size <= 0) goto done;
  *out = malloc((size_t)packet->size);
  if (!*out) goto done;
  memcpy(*out, packet->data, (size_t)packet->size);
  *out_len = (size_t)packet->size;
  result = 0;
done:
  av_packet_free(&packet);
  avcodec_free_context(&ctx);
  return result;
}

int frame_jpeg_encode(const uint8_t *h264, size_t len, int qscale,
                      uint8_t **out, size_t *out_len) {
  if (!h264 || !len || !out || !out_len || qscale < 1 || qscale > 31) return -1;
  av_log_set_level(AV_LOG_QUIET); /* payload log stays readable */
  AVFrame *decoded = decode_keyframe(h264, len);
  if (!decoded) return -1;
  AVFrame *jfif = to_jfif(decoded);
  av_frame_free(&decoded);
  if (!jfif) return -1;
  int r = encode_jpeg(jfif, qscale, out, out_len);
  av_frame_free(&jfif);
  return r;
}

int frame_jpeg_prepare(snapshot_frame *frame) {
  if (!frame) return -1;
  pthread_mutex_lock(&convert_lock);
  int r = 0;
  if (!frame->jpeg) {
    uint8_t *jpeg = NULL; size_t len = 0;
    r = frame_jpeg_encode(frame->data, frame->len, FRAME_JPEG_QSCALE, &jpeg, &len);
    if (!r) { frame->jpeg_len = len; frame->jpeg = jpeg; }
    else LOG("frame: JPEG conversion of %dx%d keyframe failed", frame->width, frame->height);
  }
  pthread_mutex_unlock(&convert_lock);
  return r;
}
