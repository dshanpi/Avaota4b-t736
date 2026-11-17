/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Private FFmpeg 7.1 Cedar proxy decoder. Hardware handles never enter this
 * process; a preloaded, pre-connected client exchanges bounded compressed
 * packets and ordinary I420 pixels with the separately confined broker.
 */
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "avcodec.h"
#include "bsf.h"
#include "codec_internal.h"
#include "decode.h"
#include "libavutil/buffer.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/time.h"
#include "cedar_client.h"

#define REPLAY_PACKETS 64
#define TIMING_PACKETS 256

typedef struct CedarAPI {
    int (*available)(void);
    int (*open)(int, unsigned, unsigned);
    int (*push)(int, const uint8_t *, size_t, int64_t, int64_t);
    int (*pull)(int, uint8_t *, size_t, struct t736_frame *, unsigned);
    int (*drain)(int);
    int (*flush)(int);
    int (*close)(int);
} CedarAPI;

typedef struct CedarTiming {
    int64_t pts, duration, dts, pos;
    int flags;
} CedarTiming;

typedef struct CedarContext {
    CedarAPI api;
    AVCodecContext *software;
    AVBSFContext *bsf;
    AVPacket *input, *filtered;
    AVPacket *replay[REPLAY_PACKETS];
    int replay_count, replay_read;
    size_t replay_bytes;
    int input_pending, input_in_replay, filtered_pending;
    int input_eof, bsf_draining, broker_draining, software_draining;
    int session, delivered, fatal_error;
    int offset_set;
    int64_t offset_us, drain_started, backpressure_started, first_frame_wait_started;
    AVBufferPool *pixel_pool;
    AVBufferRef *receive_buffer;
    unsigned generation;
    int parameter_sets_seeded, input_logged, filtered_logged;
    CedarTiming timing[TIMING_PACKETS];
    unsigned timing_next;
    unsigned batch_frames, batch_inputs, batch_logs;
    int64_t batch_started;
} CedarContext;

typedef struct CedarNALSummary {
    int first_type, count, parameter_count, parameter_mask, irap, leading_aud_end;
} CedarNALSummary;

static int start_code(const uint8_t *data, int size, int from, int *length)
{
    for (int i = from; i + 2 < size; i++) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { *length = 3; return i; }
        if (i + 3 < size && !data[i + 2] && data[i + 3] == 1) {
            *length = 4; return i;
        }
    }
    return -1;
}

static int next_nal(const uint8_t *data, int size, int *cursor,
                    int *prefix, int *payload, int *end)
{
    int length, ignored, next;
    *prefix = start_code(data, size, *cursor, &length);
    if (*prefix < 0) return 0;
    *payload = *prefix + length;
    next = start_code(data, size, *payload, &ignored);
    *end = next < 0 ? size : next;
    *cursor = *end;
    return 1;
}

static int nal_type(enum AVCodecID codec, const uint8_t *data, int size)
{
    if (size < (codec == AV_CODEC_ID_HEVC ? 2 : 1)) return -1;
    return codec == AV_CODEC_ID_HEVC ? (data[0] >> 1) & 63 : data[0] & 31;
}

static int parameter_bit(enum AVCodecID codec, int type)
{
    if (codec == AV_CODEC_ID_HEVC)
        return type >= 32 && type <= 34 ? 1 << (type - 32) : 0;
    return type == 7 ? 2 : type == 8 ? 4 : 0;
}

static CedarNALSummary nal_summary(enum AVCodecID codec, const uint8_t *data, int size)
{
    CedarNALSummary summary = { .first_type = -1 };
    int cursor = 0, prefix, payload, end, first_length;
    int first = start_code(data, size, 0, &first_length);
    if (first < 0) return summary;
    for (int i = 0; i < first; i++) if (data[i]) return summary;
    while (next_nal(data, size, &cursor, &prefix, &payload, &end)) {
        int type = nal_type(codec, data + payload, end - payload);
        int bit = parameter_bit(codec, type);
        if (!summary.count) {
            summary.first_type = type;
            if (type == (codec == AV_CODEC_ID_HEVC ? 35 : 9))
                summary.leading_aud_end = end;
        }
        summary.count++;
        summary.parameter_count += !!bit;
        summary.parameter_mask |= bit;
        summary.irap |= codec == AV_CODEC_ID_HEVC ? type >= 16 && type <= 23 : type == 5;
    }
    return summary;
}

static CedarNALSummary length_prefixed_summary(enum AVCodecID codec,
                                                const uint8_t *data, int size, int length_size)
{
    CedarNALSummary summary = { .first_type = -1 };
    int offset = 0;
    while (size - offset >= length_size) {
        uint32_t bytes = 0;
        for (int i = 0; i < length_size; i++) bytes = (bytes << 8) | data[offset++];
        if (!bytes || bytes > (unsigned)(size - offset)) break;
        int type = nal_type(codec, data + offset, bytes);
        int bit = parameter_bit(codec, type);
        if (!summary.count) summary.first_type = type;
        summary.count++;
        summary.parameter_count += !!bit;
        summary.parameter_mask |= bit;
        summary.irap |= codec == AV_CODEC_ID_HEVC ? type >= 16 && type <= 23 : type == 5;
        offset += bytes;
    }
    return summary;
}

static void log_extradata(AVCodecContext *avctx, const char *stage,
                           const uint8_t *data, int size)
{
    char head[17] = { 0 };
    CedarNALSummary n = nal_summary(avctx->codec_id, data, size);
    for (int i = 0; i < FFMIN(size, 8); i++)
        snprintf(head + 2*i, 3, "%02x", data[i]);
    fprintf(stderr,
           "T736_CEDAR_PROXY: init codec=%s %s extradata_size=%d first8=%s annexb_parameter_count=%d annexb_parameter_mask=%d\n",
           avctx->codec->name, stage, size, head[0] ? head : "none",
           n.parameter_count, n.parameter_mask);
}

static void log_packet_shape(AVCodecContext *avctx, const char *stage, const AVPacket *packet)
{
    CedarContext *s = avctx->priv_data;
    CedarNALSummary n = nal_summary(avctx->codec_id, packet->data, packet->size);
    int length_size = 0;
    if (!strcmp(stage, "first_input") && avctx->extradata &&
        avctx->extradata_size > 0 && avctx->extradata[0] == 1) {
        if (avctx->codec_id == AV_CODEC_ID_HEVC && avctx->extradata_size >= 23)
            length_size = (avctx->extradata[21] & 3) + 1;
        if (avctx->codec_id == AV_CODEC_ID_H264 && avctx->extradata_size >= 5)
            length_size = (avctx->extradata[4] & 3) + 1;
    }
    if (length_size)
        n = length_prefixed_summary(avctx->codec_id, packet->data, packet->size, length_size);
    fprintf(stderr,
           "T736_CEDAR_PROXY: generation=%u %s bytes=%d key=%d length_prefix=%d first_nal=%d nals=%d parameter_count=%d parameter_mask=%d irap=%d\n",
           s->generation, stage, packet->size, !!(packet->flags & AV_PKT_FLAG_KEY), length_size,
           n.first_type, n.count, n.parameter_count, n.parameter_mask, n.irap);
}

static int seed_parameter_sets(AVCodecContext *avctx, AVPacket *packet)
{
    CedarContext *s = avctx->priv_data;
    const AVCodecParameters *extra = s->bsf->par_out;
    const int required = avctx->codec_id == AV_CODEC_ID_HEVC ? 7 : 6;
    CedarNALSummary input, configuration;
    int prefix_size = 0, cursor, prefix, payload, end, inserted;
    if (s->parameter_sets_seeded) return 0;
    if (packet->size < 0 || packet->size > T736_MAX_PACKET) return AVERROR(EMSGSIZE);
    input = nal_summary(avctx->codec_id, packet->data, packet->size);
    if ((input.parameter_mask & required) == required) {
        s->parameter_sets_seeded = 1;
        return 0;
    }
    /* Bootstrap a random-access AU. Never append a second VCL access unit or
     * stale configuration after in-band updates. Keep a leading AUD first. */
    if (!input.irap && !(packet->flags & AV_PKT_FLAG_KEY)) return 0;
    if (extra->extradata_size <= 0) return 0;
    if (extra->extradata_size > T736_MAX_PACKET) return AVERROR(EMSGSIZE);
    configuration = nal_summary(avctx->codec_id, extra->extradata, extra->extradata_size);
    if ((configuration.parameter_mask & required) != required) return 0;
    /* A partial in-band configuration cannot receive only the missing types
     * at a common prefix: that can put PPS before its SPS. Seed a complete
     * dependency-ordered configuration, then preserve all newer in-band NALs
     * so they override cached values. A complete in-band AU is untouched. */
    for (int bit = 1; bit <= 4; bit <<= 1) {
        if (!(required & bit)) continue;
        cursor = 0;
        while (next_nal(extra->extradata, extra->extradata_size, &cursor, &prefix, &payload, &end)) {
            int type = nal_type(avctx->codec_id, extra->extradata + payload, end - payload);
            if (parameter_bit(avctx->codec_id, type) != bit) continue;
            if ((extra->extradata[payload] & 0x80) ||
                (avctx->codec_id == AV_CODEC_ID_HEVC && !(extra->extradata[payload + 1] & 7)))
                return AVERROR_INVALIDDATA;
            if (end - prefix > T736_MAX_PACKET - packet->size - prefix_size)
                return AVERROR(EMSGSIZE);
            prefix_size += end - prefix;
        }
    }
    if (!prefix_size) return 0;
    int ret = av_grow_packet(packet, prefix_size);
    if (ret < 0) return ret;
    inserted = input.leading_aud_end;
    memmove(packet->data + inserted + prefix_size, packet->data + inserted,
            packet->size - prefix_size - inserted);
    for (int bit = 1; bit <= 4; bit <<= 1) {
        if (!(required & bit)) continue;
        cursor = 0;
        while (next_nal(extra->extradata, extra->extradata_size, &cursor, &prefix, &payload, &end)) {
            int type = nal_type(avctx->codec_id, extra->extradata + payload, end - payload);
            if (parameter_bit(avctx->codec_id, type) != bit) continue;
            memcpy(packet->data + inserted, extra->extradata + prefix, end - prefix);
            inserted += end - prefix;
        }
    }
    input = nal_summary(avctx->codec_id, packet->data, packet->size);
    s->parameter_sets_seeded = (input.parameter_mask & required) == required;
    fprintf(stderr,
           "T736_CEDAR_PROXY: generation=%u parameter_bootstrap added=%d parameter_mask=%d complete=%d\n",
           s->generation, prefix_size, input.parameter_mask, s->parameter_sets_seeded);
    return 0;
}

static AVRational packet_timebase(const AVCodecContext *avctx)
{
    if (avctx->pkt_timebase.num > 0 && avctx->pkt_timebase.den > 0)
        return avctx->pkt_timebase;
    if (avctx->time_base.num > 0 && avctx->time_base.den > 0)
        return avctx->time_base;
    return AV_TIME_BASE_Q;
}

static int to_us(const AVCodecContext *avctx, int64_t value, int64_t *result)
{
    if (value == AV_NOPTS_VALUE) {
        *result = INT64_MIN;
        return 0;
    }
    *result = av_rescale_q(value, packet_timebase(avctx), AV_TIME_BASE_Q);
    /* av_rescale_q returns INT64_MIN on overflow; do not silently turn a
     * real, unrepresentable timestamp into the protocol's unknown value. */
    return *result == INT64_MIN ? AVERROR(ERANGE) : 0;
}

static int hardware_format_supported(const AVCodecContext *avctx)
{
    const AVPixFmtDescriptor *description;
    if (avctx->bits_per_raw_sample > 8 ||
        (avctx->field_order != AV_FIELD_UNKNOWN &&
         avctx->field_order != AV_FIELD_PROGRESSIVE))
        return 0;
    if (avctx->pix_fmt != AV_PIX_FMT_NONE) {
        description = av_pix_fmt_desc_get(avctx->pix_fmt);
        if (!description || description->nb_components != 3 ||
            description->log2_chroma_w != 1 || description->log2_chroma_h != 1 ||
            (description->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL)))
            return 0;
        for (unsigned i = 0; i < description->nb_components; i++)
            if (description->comp[i].depth != 8)
                return 0;
    }
    if (avctx->profile == AV_PROFILE_UNKNOWN)
        return 1; /* Parser/backend still validates the actual stream. */
    if (avctx->codec_id == AV_CODEC_ID_HEVC)
        return avctx->profile == AV_PROFILE_HEVC_MAIN ||
               avctx->profile == AV_PROFILE_HEVC_MAIN_STILL_PICTURE;
    return avctx->profile == AV_PROFILE_H264_BASELINE ||
           avctx->profile == AV_PROFILE_H264_CONSTRAINED_BASELINE ||
           avctx->profile == AV_PROFILE_H264_MAIN ||
           avctx->profile == AV_PROFILE_H264_HIGH;
}

static int resolve_client(CedarAPI *api)
{
#define LOAD(field) *(void **)(&api->field) = dlsym(RTLD_DEFAULT, "t736_client_" #field)
    LOAD(available); LOAD(open); LOAD(push); LOAD(pull);
    LOAD(drain); LOAD(flush); LOAD(close);
#undef LOAD
    return api->available && api->open && api->push && api->pull &&
           api->drain && api->flush && api->close && api->available() == 1;
}

static void clear_replay(CedarContext *s)
{
    for (int i = 0; i < s->replay_count; i++)
        av_packet_free(&s->replay[i]);
    s->replay_count = s->replay_read = 0;
    s->replay_bytes = 0;
    s->input_in_replay = 0;
}

static void stop_hardware(CedarContext *s)
{
    if (s->session > 0) {
        s->api.close(s->session);
        s->session = 0;
    }
    av_bsf_free(&s->bsf);
    av_buffer_unref(&s->receive_buffer);
    /* Already-delivered AVFrames retain their own buffer references. Pool
     * uninitialization defers destruction of those allocations until release. */
    av_buffer_pool_uninit(&s->pixel_pool);
    if (s->filtered)
        av_packet_unref(s->filtered);
    s->filtered_pending = 0;
}

static int start_software(AVCodecContext *avctx, int reason)
{
    CedarContext *s = avctx->priv_data;
    AVCodecParameters *parameters;
    const AVCodec *codec;
    int ret;

    /* Replaying after any delivered frame would duplicate/reorder output. */
    if (s->delivered)
        return reason < 0 ? reason : AVERROR(EIO);
    stop_hardware(s);
    if (s->input_in_replay) {
        av_packet_unref(s->input);
        s->input_pending = s->input_in_replay = 0;
    }
    codec = avcodec_find_decoder_by_name(avctx->codec_id == AV_CODEC_ID_H264 ?
                                         "h264" : "hevc");
    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    s->software = avcodec_alloc_context3(codec);
    parameters = avcodec_parameters_alloc();
    if (!s->software || !parameters) {
        avcodec_parameters_free(&parameters);
        avcodec_free_context(&s->software);
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_from_context(parameters, avctx);
    if (ret >= 0)
        ret = avcodec_parameters_to_context(s->software, parameters);
    avcodec_parameters_free(&parameters);
    if (ret < 0) {
        avcodec_free_context(&s->software);
        return ret;
    }
    s->software->pkt_timebase = avctx->pkt_timebase;
    s->software->time_base = avctx->time_base;
    s->software->thread_count = avctx->thread_count;
    s->software->thread_type = avctx->thread_type;
    s->software->flags = avctx->flags;
    s->software->flags2 = avctx->flags2;
    s->software->skip_frame = avctx->skip_frame;
    s->software->err_recognition = avctx->err_recognition;
    s->software->max_pixels = avctx->max_pixels;
    s->software->apply_cropping = avctx->apply_cropping;
    ret = avcodec_open2(s->software, codec, NULL);
    if (ret >= 0)
        av_log(avctx, AV_LOG_INFO, "T736_CEDAR_PROXY: native fallback=%s reason=%d replay=%d\n",
               codec->name, reason, s->replay_count);
    else
        avcodec_free_context(&s->software);
    return ret;
}

static int receive_software(AVCodecContext *avctx, AVFrame *frame)
{
    CedarContext *s = avctx->priv_data;
    int ret;
    for (;;) {
        ret = avcodec_receive_frame(s->software, frame);
        if (ret != AVERROR(EAGAIN)) {
            if (ret >= 0) {
                avctx->pix_fmt = frame->format;
                avctx->sw_pix_fmt = frame->format;
                avctx->width = frame->width;
                avctx->height = frame->height;
                avctx->has_b_frames = s->software->has_b_frames;
                avctx->profile = s->software->profile;
                avctx->level = s->software->level;
            }
            return ret;
        }
        if (s->replay_read < s->replay_count) {
            AVPacket *packet = s->replay[s->replay_read];
            ret = avcodec_send_packet(s->software, packet);
            if (ret < 0)
                return ret;
            av_packet_free(&s->replay[s->replay_read++]);
            continue;
        }
        if (!s->input_pending && !s->input_eof) {
            ret = ff_decode_get_packet(avctx, s->input);
            if (ret == AVERROR_EOF)
                s->input_eof = 1;
            else if (ret < 0)
                return ret;
            else
                s->input_pending = 1;
        }
        if (s->input_pending) {
            if (!s->input_logged) {
                log_packet_shape(avctx, "first_input", s->input);
                s->input_logged = 1;
            }
            ret = avcodec_send_packet(s->software, s->input);
            if (ret < 0)
                return ret;
            av_packet_unref(s->input);
            s->input_pending = 0;
        } else if (!s->software_draining) {
            ret = avcodec_send_packet(s->software, NULL);
            if (ret < 0 && ret != AVERROR_EOF)
                return ret;
            s->software_draining = 1;
        } else {
            return AVERROR_BUG; /* Native drain may not require new input. */
        }
    }
}

static int pull_frame(AVCodecContext *avctx, AVFrame *frame, unsigned timeout_ms)
{
    CedarContext *s = avctx->priv_data;
    struct t736_frame picture = { 0 };
    size_t expected;
    int64_t pts;
    int ret;
    if (!s->receive_buffer)
        s->receive_buffer = av_buffer_pool_get(s->pixel_pool);
    if (!s->receive_buffer)
        return AVERROR(ENOMEM);
    ret = s->api.pull(s->session, s->receive_buffer->data, T736_MAX_FRAME,
                      &picture, timeout_ms);
    if (ret != 1)
        return ret;
    if (!picture.width || !picture.height || picture.width > T736_MAX_WIDTH ||
        picture.height > T736_MAX_HEIGHT || (picture.width | picture.height) & 1)
        return AVERROR(EPROTO);
    expected = (size_t)picture.width * picture.height * 3 / 2;
    if (picture.bytes_written != expected)
        return AVERROR(EPROTO);
    if (picture.pts_us != INT64_MIN &&
        (picture.pts_us < 0 || picture.pts_us > INT64_MAX / 1000 ||
         picture.pts_us < s->offset_us + INT64_MIN))
        return AVERROR(EPROTO);
    pts = picture.pts_us == INT64_MIN ? AV_NOPTS_VALUE :
          av_rescale_q(picture.pts_us - s->offset_us, AV_TIME_BASE_Q, packet_timebase(avctx));
    ret = ff_set_dimensions(avctx, picture.width, picture.height);
    if (ret < 0)
        return ret;
    avctx->pix_fmt = avctx->sw_pix_fmt = AV_PIX_FMT_YUV420P;
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = picture.width;
    frame->height = picture.height;
    if (!(picture.width & 63)) {
        /* Packed I420 has 32-byte-aligned Y/U/V strides and plane starts
         * when luma width is a multiple of 64. Transfer ownership of the
         * receive allocation; never overwrite it while Firefox holds it. */
        memset(s->receive_buffer->data + expected, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        ret = av_image_fill_arrays(frame->data, frame->linesize,
                                    s->receive_buffer->data, AV_PIX_FMT_YUV420P,
                                    picture.width, picture.height, 1);
        if (ret < 0)
            return ret;
        frame->buf[0] = s->receive_buffer;
        s->receive_buffer = NULL;
    } else {
        /* Preserve the established aligned copy path for other widths;
         * chroma stride must also meet the consumer's 32-byte alignment. */
        ret = av_frame_get_buffer(frame, 32);
        if (ret < 0)
            return ret;
        const uint8_t *src = s->receive_buffer->data;
        for (int plane = 0; plane < 3; plane++) {
            unsigned width = picture.width >> (plane != 0);
            unsigned height = picture.height >> (plane != 0);
            for (unsigned y = 0; y < height; y++)
                memcpy(frame->data[plane] + (size_t)y * frame->linesize[plane],
                       src + (size_t)y * width, width);
            src += (size_t)width * height;
        }
    }
    frame->pts = frame->best_effort_timestamp = pts;
    frame->pkt_dts = AV_NOPTS_VALUE;
    frame->color_range = picture.color_range == 1 || picture.color_range == 2 ?
                         picture.color_range : avctx->color_range;
    frame->colorspace = picture.colorspace > 0 && picture.colorspace < AVCOL_SPC_NB ?
                        picture.colorspace : avctx->colorspace;
    frame->color_primaries = avctx->color_primaries;
    frame->color_trc = avctx->color_trc;
    frame->chroma_location = avctx->chroma_sample_location;
    frame->sample_aspect_ratio = avctx->sample_aspect_ratio;
    for (unsigned i = 0; i < FFMIN(s->timing_next, TIMING_PACKETS); i++) {
        if (s->timing[i].pts == pts) {
            frame->duration = s->timing[i].duration;
            frame->pkt_dts = s->timing[i].dts;
            frame->pkt_pos = s->timing[i].pos;
            if (s->timing[i].flags & AV_PKT_FLAG_KEY)
                frame->flags |= AV_FRAME_FLAG_KEY;
            break;
        }
    }
    if (s->first_frame_wait_started) {
        fprintf(stderr,
                "T736_CEDAR_PROXY: first_frame_wait_ready elapsed_us=%lld pending_input=%d\n",
                (long long)(av_gettime_relative() - s->first_frame_wait_started), s->input_pending);
        s->first_frame_wait_started = 0;
    }
    s->delivered = 1;
    s->batch_frames++;
    s->backpressure_started = 0;
    clear_replay(s);
    return 1;
}

static int hardware_error(AVCodecContext *avctx, AVFrame *frame, int error)
{
    CedarContext *s = avctx->priv_data;
    int ret;
    if (s->delivered || error == AVERROR(EPROTO)) {
        s->fatal_error = error;
        av_log(avctx, AV_LOG_ERROR, "T736_CEDAR_PROXY: explicit hardware failure after_delivery=%d error=%d\n", s->delivered, error);
        return error;
    }
    ret = start_software(avctx, error);
    if (ret < 0) {
        s->fatal_error = ret;
        return ret;
    }
    return receive_software(avctx, frame);
}

static int normalize_timestamp(CedarContext *s, int64_t original, int64_t *result)
{
    if (original == INT64_MIN) {
        *result = INT64_MIN;
        return 0;
    }
    if (original < -s->offset_us || original > INT64_MAX - s->offset_us ||
        original + s->offset_us > INT64_MAX / 1000)
        return AVERROR(EINVAL);
    *result = original + s->offset_us;
    return 0;
}

static int cedar_receive(AVCodecContext *avctx, AVFrame *frame)
{
    CedarContext *s = avctx->priv_data;
    int ret;
    if (s->fatal_error)
        return s->fatal_error;
    if (s->software)
        return receive_software(avctx, frame);
    if (!s->batch_started)
        s->batch_started = av_gettime_relative();
    for (;;) {
        /* Firefox publishes this whole receive-until-EAGAIN batch only
         * after the loop ends. A ready hardware backlog must not turn one
         * DecodePromise into many serial WC copies. After one output,
         * prioritize consuming all already-owned/queued input before the
         * safe EAGAIN point below. Backpressure may still need extra output
         * to make progress, and EOS must drain every remaining picture. */
        if (!s->batch_frames || s->broker_draining) {
            unsigned timeout = s->broker_draining ? 1000 : 0;
            ret = pull_frame(avctx, frame, timeout);
            if (ret == 1)
                return 0;
            if (ret < 0)
                return hardware_error(avctx, frame, ret);
            if (ret == 2)
                return s->broker_draining ? AVERROR_EOF : hardware_error(avctx, frame, AVERROR(EPROTO));
        }
        if (s->broker_draining) {
            if (av_gettime_relative() - s->drain_started > 5000000)
                return hardware_error(avctx, frame, AVERROR(ETIMEDOUT));
            continue;
        }
        if (s->filtered_pending) {
            int64_t pts, dts;
            ret = to_us(avctx, s->filtered->pts, &pts);
            if (ret >= 0) ret = to_us(avctx, s->filtered->dts, &dts);
            if (ret < 0)
                return hardware_error(avctx, frame, ret);
            if (!s->offset_set) {
                int64_t earliest = 0;
                if (pts != INT64_MIN && pts < earliest) earliest = pts;
                if (dts != INT64_MIN && dts < earliest) earliest = dts;
                s->offset_us = -earliest;
                s->offset_set = 1;
            }
            ret = normalize_timestamp(s, pts, &pts);
            if (ret >= 0) ret = normalize_timestamp(s, dts, &dts);
            if (ret < 0 || s->filtered->size > T736_MAX_PACKET)
                return hardware_error(avctx, frame, ret < 0 ? ret : AVERROR(EMSGSIZE));
            /* Firefox's mTimecode can put B-frame DTS after its PTS.
             * GstBaseParse clamps such PTS to DTS, destroying presentation
             * order. The hardware bitstream already carries decode order;
             * leave DTS unknown only on this broker path. Original packet
             * DTS remains intact in replay and output timing metadata. */
            ret = s->api.push(s->session, s->filtered->data, s->filtered->size,
                              pts, INT64_MIN);
            if (ret == AVERROR(EAGAIN)) {
                if (!s->backpressure_started)
                    s->backpressure_started = av_gettime_relative();
                ret = pull_frame(avctx, frame, 20);
                if (ret == 1) return 0;
                if (ret < 0) return hardware_error(avctx, frame, ret);
                if (ret == 2) return hardware_error(avctx, frame, AVERROR(EPROTO));
                /* This AU is already owned by the decoder. Returning EAGAIN
                 * while it is blocked can leave another packet in FFmpeg's
                 * input slot, making both send and receive report EAGAIN.
                 * Wait for bounded internal progress instead. */
                if (av_gettime_relative() - s->backpressure_started > 5000000)
                    return hardware_error(avctx, frame, AVERROR(ETIMEDOUT));
                continue;
            }
            if (ret < 0)
                return hardware_error(avctx, frame, ret);
            s->backpressure_started = 0;
            av_packet_unref(s->filtered);
            s->filtered_pending = 0;
            continue;
        }
        ret = av_bsf_receive_packet(s->bsf, s->filtered);
        if (ret == 0) {
            if (!s->filtered_logged) {
                log_packet_shape(avctx, "first_filtered", s->filtered);
                s->filtered_logged = 1;
            }
            ret = seed_parameter_sets(avctx, s->filtered);
            if (ret < 0)
                return hardware_error(avctx, frame, ret);
            s->filtered_pending = 1;
            continue;
        }
        if (ret == AVERROR_EOF) {
            ret = s->api.drain(s->session);
            if (ret < 0)
                return hardware_error(avctx, frame, ret);
            s->broker_draining = 1;
            s->drain_started = av_gettime_relative();
            continue;
        }
        if (ret != AVERROR(EAGAIN))
            return hardware_error(avctx, frame, ret);
        /* A first-frame wait may return output while retaining the next
         * original AU. Consume that AU before requesting another packet. */
        ret = s->input_pending ? 0 : ff_decode_get_packet(avctx, s->input);
        if (ret == AVERROR(EAGAIN)) {
            /* This is the only bounded-yield point: filtered_pending and
             * input_pending are empty, our BSF requested input, and the
             * FFmpeg input filterchain (including buffer_pkt) is empty.
             * Therefore the next send_packet can accept input. Never return
             * EAGAIN merely because an output quota was reached while any
             * of those slots still owns data. Do not wait for new output. */
            if (!s->batch_frames) {
                ret = pull_frame(avctx, frame, 0);
                if (ret == 1) return 0;
                if (ret < 0) return hardware_error(avctx, frame, ret);
                if (ret == 2) return hardware_error(avctx, frame, AVERROR(EPROTO));
            }
            if (s->batch_frames && s->batch_logs < 12) {
                fprintf(stderr,
                        "T736_CEDAR_PROXY: bounded_batch generation=%u frames=%u inputs=%u elapsed_us=%lld safe_input_empty=1\n",
                        s->generation, s->batch_frames, s->batch_inputs,
                        (long long)(av_gettime_relative() - s->batch_started));
                s->batch_logs++;
            }
            s->batch_frames = s->batch_inputs = 0;
            s->batch_started = 0;
            return AVERROR(EAGAIN);
        }
        if (ret == AVERROR_EOF) {
            s->input_eof = 1;
            if (s->bsf_draining)
                return hardware_error(avctx, frame, AVERROR_BUG);
            ret = av_bsf_send_packet(s->bsf, NULL);
            if (ret < 0)
                return hardware_error(avctx, frame, ret);
            s->bsf_draining = 1;
            continue;
        }
        if (ret < 0)
            return ret;
        if (!s->input_logged) {
            log_packet_shape(avctx, "first_input", s->input);
            s->input_logged = 1;
        }
        s->input_pending = 1;
        if (!s->delivered) {
            if (s->input->size > T736_MAX_PACKET)
                return hardware_error(avctx, frame, AVERROR(EMSGSIZE));
            if (s->replay_count >= REPLAY_PACKETS || s->input->size > T736_MAX_PACKET - s->replay_bytes) {
                int64_t remaining;
                if (!s->replay_count)
                    return hardware_error(avctx, frame, AVERROR(ENOSPC));
                if (!s->first_frame_wait_started) {
                    s->first_frame_wait_started = av_gettime_relative();
                    fprintf(stderr,
                            "T736_CEDAR_PROXY: first_frame_wait_start replay_packets=%d replay_bytes=%zu pending_bytes=%d deadline_ms=5000\n",
                            s->replay_count, s->replay_bytes, s->input->size);
                }
                remaining = 5000000 - (av_gettime_relative() - s->first_frame_wait_started);
                if (remaining <= 0) {
                    fprintf(stderr, "T736_CEDAR_PROXY: first_frame_wait_timeout native_replay_pending=%d\n", s->input_pending);
                    return hardware_error(avctx, frame, AVERROR(ETIMEDOUT));
                }
                /* Do not feed beyond the replay budget while the asynchronous
                 * pipeline initializes. Keep the not-yet-cached original AU
                 * for either subsequent hardware submission or native replay. */
                ret = pull_frame(avctx, frame, (unsigned)FFMIN(1000, (remaining + 999) / 1000));
                if (ret == 1) return 0;
                if (ret < 0) return hardware_error(avctx, frame, ret);
                if (ret == 2) return hardware_error(avctx, frame, AVERROR(EPROTO));
                continue;
            }
            AVPacket *copy = av_packet_clone(s->input);
            if (!copy)
                return hardware_error(avctx, frame, AVERROR(ENOMEM));
            s->replay[s->replay_count++] = copy;
            s->replay_bytes += copy->size;
            s->input_in_replay = 1;
        }
        CedarTiming *timing = &s->timing[s->timing_next++ % TIMING_PACKETS];
        *timing = (CedarTiming){s->input->pts, s->input->duration, s->input->dts, s->input->pos, s->input->flags};
        ret = av_bsf_send_packet(s->bsf, s->input);
        if (ret < 0)
            return hardware_error(avctx, frame, ret);
        s->batch_inputs++;
        s->input_pending = s->input_in_replay = 0;
    }
}

static av_cold int cedar_init(AVCodecContext *avctx)
{
    CedarContext *s = avctx->priv_data;
    const AVBitStreamFilter *filter;
    int ret;
    log_extradata(avctx, "input", avctx->extradata, avctx->extradata_size);
    s->input = av_packet_alloc();
    s->filtered = av_packet_alloc();
    if (!s->input || !s->filtered)
        return AVERROR(ENOMEM);
    if (avctx->width <= 0 || avctx->height <= 0 || avctx->width > T736_MAX_WIDTH ||
        avctx->height > T736_MAX_HEIGHT || (avctx->width | avctx->height) & 1 ||
        !hardware_format_supported(avctx) || !resolve_client(&s->api))
        return start_software(avctx, AVERROR(ENOTSUP));
    filter = av_bsf_get_by_name(avctx->codec_id == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb");
    ret = filter ? av_bsf_alloc(filter, &s->bsf) : AVERROR_BSF_NOT_FOUND;
    if (ret >= 0) ret = avcodec_parameters_from_context(s->bsf->par_in, avctx);
    if (ret >= 0) {
        s->bsf->time_base_in = packet_timebase(avctx);
        ret = av_bsf_init(s->bsf);
    }
    if (ret < 0)
        return start_software(avctx, ret);
    log_extradata(avctx, "bsf_output", s->bsf->par_out->extradata,
                  s->bsf->par_out->extradata_size);
    s->pixel_pool = av_buffer_pool_init(T736_MAX_FRAME + AV_INPUT_BUFFER_PADDING_SIZE, NULL);
    if (!s->pixel_pool)
        return start_software(avctx, AVERROR(ENOMEM));
    ret = s->api.open(avctx->codec_id == AV_CODEC_ID_H264 ? T736_CODEC_H264 : T736_CODEC_HEVC,
                      avctx->width, avctx->height);
    if (ret <= 0)
        return start_software(avctx, ret < 0 ? ret : AVERROR(EPROTO));
    s->session = ret;
    avctx->pix_fmt = avctx->sw_pix_fmt = AV_PIX_FMT_YUV420P;
    av_log(avctx, AV_LOG_INFO, "T736_CEDAR_PROXY: broker session=%d codec=%s %dx%d ordinary-I420\n",
           s->session, avctx->codec->name, avctx->width, avctx->height);
    return 0;
}

static void cedar_flush(AVCodecContext *avctx)
{
    CedarContext *s = avctx->priv_data;
    clear_replay(s);
    av_packet_unref(s->input);
    av_packet_unref(s->filtered);
    s->input_pending = s->filtered_pending = s->input_eof = 0;
    s->bsf_draining = s->broker_draining = s->software_draining = 0;
    s->delivered = s->offset_set = 0;
    s->offset_us = s->drain_started = s->backpressure_started = s->first_frame_wait_started = 0;
    s->timing_next = 0;
    s->batch_frames = s->batch_inputs = s->batch_logs = 0;
    s->batch_started = 0;
    s->generation++;
    s->parameter_sets_seeded = s->input_logged = s->filtered_logged = 0;
    if (s->software)
        avcodec_flush_buffers(s->software);
    else if (s->bsf && s->session > 0) {
        av_bsf_flush(s->bsf);
        s->fatal_error = s->api.flush(s->session);
        if (s->fatal_error < 0)
            av_log(avctx, AV_LOG_ERROR, "T736_CEDAR_PROXY: flush failed %d\n", s->fatal_error);
    }
}

static av_cold int cedar_close(AVCodecContext *avctx)
{
    CedarContext *s = avctx->priv_data;
    stop_hardware(s);
    clear_replay(s);
    avcodec_free_context(&s->software);
    av_packet_free(&s->input);
    av_packet_free(&s->filtered);
    return 0;
}

#define CEDAR_DECODER(symbol, name_, codec_) \
const FFCodec symbol = { \
    .p.name = name_, .p.long_name = NULL_IF_CONFIG_SMALL("T736 Cedar broker with native fallback"), \
    .p.type = AVMEDIA_TYPE_VIDEO, .p.id = codec_, \
    .p.capabilities = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_OTHER_THREADS, \
    .caps_internal = FF_CODEC_CAP_INIT_CLEANUP | FF_CODEC_CAP_SETS_FRAME_PROPS | FF_CODEC_CAP_AUTO_THREADS, \
    .priv_data_size = sizeof(CedarContext), .init = cedar_init, .close = cedar_close, .flush = cedar_flush, \
    FF_CODEC_RECEIVE_FRAME_CB(cedar_receive), \
    .p.wrapper_name = "t736_cedar_proxy", \
}

CEDAR_DECODER(ff_h264_cedar_decoder, "h264_cedar", AV_CODEC_ID_H264);
CEDAR_DECODER(ff_hevc_cedar_decoder, "hevc_cedar", AV_CODEC_ID_HEVC);
