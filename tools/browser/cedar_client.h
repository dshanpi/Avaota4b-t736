/* SPDX-License-Identifier: MIT */
#ifndef T736_CEDAR_CLIENT_H
#define T736_CEDAR_CLIENT_H
#include <stddef.h>
#include <stdint.h>

enum { T736_CODEC_H264 = 1, T736_CODEC_HEVC = 2 };
#define T736_MAX_WIDTH 1920U
#define T736_MAX_HEIGHT 1080U
#define T736_MAX_PACKET (8U * 1024U * 1024U)
#define T736_MAX_FRAME (T736_MAX_WIDTH * T736_MAX_HEIGHT * 3U / 2U)

struct t736_frame {
    uint32_t width, height, bytes_written;
    uint32_t color_range, colorspace;
    int64_t pts_us;
};

/* A connection is established by the preload constructor, before the RDD
 * sandbox starts. None of these methods opens a device or connects a socket.
 * All negative returns are -errno. Pixels are tightly packed 8-bit I420.
 */
int t736_client_available(void);
int t736_client_open(int codec, unsigned width, unsigned height);
int t736_client_push(int session, const uint8_t *data, size_t size,
                     int64_t pts_us, int64_t dts_us);
int t736_client_pull(int session, uint8_t *dst, size_t capacity,
                     struct t736_frame *frame, unsigned timeout_ms);
int t736_client_drain(int session);
int t736_client_flush(int session);
int t736_client_close(int session);
#endif
