/* SPDX-License-Identifier: MIT
 * Bounded experimental Cedar broker. One hardware lease and one retained
 * codec/size context. The v27 kernel quarantines imported DMA until reboot;
 * keeping the pipelines alive avoids repeated allocation on every tab/video.
 */
#define _GNU_SOURCE
#include "cedar_protocol.h"
#include "cedar_backend.h"
#include "cedar_shm.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct retained {
    cedar_decoder *decoder;
    unsigned codec, width, height;
    uint32_t lease;
    int owner, failed;
};
#define CONTEXT_COUNT 1U
static struct retained contexts[CONTEXT_COUNT];
static pthread_mutex_t hardware_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t peers_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_lease;
static unsigned peers;
static uid_t allowed_uid;
static int poisoned;
static unsigned long long submitted, delivered;

static int backend_result(int result)
{
    if (result == CEDAR_BACKPRESSURE) return -EAGAIN;
    if (result == CEDAR_ERROR) return -EIO;
    return result;
}

static int io_all(int fd, void *buf, size_t size, int writing)
{
    unsigned char *p = buf;
    while (size) {
        ssize_t n = writing ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n ? -errno : -EPIPE;
        p += n; size -= (size_t)n;
    }
    return 0;
}

static struct retained *lookup(uint32_t lease, int owner)
{
    for (unsigned i = 0; i < CONTEXT_COUNT; ++i)
        if (lease && contexts[i].lease == lease && contexts[i].owner == owner)
            return &contexts[i];
    return NULL;
}

static int open_context(int owner, const struct t736_open *args, uint32_t *lease)
{
    if (poisoned) return -EIO;
    if ((args->codec != T736_CODEC_H264 && args->codec != T736_CODEC_HEVC) ||
        !args->width || !args->height || args->width > T736_MAX_WIDTH ||
        args->height > T736_MAX_HEIGHT || ((args->width | args->height) & 1) || args->reserved)
        return -EINVAL;
    struct retained *slot = NULL;
    for (unsigned i = 0; i < CONTEXT_COUNT; ++i) {
        if (contexts[i].lease) return -EBUSY;
        if (contexts[i].decoder && contexts[i].codec == args->codec &&
            contexts[i].width == args->width && contexts[i].height == args->height)
            slot = &contexts[i];
    }
    if (!slot) {
        for (unsigned i = 0; i < CONTEXT_COUNT; ++i)
            if (!contexts[i].decoder && !contexts[i].failed) { slot = &contexts[i]; break; }
        if (!slot) return -ENOSPC;
        char error[512] = {0};
        slot->decoder = cedar_open(args->codec, args->width, args->height, error, sizeof(error));
        if (!slot->decoder) {
            slot->failed = 1;
            fprintf(stderr, "CEDAR_BROKER: init failed: %s\n", error);
            return -EIO;
        }
        slot->codec = args->codec; slot->width = args->width; slot->height = args->height;
    } else {
        if (slot->failed) return -EIO;
        int ret = backend_result(cedar_resume(slot->decoder));
        if (ret < 0) { slot->failed = 1; return ret; }
    }
    if (++next_lease > INT_MAX) { poisoned = 1; return -EOVERFLOW; }
    slot->lease = next_lease; slot->owner = owner; *lease = next_lease;
    fprintf(stderr, "CEDAR_BROKER: open session=%u codec=%u %ux%u\n",
            *lease, args->codec, args->width, args->height);
    return 0;
}

static int64_t us_to_ns(int64_t value)
{
    if (value == INT64_MIN) return INT64_MIN;
    return value * 1000;
}

static int valid_time(int64_t value)
{
    return value == INT64_MIN || (value >= 0 && value <= INT64_MAX / 1000);
}

static int execute(int fd, struct t736_message *message, unsigned char *body,
                   struct t736_picture *picture, cedar_frame *frame)
{
    unsigned length = message->length;
    message->length = 0;
    if (message->operation == T736_OPEN) {
        if (length != sizeof(struct t736_open) || message->session) return -EINVAL;
        return open_context(fd, (const struct t736_open *)body, &message->session);
    }
    struct retained *slot = lookup(message->session, fd);
    if (!slot) return message->operation == T736_CLOSE && !length ? 0 : -ESTALE;
    if (message->operation == T736_CLOSE) {
        if (length) return -EINVAL;
        fprintf(stderr, "CEDAR_BROKER: close session=%u submitted=%llu delivered=%llu\n",
                slot->lease, submitted, delivered);
        int ret = backend_result(cedar_idle(slot->decoder));
        if (ret < 0) { slot->failed = 1; poisoned = 1; }
        slot->lease = 0; slot->owner = -1;
        return ret;
    }
    if (poisoned || slot->failed) return -EIO;
    int result;
    switch (message->operation) {
    case T736_PUSH: {
        if (length <= sizeof(struct t736_packet) || length > T736_MAX_PACKET + sizeof(struct t736_packet))
            return -EINVAL;
        struct t736_packet *packet = (struct t736_packet *)body;
        if (!valid_time(packet->pts_us) || !valid_time(packet->dts_us)) return -EINVAL;
        result = cedar_push(slot->decoder, body + sizeof(*packet), length - sizeof(*packet),
                            us_to_ns(packet->pts_us), us_to_ns(packet->dts_us));
        if (!result) submitted++;
        break;
    }
    case T736_PULL: {
        if (length != sizeof(struct t736_pull)) return -EINVAL;
        struct t736_pull *args = (struct t736_pull *)body;
        if (args->capacity > T736_MAX_FRAME || args->timeout_ms > 1000) return -EINVAL;
        if (args->capacity < slot->width * slot->height * 3U / 2U) return -ENOSPC;
        result = cedar_pull(slot->decoder, frame, args->timeout_ms);
        if (result == 1) {
            if (!frame->data || !frame->width || !frame->height ||
                frame->width > T736_MAX_WIDTH || frame->height > T736_MAX_HEIGHT ||
                ((frame->width | frame->height) & 1) ||
                frame->size != (size_t)frame->width * frame->height * 3 / 2 ||
                frame->size > args->capacity) {
                cedar_frame_free(frame); slot->failed = 1; return -EOVERFLOW;
            }
            *picture = (struct t736_picture){ .width = frame->width, .height = frame->height,
                .bytes = (uint32_t)frame->size, .color_range = frame->color_range,
                .colorspace = frame->color_matrix == 1 ? 1 :
                              frame->color_matrix == 2 ? 6 :
                              frame->color_matrix == 3 ? 9 : 0,
                .pts_us = frame->pts_ns == INT64_MIN ? INT64_MIN : frame->pts_ns / 1000 };
            message->length = sizeof(*picture);
            delivered++;
            if (delivered == 1 || delivered % 300 == 0)
                fprintf(stderr, "CEDAR_BROKER: hardware frames=%llu session=%u visible=%ux%u\n",
                        delivered, slot->lease, picture->width, picture->height);
        }
        break;
    }
    case T736_DRAIN:
        if (length) return -EINVAL;
        result = cedar_eos(slot->decoder);
        break;
    case T736_FLUSH:
        if (length) return -EINVAL;
        result = cedar_flush(slot->decoder);
        break;
    default: return -ENOSYS;
    }
    result = backend_result(result);
    if (result < 0 && result != -EAGAIN && result != -EINVAL) {
        slot->failed = 1;
        poisoned = 1;
        fprintf(stderr, "CEDAR_BROKER: failed session=%u op=%u rc=%d detail=%s\n",
                slot->lease, message->operation, result, cedar_error(slot->decoder));
    }
    return result;
}

static int setup_peer(int fd, struct t736_shm *shm)
{
    struct t736_message request;
    struct { struct t736_message message; struct t736_setup setup; } response = {0};
    struct ucred peer;
    socklen_t size = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) ||
        size != sizeof(peer) || peer.uid != allowed_uid || peer.pid <= 0) return -EACCES;
    int received = t736_receive_descriptor(fd, &request, sizeof(request), NULL);
    if (received < 0) return received;
    if (request.magic != T736_MAGIC || request.version != T736_PROTOCOL_VERSION ||
        request.operation != T736_SETUP || request.length || request.sequence != 1 ||
        request.session || request.status || request.reserved) return -EPROTO;
    int error = t736_shm_create(shm, &response.setup);
    if (error) return error;
    response.message = request;
    response.message.length = sizeof(response.setup);
    ssize_t sent = t736_send_descriptor(fd, shm->fd, &response, sizeof(response));
    if (sent <= 0) return sent ? -errno : -EPIPE;
    if ((size_t)sent < sizeof(response))
        return io_all(fd, (uint8_t *)&response + sent, sizeof(response) - (size_t)sent, 1);
    return 0;
}

static void *serve(void *opaque)
{
    int fd = (int)(intptr_t)opaque;
    struct t736_shm shm = { .fd = -1 };
    unsigned char *body = NULL;
    uint32_t sequence = 1;
    struct timeval timeout = { .tv_sec = 15 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) goto finish;
    if (setup_peer(fd, &shm) < 0) goto finish;
    body = malloc(T736_MAX_BODY);
    if (!body) goto finish;
    for (;;) {
        struct pollfd pollfd = { .fd = fd, .events = POLLIN };
        int ready;
        do { ready = poll(&pollfd, 1, -1); } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || !(pollfd.revents & POLLIN)) break;
        struct t736_message message;
        if (io_all(fd, &message, sizeof(message), 0) < 0) break;
        if (message.magic != T736_MAGIC || message.version != T736_PROTOCOL_VERSION ||
            message.length > T736_MAX_BODY || message.reserved || message.status ||
            sequence == UINT32_MAX || message.sequence != sequence + 1 ||
            message.operation < T736_OPEN || message.operation > T736_CLOSE)
            break;
        sequence = message.sequence;
        if (message.length && io_all(fd, body, message.length, 0) < 0) break;
        cedar_frame frame = {0};
        struct t736_picture picture = {0};
        pthread_mutex_lock(&hardware_lock);
        message.status = execute(fd, &message, body, &picture, &frame);
        pthread_mutex_unlock(&hardware_lock);
        if (message.status == 1 && message.operation == T736_PULL) {
            /* This connection has one request at a time. The client consumes
             * the previous slot under its mutex before sending this request.
             * Every peer gets a distinct memfd, never a recycled live slot. */
            memcpy(shm.data, frame.data, frame.size);
            __atomic_thread_fence(__ATOMIC_RELEASE);
        }
        cedar_frame_free(&frame);
        int result = io_all(fd, &message, sizeof(message), 1);
        if (!result && message.length) result = io_all(fd, &picture, sizeof(picture), 1);
        if (result < 0) break;
    }
finish:
    free(body);
    t736_shm_destroy(&shm);
    pthread_mutex_lock(&hardware_lock);
    for (unsigned i = 0; i < CONTEXT_COUNT; ++i)
        if (contexts[i].lease && contexts[i].owner == fd) {
            if (cedar_idle(contexts[i].decoder) < 0) {
                contexts[i].failed = 1; poisoned = 1;
            }
            contexts[i].lease = 0; contexts[i].owner = -1;
            fprintf(stderr, "CEDAR_BROKER: peer left; context retained\n");
        }
    pthread_mutex_unlock(&hardware_lock);
    close(fd);
    pthread_mutex_lock(&peers_lock); peers--; pthread_mutex_unlock(&peers_lock);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "Usage: cedar-broker ALLOWED_NUMERIC_UID\n"); return 2; }
    char *end;
    unsigned long uid = strtoul(argv[1], &end, 10);
    if (!argv[1][0] || *end || uid == 0 || uid > UINT_MAX) return 2;
    allowed_uid = (uid_t)uid;
    signal(SIGPIPE, SIG_IGN);
    umask(0007);
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    strcpy(address.sun_path, T736_SOCKET_PATH);
    /* Never replace an existing service socket or arbitrary path. */
    if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) || listen(server, 8)) {
        perror("cedar-broker socket"); return 1;
    }
    fprintf(stderr, "CEDAR_BROKER: ready uid=%lu protocol=%u readonly-memfd max=1080p8bit retained_contexts=1\n",
            uid, T736_PROTOCOL_VERSION);
    for (;;) {
        int fd = accept4(server, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR) continue; perror("accept"); break; }
        struct ucred peer;
        socklen_t size = sizeof(peer);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != allowed_uid) {
            close(fd); continue;
        }
        pthread_mutex_lock(&peers_lock);
        if (peers >= 8) { pthread_mutex_unlock(&peers_lock); close(fd); continue; }
        peers++;
        pthread_mutex_unlock(&peers_lock);
        pthread_t thread;
        int error = pthread_create(&thread, NULL, serve, (void *)(intptr_t)fd);
        if (error) {
            close(fd);
            pthread_mutex_lock(&peers_lock); peers--; pthread_mutex_unlock(&peers_lock);
        } else pthread_detach(thread);
    }
    return 1;
}
