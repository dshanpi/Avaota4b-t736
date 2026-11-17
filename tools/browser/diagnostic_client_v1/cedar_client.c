/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "cedar_diagnostic.h"
#include "cedar_protocol.h"
#include "cedar_shm.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
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
static const uint8_t *shared_pixels;
static pid_t connected_pid;
static uint32_t active_session, active_width, active_height;

static void disconnect_client(void)
{
    if (shared_pixels && connected_pid == getpid())
        munmap((void *)shared_pixels, T736_MAX_FRAME);
    shared_pixels = NULL;
    if (connection >= 0) close(connection);
    connection = -1;
    active_session = active_width = active_height = 0;
}

static int current_process(void)
{
    /* DONTFORK removes the mapping in a fork child. Check before taking a
     * possibly inherited locked pthread mutex; never reconnect in sandbox. */
    return !connected_pid || connected_pid == getpid();
}

static int transfer(void *buf, size_t size, int sending)
{
    unsigned char *p = buf;
    while (size) {
        ssize_t n = sending ? send(connection, p, size, MSG_NOSIGNAL)
                            : recv(connection, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int result = n ? -errno : -EPIPE;
            disconnect_client();
            return result;
        }
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

static int setup_connection(int fd, uid_t expected_uid)
{
    struct ucred peer = {0};
    socklen_t length = sizeof(peer);
    struct t736_message request = { T736_MAGIC, T736_PROTOCOL_VERSION, T736_SETUP, 0, 1, 0, 0, 0 };
    struct { struct t736_message message; struct t736_setup setup; } response;
    int descriptor = -1, error;
    int rc = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length);
    diag("peer.credentials", rc, rc ? errno : 0, peer.uid, peer.pid);
    diag("peer.expectation", length, 0, expected_uid, sizeof(peer));
    if (rc || length != sizeof(peer) || peer.uid != expected_uid || peer.pid <= 0) {
        diag("peer.reject", -EACCES, 0, peer.uid, peer.pid);
        close(fd); return -EACCES;
    }
    connection = fd; connected_pid = getpid();
    error = transfer(&request, sizeof(request), 1);
    diag("setup.send", error, error < 0 ? -error : 0, 0, 0);
    if (!error) {
        error = t736_receive_descriptor(fd, &response, sizeof(response), &descriptor);
        diag("setup.receive", error, error < 0 ? -error : 0, descriptor >= 0, 0);
    }
    if (!error && (response.message.magic != T736_MAGIC ||
        response.message.version != T736_PROTOCOL_VERSION || response.message.operation != T736_SETUP ||
        response.message.length != sizeof(response.setup) || response.message.sequence != 1 ||
        response.message.session || response.message.status || response.message.reserved)) {
        diag("setup.header.reject", -EPROTO, 0, response.message.version, response.message.operation);
        error = -EPROTO;
    }
    if (!error) {
        error = t736_shm_map_receiver(descriptor, &response.setup, expected_uid, &shared_pixels);
        diag("receiver.result", error, error < 0 ? -error : 0, 0, 0);
    }
    if (descriptor >= 0) close(descriptor);
    if (error) { disconnect_client(); return error; }
    serial_number = 1;
    return 0;
}

/* glibc invokes init-array functions with the original process arguments.
 * Firefox's forkserver must be disabled by the isolated launcher, so RDD has
 * its own exec and constructor. Browser/content/utility processes never connect.
 */
__attribute__((constructor)) static void preconnect(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *enabled = getenv("T736_CEDAR_ENABLE");
    const char *test = getenv("T736_CEDAR_TEST_CLIENT");
    const char *trace_env = getenv("T736_CEDAR_TRACE");
    trace = trace_env && !strcmp(trace_env, "1");
    int rdd = argc > 1 && !strcmp(argv[argc - 1], "rdd");
    int enable_gate = enabled && !strcmp(enabled,"1");
    int test_gate = test && !strcmp(test,"1");
    diag("constructor", argc, 0, enable_gate, rdd);
    if (!enable_gate || (!rdd && !test_gate)) { diag("gate.reject", 0, 0, enable_gate, test_gate); return; }
    diag_namespace();
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    struct timeval deadline = { .tv_sec = 15 };
    struct passwd account, *resolved = NULL;
    char account_buffer[16384];
    int rc = getpwnam_r("t736-cedar", &account, account_buffer, sizeof(account_buffer), &resolved);
    diag("account.lookup", rc, rc, resolved != NULL, resolved && !rc ? (long)account.pw_uid : -1);
    if (rc || !resolved || account.pw_uid == 0 || account.pw_uid == getuid()) {
        diag("account.reject", rc, rc, resolved && !rc ? (long)account.pw_uid : -1, 0); return;
    }
    struct stat directory, endpoint;
    rc = lstat("/run/t736-cedar", &directory);
    diag("directory.stat", rc, rc ? errno : 0, rc ? -1 : (long)directory.st_uid, rc ? -1 : (long)directory.st_mode);
    if (rc || !S_ISDIR(directory.st_mode) || directory.st_uid != account.pw_uid || (directory.st_mode & 0022)) {
        diag("directory.reject", -EACCES, 0, account.pw_uid, 0); return;
    }
    rc = lstat(T736_SOCKET_PATH, &endpoint);
    diag("socket.stat", rc, rc ? errno : 0, rc ? -1 : (long)endpoint.st_uid, rc ? -1 : (long)endpoint.st_mode);
    if (rc || !S_ISSOCK(endpoint.st_mode) || endpoint.st_uid != account.pw_uid) {
        diag("socket.owner.reject", -EACCES, 0, account.pw_uid, 0); return;
    }
    strcpy(address.sun_path, T736_SOCKET_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    diag("socket.create", fd < 0 ? -1 : 0, fd < 0 ? errno : 0, 0, 0);
    if (fd < 0) return;
    rc = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline));
    diag("socket.recv_timeout", rc, rc ? errno : 0, 0, 0);
    if (rc) { close(fd); return; }
    rc = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline));
    diag("socket.send_timeout", rc, rc ? errno : 0, 0, 0);
    if (rc) { close(fd); return; }
    rc = connect(fd, (struct sockaddr *)&address, sizeof(address));
    diag("socket.connect", rc, rc ? errno : 0, 0, 0);
    if (rc) { close(fd); return; }
    rc = setup_connection(fd, account.pw_uid);
    diag("setup.result", rc, rc < 0 ? -rc : 0, 0, 0);
    if (rc) return;
    fprintf(stderr, "T736_CEDAR_CLIENT: preconnected protocol=2 readonly-memfd pid=%ld rdd=%d\n", (long)getpid(), rdd);
}

__attribute__((destructor)) static void client_exit(void)
{
    disconnect_client();
}

int t736_client_available(void)
{
    if (!current_process()) return 0;
    pthread_mutex_lock(&lock);
    int available = connection >= 0 && shared_pixels;
    pthread_mutex_unlock(&lock);
    return available;
}

static int request(unsigned op, int session, const void *prefix, size_t prefix_n,
                   const void *data, size_t data_n, struct t736_message *reply)
{
    if (connection < 0 || !shared_pixels) return -ENOTCONN;
    if (prefix_n + data_n > T736_MAX_BODY) return -E2BIG;
    if (serial_number == UINT32_MAX) { disconnect_client(); return -EOVERFLOW; }
    struct t736_message message = { T736_MAGIC, T736_PROTOCOL_VERSION, op,
        (uint32_t)(prefix_n + data_n), ++serial_number, (uint32_t)session, 0, 0 };
    int error = transfer(&message, sizeof(message), 1);
    if (!error && prefix_n) error = transfer((void *)prefix, prefix_n, 1);
    if (!error && data_n) error = transfer((void *)data, data_n, 1);
    if (!error) error = transfer(reply, sizeof(*reply), 0);
    if (error) return error;
    if (reply->magic != T736_MAGIC || reply->version != T736_PROTOCOL_VERSION ||
        reply->operation != op || reply->sequence != message.sequence ||
        reply->reserved || reply->length > sizeof(struct t736_picture) ||
        reply->status < -4095 ||
        (op != T736_OPEN && reply->session != message.session)) {
        disconnect_client();
        return -EPROTO;
    }
    return 0;
}

static int simple(unsigned op, int session, const void *prefix, size_t prefix_n,
                  const void *data, size_t data_n)
{
    struct t736_message reply;
    if (!current_process()) return -ENOTCONN;
    pthread_mutex_lock(&lock);
    int error = request(op, session, prefix, prefix_n, data, data_n, &reply);
    if (!error && (reply.length || reply.status > 0 ||
        (op == T736_OPEN && (reply.status ? reply.session != 0 :
                            (!reply.session || reply.session > INT_MAX))))) {
        disconnect_client(); error = -EPROTO;
    }
    if (!error) error = reply.status;
    if (!error && op == T736_OPEN) {
        const struct t736_open *args = prefix;
        active_session = reply.session; active_width = args->width; active_height = args->height;
        error = (int)reply.session;
    }
    if (!error && op == T736_CLOSE && (uint32_t)session == active_session)
        active_session = active_width = active_height = 0;
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
    if (!current_process()) return -ENOTCONN;
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
    if (reply.length != sizeof(picture)) goto malformed;
    error = transfer(&picture, sizeof(picture), 0);
    if (error) goto done;
    if (!picture.width || !picture.height || picture.width > T736_MAX_WIDTH ||
        picture.height > T736_MAX_HEIGHT || ((picture.width | picture.height) & 1) ||
        picture.bytes != picture.width * picture.height * 3 / 2 ||
        picture.bytes > capacity || picture.bytes > T736_MAX_FRAME ||
        (uint32_t)session != active_session || picture.width != active_width ||
        picture.height != active_height || picture.color_range > 2 ||
        (picture.colorspace != 0 && picture.colorspace != 1 && picture.colorspace != 6 && picture.colorspace != 9) ||
        picture.reserved) goto malformed;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    memcpy(dst, shared_pixels, picture.bytes);
    {
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
    disconnect_client(); error = -EPROTO;
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
