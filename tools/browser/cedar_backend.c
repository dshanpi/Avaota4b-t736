/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "cedar_backend.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

/* Ordinary CPU RAM only. Outstanding frames own their block and keep the
 * pool alive independently of the decoder. Only returned blocks are reused.
 */
#define CEDAR_RAM_CACHE_LIMIT 2U
typedef struct cedar_ram_pool cedar_ram_pool;
typedef struct cedar_ram_block {
    cedar_ram_pool *pool;
    struct cedar_ram_block *next;
    size_t capacity;
    _Alignas(64) uint8_t bytes[];
} cedar_ram_block;

struct cedar_ram_pool {
    GMutex lock;
    gint references;
    gboolean closed;
    unsigned cached;
    cedar_ram_block *idle;
    uint64_t allocations, reuses;
};

_Static_assert(offsetof(cedar_ram_block, bytes) % 64 == 0,
               "I420 RAM output must remain cache-line aligned");

struct cedar_decoder {
    GstElement *pipeline, *source, *sink, *decoder;
    GstBus *bus;
    unsigned width, height;
    gboolean eos, failed, discontinuity, idle;
    uint64_t copied_frames, total_copy_ns, total_map_ns;
    cedar_ram_pool *ram_pool;
    char error[512];
};

static void ram_pool_unref(cedar_ram_pool *pool)
{
    if (g_atomic_int_dec_and_test(&pool->references)) {
        g_mutex_clear(&pool->lock);
        free(pool);
    }
}

static uint8_t *ram_acquire(cedar_decoder *d, size_t size)
{
    cedar_ram_pool *pool = d->ram_pool;
    cedar_ram_block *block = NULL, **entry;
    if (!size || size > CEDAR_MAX_OUTPUT)
        return NULL;
    if (!pool) {
        pool = calloc(1, sizeof(*pool));
        if (!pool)
            return NULL;
        g_mutex_init(&pool->lock);
        pool->references = 1; /* Decoder owner, separate from each frame. */
        d->ram_pool = pool;
    }
    g_mutex_lock(&pool->lock);
    if (pool->closed) {
        g_mutex_unlock(&pool->lock);
        return NULL;
    }
    for (entry = &pool->idle; *entry; entry = &(*entry)->next) {
        if ((*entry)->capacity == size) {
            block = *entry;
            *entry = block->next;
            pool->cached--;
            pool->reuses++;
            break;
        }
    }
    g_mutex_unlock(&pool->lock);
    if (!block) {
        void *allocation = NULL;
        if (posix_memalign(&allocation, 64, sizeof(*block) + size))
            return NULL;
        block = allocation;
        block->pool = pool;
        block->capacity = size;
        pool->allocations++;
    }
    block->next = NULL;
    g_atomic_int_inc(&pool->references);
    return block->bytes;
}

static void ram_release(uint8_t *data)
{
    cedar_ram_block *block;
    cedar_ram_pool *pool;
    gboolean keep;
    if (!data)
        return;
    block = (cedar_ram_block *)(data - offsetof(cedar_ram_block, bytes));
    pool = block->pool;
    g_mutex_lock(&pool->lock);
    keep = !pool->closed && pool->cached < CEDAR_RAM_CACHE_LIMIT;
    if (keep) {
        block->next = pool->idle;
        pool->idle = block;
        pool->cached++;
    }
    g_mutex_unlock(&pool->lock);
    if (!keep)
        free(block);
    /* A cached block owns no reference; decoder close drains all cached RAM.
     * Do not touch block after unlock: close may already have freed it. */
    ram_pool_unref(pool);
}

static void ram_pool_close(cedar_decoder *d)
{
    cedar_ram_pool *pool = d->ram_pool;
    cedar_ram_block *block, *next;
    if (!pool)
        return;
    d->ram_pool = NULL;
    g_mutex_lock(&pool->lock);
    pool->closed = TRUE;
    block = pool->idle;
    pool->idle = NULL;
    pool->cached = 0;
    g_mutex_unlock(&pool->lock);
    for (; block; block = next) {
        next = block->next;
        free(block);
    }
    ram_pool_unref(pool);
}

static int fail(cedar_decoder *d, const char *format, ...)
{
    va_list args;
    if (d->failed)
        return CEDAR_ERROR; /* Preserve the first failure; never clear poison. */
    va_start(args, format);
    vsnprintf(d->error, sizeof(d->error), format, args);
    va_end(args);
    d->failed = TRUE;
    fprintf(stderr, "CEDAR_BACKEND_ERROR: %s\n", d->error);
    return CEDAR_ERROR;
}

static int check_bus(cedar_decoder *d)
{
    GstMessage *message;
    if (d->failed)
        return CEDAR_ERROR;
    while ((message = gst_bus_pop_filtered(d->bus, GST_MESSAGE_ERROR))) {
        GError *error = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(message, &error, &debug);
        if (debug && *debug)
            fail(d, "%s: %.320s | %.150s", GST_OBJECT_NAME(message->src),
                 error ? error->message : "GStreamer error", debug);
        else
            fail(d, "%s: %s", GST_OBJECT_NAME(message->src),
                 error ? error->message : "GStreamer error");
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(message);
        return CEDAR_ERROR;
    }
    return d->failed ? CEDAR_ERROR : 0;
}

static gboolean seek_data(GstAppSrc *source, guint64 offset, gpointer data)
{
    (void)source;
    (void)data;
    /* appsrc is a caller-fed time stream. Only restart-at-zero is supported. */
    return offset == 0;
}

static GstPadProbeReturn allocation_query(GstPad *pad, GstPadProbeInfo *info,
                                          gpointer data)
{
    GstQuery *query = GST_PAD_PROBE_INFO_QUERY(info);
    (void)pad;
    (void)data;
    if (GST_QUERY_TYPE(query) == GST_QUERY_ALLOCATION) {
        /* Accept the OMX pool's real layout. A generic downstream copy can
         * lose vendor crop metadata and obscure the padded chroma offset. */
        gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
        gst_query_add_allocation_meta(query, GST_VIDEO_CROP_META_API_TYPE, NULL);
        return GST_PAD_PROBE_HANDLED;
    }
    return GST_PAD_PROBE_OK;
}

cedar_decoder *cedar_open(enum cedar_codec codec, unsigned width, unsigned height,
                          char *error, size_t error_size)
{
    cedar_decoder *d = NULL;
    GstElement *parser = NULL, *decoder = NULL, *gate = NULL;
    GstCaps *caps = NULL;
    GstPad *pad;
    GError *init_error = NULL;
    const char *media;
    if (error && error_size)
        error[0] = 0;
    if ((codec != CEDAR_H264 && codec != CEDAR_HEVC) || !width || !height ||
        width > 1920 || height > 1080) {
        if (error && error_size)
            snprintf(error, error_size, "unsupported codec/dimensions (max 1920x1080)");
        return NULL;
    }
    if (!gst_init_check(NULL, NULL, &init_error)) {
        if (error && error_size)
            snprintf(error, error_size, "%s", init_error->message);
        g_clear_error(&init_error);
        return NULL;
    }
    d = calloc(1, sizeof(*d));
    if (!d) {
        if (error && error_size)
            snprintf(error, error_size, "out of memory");
        return NULL;
    }
    d->width = width;
    d->height = height;
    d->discontinuity = TRUE;
    d->pipeline = gst_pipeline_new("t736-cedar-backend");
    d->source = gst_element_factory_make("appsrc", "compressed");
    d->sink = gst_element_factory_make("appsink", "decoded");
    parser = gst_element_factory_make(codec == CEDAR_H264 ? "h264parse" : "h265parse", "parser");
    gate = gst_element_factory_make("capsfilter", "parsed-format-limit");
    decoder = gst_element_factory_make(codec == CEDAR_H264 ? "omxh264dec" : "omxhevcvideodec", "cedar");
    if (!d->pipeline || !d->source || !d->sink || !parser || !gate || !decoder) {
        fail(d, "missing GStreamer appsrc/appsink/parser/private OMX plugin");
        goto unowned_failure;
    }
    gst_bin_add_many(GST_BIN(d->pipeline), d->source, parser, gate, decoder, d->sink, NULL);
    d->decoder = decoder; /* Borrowed; pipeline owns the decoder. */
    d->bus = gst_element_get_bus(d->pipeline);
    {
        GSignalQuery query = {0};
        guint signal = g_signal_lookup("t736-set-idle", G_OBJECT_TYPE(decoder));
        if (signal)
            g_signal_query(signal, &query);
        if (!signal || query.return_type != G_TYPE_BOOLEAN || query.n_params != 1 ||
            query.param_types[0] != G_TYPE_BOOLEAN) {
            fail(d, "private WC-v4 OMX idle action is missing/incompatible");
            goto failure;
        }
    }
    media = codec == CEDAR_H264 ? "video/x-h264" : "video/x-h265";
    caps = gst_caps_new_simple(media, "stream-format", G_TYPE_STRING, "byte-stream",
                              "alignment", G_TYPE_STRING, "au", NULL);
    gst_app_src_set_caps(GST_APP_SRC(d->source), caps);
    gst_caps_unref(caps);
    g_object_set(d->source, "format", GST_FORMAT_TIME, "is-live", FALSE,
                 "stream-type", GST_APP_STREAM_TYPE_SEEKABLE, "block", FALSE,
                 "max-bytes", (guint64)CEDAR_MAX_INPUT, NULL);
    g_signal_connect(d->source, "seek-data", G_CALLBACK(seek_data), d);
    /* Do not put requested width/height on appsrc: these parsers explicitly
     * let upstream dimensions override the SPS. Derive them from the bitstream
     * and reject incompatible caps before OMX can allocate a new FBM. */
    g_object_set(parser, "config-interval", -1, "disable-passthrough", TRUE, NULL);
    caps = gst_caps_new_simple(media, "stream-format", G_TYPE_STRING, "byte-stream",
                              "alignment", G_TYPE_STRING, "au",
                              "width", G_TYPE_INT, (int)width,
                              "height", G_TYPE_INT, (int)height,
                              "chroma-format", G_TYPE_STRING, "4:2:0",
                              "bit-depth-luma", G_TYPE_UINT, 8U,
                              "bit-depth-chroma", G_TYPE_UINT, 8U, NULL);
    g_object_set(gate, "caps", caps, NULL);
    gst_caps_unref(caps);
    g_object_set(decoder, "afbc", FALSE, "disable-dma-feature", TRUE, NULL);
    caps = gst_caps_from_string("video/x-raw,format=(string){NV21,NV12,I420,YV12}");
    gst_app_sink_set_caps(GST_APP_SINK(d->sink), caps);
    gst_caps_unref(caps);
    g_object_set(d->sink, "sync", FALSE, "async", FALSE, "max-buffers", 4U,
                 "drop", FALSE, "enable-last-sample", FALSE,
                 "wait-on-eos", FALSE, NULL);
    pad = gst_element_get_static_pad(d->sink, "sink");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                      allocation_query, d, NULL);
    gst_object_unref(pad);
    if (!gst_element_link_many(d->source, parser, gate, decoder, d->sink, NULL)) {
        fail(d, "cannot link private Cedar decode pipeline");
        goto failure;
    }
    if (gst_element_set_state(d->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        check_bus(d);
        if (!d->failed)
            fail(d, "cannot start private Cedar decode pipeline");
        goto failure;
    }
    return d;

unowned_failure:
    if (d->source) gst_object_unref(d->source);
    if (d->sink) gst_object_unref(d->sink);
    if (parser) gst_object_unref(parser);
    if (gate) gst_object_unref(gate);
    if (decoder) gst_object_unref(decoder);
    d->source = d->sink = NULL;
failure:
    if (error && error_size)
        snprintf(error, error_size, "%s", d->error);
    cedar_close(d);
    return NULL;
}

int cedar_push(cedar_decoder *d, const void *data, size_t size,
               int64_t pts_ns, int64_t dts_ns)
{
    GstBuffer *buffer;
    GstFlowReturn flow;
    guint64 queued;
    if (!d || check_bus(d) < 0)
        return CEDAR_ERROR;
    if (d->idle && cedar_resume(d) < 0)
        return CEDAR_ERROR;
    if (!data || !size || size > CEDAR_MAX_INPUT || d->eos)
        return fail(d, "invalid input or push after EOS");
    if ((pts_ns < 0 && pts_ns != CEDAR_NOPTS) ||
        (dts_ns < 0 && dts_ns != CEDAR_NOPTS))
        return fail(d, "negative timestamp; normalize stream timestamps before push");
    queued = gst_app_src_get_current_level_bytes(GST_APP_SRC(d->source));
    if (queued > CEDAR_MAX_INPUT - size)
        return CEDAR_BACKPRESSURE;
    buffer = gst_buffer_new_allocate(NULL, size, NULL);
    if (!buffer)
        return fail(d, "input allocation failed");
    gst_buffer_fill(buffer, 0, data, size);
    GST_BUFFER_PTS(buffer) = pts_ns == CEDAR_NOPTS ? GST_CLOCK_TIME_NONE : (guint64)pts_ns;
    GST_BUFFER_DTS(buffer) = dts_ns == CEDAR_NOPTS ? GST_CLOCK_TIME_NONE : (guint64)dts_ns;
    if (d->discontinuity) {
        GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
        d->discontinuity = FALSE;
    }
    flow = gst_app_src_push_buffer(GST_APP_SRC(d->source), buffer);
    if (flow != GST_FLOW_OK)
        return fail(d, "appsrc push failed: %s", gst_flow_get_name(flow));
    return 0;
}

static gboolean plane_span_valid(const GstVideoFrame *frame, unsigned plane,
                                  unsigned x, unsigned y, unsigned bytes,
                                  unsigned rows)
{
    const GstMapInfo *map = &frame->map[frame->meta ? plane : 0];
    uintptr_t start = (uintptr_t)GST_VIDEO_FRAME_PLANE_DATA(frame, plane);
    uintptr_t base = (uintptr_t)map->data;
    int stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, plane);
    uint64_t extent;
    if (!map->data || !rows || !bytes || stride <= 0 || start < base ||
        start - base > map->size)
        return FALSE;
    /* VideoMeta's default map checks only the first byte of each plane, not
     * stride * height. Bound every last byte that our crop copy will read. */
    extent = ((uint64_t)y + rows - 1) * (unsigned)stride + x + bytes;
    return extent <= map->size - (start - base);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void split_uv_row(uint8_t *du, uint8_t *dv, const uint8_t *source,
                          unsigned count, unsigned uoff)
{
    unsigned column = 0;
#ifdef __aarch64__
    uint8_t *even = uoff ? dv : du;
    uint8_t *odd = uoff ? du : dv;
    /* The private dma_mmap_wc backing is Normal Non-cacheable memory.
     * Load each interleaved block once, instead of issuing individual byte
     * loads to WC memory. No GPU/foreign-buffer import is involved. */
    for (; column + 16 <= count; column += 16) {
        uint8x16x2_t chroma = vld2q_u8(source + 2*column);
        vst1q_u8(even + column, chroma.val[0]);
        vst1q_u8(odd + column, chroma.val[1]);
    }
#endif
    for (; column < count; column++) {
        du[column] = source[2*column + uoff];
        dv[column] = source[2*column + 1 - uoff];
    }
}

static int copy_sample(cedar_decoder *d, GstSample *sample, cedar_frame *out)
{
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    GstVideoFrame frame;
    GstVideoCropMeta *crop;
    GstVideoFormat format;
    unsigned x, y, w, h, cw, ch, row;
    size_t ysize, csize;
    uint8_t *dy, *du, *dv;
    const uint8_t *sy, *su, *sv;
    int ys, us, vs;
    int result = CEDAR_ERROR;
    uint64_t map_begin, map_end, copy_begin, copy_end;
    if (!buffer || !caps || !gst_video_info_from_caps(&info, caps))
        return fail(d, "decoded sample lacks valid raw-video caps");
    if (gst_buffer_get_size(buffer) > CEDAR_MAX_OUTPUT ||
        GST_VIDEO_INFO_IS_INTERLACED(&info))
        return fail(d, "unsupported interlaced/oversized decoded sample");
    crop = gst_buffer_get_video_crop_meta(buffer);
    if (!crop)
        return fail(d, "decoded sample lacks authoritative GstVideoCropMeta (use WC-v3)");
    x = crop->x; y = crop->y; w = crop->width; h = crop->height;
    if (!w || !h || w > 1920 || h > 1080 || x > (unsigned)info.width ||
        y > (unsigned)info.height || w > (unsigned)info.width - x ||
        h > (unsigned)info.height - y || (x & 1) || (y & 1))
        return fail(d, "invalid/out-of-range or odd-origin 4:2:0 crop");
    if (w != d->width || h != d->height)
        return fail(d, "stream resolution changed: expected %ux%u, got %ux%u",
                    d->width, d->height, w, h);
    format = GST_VIDEO_INFO_FORMAT(&info);
    if (format != GST_VIDEO_FORMAT_NV12 && format != GST_VIDEO_FORMAT_NV21 &&
        format != GST_VIDEO_FORMAT_I420 && format != GST_VIDEO_FORMAT_YV12)
        return fail(d, "unsupported decoded pixel format");
    map_begin = monotonic_ns();
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ))
        return fail(d, "cannot map decoded CPU-readable frame");
    map_end = monotonic_ns();
    cw = (w + 1) / 2; ch = (h + 1) / 2;
    ys = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    us = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
    vs = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2);
    if (ys <= 0 || us <= 0 || (unsigned)ys < x + w ||
        ((format == GST_VIDEO_FORMAT_NV12 || format == GST_VIDEO_FORMAT_NV21) ?
         (unsigned)us < x + 2*cw :
         ((unsigned)us < x/2 + cw || vs <= 0 || (unsigned)vs < x/2 + cw))) {
        fail(d, "invalid raw-video plane strides");
        goto done;
    }
    if (!plane_span_valid(&frame, 0, x, y, w, h) ||
        ((format == GST_VIDEO_FORMAT_NV12 || format == GST_VIDEO_FORMAT_NV21) ?
         !plane_span_valid(&frame, 1, x, y/2, 2*cw, ch) :
         (!plane_span_valid(&frame, 1, x/2, y/2, cw, ch) ||
          !plane_span_valid(&frame, 2, x/2, y/2, cw, ch)))) {
        fail(d, "raw-video crop extends beyond a mapped plane");
        goto done;
    }
    ysize = (size_t)w*h;
    csize = (size_t)cw*ch;
    out->size = ysize + 2*csize;
    copy_begin = monotonic_ns();
    out->data = ram_acquire(d, out->size);
    if (!out->data) {
        fail(d, "I420 output allocation failed");
        goto done;
    }
    dy = out->data; du = dy + ysize; dv = du + csize;
    sy = (const uint8_t *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) + (size_t)y*ys + x;
    for (row = 0; row < h; row++)
        memcpy(dy + (size_t)row*w, sy + (size_t)row*ys, w);
    su = GST_VIDEO_FRAME_PLANE_DATA(&frame, 1);
    if (format == GST_VIDEO_FORMAT_NV12 || format == GST_VIDEO_FORMAT_NV21) {
        unsigned uoff = format == GST_VIDEO_FORMAT_NV12 ? 0 : 1;
        su += (size_t)(y/2)*us + x;
        for (row = 0; row < ch; row++) {
            const uint8_t *source = su + (size_t)row*us;
            split_uv_row(du + (size_t)row*cw, dv + (size_t)row*cw,
                         source, cw, uoff);
        }
    } else {
        sv = GST_VIDEO_FRAME_PLANE_DATA(&frame, 2);
        su += (size_t)(y/2)*us + x/2;
        sv += (size_t)(y/2)*vs + x/2;
        if (format == GST_VIDEO_FORMAT_YV12) {
            const uint8_t *swap = su; su = sv; sv = swap;
            int stride = us; us = vs; vs = stride;
        }
        for (row = 0; row < ch; row++) {
            memcpy(du + (size_t)row*cw, su + (size_t)row*us, cw);
            memcpy(dv + (size_t)row*cw, sv + (size_t)row*vs, cw);
        }
    }
    copy_end = monotonic_ns();
    d->copied_frames++;
    d->total_map_ns += map_end >= map_begin ? map_end - map_begin : 0;
    d->total_copy_ns += copy_end >= copy_begin ? copy_end - copy_begin : 0;
    if (d->copied_frames == 1 || d->copied_frames % 300 == 0) {
        fprintf(stderr, "CEDAR_COPY frames=%" PRIu64 " format=%s storage=%dx%d "
                "crop=%u,%u,%u,%u strides=%d,%d,%d map_us=%" PRIu64
                " copy_us=%" PRIu64 " total_map_us=%" PRIu64
                " total_copy_us=%" PRIu64 " avg_copy_us=%" PRIu64
                " ram_allocations=%" PRIu64 " ram_reuses=%" PRIu64 "\n",
                d->copied_frames, gst_video_format_to_string(format),
                info.width, info.height, x, y, w, h,
                GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0),
                GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1),
                GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2),
                (map_end >= map_begin ? map_end-map_begin : 0)/1000,
                (copy_end >= copy_begin ? copy_end-copy_begin : 0)/1000,
                d->total_map_ns/1000, d->total_copy_ns/1000,
                d->total_copy_ns/d->copied_frames/1000,
                d->ram_pool->allocations, d->ram_pool->reuses);
    }
    out->width = w; out->height = h;
    out->pts_ns = GST_BUFFER_PTS_IS_VALID(buffer) && GST_BUFFER_PTS(buffer) <= INT64_MAX ?
                  (int64_t)GST_BUFFER_PTS(buffer) : CEDAR_NOPTS;
    out->dts_ns = GST_BUFFER_DTS_IS_VALID(buffer) && GST_BUFFER_DTS(buffer) <= INT64_MAX ?
                  (int64_t)GST_BUFFER_DTS(buffer) : CEDAR_NOPTS;
    out->color_range = info.colorimetry.range == GST_VIDEO_COLOR_RANGE_16_235 ? 1 :
                       info.colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ? 2 : 0;
    out->color_matrix = info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? 1 :
                        info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT601 ? 2 :
                        info.colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT2020 ? 3 : 0;
    result = CEDAR_FRAME;
done:
    gst_video_frame_unmap(&frame);
    return result;
}

int cedar_pull(cedar_decoder *d, cedar_frame *out, unsigned timeout_ms)
{
    GstSample *sample;
    int result;
    if (!d || !out || check_bus(d) < 0)
        return CEDAR_ERROR;
    memset(out, 0, sizeof(*out));
    if (d->idle)
        return CEDAR_AGAIN;
    if (timeout_ms > 10000)
        return fail(d, "pull timeout exceeds 10000 ms");
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(d->sink),
                                          (GstClockTime)timeout_ms * GST_MSECOND);
    if (!sample) {
        if (check_bus(d) < 0)
            return CEDAR_ERROR;
        return gst_app_sink_is_eos(GST_APP_SINK(d->sink)) ? CEDAR_EOF : CEDAR_AGAIN;
    }
    result = copy_sample(d, sample, out);
    gst_sample_unref(sample);
    return result;
}

int cedar_eos(cedar_decoder *d)
{
    GstFlowReturn result;
    if (!d || check_bus(d) < 0)
        return CEDAR_ERROR;
    if (d->eos)
        return 0;
    result = gst_app_src_end_of_stream(GST_APP_SRC(d->source));
    if (result != GST_FLOW_OK)
        return fail(d, "cannot send EOS: %s", gst_flow_get_name(result));
    d->eos = TRUE;
    return 0;
}

int cedar_flush(cedar_decoder *d)
{
    if (!d || check_bus(d) < 0)
        return CEDAR_ERROR;
    if (d->idle && cedar_resume(d) < 0)
        return CEDAR_ERROR;
    /* GstBaseSrc's flushing seek stops/restarts its streaming task, clears
     * appsrc EOS/queues, and propagates flush through parser/OMX/appsink.
     * Crucially it does not transition the pipeline to READY/NULL. */
    if (!gst_element_seek_simple(d->pipeline, GST_FORMAT_TIME,
                                 GST_SEEK_FLAG_FLUSH, 0))
        return fail(d, "pipeline flush seek failed; retain failed instance");
    d->eos = FALSE;
    d->discontinuity = TRUE;
    return check_bus(d);
}

int cedar_idle(cedar_decoder *d)
{
    gboolean accepted = FALSE;
    GstState state = GST_STATE_VOID_PENDING;
    GstStateChangeReturn result;
    if (!d || check_bus(d) < 0)
        return CEDAR_ERROR;
    if (d->idle)
        return 0;
    if (cedar_flush(d) < 0)
        return CEDAR_ERROR;
    result = gst_element_set_state(d->pipeline, GST_STATE_PAUSED);
    if (result == GST_STATE_CHANGE_FAILURE)
        return fail(d, "cannot pause idle Gst pipeline");
    result = gst_element_get_state(d->pipeline, &state, NULL, 5 * GST_SECOND);
    if (result == GST_STATE_CHANGE_FAILURE || result == GST_STATE_CHANGE_ASYNC ||
        state != GST_STATE_PAUSED)
        return fail(d, "idle Gst pause did not complete within 5 seconds");
    /* Upstream gst-omx deliberately runs OMX in Executing during Gst PAUSED.
     * The private action is therefore necessary to stop the vendor no-input
     * DecodeVideoStream/2ms warning loop. It does not destroy the component. */
    g_signal_emit_by_name(d->decoder, "t736-set-idle", TRUE, &accepted);
    if (!accepted) {
        if (check_bus(d) < 0)
            return CEDAR_ERROR;
        return fail(d, "private OMX idle transition failed; retain instance");
    }
    d->idle = TRUE;
    return check_bus(d);
}

int cedar_resume(cedar_decoder *d)
{
    gboolean accepted = FALSE;
    if (!d || check_bus(d) < 0)
        return CEDAR_ERROR;
    if (!d->idle)
        return 0;
    g_signal_emit_by_name(d->decoder, "t736-set-idle", FALSE, &accepted);
    if (!accepted) {
        if (check_bus(d) < 0)
            return CEDAR_ERROR;
        return fail(d, "private OMX resume transition failed; retain instance");
    }
    if (gst_element_set_state(d->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        return fail(d, "cannot resume retained Gst pipeline");
    d->idle = FALSE;
    d->discontinuity = TRUE;
    return check_bus(d);
}

void cedar_frame_free(cedar_frame *frame)
{
    if (frame) {
        ram_release(frame->data);
        memset(frame, 0, sizeof(*frame));
    }
}

const char *cedar_error(const cedar_decoder *d)
{
    return d ? d->error : "null Cedar decoder";
}

void cedar_close(cedar_decoder *d)
{
    if (!d) return;
    if (d->pipeline) {
        gst_element_set_state(d->pipeline, GST_STATE_NULL);
        gst_object_unref(d->pipeline);
    }
    if (d->bus) gst_object_unref(d->bus);
    ram_pool_close(d);
    free(d);
}
