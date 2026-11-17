/* SPDX-License-Identifier: MIT
 * Private local protocol v1. Native little endian, fixed width, no pointers or
 * hardware addresses. Both peers must be built from this header.
 */
#ifndef T736_CEDAR_PROTOCOL_H
#define T736_CEDAR_PROTOCOL_H
#include "cedar_client.h"
#define T736_MAGIC 0x54373643U
#define T736_PROTOCOL_VERSION 1U
#define T736_SOCKET_PATH "/run/t736-cedar/decoder.sock"
#define T736_MAX_BODY (T736_MAX_PACKET + 64U)
enum t736_op { T736_OPEN = 1, T736_PUSH, T736_PULL, T736_DRAIN,
               T736_FLUSH, T736_CLOSE };
struct t736_message {
    uint32_t magic, version, operation, length;
    uint32_t sequence, session;
    int32_t status;
    uint32_t reserved;
};
struct t736_open { uint32_t codec, width, height, reserved; };
struct t736_packet { int64_t pts_us, dts_us; };
struct t736_pull { uint32_t capacity, timeout_ms; };
struct t736_picture {
    uint32_t width, height, bytes, color_range, colorspace, reserved;
    int64_t pts_us;
};
_Static_assert(sizeof(struct t736_message) == 32, "wire header");
_Static_assert(sizeof(struct t736_picture) == 32, "wire picture");
#endif
