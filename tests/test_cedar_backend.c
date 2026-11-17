/* SPDX-License-Identifier: MIT
 * Exercise the actual backend copy path with synthetic, padded Gst samples.
 * No hardware, decoder plugin, device descriptor, or vendor library is used.
 */
#include "../tools/browser/cedar_backend.c"
#include <assert.h>
#include <sys/resource.h>

static GstSample *sample_for(GstVideoFormat fmt, gboolean with_crop,
                             unsigned x, unsigned y, unsigned w, unsigned h)
{
    GstVideoInfo info;
    GstCaps *caps;
    GstBuffer *buffer = gst_buffer_new_allocate(NULL, 144, NULL);
    GstMapInfo map;
    GstVideoCropMeta *crop;
    gsize offsets[GST_VIDEO_MAX_PLANES] = {0, 96, 120, 0};
    gint strides[GST_VIDEO_MAX_PLANES] = {12, 6, 6, 0};
    guint planes = 3;
    GstSample *sample;
    assert(buffer);
    gst_video_info_set_format(&info, fmt, 8, 8);
    info.colorimetry.range = GST_VIDEO_COLOR_RANGE_16_235;
    info.colorimetry.matrix = GST_VIDEO_COLOR_MATRIX_BT709;
    if (fmt == GST_VIDEO_FORMAT_NV12 || fmt == GST_VIDEO_FORMAT_NV21) {
        planes = 2;
        strides[1] = 12;
    }
    assert(gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                          fmt, 8, 8, planes, offsets, strides));
    assert(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
    memset(map.data, 0xee, map.size);
    for (unsigned row = 0; row < 8; row++)
        for (unsigned col = 0; col < 8; col++)
            map.data[row*12 + col] = (uint8_t)(10 + row*8 + col);
    for (unsigned row = 0; row < 4; row++) {
        for (unsigned col = 0; col < 4; col++) {
            uint8_t u = (uint8_t)(80 + row*4 + col);
            uint8_t v = (uint8_t)(160 + row*4 + col);
            if (planes == 2) {
                unsigned uoff = fmt == GST_VIDEO_FORMAT_NV12 ? 0 : 1;
                map.data[96 + row*12 + col*2 + uoff] = u;
                map.data[96 + row*12 + col*2 + 1-uoff] = v;
            } else {
                map.data[(fmt == GST_VIDEO_FORMAT_I420 ? 96 : 120) + row*6 + col] = u;
                map.data[(fmt == GST_VIDEO_FORMAT_I420 ? 120 : 96) + row*6 + col] = v;
            }
        }
    }
    gst_buffer_unmap(buffer, &map);
    if (with_crop) {
        crop = gst_buffer_add_video_crop_meta(buffer);
        crop->x = x; crop->y = y; crop->width = w; crop->height = h;
    }
    GST_BUFFER_PTS(buffer) = 123456789;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    caps = gst_video_info_to_caps(&info);
    sample = gst_sample_new(buffer, caps, NULL, NULL);
    gst_buffer_unref(buffer);
    gst_caps_unref(caps);
    return sample;
}

static void test_format(GstVideoFormat fmt, unsigned w, unsigned h)
{
    cedar_decoder d = {.width = w, .height = h};
    cedar_frame out = {0};
    GstSample *sample = sample_for(fmt, TRUE, 2, 2, w, h);
    unsigned cw = (w+1)/2, ch = (h+1)/2;
    assert(copy_sample(&d, sample, &out) == CEDAR_FRAME);
    assert(out.width == w && out.height == h);
    assert(out.size == w*h + 2*cw*ch);
    assert(out.pts_ns == 123456789 && out.dts_ns == CEDAR_NOPTS);
    assert(out.color_range == 1 && out.color_matrix == 1);
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            assert(out.data[y*w+x] == 10 + (y+2)*8 + x+2);
    for (unsigned y = 0; y < ch; y++) {
        for (unsigned x = 0; x < cw; x++) {
            assert(out.data[w*h + y*cw+x] == 80 + (y+1)*4 + x+1);
            assert(out.data[w*h + cw*ch + y*cw+x] == 160 + (y+1)*4 + x+1);
        }
    }
    cedar_frame_free(&out);
    assert(out.data == NULL && out.size == 0);
    ram_pool_close(&d);
    gst_sample_unref(sample);
}

static void queue_raw_sample(cedar_decoder *d, unsigned bias)
{
    GstSample *sample = sample_for(GST_VIDEO_FORMAT_YV12, TRUE, 2, 2, 4, 4);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    assert(gst_buffer_map(buffer, &map, GST_MAP_WRITE));
    for (size_t i = 0; i < map.size; i++)
        map.data[i] += bias;
    gst_buffer_unmap(buffer, &map);
    assert(gst_app_src_push_sample(GST_APP_SRC(d->source), sample) == GST_FLOW_OK);
    gst_sample_unref(sample);
}

static void test_owned_frames_across_pull_flush_close(void)
{
    cedar_decoder *d = calloc(1, sizeof(*d));
    cedar_frame first = {0}, second = {0}, third = {0};
    uint8_t expected[24];
    assert(d);
    d->width = d->height = 4;
    d->pipeline = gst_pipeline_new("owned-output-test");
    d->source = g_object_new(GST_TYPE_APP_SRC, "format", GST_FORMAT_TIME,
                            "stream-type", GST_APP_STREAM_TYPE_SEEKABLE, NULL);
    d->sink = g_object_new(GST_TYPE_APP_SINK, "sync", FALSE, "async", FALSE,
                          "wait-on-eos", FALSE, NULL);
    g_signal_connect(d->source, "seek-data", G_CALLBACK(seek_data), d);
    gst_bin_add_many(GST_BIN(d->pipeline), d->source, d->sink, NULL);
    assert(gst_element_link(d->source, d->sink));
    d->bus = gst_element_get_bus(d->pipeline);
    assert(gst_element_set_state(d->pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    queue_raw_sample(d, 0);
    assert(cedar_pull(d, &first, 1000) == CEDAR_FRAME && first.size == sizeof(expected));
    memcpy(expected, first.data, sizeof(expected));
    queue_raw_sample(d, 1);
    assert(cedar_pull(d, &second, 1000) == CEDAR_FRAME);
    assert(first.data != second.data && !memcmp(first.data, expected, sizeof(expected)));
    for (size_t i = 0; i < sizeof(expected); i++) assert(second.data[i] == (uint8_t)(expected[i] + 1));
    assert(cedar_flush(d) == 0);
    assert(!memcmp(first.data, expected, sizeof(expected)));
    queue_raw_sample(d, 2);
    assert(cedar_pull(d, &third, 1000) == CEDAR_FRAME);
    assert(third.data != first.data && third.data != second.data);
    assert(!memcmp(first.data, expected, sizeof(expected)));
    cedar_frame_free(&third); /* Cached block is destroyed by close. */
    cedar_close(d);          /* Outstanding first/second retain the pool. */
    assert(!memcmp(first.data, expected, sizeof(expected)));
    for (size_t i = 0; i < sizeof(expected); i++) assert(second.data[i] == (uint8_t)(expected[i] + 1));
    cedar_frame_free(&second);
    assert(!memcmp(first.data, expected, sizeof(expected)));
    cedar_frame_free(&first);
}

static gpointer release_frame_thread(gpointer frame)
{
    cedar_frame_free(frame);
    return NULL;
}

static void test_ram_reuse_and_lifetime(void)
{
    cedar_decoder d = {0};
    cedar_frame frames[8] = {{0}};
    uint8_t *first = NULL;
    for (unsigned i = 0; i < 1000; i++) {
        cedar_frame f = {.data = ram_acquire(&d, 3110400)};
        assert(f.data && !((uintptr_t)f.data & 63));
        if (!i) first = f.data;
        assert(f.data == first);
        f.data[0] = (uint8_t)i;
        f.data[3110399] = (uint8_t)(i + 1);
        cedar_frame_free(&f);
    }
    assert(d.ram_pool->allocations == 1 && d.ram_pool->reuses == 999);
    assert(d.ram_pool->cached == 1 && g_atomic_int_get(&d.ram_pool->references) == 1);
    for (unsigned i = 0; i < 8; i++) {
        frames[i].data = ram_acquire(&d, 3110400);
        assert(frames[i].data);
        for (unsigned j = 0; j < i; j++) assert(frames[i].data != frames[j].data);
    }
    for (unsigned i = 0; i < 8; i++) cedar_frame_free(&frames[i]);
    assert(d.ram_pool->cached == CEDAR_RAM_CACHE_LIMIT);
    assert(g_atomic_int_get(&d.ram_pool->references) == 1);
    ram_pool_close(&d);
    for (unsigned trial = 0; trial < 64; trial++) {
        cedar_decoder *owned = calloc(1, sizeof(*owned));
        cedar_frame a = {.data = ram_acquire(owned, 4096)};
        cedar_frame b = {.data = ram_acquire(owned, 4096)};
        assert(a.data && b.data && a.data != b.data);
        GThread *t1 = g_thread_new("frame-return-a", release_frame_thread, &a);
        GThread *t2 = g_thread_new("frame-return-b", release_frame_thread, &b);
        cedar_close(owned);
        g_thread_join(t1); g_thread_join(t2);
        assert(!a.data && !b.data);
    }
}

static int ram_benchmark(gboolean use_pool)
{
    const size_t size = 1920U * 1080U * 3U / 2U;
    const unsigned count = 300;
    cedar_decoder d = {0};
    uint8_t *source = malloc(size);
    struct rusage before, after;
    volatile uint64_t checksum = 0;
    assert(source);
    memset(source, 0x5a, size);
    assert(getrusage(RUSAGE_SELF, &before) == 0);
    uint64_t start = monotonic_ns();
    for (unsigned i = 0; i < count; i++) {
        uint8_t *data = use_pool ? ram_acquire(&d, size) : malloc(size);
        assert(data);
        memcpy(data, source, size);
        checksum += data[(i * 4099U) % size];
        if (use_pool) ram_release(data); else free(data);
    }
    uint64_t elapsed = monotonic_ns() - start;
    assert(getrusage(RUSAGE_SELF, &after) == 0);
    printf("RAM_BENCH mode=%s frames=%u bytes=%zu elapsed_us=%" PRIu64
           " minor_faults=%ld major_faults=%ld allocations=%" PRIu64
           " reuses=%" PRIu64 " checksum=%" PRIu64 " scope=ordinary-host-RAM-not-WC\n",
           use_pool ? "pool" : "malloc", count, size, elapsed/1000,
           after.ru_minflt-before.ru_minflt, after.ru_majflt-before.ru_majflt,
           use_pool ? d.ram_pool->allocations : count,
           use_pool ? d.ram_pool->reuses : 0, (uint64_t)checksum);
    ram_pool_close(&d);
    free(source);
    return 0;
}

typedef struct idle_test_state {
    cedar_decoder *decoder;
    unsigned idle_calls, resume_calls;
    gboolean reject;
} idle_test_state;

static gboolean mock_idle_action(GstElement *element, gboolean idle, gpointer data)
{
    idle_test_state *test = data;
    GstState state;
    (void)element;
    assert(gst_element_get_state(test->decoder->pipeline, &state, NULL, GST_SECOND)
           != GST_STATE_CHANGE_FAILURE);
    assert(state == GST_STATE_PAUSED);
    if (test->reject) {
        GError *error = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED,
                                             "OMX state=Executing started=1 port=1");
        gst_element_post_message(element,
            gst_message_new_error(GST_OBJECT(element), error,
                                   "raw_state=3 pending=0 last_error=0x123"));
        g_error_free(error);
        return FALSE;
    }
    if (idle) test->idle_calls++;
    else test->resume_calls++;
    return TRUE;
}

static void test_flush_eos_reuse(void)
{
    cedar_decoder d = {0};
    idle_test_state test = {.decoder = &d};
    GstSample *sample;
    const uint8_t first[] = {1, 2, 3, 4}, second[] = {9, 8, 7, 6};
    uint8_t actual[4];
    d.pipeline = gst_pipeline_new("software-flush-test");
    d.source = g_object_new(GST_TYPE_APP_SRC, "format", GST_FORMAT_TIME,
                            "stream-type", GST_APP_STREAM_TYPE_SEEKABLE, NULL);
    d.sink = g_object_new(GST_TYPE_APP_SINK, "sync", FALSE, "async", FALSE,
                          "wait-on-eos", FALSE, NULL);
    /* A software-only stand-in verifies backend ordering and object retention.
     * It does not simulate or establish the vendor OMX action's behavior. */
    if (!g_signal_lookup("t736-set-idle", GST_TYPE_ELEMENT))
        g_signal_new("t736-set-idle", GST_TYPE_ELEMENT, G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL, G_TYPE_BOOLEAN, 1, G_TYPE_BOOLEAN);
    d.decoder = d.source;
    g_signal_connect(d.decoder, "t736-set-idle", G_CALLBACK(mock_idle_action), &test);
    g_signal_connect(d.source, "seek-data", G_CALLBACK(seek_data), &d);
    gst_bin_add_many(GST_BIN(d.pipeline), d.source, d.sink, NULL);
    assert(gst_element_link(d.source, d.sink));
    d.bus = gst_element_get_bus(d.pipeline);
    assert(gst_element_set_state(d.pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    assert(cedar_push(&d, first, sizeof(first), 1, CEDAR_NOPTS) == 0);
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(d.sink), GST_SECOND);
    assert(sample);
    gst_sample_unref(sample);
    assert(cedar_eos(&d) == 0);
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(d.sink), GST_SECOND);
    assert(!sample && gst_app_sink_is_eos(GST_APP_SINK(d.sink)));
    assert(cedar_flush(&d) == 0);
    assert(!d.eos && !d.failed);
    assert(cedar_push(&d, second, sizeof(second), 2, CEDAR_NOPTS) == 0);
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(d.sink), GST_SECOND);
    if (!sample)
        fprintf(stderr, "flush error: %s\n", cedar_error(&d));
    assert(sample);
    assert(gst_buffer_extract(gst_sample_get_buffer(sample), 0, actual, sizeof(actual)) == sizeof(actual));
    assert(memcmp(actual, second, sizeof(actual)) == 0);
    assert(GST_BUFFER_PTS(gst_sample_get_buffer(sample)) == 2);
    gst_sample_unref(sample);
    assert(cedar_idle(&d) == 0 && d.idle);
    assert(cedar_idle(&d) == 0 && test.idle_calls == 1);
    assert(cedar_push(&d, first, sizeof(first), 3, CEDAR_NOPTS) == 0);
    assert(!d.idle && test.resume_calls == 1 && d.decoder == d.source);
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(d.sink), GST_SECOND);
    assert(sample);
    assert(gst_buffer_extract(gst_sample_get_buffer(sample), 0, actual, sizeof(actual)) == sizeof(actual));
    assert(memcmp(actual, first, sizeof(actual)) == 0);
    assert(GST_BUFFER_PTS(gst_sample_get_buffer(sample)) == 3);
    gst_sample_unref(sample);
    test.reject = TRUE;
    assert(cedar_idle(&d) == CEDAR_ERROR && d.failed && !d.idle);
    assert(strstr(cedar_error(&d), "started=1"));
    assert(strstr(cedar_error(&d), "last_error=0x123"));
    fail(&d, "later generic failure must not replace the first cause");
    assert(strstr(cedar_error(&d), "started=1"));
    assert(cedar_resume(&d) == CEDAR_ERROR);
    gst_element_set_state(d.pipeline, GST_STATE_NULL);
    gst_object_unref(d.pipeline);
    gst_object_unref(d.bus);
}

static void test_split_uv_vectors(void)
{
    unsigned sizes[] = {1, 15, 16, 17, 31, 32, 35, 960};
    uint8_t raw[1930], u[970], v[970];
    for (size_t n = 0; n < sizeof(sizes)/sizeof(sizes[0]); n++) {
        unsigned count = sizes[n];
        for (unsigned i = 0; i < 2*count; i++)
            raw[3+i] = (uint8_t)(i*17+11);
        for (unsigned order = 0; order < 2; order++) {
            memset(u, 0xee, sizeof(u)); memset(v, 0xee, sizeof(v));
            split_uv_row(u+5, v+7, raw+3, count, order);
            for (unsigned i = 0; i < count; i++) {
                assert(u[5+i] == raw[3+2*i+order]);
                assert(v[7+i] == raw[3+2*i+1-order]);
            }
            assert(u[4] == 0xee && u[5+count] == 0xee);
            assert(v[6] == 0xee && v[7+count] == 0xee);
        }
    }
}

int main(int argc, char **argv)
{
    GstVideoFormat formats[] = { GST_VIDEO_FORMAT_NV21, GST_VIDEO_FORMAT_NV12,
                                GST_VIDEO_FORMAT_I420, GST_VIDEO_FORMAT_YV12 };
    gst_init(&argc, &argv);
    if (argc == 2 && !strcmp(argv[1], "--ram-bench-malloc")) return ram_benchmark(FALSE);
    if (argc == 2 && !strcmp(argv[1], "--ram-bench-pool")) return ram_benchmark(TRUE);
    for (size_t i = 0; i < sizeof(formats)/sizeof(formats[0]); i++) {
        test_format(formats[i], 4, 4);
        test_format(formats[i], 5, 3);
    }
    for (unsigned bad = 0; bad < 4; bad++) {
        cedar_decoder d = {.width = 4, .height = 4};
        cedar_frame out = {0};
        GstSample *sample = sample_for(GST_VIDEO_FORMAT_NV21, bad != 0,
                                       bad == 1 ? 1 : 2, 2,
                                       bad == 2 ? 9 : bad == 3 ? 2 : 4, 4);
        assert(copy_sample(&d, sample, &out) == CEDAR_ERROR);
        assert(d.failed && out.data == NULL);
        gst_sample_unref(sample);
    }
    for (unsigned bad = 0; bad < 3; bad++) {
        cedar_decoder d = {.width = 4, .height = 4};
        cedar_frame out = {0};
        GstSample *sample = sample_for(GST_VIDEO_FORMAT_NV21, TRUE, 2, 2, 4, 4);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
        if (bad == 0)
            meta->stride[0] = 4096;
        else if (bad == 1)
            meta->offset[1] = 140;
        else
            gst_buffer_resize(buffer, 0, 100);
        assert(copy_sample(&d, sample, &out) == CEDAR_ERROR);
        assert(d.failed && out.data == NULL);
        gst_sample_unref(sample);
    }
    test_flush_eos_reuse();
    test_split_uv_vectors();
    test_owned_frames_across_pull_flush_close();
    test_ram_reuse_and_lifetime();
    puts("PASS: actual Gst copy path; 4 layouts, padded strides/heights, crop origin, odd sizes, timestamps, color, malformed/missing crop, resolution change, mapped-plane span rejection; software EOS-flush-idle-resume-reuse and first-error preservation with mock action; UV vector/tail/unaligned ranges; owned frames survive actual pull/flush/close, bounded RAM reuse, concurrent frame return/close");
    return 0;
}
