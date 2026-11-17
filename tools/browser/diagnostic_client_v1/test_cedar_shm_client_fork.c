/* SPDX-License-Identifier: MIT
 * Independent fork/exec lifetime test of the unmodified v2 client. The only
 * peer is an ordinary memfd fixture; no broker/backend/device is used.
 */
#include "cedar_client.c"
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <sys/wait.h>

struct fixture {
    int socket;
    struct t736_shm shm;
};

static unsigned fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    assert(directory);
    unsigned count = 0;
    while (readdir(directory)) count++;
    assert(!closedir(directory));
    return count;
}

static void *setup_fixture(void *opaque)
{
    struct fixture *fixture = opaque;
    struct t736_message request;
    struct { struct t736_message message; struct t736_setup setup; } response;
    assert(!t736_receive_descriptor(fixture->socket, &request, sizeof(request), NULL));
    assert(request.magic == T736_MAGIC && request.version == 2 &&
           request.operation == T736_SETUP && request.sequence == 1);
    assert(!t736_shm_create(&fixture->shm, &response.setup));
    fixture->shm.data[0] = 0x39;
    fixture->shm.data[T736_MAX_FRAME - 1] = 0x93;
    response.message = request;
    response.message.length = sizeof(response.setup);
    ssize_t sent = t736_send_descriptor(fixture->socket, fixture->shm.fd,
                                       &response, sizeof(response));
    assert(sent > 0);
    while ((size_t)sent < sizeof(response)) {
        ssize_t n = send(fixture->socket, (uint8_t *)&response + sent,
                         sizeof(response) - (size_t)sent, MSG_NOSIGNAL);
        assert(n > 0);
        sent += n;
    }
    return NULL;
}

static void fork_child_checks(int inherited_connection)
{
    /* A missing PID gate would deadlock on the intentionally inherited locked
     * mutex. Fail promptly, without relying on a parent timeout or scheduling. */
    alarm(3);
    const uint8_t *old_address = shared_pixels;
    unsigned char resident = 0;
    errno = 0;
    assert(mincore((void *)old_address, 4096, &resident) == -1 && errno == ENOMEM);
    assert(fcntl(inherited_connection, F_GETFD) & FD_CLOEXEC);
    uint8_t bytes[24];
    memset(bytes, 0x47, sizeof(bytes));
    struct t736_frame frame;
    memset(&frame, 0x59, sizeof(frame));
    struct t736_frame saved = frame;
    assert(t736_client_available() == 0);
    assert(t736_client_open(T736_CODEC_H264, 4, 4) == -ENOTCONN);
    assert(t736_client_push(1, bytes, 1, 0, INT64_MIN) == -ENOTCONN);
    assert(t736_client_pull(1, bytes, sizeof(bytes), &frame, 0) == -ENOTCONN);
    assert(t736_client_drain(1) == -ENOTCONN);
    assert(t736_client_flush(1) == -ENOTCONN);
    assert(t736_client_close(1) == -ENOTCONN);
    assert(!memcmp(&frame, &saved, sizeof(frame)));
    for (unsigned i = 0; i < sizeof(bytes); i++) assert(bytes[i] == 0x47);

    /* The disappeared DONTFORK map's address may be reused by unrelated child
     * memory. Child destruction must not munmap that new allocation. */
    uint8_t *replacement = mmap((void *)old_address, T736_MAX_FRAME,
        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(replacement == old_address);
    replacement[0] = 0x16;
    replacement[T736_MAX_FRAME - 1] = 0x61;
    client_exit();
    assert(connection == -1 && shared_pixels == NULL);
    errno = 0;
    assert(fcntl(inherited_connection, F_GETFD) == -1 && errno == EBADF);
    assert(!mincore(replacement, 4096, &resident));
    assert(replacement[0] == 0x16 && replacement[T736_MAX_FRAME - 1] == 0x61);
    client_exit(); /* Idempotent destructor. */
    assert(!munmap(replacement, T736_MAX_FRAME));
    _exit(0);
}

static void wait_ok(pid_t child)
{
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--check-exec-fd")) {
        int descriptor = atoi(argv[2]);
        assert(descriptor >= 3);
        errno = 0;
        assert(fcntl(descriptor, F_GETFD) == -1 && errno == EBADF);
        assert(t736_client_available() == 0 && shared_pixels == NULL);
        return 0;
    }
    assert(argc == 1 && t736_client_available() == 0);
    unsigned before = fd_count();
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
    struct fixture fixture = { .socket = sockets[1], .shm = { .fd = -1 } };
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, setup_fixture, &fixture));
    assert(!setup_connection(sockets[0], getuid()));
    assert(!pthread_join(thread, NULL));
    assert(t736_client_available() == 1 && shared_pixels[0] == 0x39);
    assert(shared_pixels[T736_MAX_FRAME - 1] == 0x93);
    assert(fcntl(connection, F_GETFD) & FD_CLOEXEC);
    assert(fd_count() == before + 3); /* two sockets and fixture producer fd */

    assert(!pthread_mutex_lock(&lock));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) fork_child_checks(connection);
    wait_ok(child);
    assert(!pthread_mutex_unlock(&lock));
    assert(t736_client_available() == 1 && shared_pixels[0] == 0x39);
    assert(shared_pixels[T736_MAX_FRAME - 1] == 0x93);
    fixture.shm.data[0] = 0x51;
    assert(shared_pixels[0] == 0x51);
    uint8_t unexpected;
    errno = 0;
    assert(recv(fixture.socket, &unexpected, 1, MSG_DONTWAIT) == -1 && errno == EAGAIN);
    assert(fd_count() == before + 3);

    child = fork();
    assert(child >= 0);
    if (!child) {
        char descriptor[24];
        snprintf(descriptor, sizeof(descriptor), "%d", connection);
        execl("/proc/self/exe", argv[0], "--check-exec-fd", descriptor, (char *)NULL);
        _exit(99);
    }
    wait_ok(child);
    assert(t736_client_available() == 1 && shared_pixels[0] == 0x51);
    disconnect_client();
    assert(!close(fixture.socket));
    t736_shm_destroy(&fixture.shm);
    assert(fd_count() == before);
    puts("PASS: actual v2 client PID gates all APIs before inherited mutex; no child socket traffic; DONTFORK and address reuse destructor safety; parent mapping/socket retained; exec CLOEXEC; fd balance0");
    return 0;
}
