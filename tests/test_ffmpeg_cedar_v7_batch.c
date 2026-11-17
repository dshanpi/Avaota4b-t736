/* Host-only audit of the actual ARM64 candidate library through public APIs.
 * Exported client stubs never connect to a broker or touch a device. */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
#include "cedar_client.h"

#define LIMIT 512
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned checks;
static struct {
    unsigned width, height, opens, closes, flushes, pushes, accepted, emitted;
    unsigned read, write, gate, capacity, queue_max, delay_us, blocked_pushes;
    unsigned startup_waits;
    int ready, startup64, eos;
    int64_t pts[LIMIT];
} stub;

int t736_client_available(void) { return 1; }
int t736_client_open(int codec, unsigned width, unsigned height)
{
    CHECK(codec == T736_CODEC_H264 || codec == T736_CODEC_HEVC);
    stub.opens++; stub.width = width; stub.height = height;
    return 1;
}
int t736_client_push(int session, const uint8_t *data, size_t size,
                     int64_t pts, int64_t dts)
{
    CHECK(session == 1 && data && size && dts == INT64_MIN);
    stub.pushes++;
    if (stub.ready && stub.capacity && stub.write - stub.read >= stub.capacity) {
        stub.blocked_pushes++;
        return -EAGAIN;
    }
    CHECK(stub.write < LIMIT && pts == (int64_t)stub.accepted * 1000);
    stub.pts[stub.write++] = pts;
    stub.accepted++;
    if (stub.write - stub.read > stub.queue_max)
        stub.queue_max = stub.write - stub.read;
    return 0;
}
int t736_client_pull(int session, uint8_t *data, size_t capacity,
                     struct t736_frame *frame, unsigned timeout_ms)
{
    CHECK(session == 1);
    if (!stub.ready) {
        if (stub.startup64) {
            if (stub.accepted < 64 || !timeout_ms) return 0;
            stub.startup_waits++;
            if (stub.startup_waits < 3) return 0;
        } else if (stub.accepted < stub.gate && !stub.eos) {
            return 0;
        }
        stub.ready = 1;
    }
    if (stub.read < stub.write) {
        size_t bytes = (size_t)stub.width * stub.height * 3 / 2;
        CHECK(capacity >= bytes);
        if (stub.delay_us) av_usleep(stub.delay_us);
        memset(data, (stub.read % 251) + 1, bytes);
        *frame = (struct t736_frame){.width = stub.width, .height = stub.height,
            .bytes_written = bytes, .color_range = 1, .colorspace = 6,
            .pts_us = stub.pts[stub.read++]};
        stub.emitted++;
        return 1;
    }
    return stub.eos ? 2 : 0;
}
int t736_client_drain(int session) { CHECK(session == 1); stub.eos = 1; return 0; }
int t736_client_flush(int session)
{
    CHECK(session == 1); stub.flushes++;
    stub.read = stub.write = stub.accepted = stub.emitted = 0;
    stub.ready = stub.eos = 0;
    return 0;
}
int t736_client_close(int session) { CHECK(session == 1); stub.closes++; return 0; }

typedef struct Stats {
    unsigned frames, nonempty_batches, max_batch, send_eagain, drain_frames;
    int64_t max_first_frame_wait_us, max_batch_us;
} Stats;

static AVCodecContext *context(enum AVCodecID id)
{
    const AVCodec *codec = avcodec_find_decoder(id);
    CHECK(codec && strstr(codec->name, "_cedar"));
    AVCodecContext *ctx = avcodec_alloc_context3(codec); CHECK(ctx);
    ctx->width = 16; ctx->height = 16;
    ctx->pkt_timebase = (AVRational){1, 1000000};
    ctx->thread_count = 1; ctx->thread_type = 0;
    CHECK(avcodec_open2(ctx, codec, NULL) == 0);
    return ctx;
}

static AVPacket *packet(enum AVCodecID id, unsigned number)
{
    static const uint8_t h264[] = {0, 0, 0, 1, 0x65, 0x88, 0x80};
    static const uint8_t hevc[] = {0, 0, 0, 1, 0x26, 1, 0x80};
    AVPacket *p = av_packet_alloc(); CHECK(p);
    CHECK(av_new_packet(p, sizeof(h264)) == 0);
    memcpy(p->data, id == AV_CODEC_ID_H264 ? h264 : hevc, sizeof(h264));
    p->pts = p->dts = (int64_t)number * 1000;
    p->duration = 1000; p->pos = number; p->flags = AV_PKT_FLAG_KEY;
    return p;
}

static int collect(AVCodecContext *ctx, Stats *stats, int draining,
                   int64_t batch_start, unsigned *batch_frames)
{
    AVFrame *frame = av_frame_alloc(); CHECK(frame);
    unsigned count = 0;
    int64_t first = 0;
    int ret;
    while ((ret = avcodec_receive_frame(ctx, frame)) == 0) {
        if (!count) first = av_gettime_relative();
        CHECK(frame->pts == (int64_t)stats->frames * 1000);
        CHECK(frame->best_effort_timestamp == frame->pts);
        CHECK(frame->pkt_dts == frame->pts && frame->duration == 1000);
        CHECK(frame->width == 16 && frame->height == 16);
        CHECK(frame->data[0][0] == (stats->frames % 251) + 1);
        CHECK(frame->data[2][7] == (stats->frames % 251) + 1);
        stats->frames++; count++;
        av_frame_unref(frame);
    }
    int64_t finish = av_gettime_relative();
    CHECK(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    if (draining) {
        CHECK(ret == AVERROR_EOF);
        stats->drain_frames += count;
    } else {
        CHECK(ret == AVERROR(EAGAIN));
        if (count) {
            stats->nonempty_batches++;
            if (count > stats->max_batch) stats->max_batch = count;
            if (finish - first > stats->max_first_frame_wait_us)
                stats->max_first_frame_wait_us = finish - first;
            if (finish - batch_start > stats->max_batch_us)
                stats->max_batch_us = finish - batch_start;
        }
    }
    if (batch_frames) *batch_frames = count;
    av_frame_free(&frame);
    return ret;
}

static void submit(AVCodecContext *ctx, enum AVCodecID codec, unsigned number,
                   Stats *stats, int immediate_collect)
{
    AVPacket *p = packet(codec, number);
    int64_t start = av_gettime_relative();
    int ret = avcodec_send_packet(ctx, p);
    if (ret == AVERROR(EAGAIN)) {
        unsigned emitted = 0;
        stats->send_eagain++;
        collect(ctx, stats, 0, start, &emitted);
        /* Public send EAGAIN must always permit receive progress. */
        CHECK(emitted > 0);
        ret = avcodec_send_packet(ctx, p);
    }
    CHECK(ret == 0);
    CHECK(p->pts == (int64_t)number * 1000 && p->dts == p->pts);
    if (immediate_collect) collect(ctx, stats, 0, start, NULL);
    av_packet_free(&p);
}

static void drain(AVCodecContext *ctx, Stats *stats, unsigned total)
{
    CHECK(avcodec_send_packet(ctx, NULL) == 0);
    collect(ctx, stats, 1, av_gettime_relative(), NULL);
    CHECK(stats->frames == total && stub.accepted == total && stub.emitted == total);
    CHECK(avcodec_send_packet(ctx, NULL) == AVERROR_EOF);
}

static void trial(const char *name, enum AVCodecID codec, unsigned gate,
                  unsigned capacity, int startup64, int accumulate_send,
                  int expect_bounded)
{
    memset(&stub, 0, sizeof(stub));
    stub.gate = gate; stub.capacity = capacity; stub.startup64 = startup64;
    stub.delay_us = 10000;
    AVCodecContext *ctx = context(codec);
    Stats stats = {0};
    unsigned total = startup64 ? 70 : 24;
    for (unsigned n = 0; n < total; n++)
        submit(ctx, codec, n, &stats, !accumulate_send);
    if (accumulate_send) collect(ctx, &stats, 0, av_gettime_relative(), NULL);
    if (expect_bounded && !capacity) CHECK(stats.max_batch <= 1);
    if (!expect_bounded && gate >= 8 && !capacity && !startup64)
        CHECK(stats.max_batch >= 8 && stats.max_first_frame_wait_us >= 60000);
    if (startup64) CHECK(stub.startup_waits == 3);
    if (expect_bounded && capacity) CHECK(stub.blocked_pushes > 0 && stats.max_batch > 1);
    if (accumulate_send) CHECK(stats.send_eagain > 0);
    drain(ctx, &stats, total);
    printf("case=%s codec=%d frames=%u max_normal_batch=%u max_first_frame_wait_us=%" PRId64
           " max_batch_us=%" PRId64 " send_eagain=%u backpressure=%u eos_frames=%u PASS\n",
           name, codec, stats.frames, stats.max_batch, stats.max_first_frame_wait_us,
           stats.max_batch_us, stats.send_eagain, stub.blocked_pushes, stats.drain_frames);

    avcodec_flush_buffers(ctx);
    CHECK(stub.flushes == 1);
    stats = (Stats){0}; stub.gate = 1; stub.capacity = 0; stub.startup64 = 0;
    for (unsigned n = 0; n < 3; n++) submit(ctx, codec, n, &stats, 1);
    CHECK(stats.frames == 3);
    drain(ctx, &stats, 3);
    avcodec_free_context(&ctx);
    CHECK(stub.opens == 1 && stub.closes == 1);
}

static void occupied_input_terminal(int flush)
{
    memset(&stub, 0, sizeof(stub));
    stub.gate = 1; stub.delay_us = 10000;
    AVCodecContext *ctx = context(AV_CODEC_ID_HEVC);
    Stats stats = {0};
    /* send0 leaves an output in FFmpeg buffer_frame; send1 then leaves an
     * unconsumed packet in its buffer_pkt, without entering the codec. */
    submit(ctx, AV_CODEC_ID_HEVC, 0, &stats, 0);
    submit(ctx, AV_CODEC_ID_HEVC, 1, &stats, 0);
    CHECK(stub.accepted == 1 && stub.emitted == 1 && stats.frames == 0);
    if (flush) {
        avcodec_flush_buffers(ctx);
        CHECK(stub.flushes == 1 && !stub.accepted && !stub.emitted);
        AVFrame *frame = av_frame_alloc(); CHECK(frame);
        CHECK(avcodec_receive_frame(ctx, frame) == AVERROR(EAGAIN));
        CHECK(!frame->buf[0]);
        av_frame_free(&frame);
        submit(ctx, AV_CODEC_ID_HEVC, 0, &stats, 1);
        CHECK(stats.frames == 1 && stub.accepted == 1);
        drain(ctx, &stats, 1);
    } else {
        /* EOF must preserve and process packet1 before draining the broker. */
        drain(ctx, &stats, 2);
        CHECK(stats.drain_frames == 2);
    }
    printf("case=occupied-input-%s codec=%d frames=%u eos_frames=%u PASS\n",
           flush ? "flush" : "eos", AV_CODEC_ID_HEVC, stats.frames, stats.drain_frames);
    avcodec_free_context(&ctx);
    CHECK(stub.opens == 1 && stub.closes == 1);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    int bounded = !strcmp(argv[1], "v7");
    CHECK(bounded || !strcmp(argv[1], "v6"));
    trial("delayed-eight-frame-backlog", AV_CODEC_ID_H264, 8, 0, 0, 0, bounded);
    trial("delayed-eight-frame-backlog", AV_CODEC_ID_HEVC, 8, 0, 0, 0, bounded);
    trial("public-input-slot-backpressure", AV_CODEC_ID_H264, 1, 0, 0, 1, bounded);
    trial("broker-backpressure-progress", AV_CODEC_ID_HEVC, 8, 2, 0, 0, bounded);
    trial("replay64-pending65-startup", AV_CODEC_ID_H264, 64, 0, 1, 0, bounded);
    occupied_input_terminal(0);
    occupied_input_terminal(1);
    printf("PASS checks=%u actual_library=%s delay_per_frame_us=10000 board_access=0\n", checks, argv[1]);
    return 0;
}
