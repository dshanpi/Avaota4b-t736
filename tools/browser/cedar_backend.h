/* SPDX-License-Identifier: MIT */
#ifndef T736_CEDAR_BACKEND_H
#define T736_CEDAR_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#define CEDAR_NOPTS INT64_MIN
#define CEDAR_MAX_INPUT (8U * 1024U * 1024U)
#define CEDAR_MAX_OUTPUT (16U * 1024U * 1024U)

enum cedar_codec { CEDAR_H264 = 1, CEDAR_HEVC = 2 };
enum cedar_result { CEDAR_AGAIN = 0, CEDAR_FRAME = 1, CEDAR_EOF = 2,
                    CEDAR_ERROR = -1, CEDAR_BACKPRESSURE = -2 };
typedef struct cedar_decoder cedar_decoder;

typedef struct cedar_frame {
    unsigned width, height;
    /* Owned, tightly packed I420: Y, U, V; chroma uses ceil(w/2), ceil(h/2).
     * Release only with cedar_frame_free, never free(data) or duplicate this
     * owning frame by value. Pixels survive later pulls, flush and close. */
    uint8_t *data;
    size_t size;
    int64_t pts_ns, dts_ns;
    /* 0 unknown, 1 limited, 2 full; matrix: 0 unknown, 1 BT709, 2 BT601,
     * 3 BT2020. No Gst objects or hardware handles cross this API. */
    unsigned color_range, color_matrix;
} cedar_frame;

/* Calls on one decoder must be externally serialized. Input is Annex-B H264
 * or HEVC, one complete AU per push, timestamps in ns or CEDAR_NOPTS.
 * Maximum visible dimensions: 1920x1080, progressive 8-bit 4:2:0.
 * The broker must retain this instance across equal-format leases, flush it,
 * and enforce one active lease. Do not close/reopen per browser decoder. */
cedar_decoder *cedar_open(enum cedar_codec codec, unsigned width, unsigned height,
                          char *error, size_t error_size);
int cedar_push(cedar_decoder *dec, const void *data, size_t size,
               int64_t pts_ns, int64_t dts_ns);
int cedar_pull(cedar_decoder *dec, cedar_frame *out, unsigned timeout_ms);
int cedar_eos(cedar_decoder *dec);
/* A flushing seek clears appsrc/parser/decoder/appsink while retaining the
 * pipeline and OMX output pool. Hardware repeat-stream acceptance is separate. */
int cedar_flush(cedar_decoder *dec);
/* End an already-owned lease: flush, pause the Gst source, then explicitly
 * pause the vendor OMX decoder via the WC-v4 private action. Pool retained.
 * This is not an ordinary lossless player pause: it discards queued data. */
int cedar_idle(cedar_decoder *dec);
int cedar_resume(cedar_decoder *dec);
void cedar_frame_free(cedar_frame *frame);
const char *cedar_error(const cedar_decoder *dec);
/* Process shutdown only. Kernel bring-up DMA imports remain pinned to reboot. */
void cedar_close(cedar_decoder *dec);

#endif
