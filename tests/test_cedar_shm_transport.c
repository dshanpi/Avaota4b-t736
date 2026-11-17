/* SPDX-License-Identifier: MIT
 * Exercise the actual v2 client/broker over Unix sockets with a software-only
 * backend. No vendor library, GStreamer decoder or device is opened.
 */
#define main t736_broker_program_main
#include "../tools/browser/shm_v2/cedar_broker.c"
#undef main
static void *client_copy_probe(void *destination, const void *source, size_t size);
#define memcpy client_copy_probe
#include "../tools/browser/shm_v2/cedar_client.c"
#undef memcpy
#include <assert.h>
#include <dirent.h>
#include <sched.h>
#include <stdatomic.h>

struct cedar_decoder { unsigned width, height, pending, counter; int64_t pts; int eos; };
static _Atomic unsigned backend_calls, auto_frames;
static pthread_mutex_t copy_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t copy_probe_cond = PTHREAD_COND_INITIALIZER;
static unsigned pause_copy, copy_entered, release_copy;

static void *client_copy_probe(void *destination, const void *source, size_t size)
{
    pthread_mutex_lock(&copy_probe_lock);
    if (pause_copy && source == shared_pixels) {
        pause_copy = 0; copy_entered = 1;
        pthread_cond_broadcast(&copy_probe_cond);
        while (!release_copy) pthread_cond_wait(&copy_probe_cond, &copy_probe_lock);
    }
    pthread_mutex_unlock(&copy_probe_lock);
    return memcpy(destination, source, size);
}

cedar_decoder *cedar_open(enum cedar_codec codec, unsigned width, unsigned height,
                          char *error, size_t size)
{
    (void)codec; (void)error; (void)size;
    cedar_decoder *d = calloc(1, sizeof(*d));
    assert(d); d->width = width; d->height = height; backend_calls++;
    return d;
}
int cedar_push(cedar_decoder *d, const void *data, size_t size, int64_t pts, int64_t dts)
{
    (void)data; (void)size; (void)dts;
    d->pending++; d->pts = pts; backend_calls++; return 0;
}
int cedar_pull(cedar_decoder *d, cedar_frame *out, unsigned timeout)
{
    (void)timeout; backend_calls++;
    if (!d->pending && !auto_frames) return d->eos ? 2 : 0;
    if (d->pending) d->pending--;
    unsigned value = ++d->counter;
    *out = (cedar_frame){ .width = d->width, .height = d->height,
        .size = (size_t)d->width * d->height * 3 / 2,
        .pts_ns = (int64_t)value * 1000, .dts_ns = INT64_MIN,
        .color_range = 1, .color_matrix = 1 };
    out->data = malloc(out->size); assert(out->data);
    for (size_t i = 0; i < out->size; i++) out->data[i] = (uint8_t)(value + i * 17U);
    return 1;
}
int cedar_eos(cedar_decoder *d) { backend_calls++; d->eos = 1; return 0; }
int cedar_flush(cedar_decoder *d) { backend_calls++; d->pending = 0; d->eos = 0; return 0; }
int cedar_idle(cedar_decoder *d) { return cedar_flush(d); }
int cedar_resume(cedar_decoder *d) { return cedar_flush(d); }
void cedar_frame_free(cedar_frame *frame) { free(frame->data); memset(frame, 0, sizeof(*frame)); }
const char *cedar_error(const cedar_decoder *d) { (void)d; return "software transport stub"; }
void cedar_close(cedar_decoder *d) { free(d); }

static unsigned open_fds(void)
{
    DIR *dir = opendir("/proc/self/fd"); assert(dir);
    unsigned count = 0;
    while (readdir(dir)) count++;
    closedir(dir); return count;
}

static pthread_t start_peer(int *client)
{
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
    pthread_t thread;
    pthread_mutex_lock(&peers_lock); peers++; pthread_mutex_unlock(&peers_lock);
    assert(!pthread_create(&thread, NULL, serve, (void *)(intptr_t)sockets[1]));
    *client = sockets[0]; return thread;
}

static void end_peer(pthread_t thread)
{
    disconnect_client();
    assert(!pthread_join(thread, NULL));
    assert(peers == 0);
    for (unsigned i = 0; i < CONTEXT_COUNT; i++) {
        cedar_close(contexts[i].decoder);
        contexts[i] = (struct retained){0};
    }
    next_lease = 0; poisoned = 0; auto_frames = 0;
}

static void expect_frame(const uint8_t *data, const struct t736_frame *frame)
{
    assert(frame->color_range == 1 && frame->colorspace == 1 && frame->pts_us > 0);
    for (size_t i = 0; i < frame->bytes_written; i++)
        assert(data[i] == (uint8_t)((unsigned)frame->pts_us + i * 17U));
}

static void test_roundtrip(unsigned width, unsigned height)
{
    int fd;
    unsigned before = open_fds();
    pthread_t thread = start_peer(&fd);
    assert(setup_connection(fd, getuid()) == 0 && t736_client_available() == 1);
    assert(!mprotect((void *)shared_pixels, T736_MAX_FRAME, PROT_READ));
    errno = 0;
    assert(mprotect((void *)shared_pixels, T736_MAX_FRAME, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    int session = t736_client_open(T736_CODEC_HEVC, width, height);
    assert(session > 0);
    assert(t736_client_open(T736_CODEC_HEVC, width, height) == -EBUSY);
    size_t bytes = (size_t)width * height * 3 / 2;
    uint8_t *first = malloc(bytes + 32), *next = malloc(bytes + 32);
    assert(first && next); memset(first, 0x71, bytes + 32); memset(next, 0x72, bytes + 32);
    uint8_t packet = 1;
    struct t736_frame one = {0}, two = {0};
    assert(t736_client_pull(session, first + 16, bytes, &one, 0) == 0);
    assert(t736_client_push(session, &packet, 1, 100, INT64_MIN) == 0);
    assert(t736_client_pull(session, first + 16, bytes - 1, &one, 0) == -ENOSPC);
    assert(t736_client_pull(session, first + 16, bytes, &one, 0) == 1);
    assert(one.width == width && one.height == height && one.bytes_written == bytes);
    expect_frame(first + 16, &one);
    for (unsigned i = 0; i < 16; i++) assert(first[i] == 0x71 && first[bytes + 16 + i] == 0x71);
    for (unsigned n = 0; n < 12; n++) {
        assert(t736_client_push(session, &packet, 1, n + 101, INT64_MIN) == 0);
        assert(t736_client_pull(session, next + 16, bytes, &two, 0) == 1);
        expect_frame(next + 16, &two);
        expect_frame(first + 16, &one); /* Slot overwrite cannot alter old dst. */
    }
    for (unsigned i = 0; i < 16; i++) assert(next[i] == 0x72 && next[bytes + 16 + i] == 0x72);
    assert(t736_client_drain(session) == 0);
    assert(t736_client_pull(session, next + 16, bytes, &two, 0) == 2);
    expect_frame(first + 16, &one);
    assert(t736_client_flush(session) == 0);
    assert(t736_client_pull(session, next + 16, bytes, &two, 0) == 0);
    assert(t736_client_close(session) == 0);
    expect_frame(first + 16, &one);
    int again = t736_client_open(T736_CODEC_HEVC, width, height);
    assert(again > session && t736_client_close(again) == 0);
    end_peer(thread);
    expect_frame(first + 16, &one);
    assert(!t736_client_available() && !shared_pixels && connection == -1);
    free(first); free(next);
    assert(open_fds() == before);
}

struct pull_worker { int session; unsigned frames; };
static void *parallel_pull(void *opaque)
{
    struct pull_worker *worker = opaque;
    uint8_t output[24], retained[24];
    struct t736_frame picture, retained_picture = {0};
    for (unsigned i = 0; i < worker->frames; i++) {
        assert(t736_client_pull(worker->session, output, sizeof(output), &picture, 0) == 1);
        expect_frame(output, &picture);
        if (i) expect_frame(retained, &retained_picture);
        memcpy(retained, output, sizeof(output)); retained_picture = picture;
        sched_yield();
    }
    return NULL;
}

static void test_parallel_consumption(void)
{
    int fd;
    pthread_t server = start_peer(&fd);
    assert(!setup_connection(fd, getuid()));
    int session = t736_client_open(T736_CODEC_HEVC, 4, 4);
    assert(session > 0); auto_frames = 1;
    struct pull_worker worker = {session, 500};
    pthread_t a, b;
    pthread_mutex_lock(&copy_probe_lock);
    pause_copy = 1; copy_entered = release_copy = 0;
    pthread_mutex_unlock(&copy_probe_lock);
    assert(!pthread_create(&a, NULL, parallel_pull, &worker));
    pthread_mutex_lock(&copy_probe_lock);
    while (!copy_entered) pthread_cond_wait(&copy_probe_cond, &copy_probe_lock);
    /* Pause inside the actual client memcpy. Its API mutex must still be held
     * until ordinary dst owns the pixels; no next request can overwrite SHM. */
    assert(pthread_mutex_trylock(&lock) == EBUSY);
    assert(!pthread_create(&b, NULL, parallel_pull, &worker));
    release_copy = 1; pthread_cond_broadcast(&copy_probe_cond);
    pthread_mutex_unlock(&copy_probe_lock);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(t736_client_close(session) == 0);
    end_peer(server);
}

struct fake_peer { int fd; unsigned fault; };
static void *malformed_server(void *opaque)
{
    struct fake_peer *peer = opaque;
    struct t736_message request;
    struct t736_shm shm = {.fd = -1};
    struct { struct t736_message message; struct t736_setup setup; } setup;
    assert(!t736_receive_descriptor(peer->fd, &request, sizeof(request), NULL));
    assert(request.operation == T736_SETUP);
    assert(!t736_shm_create(&shm, &setup.setup));
    setup.message = request; setup.message.length = sizeof(setup.setup);
    if (peer->fault == 8) setup.message.version = 1;
    ssize_t sent = t736_send_descriptor(peer->fd, shm.fd, &setup, sizeof(setup));
    assert(sent > 0);
    if ((size_t)sent < sizeof(setup))
        assert(!io_all(peer->fd, (uint8_t *)&setup + sent, sizeof(setup) - (size_t)sent, 1));
    if (peer->fault == 8) goto done;
    struct t736_open open;
    assert(!io_all(peer->fd, &request, sizeof(request), 0));
    assert(request.operation == T736_OPEN && request.length == sizeof(open));
    assert(!io_all(peer->fd, &open, sizeof(open), 0));
    request.session = 99; request.length = 0;
    assert(!io_all(peer->fd, &request, sizeof(request), 1));
    for (unsigned index = 0; index < 3; index++) {
        struct t736_pull pull;
        assert(!io_all(peer->fd, &request, sizeof(request), 0));
        assert(request.operation == T736_PULL && request.length == sizeof(pull));
        assert(!io_all(peer->fd, &pull, sizeof(pull), 0));
        for (unsigned i = 0; i < 24; i++) shm.data[i] = (uint8_t)(17 + i * 17U + index);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        struct t736_picture picture = {4, 4, 24, 1, 1, 0, 17};
        request.status = index == 1 ? 0 : 1;
        request.length = index == 1 ? 0 : sizeof(picture);
        if (index == 2) {
            switch (peer->fault) {
            case 0: request.sequence++; break;
            case 1: request.version = 1; break;
            case 2: request.length += picture.bytes; break; /* v1 body shape */
            case 3: picture.width = 2; picture.bytes = 12; break;
            case 4: picture.reserved = 1; break;
            case 5: picture.bytes = T736_MAX_FRAME + 1; break;
            case 6: picture.color_range = 3; break;
            case 7: request.length = 0; break;
            default: assert(0);
            }
        }
        int result = io_all(peer->fd, &request, sizeof(request), 1);
        if (!result && request.length) result = io_all(peer->fd, &picture, sizeof(picture), 1);
        if (index < 2) assert(!result);
    }
done:
    t736_shm_destroy(&shm); close(peer->fd); return NULL;
}

static void test_client_rejects_malformed_reply(void)
{
    unsigned before = open_fds();
    for (unsigned fault = 0; fault <= 8; fault++) {
        int sockets[2];
        assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
        struct fake_peer peer = {sockets[1], fault};
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, malformed_server, &peer));
        int result = setup_connection(sockets[0], getuid());
        if (fault == 8) assert(result == -EPROTO);
        else {
            assert(result == 0);
            int session = t736_client_open(T736_CODEC_HEVC, 4, 4);
            assert(session == 99);
            uint8_t data[24]; struct t736_frame picture = {0};
            assert(t736_client_pull(session, data, sizeof(data), &picture, 0) == 1);
            expect_frame(data, &picture);
            assert(t736_client_pull(session, data, sizeof(data), &picture, 0) == 0);
            expect_frame(data, &picture); /* AGAIN must not copy changed SHM. */
            assert(t736_client_pull(session, data, sizeof(data), &picture, 0) == -EPROTO);
            expect_frame(data, &picture); /* Invalid metadata must not copy. */
        }
        assert(!t736_client_available() && !shared_pixels && connection == -1);
        assert(!pthread_join(thread, NULL));
        assert(open_fds() == before);
    }
}

static void test_rejected_setup_and_sequence(void)
{
    int fd; unsigned before = backend_calls;
    pthread_t thread = start_peer(&fd);
    struct t736_message legacy = {T736_MAGIC, 1, T736_OPEN, 0, 1, 0, 0, 0};
    assert(!io_all(fd, &legacy, sizeof(legacy), 1));
    assert(recv(fd, &legacy, sizeof(legacy), 0) == 0);
    close(fd); assert(!pthread_join(thread, NULL) && peers == 0 && backend_calls == before);
    thread = start_peer(&fd);
    assert(setup_connection(fd, getuid() + 1) == -EACCES);
    assert(!pthread_join(thread, NULL) && peers == 0 && backend_calls == before);
    thread = start_peer(&fd);
    assert(!setup_connection(fd, getuid()));
    struct t736_message duplicate = {T736_MAGIC, 2, T736_DRAIN, 0, 1, 0, 0, 0};
    assert(!io_all(connection, &duplicate, sizeof(duplicate), 1));
    assert(recv(connection, &duplicate, sizeof(duplicate), 0) == 0);
    end_peer(thread); assert(backend_calls == before);
}

int main(void)
{
    allowed_uid = getuid();
    assert(connection == -1 && !t736_client_available());
    unsigned baseline = open_fds();
    test_rejected_setup_and_sequence();
    test_roundtrip(4, 4);
    test_roundtrip(1920, 1080);
    test_parallel_consumption();
    test_client_rejects_malformed_reply();
    assert(open_fds() == baseline);
    puts("PASS: actual v2 client/broker Unix+SCM_RIGHTS setup, peer UID/version/sequence rejection before backend use, readonly sealed slot, bounded 4x4/1080p pixels, guards, no old-frame cross-talk, 1000 concurrent pulls with API lock verified inside memcpy, flush/EOS/close/reopen, both-direction v1 refusal, 8 malformed reply cases and AGAIN without stale copy, fd balance0; software backend only");
    return 0;
}
