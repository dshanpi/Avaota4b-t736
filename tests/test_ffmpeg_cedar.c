/* Software-only FFmpeg proxy audit. Link the actual candidate shared libs
 * with -Wl,--export-dynamic so dlsym resolves these stub client entry points.
 * No broker socket, device, vendor codec, or DMA access is possible here.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include <libavutil/time.h>
#include "cedar_client.h"

enum { OFF, OPEN_FAIL, EARLY_FAIL, DRAIN_FAIL, SYNTHETIC, FATAL_PUSH, BAD_EOF,
       ASYNC_STARTUP, STALL_STARTUP };
static struct {
    int mode, opens, pushes, accepted, pulls, closes, flushes, eos, pressure;
    unsigned width, height;
    int queued;
    int64_t pts;
    uint8_t *last_destination;
    int startup_ready, waiting_pull_calls, queue_read;
    int64_t ordered_pts[256];
} stub;
static unsigned checks;
static uint8_t captured_packet[1024 * 1024];
static size_t captured_size;
static int capture_enabled;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

int t736_client_available(void) { return stub.mode != OFF; }
int t736_client_open(int codec, unsigned width, unsigned height)
{
    (void)codec; stub.opens++;
    if (stub.mode == OPEN_FAIL) return -EBUSY;
    stub.width=width; stub.height=height; return 1;
}
int t736_client_push(int session, const uint8_t *data, size_t size,
                     int64_t pts, int64_t dts)
{
    (void)session; (void)data; (void)size;
    stub.pushes++;
    CHECK((pts == INT64_MIN || pts >= 0) && (dts == INT64_MIN || dts >= 0));
    CHECK(dts == INT64_MIN); /* Hardware-only DTS omission preserves B-frame PTS. */
    if (stub.mode == FATAL_PUSH || (stub.mode == EARLY_FAIL && stub.accepted == 7))
        return -EIO;
    if (stub.pressure > 0) { stub.pressure--; return -EAGAIN; }
    if (capture_enabled) {
        CHECK(size <= sizeof(captured_packet));
        memcpy(captured_packet,data,size); captured_size=size;
    }
    CHECK(stub.accepted < 256);
    stub.ordered_pts[stub.accepted]=pts;
    stub.accepted++; stub.queued++; stub.pts=pts; return 0;
}
int t736_client_pull(int session, uint8_t *dst, size_t capacity,
                     struct t736_frame *frame, unsigned timeout_ms)
{
    (void)session; stub.pulls++;
    if (stub.mode == BAD_EOF) return 2;
    if (stub.mode == EARLY_FAIL || stub.mode == DRAIN_FAIL) return 0;
    if (stub.mode == STALL_STARTUP) {
        if (timeout_ms) {
            stub.waiting_pull_calls++;
            av_usleep((unsigned long)timeout_ms*1000);
        }
        return 0;
    }
    if (stub.mode == ASYNC_STARTUP && !stub.startup_ready) {
        /* Initialization only makes progress while the proxy waits after
         * filling its bounded replay queue, never during polling/feed. */
        if (stub.accepted < 64 || !timeout_ms) return 0;
        stub.waiting_pull_calls++;
        if (stub.waiting_pull_calls < 3) return 0;
        stub.startup_ready=1;
    }
    if (stub.queued) {
        unsigned bytes=stub.width*stub.height*3/2;
        CHECK(capacity >= bytes);
        memset(dst, 0x70 + stub.accepted, bytes);
        stub.last_destination = dst;
        int64_t pts=stub.mode==ASYNC_STARTUP ? stub.ordered_pts[stub.queue_read++] : stub.pts;
        *frame = (struct t736_frame){.width=stub.width, .height=stub.height,
            .bytes_written=bytes, .color_range=1, .colorspace=6, .pts_us=pts};
        stub.queued--; return 1;
    }
    if (stub.eos) return 2;
    if (timeout_ms) av_usleep((unsigned long)timeout_ms*1000);
    return 0;
}
int t736_client_drain(int session)
{ (void)session; if (stub.mode == DRAIN_FAIL) return -EIO; stub.eos=1; return 0; }
int t736_client_flush(int session)
{ (void)session; stub.flushes++; stub.eos=stub.queued=0; return 0; }
int t736_client_close(int session)
{ (void)session; stub.closes++; return 0; }

typedef struct Record {
    int width, height, format;
    int64_t pts, best_effort, duration, dts;
    unsigned char md5[16];
} Record;
typedef struct Output { Record frames[256]; unsigned count; } Output;

static void record_frame(Output *out, const AVFrame *frame)
{
    CHECK(out->count < 256);
    Record *record=&out->frames[out->count++];
    record->width=frame->width; record->height=frame->height; record->format=frame->format;
    record->pts=frame->pts; record->best_effort=frame->best_effort_timestamp;
    record->duration=frame->duration; record->dts=frame->pkt_dts;
    int size=av_image_get_buffer_size(frame->format,frame->width,frame->height,1);
    CHECK(size > 0);
    uint8_t *pixels=av_malloc(size); CHECK(pixels);
    CHECK(av_image_copy_to_buffer(pixels,size,(const uint8_t *const *)frame->data,
          frame->linesize,frame->format,frame->width,frame->height,1) == size);
    av_md5_sum(record->md5,pixels,size); av_free(pixels);
}

static int collect(AVCodecContext *ctx, Output *out)
{
    AVFrame *frame=av_frame_alloc(); CHECK(frame);
    int ret;
    while ((ret=avcodec_receive_frame(ctx,frame)) >= 0) {
        record_frame(out,frame); av_frame_unref(frame);
    }
    av_frame_free(&frame);
    CHECK(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    return ret;
}

static void decode_file(const char *file, int native, int mode, Output *out)
{
    memset(&stub,0,sizeof(stub));
    AVFormatContext *format=NULL;
    CHECK(avformat_open_input(&format,file,NULL,NULL) >= 0);
    CHECK(avformat_find_stream_info(format,NULL) >= 0);
    int stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
    CHECK(stream >= 0);
    AVCodecParameters *par=format->streams[stream]->codecpar;
    const char *native_name=par->codec_id==AV_CODEC_ID_H264 ? "h264" : "hevc";
    const AVCodec *codec=native ? avcodec_find_decoder_by_name(native_name) :
                                avcodec_find_decoder(par->codec_id);
    CHECK(codec);
    CHECK(native || strstr(codec->name,"_cedar"));
    AVCodecContext *ctx=avcodec_alloc_context3(codec); CHECK(ctx);
    CHECK(avcodec_parameters_to_context(ctx,par) >= 0);
    ctx->pkt_timebase=format->streams[stream]->time_base;
    ctx->thread_count=1; ctx->thread_type=0;
    memset(&stub,0,sizeof(stub)); stub.mode=mode;
    CHECK(avcodec_open2(ctx,codec,NULL) >= 0);
    AVPacket *packet=av_packet_alloc(); CHECK(packet);
    while (av_read_frame(format,packet) >= 0) {
        if (packet->stream_index == stream) {
            int ret=avcodec_send_packet(ctx,packet);
            if (ret == AVERROR(EAGAIN)) {
                unsigned before=out->count;
                collect(ctx,out); CHECK(out->count > before);
                ret=avcodec_send_packet(ctx,packet);
            }
            CHECK(ret >= 0);
            CHECK(collect(ctx,out) == AVERROR(EAGAIN));
        }
        av_packet_unref(packet);
    }
    CHECK(avcodec_send_packet(ctx,NULL) >= 0);
    CHECK(collect(ctx,out) == AVERROR_EOF);
    CHECK(out->count > 0);
    av_packet_free(&packet); avcodec_free_context(&ctx); avformat_close_input(&format);
    if (!native) {
        if (mode == OFF) CHECK(stub.opens == 0);
        if (mode == OPEN_FAIL) CHECK(stub.opens == 1 && stub.pushes == 0);
        if (mode == EARLY_FAIL) CHECK(stub.accepted == 7 && stub.closes == 1);
        if (mode == DRAIN_FAIL) CHECK(stub.accepted > 7 && stub.closes == 1);
        if (mode == STALL_STARTUP) {
            CHECK(stub.opens==1 && stub.closes==1);
            CHECK(stub.pushes==64 && stub.accepted==64 && stub.waiting_pull_calls>=5);
        }
    }
}

static void compare_file(const char *file)
{
    Output *reference=av_mallocz(sizeof(*reference)); CHECK(reference);
    decode_file(file,1,OFF,reference);
    for (int mode=OFF; mode<=DRAIN_FAIL; mode++) {
        Output *actual=av_mallocz(sizeof(*actual)); CHECK(actual);
        decode_file(file,0,mode,actual);
        CHECK(actual->count == reference->count);
        for (unsigned i=0;i<actual->count;i++) {
            Record *a=&actual->frames[i], *b=&reference->frames[i];
            CHECK(a->width==b->width && a->height==b->height && a->format==b->format);
            CHECK(!memcmp(a->md5,b->md5,16));
            CHECK(a->pts==b->pts && a->best_effort==b->best_effort && a->duration==b->duration);
        }
        printf("fallback mode=%d frames=%u exact_md5_pts_duration file=%s\n",mode,actual->count,file);
        av_free(actual);
    }
    av_free(reference);
}

static void compare_startup_timeout_file(const char *file)
{
    Output *reference=av_mallocz(sizeof(*reference)), *actual=av_mallocz(sizeof(*actual));
    CHECK(reference && actual);
    decode_file(file,1,OFF,reference);
    CHECK(reference->count>65);
    int64_t start=av_gettime_relative();
    decode_file(file,0,STALL_STARTUP,actual);
    int64_t elapsed=av_gettime_relative()-start;
    /* Exercise the actual monotonic deadline, without clock interposition.
     * The upper allowance includes QEMU decode and host scheduling overhead. */
    CHECK(elapsed>=4900000 && elapsed<15000000);
    CHECK(actual->count==reference->count);
    for (unsigned i=0;i<actual->count;i++) {
        Record *a=&actual->frames[i], *b=&reference->frames[i];
        CHECK(a->width==b->width && a->height==b->height && a->format==b->format);
        CHECK(!memcmp(a->md5,b->md5,16));
        CHECK(a->pts==b->pts && a->best_effort==b->best_effort && a->duration==b->duration);
    }
    printf("startup_real_timeout elapsed_us=%" PRId64 " frames=%u pushed=64 pending65_replayed exact_md5_pts_duration PASS\n",
           elapsed,actual->count);
    av_free(reference); av_free(actual);
}

static AVCodecContext *synthetic_context(enum AVCodecID id)
{
    const AVCodec *codec=avcodec_find_decoder(id); CHECK(codec && strstr(codec->name,"_cedar"));
    AVCodecContext *ctx=avcodec_alloc_context3(codec); CHECK(ctx);
    ctx->width=16; ctx->height=16; ctx->pkt_timebase=(AVRational){1,1000000};
    ctx->thread_count=1; ctx->thread_type=0;
    return ctx;
}

static AVPacket *synthetic_packet(int64_t pts, int64_t dts)
{
    static const uint8_t annexb[]={0,0,0,1,0x65,0x88,0x80};
    AVPacket *packet=av_packet_alloc(); CHECK(packet);
    CHECK(av_new_packet(packet,sizeof(annexb)) >= 0);
    memcpy(packet->data,annexb,sizeof(annexb));
    packet->pts=pts; packet->dts=dts; packet->duration=33333; packet->pos=123;
    packet->flags=AV_PKT_FLAG_KEY;
    return packet;
}

static void synthetic_tests(void)
{
    memset(&stub,0,sizeof(stub)); stub.mode=SYNTHETIC; stub.pressure=3;
    AVCodecContext *ctx=synthetic_context(AV_CODEC_ID_H264);
    CHECK(avcodec_open2(ctx,ctx->codec,NULL) >= 0);
    AVPacket *packet=synthetic_packet(0,-2000);
    AVFrame *frame=av_frame_alloc(); CHECK(frame);
    CHECK(avcodec_send_packet(ctx,packet) >= 0);
    /* The original defect returned from send after its first EAGAIN. */
    CHECK(stub.pushes == 4 && stub.accepted == 1);
    CHECK(avcodec_receive_frame(ctx,frame) == 0);
    CHECK(frame->pts==0 && frame->pkt_dts==-2000 && frame->duration==33333);
    CHECK(frame->width==16 && frame->height==16 && frame->data[0][0]==0x71);
    av_frame_unref(frame);
    CHECK(avcodec_send_packet(ctx,NULL) >= 0);
    CHECK(avcodec_receive_frame(ctx,frame) == AVERROR_EOF);
    avcodec_flush_buffers(ctx); CHECK(stub.flushes == 1);
    packet->pts=-5000; packet->dts=-6000;
    CHECK(avcodec_send_packet(ctx,packet) >= 0);
    CHECK(avcodec_receive_frame(ctx,frame) == 0);
    CHECK(frame->pts == -5000 && frame->pkt_dts == -6000);
    av_frame_unref(frame);
    stub.mode=FATAL_PUSH;
    CHECK(avcodec_send_packet(ctx,packet) == AVERROR(EIO));
    CHECK(avcodec_receive_frame(ctx,frame) == AVERROR(EIO));
    avcodec_free_context(&ctx); av_packet_free(&packet);

    memset(&stub,0,sizeof(stub)); stub.mode=BAD_EOF;
    ctx=synthetic_context(AV_CODEC_ID_H264);
    CHECK(avcodec_open2(ctx,ctx->codec,NULL) >= 0);
    packet=synthetic_packet(0,0);
    CHECK(avcodec_send_packet(ctx,packet) == AVERROR(EPROTO));
    avcodec_free_context(&ctx); av_packet_free(&packet);

    memset(&stub,0,sizeof(stub)); stub.mode=SYNTHETIC;
    ctx=synthetic_context(AV_CODEC_ID_HEVC);
    ctx->profile=AV_PROFILE_HEVC_MAIN_10; ctx->pix_fmt=AV_PIX_FMT_YUV420P10LE;
    CHECK(avcodec_open2(ctx,ctx->codec,NULL) >= 0);
    CHECK(stub.opens == 0); avcodec_free_context(&ctx);
    av_frame_free(&frame);
    printf("synthetic backpressure_negative_pts_eos_flush_fatal_format PASS\n");
}

static void pooled_frame_tests(unsigned width)
{
    memset(&stub,0,sizeof(stub)); stub.mode=SYNTHETIC;
    AVCodecContext *ctx=synthetic_context(AV_CODEC_ID_H264);
    ctx->width=width; ctx->height=180;
    CHECK(avcodec_open2(ctx,ctx->codec,NULL) >= 0);
    AVFrame *first=av_frame_alloc(), *second=av_frame_alloc(), *third=av_frame_alloc();
    CHECK(first && second && third);
    AVPacket *packet=synthetic_packet(0,0);
    CHECK(avcodec_send_packet(ctx,packet) >= 0);
    CHECK(avcodec_receive_frame(ctx,first) == 0);
    CHECK(first->data[0][0] == 0x71);
    if (!(width & 63)) CHECK(first->data[0] == stub.last_destination);
    else CHECK(first->data[0] != stub.last_destination);
    for (unsigned plane=0;plane<3;plane++) {
        CHECK(first->linesize[plane] >= (int)(width >> (plane != 0)));
        CHECK(!(first->linesize[plane] & 31));
    }
    packet->pts=packet->dts=33333;
    CHECK(avcodec_send_packet(ctx,packet) >= 0);
    CHECK(avcodec_receive_frame(ctx,second) == 0);
    CHECK(first->data[0] != second->data[0]);
    CHECK(first->data[0][0] == 0x71 && second->data[0][0] == 0x72);
    uint8_t *released=second->data[0];
    av_frame_unref(second);
    avcodec_flush_buffers(ctx);
    CHECK(stub.flushes == 1 && first->data[0][0] == 0x71);
    packet->pts=packet->dts=0;
    CHECK(avcodec_send_packet(ctx,packet) >= 0);
    CHECK(avcodec_receive_frame(ctx,third) == 0);
    CHECK(third->data[0][0] == 0x73 && first->data[0][0] == 0x71);
    if (!(width & 63)) CHECK(third->data[0] == released);
    /* Decoder/pool shutdown may happen while the consumer owns frames. */
    avcodec_free_context(&ctx);
    CHECK(first->buf[0] && third->buf[0]);
    CHECK(first->data[0][0] == 0x71 && third->data[0][0] == 0x73);
    CHECK(first->data[2][0] == 0x71 && third->data[2][0] == 0x73);
    av_frame_free(&first); av_frame_free(&second); av_frame_free(&third);
    av_packet_free(&packet);
    printf("pooled_receive_lifetime width=%u direct=%d flush_reuse_and_close PASS\n",
           width, !(width & 63));
}

static void firefox_dts_sequence_test(enum AVCodecID id)
{
    /* Actual Firefox 60 fps packet trace: mTimecode advances in decode order
     * while B-frame presentation timestamps are earlier than that DTS.
     * A GstBaseParse-style max(PTS,DTS) must never be induced by wire DTS. */
    static const int64_t pts[]={0,50000,33333,16666,100000,83333,66666};
    static const int64_t dts[]={0,16666,33333,50000,66666,83333,100000};
    memset(&stub,0,sizeof(stub)); stub.mode=SYNTHETIC;
    AVCodecContext *ctx=synthetic_context(id);
    ctx->pkt_timebase=(AVRational){0,1}; /* Firefox supplies microseconds without this field. */
    ctx->time_base=(AVRational){0,1};
    CHECK(avcodec_open2(ctx,ctx->codec,NULL) >= 0);
    AVFrame *frame=av_frame_alloc(); CHECK(frame);
    for (unsigned i=0;i<sizeof(pts)/sizeof(pts[0]);i++) {
        AVPacket *packet=synthetic_packet(pts[i],dts[i]);
        packet->duration=16667;
        CHECK(avcodec_send_packet(ctx,packet) >= 0);
        CHECK(avcodec_receive_frame(ctx,frame) == 0);
        CHECK(frame->pts==pts[i]);
        CHECK(frame->pkt_dts==dts[i]);
        CHECK(frame->duration==16667);
        CHECK(packet->pts==pts[i] && packet->dts==dts[i]);
        av_frame_unref(frame); av_packet_free(&packet);
    }
    CHECK(stub.accepted==7);
    av_frame_free(&frame); avcodec_free_context(&ctx);
    printf("firefox_decode_order_dts codec=%d original_pts_dts_preserved_wire_dts_unknown PASS\n",id);
}

static void replay_limit_async_startup_test(void)
{
    memset(&stub,0,sizeof(stub)); stub.mode=ASYNC_STARTUP;
    AVCodecContext *ctx=synthetic_context(AV_CODEC_ID_H264);
    CHECK(avcodec_open2(ctx,ctx->codec,NULL)>=0);
    AVFrame *frame=av_frame_alloc(); CHECK(frame);
    for (unsigned i=0;i<65;i++) {
        AVPacket *packet=synthetic_packet((int64_t)i*1000,(int64_t)i*1000);
        CHECK(avcodec_send_packet(ctx,packet)>=0);
        if (i<64) CHECK(avcodec_receive_frame(ctx,frame)==AVERROR(EAGAIN));
        av_packet_free(&packet);
    }
    /* Packet 65 is owned by the proxy but must not be fed or discarded
     * before the first delayed frame establishes forward progress. */
    CHECK(stub.waiting_pull_calls>=3 && stub.accepted==64);
    CHECK(stub.opens==1 && stub.closes==0);
    unsigned frames=0;
    int ret;
    while ((ret=avcodec_receive_frame(ctx,frame))>=0) {
        CHECK(frame->pts==(int64_t)frames*1000);
        CHECK(frame->pkt_dts==(int64_t)frames*1000);
        frames++; CHECK(frames<=65); av_frame_unref(frame);
    }
    CHECK(ret==AVERROR(EAGAIN));
    /* Publish the first frame promptly instead of holding it while copying
     * the other 64 ready pictures. Packet 65 must already be consumed when
     * receive reports EAGAIN, so the public input slot can accept EOS. */
    CHECK(frames==1 && stub.accepted==65 && stub.queue_read==1);
    for (unsigned i=0;i<65;i++) CHECK(stub.ordered_pts[i]==(int64_t)i*1000);
    CHECK(avcodec_send_packet(ctx,NULL)>=0);
    while ((ret=avcodec_receive_frame(ctx,frame))>=0) {
        CHECK(frame->pts==(int64_t)frames*1000);
        CHECK(frame->pkt_dts==(int64_t)frames*1000);
        frames++; CHECK(frames<=65); av_frame_unref(frame);
    }
    CHECK(ret==AVERROR_EOF && frames==65 && stub.queue_read==65);
    av_frame_free(&frame); avcodec_free_context(&ctx);
    printf("replay_limit_async_first_frame bounded_wait_and_packet65_order PASS\n");
}

typedef struct NALSpan { size_t start, payload, end; unsigned type; } NALSpan;

static size_t find_test_start(const uint8_t *data, size_t size, size_t from, size_t *prefix)
{
    for (size_t i=from;i+3<=size;i++) {
        if (i+4<=size && !memcmp(data+i,"\0\0\0\1",4)) { *prefix=4; return i; }
        if (!memcmp(data+i,"\0\0\1",3)) { *prefix=3; return i; }
    }
    return size;
}

static unsigned parse_test_nals(enum AVCodecID codec, const uint8_t *data, size_t size,
                                NALSpan *spans, unsigned capacity)
{
    size_t prefix=0, position=find_test_start(data,size,0,&prefix);
    unsigned count=0;
    while (position<size) {
        size_t payload=position+prefix, next_prefix=0;
        size_t end=find_test_start(data,size,payload,&next_prefix);
        CHECK(payload+(codec==AV_CODEC_ID_HEVC ? 2U : 1U)<=end);
        CHECK(count<capacity);
        unsigned type=codec==AV_CODEC_ID_HEVC ? (data[payload]>>1)&63 : data[payload]&31;
        spans[count++]=(NALSpan){position,payload,end,type};
        position=end; prefix=next_prefix;
    }
    return count;
}

static int is_test_parameter(enum AVCodecID codec, unsigned type)
{ return codec==AV_CODEC_ID_HEVC ? type>=32 && type<=34 : type==7 || type==8; }

static AVPacket *without_test_parameters(enum AVCodecID codec, const AVPacket *source)
{
    NALSpan spans[128];
    unsigned count=parse_test_nals(codec,source->data,source->size,spans,128);
    AVPacket *packet=av_packet_alloc(); CHECK(packet);
    CHECK(av_new_packet(packet,source->size)>=0);
    CHECK(av_packet_copy_props(packet,source)>=0);
    size_t written=0;
    for (unsigned i=0;i<count;i++) {
        if (is_test_parameter(codec,spans[i].type)) continue;
        size_t length=spans[i].end-spans[i].start;
        memcpy(packet->data+written,source->data+spans[i].start,length);
        written+=length;
    }
    av_shrink_packet(packet,written); CHECK(written>0); return packet;
}

static void ensure_test_aud(enum AVCodecID codec, AVPacket *packet)
{
    NALSpan spans[128];
    unsigned count=parse_test_nals(codec,packet->data,packet->size,spans,128);
    unsigned aud=codec==AV_CODEC_ID_HEVC ? 35U : 9U;
    if (count && spans[0].type==aud) return;
    static const uint8_t h264_aud[]={0,0,0,1,9,0xf0};
    static const uint8_t hevc_aud[]={0,0,0,1,70,1,0x50};
    const uint8_t *data=codec==AV_CODEC_ID_HEVC ? hevc_aud : h264_aud;
    unsigned size=codec==AV_CODEC_ID_HEVC ? sizeof(hevc_aud) : sizeof(h264_aud);
    CHECK(av_grow_packet(packet,size)>=0);
    memmove(packet->data+size,packet->data,packet->size-size);
    memcpy(packet->data,data,size);
}

static AVCodecContext *parameter_test_context(const AVCodecParameters *parameters, AVRational tb)
{
    memset(&stub,0,sizeof(stub)); stub.mode=SYNTHETIC;
    capture_enabled=1; captured_size=0;
    const AVCodec *codec=avcodec_find_decoder(parameters->codec_id); CHECK(codec);
    AVCodecContext *ctx=avcodec_alloc_context3(codec); CHECK(ctx);
    CHECK(avcodec_parameters_to_context(ctx,parameters)>=0);
    ctx->pkt_timebase=tb; ctx->thread_count=1; ctx->thread_type=0;
    CHECK(avcodec_open2(ctx,codec,NULL)>=0);
    CHECK(stub.opens==1); return ctx;
}

static void capture_test_au(AVCodecContext *ctx, const AVPacket *packet)
{
    captured_size=0;
    AVFrame *frame=av_frame_alloc(); CHECK(frame);
    CHECK(avcodec_send_packet(ctx,packet)>=0);
    CHECK(avcodec_receive_frame(ctx,frame)==0);
    CHECK(captured_size>0);
    CHECK(frame->pts==packet->pts && frame->pkt_dts==packet->dts);
    av_frame_free(&frame);
}

static void parameter_bootstrap_test(const char *filename)
{
    capture_enabled=0; memset(&stub,0,sizeof(stub));
    AVFormatContext *format=NULL;
    CHECK(avformat_open_input(&format,filename,NULL,NULL)>=0);
    CHECK(avformat_find_stream_info(format,NULL)>=0);
    int stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,NULL,0);
    CHECK(stream>=0);
    AVCodecParameters *container=avcodec_parameters_alloc(); CHECK(container);
    CHECK(avcodec_parameters_copy(container,format->streams[stream]->codecpar)>=0);
    enum AVCodecID codec=container->codec_id;
    AVRational tb=format->streams[stream]->time_base;
    AVBSFContext *bsf=NULL;
    CHECK(av_bsf_alloc(av_bsf_get_by_name(codec==AV_CODEC_ID_HEVC ?
          "hevc_mp4toannexb" : "h264_mp4toannexb"),&bsf)>=0);
    CHECK(avcodec_parameters_copy(bsf->par_in,container)>=0);
    bsf->time_base_in=tb; CHECK(av_bsf_init(bsf)>=0);
    AVCodecParameters *annexb=avcodec_parameters_alloc(); CHECK(annexb);
    CHECK(avcodec_parameters_copy(annexb,bsf->par_out)>=0);
    AVPacket *original=av_packet_alloc(), *converted=av_packet_alloc();
    CHECK(original && converted);
    for (;;) {
        CHECK(av_read_frame(format,original)>=0);
        if (original->stream_index==stream && (original->flags&AV_PKT_FLAG_KEY)) break;
        av_packet_unref(original);
    }
    AVPacket *copy=av_packet_clone(original); CHECK(copy);
    CHECK(av_bsf_send_packet(bsf,copy)>=0);
    CHECK(av_bsf_receive_packet(bsf,converted)>=0);
    av_packet_free(&copy);
    NALSpan config_spans[128], full_spans[128];
    unsigned config_count=parse_test_nals(codec,annexb->extradata,annexb->extradata_size,config_spans,128);
    unsigned full_count=parse_test_nals(codec,converted->data,converted->size,full_spans,128);
    unsigned config_types[64]={0}, full_types[64]={0};
    for (unsigned i=0;i<config_count;i++) config_types[config_spans[i].type]++;
    for (unsigned i=0;i<full_count;i++) full_types[full_spans[i].type]++;
    for (unsigned t=0;t<64;t++) if (is_test_parameter(codec,t)) {
        CHECK(config_types[t]>0); CHECK(full_types[t]>0);
    }
    AVPacket *bare=without_test_parameters(codec,converted);
    ensure_test_aud(codec,bare);

    /* Browser Annex-B extradata plus an AUD/VCL AU without parameter sets. */
    AVCodecContext *ctx=parameter_test_context(annexb,tb);
    capture_test_au(ctx,bare);
    uint8_t *first=av_memdup(captured_packet,captured_size); CHECK(first);
    size_t first_size=captured_size;
    NALSpan seeded[128];
    unsigned seeded_count=parse_test_nals(codec,first,first_size,seeded,128);
    unsigned seeded_types[64]={0};
    CHECK(seeded[0].type==(codec==AV_CODEC_ID_HEVC ? 35U : 9U));
    for (unsigned i=0;i<seeded_count;i++) seeded_types[seeded[i].type]++;
    for (unsigned t=0;t<64;t++) if (is_test_parameter(codec,t))
        CHECK(seeded_types[t]==config_types[t]);
    AVPacket borrowed={.data=first,.size=(int)first_size};
    AVPacket *stripped=without_test_parameters(codec,&borrowed);
    CHECK(stripped->size==bare->size && !memcmp(stripped->data,bare->data,bare->size));
    av_packet_free(&stripped);
    capture_test_au(ctx,bare);
    CHECK(captured_size==(size_t)bare->size && !memcmp(captured_packet,bare->data,bare->size));
    avcodec_flush_buffers(ctx);
    capture_test_au(ctx,bare);
    CHECK(captured_size==first_size && !memcmp(captured_packet,first,first_size));
    avcodec_free_context(&ctx); av_free(first);

    /* A complete in-band AU must remain byte-identical, without duplicated sets. */
    ctx=parameter_test_context(annexb,tb);
    capture_test_au(ctx,converted);
    CHECK(captured_size==(size_t)converted->size && !memcmp(captured_packet,converted->data,converted->size));
    avcodec_free_context(&ctx);

    /* Partial in-band configuration needs complete dependency-ordered
     * bootstrap before the original update, not PPS-before-SPS injection. */
    unsigned partial_types[2]={codec==AV_CODEC_ID_HEVC ? 32U : 7U,33U};
    unsigned partial_cases=codec==AV_CODEC_ID_HEVC ? 2U : 1U;
    for (unsigned part=0;part<partial_cases;part++) {
        unsigned selected=config_count;
        for (unsigned i=0;i<config_count;i++) if (config_spans[i].type==partial_types[part]) {
            selected=i; break;
        }
        CHECK(selected<config_count);
        NALSpan chosen=config_spans[selected], base[128];
        unsigned base_count=parse_test_nals(codec,bare->data,bare->size,base,128);
        CHECK(base_count>0);
        size_t added=chosen.end-chosen.start, position=base[0].end;
        AVPacket *partial=av_packet_clone(bare); CHECK(partial);
        CHECK(av_grow_packet(partial,added)>=0);
        memmove(partial->data+position+added,partial->data+position,
                partial->size-added-position);
        memcpy(partial->data+position,annexb->extradata+chosen.start,added);
        ctx=parameter_test_context(annexb,tb);
        capture_test_au(ctx,partial);
        NALSpan output[128];
        unsigned output_count=parse_test_nals(codec,captured_packet,captured_size,output,128);
        CHECK(output_count>0 && output[0].type==base[0].type);
        unsigned index=1;
        for (unsigned t=0;t<64;t++) if (is_test_parameter(codec,t)) {
            for (unsigned n=0;n<config_types[t];n++) {
                CHECK(index<output_count && output[index].type==t); index++;
            }
        }
        CHECK(index<output_count && output[index].type==chosen.type);
        CHECK(output[index].end-output[index].start==added);
        CHECK(!memcmp(captured_packet+output[index].start,annexb->extradata+chosen.start,added));
        AVPacket partial_output={.data=captured_packet,.size=(int)captured_size};
        AVPacket *body=without_test_parameters(codec,&partial_output);
        CHECK(body->size==bare->size && !memcmp(body->data,bare->data,bare->size));
        av_packet_free(&body); av_packet_free(&partial); avcodec_free_context(&ctx);
    }

    /* Compare the original avcC/hvcC path with upstream BSF output exactly. */
    ctx=parameter_test_context(container,tb);
    capture_test_au(ctx,original);
    CHECK(captured_size==(size_t)converted->size && !memcmp(captured_packet,converted->data,converted->size));
    avcodec_free_context(&ctx);
    capture_enabled=0;
    av_packet_free(&bare); av_packet_free(&original); av_packet_free(&converted);
    av_bsf_free(&bsf); avcodec_parameters_free(&container); avcodec_parameters_free(&annexb);
    avformat_close_input(&format);
    printf("parameter_bootstrap codec=%d annexb_aud_flush_complete_and_container_no_duplicates PASS\n",codec);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4);
    av_log_set_level(AV_LOG_WARNING);
    const AVCodec *h264=avcodec_find_decoder(AV_CODEC_ID_H264);
    const AVCodec *hevc=avcodec_find_decoder(AV_CODEC_ID_HEVC);
    CHECK(h264 && !strcmp(h264->name,"h264_cedar"));
    CHECK(hevc && !strcmp(hevc->name,"hevc_cedar"));
    CHECK(!strcmp(avcodec_find_decoder_by_name("h264")->name,"h264"));
    CHECK(!strcmp(avcodec_find_decoder_by_name("hevc")->name,"hevc"));
    synthetic_tests(); pooled_frame_tests(320); pooled_frame_tests(318);
    firefox_dts_sequence_test(AV_CODEC_ID_H264);
    firefox_dts_sequence_test(AV_CODEC_ID_HEVC);
    replay_limit_async_startup_test();
    parameter_bootstrap_test(argv[1]); parameter_bootstrap_test(argv[2]);
    compare_file(argv[1]); compare_file(argv[2]);
    if (argc==4) compare_startup_timeout_file(argv[3]);
    printf("{\"result\":\"PASS\",\"checks\":%u,\"hardware_access\":false}\n",checks);
    return 0;
}
