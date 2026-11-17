/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "cedar_protocol.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int connection = -1;
static uint32_t serial_number;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int trace;
static unsigned trace_push, trace_pull;

static int transfer(void *buf, size_t size, int sending)
{
    unsigned char *p = buf;
    while (size) {
        ssize_t n = sending ? send(connection, p, size, MSG_NOSIGNAL)
                            : recv(connection, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int result = n ? -errno : -EPIPE;
            close(connection);
            connection = -1;
            return result;
        }
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

/* glibc invokes init-array functions with the original process arguments.
 * Firefox's forkserver must be disabled by the isolated launcher, so RDD has
 * its own exec and constructor. Browser/content/utility processes never connect.
 */
__attribute__((constructor)) static void preconnect(int argc, char **argv,
                                                    char **envp)
{
    (void)envp;
    const char *enabled = getenv("T736_CEDAR_ENABLE");
    const char *test = getenv("T736_CEDAR_TEST_CLIENT");
    const char *trace_env = getenv("T736_CEDAR_TRACE");
    trace = trace_env && !strcmp(trace_env, "1");
    int rdd = argc > 1 && !strcmp(argv[argc - 1], "rdd");
    if (!enabled || strcmp(enabled, "1") ||
        (!rdd && (!test || strcmp(test, "1")))) return;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct timeval deadline = { .tv_sec = 15 };
    struct ucred peer;
    socklen_t peer_length = sizeof(peer);
    strcpy(address.sun_path, T736_SOCKET_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline)) ||
        connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length)) {
        close(fd);
        return;
    }
    /* The service-owned runtime directory prevents a desktop-user spoofed peer.
     * Device ownership and service UID are enforced by the systemd unit.
     */
    connection = fd;
    fprintf(stderr, "T736_CEDAR_CLIENT: preconnected pid=%ld rdd=%d\n",
            (long)getpid(), rdd);
}

int t736_client_available(void)
{
    pthread_mutex_lock(&lock);
    int available = connection >= 0;
    pthread_mutex_unlock(&lock);
    return available;
}

static int request(unsigned op, int session, const void *prefix, size_t prefix_n,
                   const void *data, size_t data_n, struct t736_message *reply)
{
    if (connection < 0) return -ENOTCONN;
    if (prefix_n + data_n > T736_MAX_BODY) return -E2BIG;
    struct t736_message message = { T736_MAGIC, T736_PROTOCOL_VERSION, op,
        (uint32_t)(prefix_n + data_n), ++serial_number, (uint32_t)session, 0, 0 };
    int error = transfer(&message, sizeof(message), 1);
    if (!error && prefix_n) error = transfer((void *)prefix, prefix_n, 1);
    if (!error && data_n) error = transfer((void *)data, data_n, 1);
    if (!error) error = transfer(reply, sizeof(*reply), 0);
    if (error) return error;
    if (reply->magic != T736_MAGIC || reply->version != T736_PROTOCOL_VERSION ||
        reply->operation != op || reply->sequence != message.sequence ||
        reply->reserved || reply->length > T736_MAX_FRAME + sizeof(struct t736_picture) ||
        (op != T736_OPEN && reply->session != message.session)) {
        close(connection); connection = -1;
        return -EPROTO;
    }
    return 0;
}

static int simple(unsigned op, int session, const void *prefix, size_t prefix_n,
                  const void *data, size_t data_n)
{
    struct t736_message reply;
    pthread_mutex_lock(&lock);
    int error = request(op, session, prefix, prefix_n, data, data_n, &reply);
    if (!error && reply.length) {
        close(connection); connection = -1; error = -EPROTO;
    }
    if (!error) error = reply.status;
    if (!error && op == T736_OPEN) error = (int)reply.session;
    pthread_mutex_unlock(&lock);
    return error;
}

int t736_client_open(int codec, unsigned width, unsigned height)
{
    if ((codec != T736_CODEC_H264 && codec != T736_CODEC_HEVC) ||
        !width || !height || width > T736_MAX_WIDTH || height > T736_MAX_HEIGHT ||
        ((width | height) & 1)) return -EINVAL;
    struct t736_open args = { (uint32_t)codec, width, height, 0 };
    return simple(T736_OPEN, 0, &args, sizeof(args), NULL, 0);
}

int t736_client_push(int session, const uint8_t *data, size_t size,
                     int64_t pts_us, int64_t dts_us)
{
    if (session <= 0 || !data || !size || size > T736_MAX_PACKET) return -EINVAL;
    struct t736_packet args = { pts_us, dts_us };
    int ret = simple(T736_PUSH, session, &args, sizeof(args), data, size);
    if (!ret && trace && ++trace_push <= 24)
        fprintf(stderr, "CEDAR_TRACE push=%u session=%d bytes=%zu pts=%" PRId64 " dts=%" PRId64 "\n",
                trace_push, session, size, pts_us, dts_us);
    return ret;
}

int t736_client_pull(int session, uint8_t *dst, size_t capacity,
                     struct t736_frame *frame, unsigned timeout_ms)
{
    if (session <= 0 || !dst || !frame || capacity > T736_MAX_FRAME || timeout_ms > 1000)
        return -EINVAL;
    struct t736_pull args = { (uint32_t)capacity, timeout_ms };
    struct t736_message reply;
    struct t736_picture picture;
    pthread_mutex_lock(&lock);
    int error = request(T736_PULL, session, &args, sizeof(args), NULL, 0, &reply);
    if (error) goto done;
    if (reply.status != 1) {
        if (reply.length || reply.status > 2) goto malformed;
        error = reply.status;
        goto done;
    }
    if (reply.length < sizeof(picture)) goto malformed;
    error = transfer(&picture, sizeof(picture), 0);
    if (error) goto done;
    if (!picture.width || !picture.height || picture.width > T736_MAX_WIDTH ||
        picture.height > T736_MAX_HEIGHT || ((picture.width | picture.height) & 1) ||
        picture.bytes != picture.width * picture.height * 3 / 2 ||
        picture.bytes > capacity || reply.length != sizeof(picture) + picture.bytes ||
        picture.reserved) goto malformed;
    error = transfer(dst, picture.bytes, 0);
    if (!error) {
        frame->width = picture.width; frame->height = picture.height;
        frame->bytes_written = picture.bytes; frame->pts_us = picture.pts_us;
        frame->color_range = picture.color_range; frame->colorspace = picture.colorspace;
        if (trace && ++trace_pull <= 24)
            fprintf(stderr, "CEDAR_TRACE pull=%u session=%d pts=%" PRId64 " %ux%u\n",
                    trace_pull, session, frame->pts_us, frame->width, frame->height);
        error = 1;
    }
    goto done;
malformed:
    close(connection); connection = -1; error = -EPROTO;
done:
    pthread_mutex_unlock(&lock);
    return error;
}

int t736_client_drain(int session) { return simple(T736_DRAIN, session, NULL, 0, NULL, 0); }
int t736_client_flush(int session) { return simple(T736_FLUSH, session, NULL, 0, NULL, 0); }
int t736_client_close(int session)
{
    if (session <= 0) return 0;
    return simple(T736_CLOSE, session, NULL, 0, NULL, 0);
}
