/* SPDX-License-Identifier: MIT
 * Exercise the actual v2 ordinary-RAM helper on Linux, without a decoder,
 * vendor library, privileged service or device. Fault injection substitutes
 * only selected libc calls made by the header; normal cases use the real
 * syscalls. Build with -pthread and optionally ASan/UBSan.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/falloc.h>
#include <linux/memfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s (errno=%d: %s)\n", \
                __FILE__, __LINE__, #condition, errno, strerror(errno)); \
        abort(); \
    } \
} while (0)

enum fail_at { FAIL_NONE, FAIL_MEMFD, FAIL_CHMOD, FAIL_TRUNCATE, FAIL_MMAP,
               FAIL_ADD_SEALS, FAIL_GET_SEALS, FAIL_STAT, FAIL_METADATA,
               FAIL_MADVISE };
static enum fail_at fault;
static unsigned faults_seen, last_memfd_flags, successful_unmaps;
static void *last_mapping;
static size_t last_mapping_size;

static int injected(enum fail_at point)
{
    if (fault != point) return 0;
    faults_seen++;
    errno = EIO;
    return 1;
}

static int call_memfd_create(const char *name, unsigned flags)
{
    last_memfd_flags = flags;
    return injected(FAIL_MEMFD) ? -1 : memfd_create(name, flags);
}

static int call_fchmod(int fd, mode_t mode)
{
    return injected(FAIL_CHMOD) ? -1 : fchmod(fd, mode);
}

static int call_ftruncate(int fd, off_t size)
{
    return injected(FAIL_TRUNCATE) ? -1 : ftruncate(fd, size);
}

static void *call_mmap(void *address, size_t length, int prot, int flags,
                       int fd, off_t offset)
{
    if (injected(FAIL_MMAP)) return MAP_FAILED;
    void *result = mmap(address, length, prot, flags, fd, offset);
    if (result != MAP_FAILED) {
        last_mapping = result;
        last_mapping_size = length;
    }
    return result;
}

static int call_munmap(void *address, size_t length)
{
    int result = munmap(address, length);
    if (!result) successful_unmaps++;
    return result;
}

static int call_fcntl(int fd, int command, ...)
{
    if (command == F_ADD_SEALS) {
        va_list args;
        va_start(args, command);
        int seals = va_arg(args, int);
        va_end(args);
        return injected(FAIL_ADD_SEALS) ? -1 : fcntl(fd, command, seals);
    }
    if (command == F_GET_SEALS && injected(FAIL_GET_SEALS)) return -1;
    return fcntl(fd, command);
}

static int call_fstat(int fd, struct stat *st)
{
    if (injected(FAIL_STAT)) return -1;
    int result = fstat(fd, st);
    if (!result && injected(FAIL_METADATA)) st->st_size--;
    return result;
}

static int call_madvise(void *address, size_t length, int advice)
{
    return injected(FAIL_MADVISE) ? -1 : madvise(address, length, advice);
}

/* Include the production helper after system declarations, so fault seams
 * affect only its calls, not the real syscall wrappers or the test itself. */
#define memfd_create call_memfd_create
#define fchmod call_fchmod
#define ftruncate call_ftruncate
#define mmap call_mmap
#define munmap call_munmap
#define fcntl call_fcntl
#define fstat call_fstat
#define madvise call_madvise
#include "cedar_shm.h"
#undef memfd_create
#undef fchmod
#undef ftruncate
#undef mmap
#undef munmap
#undef fcntl
#undef fstat
#undef madvise

static unsigned cases;

static int fd_count(void)
{
    DIR *directory = opendir("/proc/self/fd");
    CHECK(directory);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
    CHECK(closedir(directory) == 0);
    return count; /* Includes the transient directory fd on every call. */
}

static void unmapped(void *address)
{
    unsigned char residency;
    errno = 0;
    CHECK(mincore(address, (size_t)sysconf(_SC_PAGESIZE), &residency) == -1);
    CHECK(errno == ENOMEM);
}

static void pair(int sockets[2])
{
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
}

static void close_pair(int sockets[2])
{
    CHECK(close(sockets[0]) == 0);
    CHECK(close(sockets[1]) == 0);
}

static void send_bytes(int fd, const void *bytes, size_t size)
{
    const uint8_t *cursor = bytes;
    while (size) {
        ssize_t written = send(fd, cursor, size, MSG_NOSIGNAL);
        CHECK(written > 0);
        cursor += written;
        size -= (size_t)written;
    }
}

static void raw_rights(int socket, int source_fd, unsigned count,
                       const void *data, size_t bytes)
{
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(24 * sizeof(int))]; } control;
    CHECK(count && count <= 24);
    memset(&control, 0, sizeof(control));
    struct iovec iov = { .iov_base = (void *)data, .iov_len = bytes };
    struct msghdr message = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = CMSG_SPACE(count * sizeof(int)) };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(count * sizeof(int));
    for (unsigned i = 0; i < count; i++)
        memcpy((uint8_t *)CMSG_DATA(cmsg) + i * sizeof(int), &source_fd, sizeof(int));
    CHECK(sendmsg(socket, &message, MSG_NOSIGNAL) == (ssize_t)bytes);
}

static void create_failures(void)
{
    int baseline = fd_count();
    for (enum fail_at point = FAIL_MEMFD; point <= FAIL_METADATA; point++) {
        struct t736_shm shm = { .fd = -1 };
        struct t736_setup setup, untouched;
        memset(&setup, 0xa7, sizeof(setup));
        untouched = setup;
        fault = point;
        faults_seen = successful_unmaps = 0;
        last_mapping = NULL;
        int result = t736_shm_create(&shm, &setup);
        fault = FAIL_NONE;
        CHECK(result == (point == FAIL_METADATA ? -EPROTO : -EIO));
        CHECK(faults_seen == 1 && shm.fd == -1 && !shm.data);
        CHECK(!memcmp(&setup, &untouched, sizeof(setup)));
        if (last_mapping) {
            CHECK(last_mapping_size == T736_MAX_FRAME && successful_unmaps == 1);
            unmapped(last_mapping);
        } else CHECK(!successful_unmaps);
        t736_shm_destroy(&shm); /* Idempotent after failed partial construction. */
        CHECK(fd_count() == baseline);
        cases++;
    }
    puts("PASS create failures: memfd/chmod/truncate/mmap/seals/stat/metadata cleanup");
}

static void receive_ok(int source, const struct t736_setup *setup, int late_fd)
{
    int baseline = fd_count(), sockets[2], descriptor = -1;
    unsigned char expected[64], actual[64];
    for (unsigned i = 0; i < sizeof(expected); i++) expected[i] = (uint8_t)(i * 3U + 7U);
    pair(sockets);
    unsigned position = 0;
    if (late_fd) send_bytes(sockets[0], expected + position++, 1);
    CHECK(t736_send_descriptor(sockets[0], source, expected + position++, 1) == 1);
    while (position < sizeof(expected)) send_bytes(sockets[0], expected + position++, 1);
    CHECK(t736_receive_descriptor(sockets[1], actual, sizeof(actual), &descriptor) == 0);
    CHECK(descriptor >= 0 && descriptor != source);
    CHECK(!memcmp(expected, actual, sizeof(actual)));
    CHECK(fcntl(descriptor, F_GETFD) & FD_CLOEXEC);
    struct stat source_stat, received_stat;
    CHECK(fstat(source, &source_stat) == 0 && fstat(descriptor, &received_stat) == 0);
    CHECK(source_stat.st_dev == received_stat.st_dev && source_stat.st_ino == received_stat.st_ino);
    const uint8_t *map = NULL;
    CHECK(t736_shm_map_receiver(descriptor, setup, getuid(), &map) == 0 && map);
    CHECK(munmap((void *)map, T736_MAX_FRAME) == 0);
    CHECK(close(descriptor) == 0);
    close_pair(sockets);
    CHECK(fd_count() == baseline);
    cases++;
}

static void send_failures(int source)
{
    int baseline = fd_count(), sockets[2];
    unsigned char byte = 1;
    pair(sockets);
    errno = 0;
    CHECK(t736_send_descriptor(sockets[0], -1, &byte, 1) == -1 && errno == EBADF);
    CHECK(close(sockets[1]) == 0);
    errno = 0;
    CHECK(t736_send_descriptor(sockets[0], source, &byte, 1) == -1 && errno == EPIPE);
    /* MSG_NOSIGNAL must preserve the process on a disconnected peer. */
    CHECK(close(sockets[0]) == 0);
    CHECK(fd_count() == baseline && fcntl(source, F_GETFD) >= 0);
    puts("PASS descriptor send failures: invalid fd/disconnected peer, source ownership retained");
    cases += 2;
}

enum bad_rights { NO_RIGHTS, TWO_RIGHTS, EIGHT_RIGHTS, TRUNCATED_RIGHTS,
                 LATER_RIGHTS, EOF_AFTER_RIGHTS, NO_DESCRIPTOR_ALLOWED,
                 CREDENTIALS_ONLY, CREDENTIALS_AND_RIGHTS, REQUEST_NO_RIGHTS };

static void receive_bad(int source, enum bad_rights scenario)
{
    int baseline = fd_count(), sockets[2], descriptor = -77;
    unsigned char bytes[64] = {0};
    pair(sockets);
    if (scenario == CREDENTIALS_ONLY || scenario == CREDENTIALS_AND_RIGHTS) {
        int enabled = 1;
        CHECK(setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)) == 0);
    }
    switch (scenario) {
    case NO_RIGHTS:
    case REQUEST_NO_RIGHTS:
    case CREDENTIALS_ONLY:
        send_bytes(sockets[0], bytes, sizeof(bytes));
        break;
    case TWO_RIGHTS:
    case EIGHT_RIGHTS:
    case TRUNCATED_RIGHTS:
        raw_rights(sockets[0], source,
                   scenario == TWO_RIGHTS ? 2 : scenario == EIGHT_RIGHTS ? 8 : 24,
                   bytes, sizeof(bytes));
        break;
    case LATER_RIGHTS:
        CHECK(t736_send_descriptor(sockets[0], source, bytes, 1) == 1);
        send_bytes(sockets[0], bytes + 1, 1);
        CHECK(t736_send_descriptor(sockets[0], source, bytes + 2, sizeof(bytes) - 2) == (ssize_t)sizeof(bytes) - 2);
        break;
    case EOF_AFTER_RIGHTS:
        CHECK(t736_send_descriptor(sockets[0], source, bytes, 1) == 1);
        CHECK(shutdown(sockets[0], SHUT_WR) == 0);
        break;
    case NO_DESCRIPTOR_ALLOWED:
    case CREDENTIALS_AND_RIGHTS:
        CHECK(t736_send_descriptor(sockets[0], source, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes));
        break;
    }
    int *output = scenario == NO_DESCRIPTOR_ALLOWED || scenario == REQUEST_NO_RIGHTS ? NULL : &descriptor;
    int result = t736_receive_descriptor(sockets[1], bytes, sizeof(bytes), output);
    CHECK(result == (scenario == REQUEST_NO_RIGHTS ? 0 : scenario == EOF_AFTER_RIGHTS ? -EPIPE : -EPROTO));
    CHECK(descriptor == -77);
    /* Includes checking installed SCM_RIGHTS descriptors before either socket
     * is closed; queued but never received rights are released by close. */
    CHECK(fd_count() == baseline + 2);
    close_pair(sockets);
    CHECK(fd_count() == baseline);
    cases++;
}

static int64_t now_ms(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (int64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

struct drip_args { int socket, source; };

static void *drip_sender(void *opaque)
{
    struct drip_args *args = opaque;
    const unsigned char byte = 42;
    CHECK(t736_send_descriptor(args->socket, args->source, &byte, 1) == 1);
    const struct timespec pause = { .tv_sec = 0, .tv_nsec = 20000000 };
    /* 10 seconds of one-byte fragments would fit a 512-byte receive. The
     * absolute helper deadline must expire while data is still arriving. */
    for (unsigned i = 1; i < 512; i++) {
        CHECK(nanosleep(&pause, NULL) == 0);
        if (send(args->socket, &byte, 1, MSG_NOSIGNAL) < 0) {
            CHECK(errno == EPIPE || errno == ECONNRESET);
            break;
        }
    }
    return NULL;
}

static void receive_timeouts(int source)
{
    for (int drip = 0; drip <= 1; drip++) {
        int baseline = fd_count(), sockets[2], descriptor = -77;
        unsigned char bytes[512];
        pair(sockets);
        pthread_t sender;
        struct drip_args args = { sockets[0], source };
        if (drip) CHECK(pthread_create(&sender, NULL, drip_sender, &args) == 0);
        int64_t start = now_ms();
        CHECK(t736_receive_descriptor(sockets[1], bytes, sizeof(bytes), &descriptor) == -ETIMEDOUT);
        int64_t elapsed = now_ms() - start;
        CHECK(elapsed >= T736_SETUP_TIMEOUT_MS - 10);
        CHECK(elapsed < T736_SETUP_TIMEOUT_MS + 1500);
        CHECK(descriptor == -77 && fd_count() == baseline + 2);
        CHECK(shutdown(sockets[1], SHUT_RDWR) == 0);
        if (drip) CHECK(pthread_join(sender, NULL) == 0);
        close_pair(sockets);
        CHECK(fd_count() == baseline);
        printf("PASS setup %s deadline: %lld ms\n", drip ? "drip-feed" : "silent", (long long)elapsed);
        cases++;
    }
}

static void reject_map(int fd, const struct t736_setup *setup, uid_t owner)
{
    const uint8_t *sentinel = (const uint8_t *)(uintptr_t)1;
    const uint8_t *map = sentinel;
    int baseline = fd_count();
    CHECK(t736_shm_map_receiver(fd, setup, owner, &map) < 0);
    CHECK(map == sentinel && fd_count() == baseline);
    cases++;
}

static struct t736_setup describe(int fd)
{
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    CHECK(seals >= 0 && fstat(fd, &st) == 0);
    return (struct t736_setup){ .capacity = T736_MAX_FRAME, .format = T736_SHM_I420,
        .seals = (uint32_t)seals, .device = (uint64_t)st.st_dev, .inode = (uint64_t)st.st_ino };
}

static int custom_memfd(off_t size, unsigned flags, int seals, mode_t mode)
{
    int fd = memfd_create("t736-invalid-shm-test", MFD_CLOEXEC | MFD_ALLOW_SEALING | flags);
    CHECK(fd >= 0 && fchmod(fd, mode) == 0 && ftruncate(fd, size) == 0);
    CHECK(fcntl(fd, F_ADD_SEALS, seals) == 0);
    return fd;
}

static void malformed_metadata(struct t736_shm *shm, const struct t736_setup *setup)
{
    int baseline = fd_count();
    for (unsigned field = 0; field < 6; field++) {
        struct t736_setup changed = *setup;
        switch (field) {
        case 0: changed.capacity--; break;
        case 1: changed.format++; break;
        case 2: changed.seals ^= F_SEAL_GROW; break;
        case 3: changed.reserved = 1; break;
        case 4: changed.device++; break;
        case 5: changed.inode++; break;
        }
        reject_map(shm->fd, &changed, getuid());
    }
    reject_map(shm->fd, setup, getuid() ^ 1U);
    int duplicate = dup(shm->fd); /* dup intentionally clears FD_CLOEXEC. */
    CHECK(duplicate >= 0 && !(fcntl(duplicate, F_GETFD) & FD_CLOEXEC));
    reject_map(duplicate, setup, getuid());
    CHECK(close(duplicate) == 0);
    const int omitted[] = { F_SEAL_SHRINK, F_SEAL_GROW, F_SEAL_FUTURE_WRITE, F_SEAL_SEAL };
    for (unsigned i = 0; i < sizeof(omitted) / sizeof(omitted[0]); i++) {
        int fd = custom_memfd(T736_MAX_FRAME, MFD_NOEXEC_SEAL, T736_SHM_SEALS & ~omitted[i], 0600);
        struct t736_setup missing = describe(fd);
        CHECK(!(missing.seals & omitted[i]));
        reject_map(fd, &missing, getuid());
        CHECK(close(fd) == 0);
    }
    int no_exec_seal = custom_memfd(T736_MAX_FRAME, 0, T736_SHM_SEALS, 0600);
    struct t736_setup missing = describe(no_exec_seal);
    CHECK(!(missing.seals & F_SEAL_EXEC));
    reject_map(no_exec_seal, &missing, getuid());
    CHECK(close(no_exec_seal) == 0);
    int executable = custom_memfd(T736_MAX_FRAME, 0, T736_SHM_SEALS | F_SEAL_EXEC, 0700);
    struct t736_setup executable_setup = describe(executable);
    reject_map(executable, &executable_setup, getuid());
    CHECK(close(executable) == 0);
    int short_file = custom_memfd(T736_MAX_FRAME - 4096, MFD_NOEXEC_SEAL, T736_SHM_SEALS, 0600);
    struct t736_setup short_setup = describe(short_file);
    reject_map(short_file, &short_setup, getuid());
    CHECK(close(short_file) == 0);
    int sockets[2];
    pair(sockets);
    reject_map(sockets[0], setup, getuid());
    close_pair(sockets);
    reject_map(-1, setup, getuid());
    CHECK(fd_count() == baseline);
    puts("PASS receiver metadata: capacity/format/seals/reserved/device/inode/owner/cloexec/type/size/mode");
}

static void receiver_failures(struct t736_shm *shm, const struct t736_setup *setup)
{
    const enum fail_at points[] = { FAIL_GET_SEALS, FAIL_STAT, FAIL_MMAP, FAIL_MADVISE };
    int baseline = fd_count();
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        const uint8_t *sentinel = (const uint8_t *)(uintptr_t)1;
        const uint8_t *map = sentinel;
        last_mapping = NULL;
        faults_seen = successful_unmaps = 0;
        fault = points[i];
        int result = t736_shm_map_receiver(shm->fd, setup, getuid(), &map);
        fault = FAIL_NONE;
        CHECK(result == -EIO && faults_seen == 1 && map == sentinel);
        if (points[i] == FAIL_MADVISE) {
            CHECK(last_mapping && successful_unmaps == 1);
            unmapped(last_mapping);
        } else CHECK(!last_mapping && !successful_unmaps);
        CHECK(fd_count() == baseline);
        CHECK(fcntl(shm->fd, F_GETFD) >= 0); /* Caller retains fd ownership. */
        cases++;
    }
    puts("PASS receiver syscall failures: seals/stat/mmap/madvise cleanup");
}

static void seal_enforcement(struct t736_shm *shm, const struct t736_setup *setup)
{
    struct stat st;
    CHECK(fstat(shm->fd, &st) == 0);
    CHECK(S_ISREG(st.st_mode) && !(st.st_mode & 0111) && (st.st_mode & 0777) == 0600);
    CHECK(st.st_nlink == 0 && st.st_size == T736_MAX_FRAME);
    CHECK((setup->seals & (T736_SHM_SEALS | F_SEAL_EXEC)) == (T736_SHM_SEALS | F_SEAL_EXEC));
    CHECK((last_memfd_flags & (MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL)) ==
          (MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL));
    const uint8_t *receiver = NULL;
    CHECK(t736_shm_map_receiver(shm->fd, setup, getuid(), &receiver) == 0 && receiver);
    shm->data[0] = 0x31;
    shm->data[T736_MAX_FRAME - 1] = 0x82;
    CHECK(receiver[0] == 0x31 && receiver[T736_MAX_FRAME - 1] == 0x82);
    const uint8_t byte = 0xff;
    errno = 0; CHECK(write(shm->fd, &byte, 1) == -1 && errno == EPERM);
    errno = 0; CHECK(pwrite(shm->fd, &byte, 1, 0) == -1 && errno == EPERM);
    errno = 0; CHECK(ftruncate(shm->fd, T736_MAX_FRAME - 4096) == -1 && errno == EPERM);
    errno = 0; CHECK(ftruncate(shm->fd, T736_MAX_FRAME + 4096) == -1 && errno == EPERM);
    errno = 0; CHECK(fallocate(shm->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096) == -1 && errno == EPERM);
    errno = 0;
    CHECK(mmap(NULL, T736_MAX_FRAME, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0) == MAP_FAILED && errno == EPERM);
    errno = 0; CHECK(mprotect((void *)receiver, T736_MAX_FRAME, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
    errno = 0; CHECK(fchmod(shm->fd, 0700) == -1 && errno == EPERM);
    errno = 0; CHECK(fcntl(shm->fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EPERM);
    CHECK(receiver[0] == 0x31 && receiver[T736_MAX_FRAME - 1] == 0x82);
    shm->data[0] = 0x93;
    CHECK(receiver[0] == 0x93); /* Preexisting broker map remains writable. */

    /* Private copy-on-write is legitimate and must not modify the slot. */
    uint8_t *private_map = mmap(NULL, T736_MAX_FRAME, PROT_READ | PROT_WRITE, MAP_PRIVATE, shm->fd, 0);
    CHECK(private_map != MAP_FAILED);
    private_map[0] = 0x44;
    CHECK(receiver[0] == 0x93 && shm->data[0] == 0x93);
    CHECK(munmap(private_map, T736_MAX_FRAME) == 0);

    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        unsigned char residency;
        errno = 0;
        int result = mincore((void *)receiver, (size_t)sysconf(_SC_PAGESIZE), &residency);
        _exit(result == -1 && errno == ENOMEM ? 0 : 1);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(receiver[0] == 0x93);
    CHECK(munmap((void *)receiver, T736_MAX_FRAME) == 0);
    puts("PASS seals: write/pwrite/shrink/grow/punch/RW mmap/mprotect/exec-mode/add-seals rejected; existing RW and DONTFORK verified");
    cases += 12;
}

static void independent_peers(struct t736_shm *first, const struct t736_setup *first_setup)
{
    int baseline = fd_count();
    struct t736_shm second = { .fd = -1 };
    struct t736_setup second_setup;
    CHECK(t736_shm_create(&second, &second_setup) == 0);
    CHECK(second_setup.device != first_setup->device || second_setup.inode != first_setup->inode);
    const uint8_t *first_map = NULL, *second_map = NULL;
    CHECK(t736_shm_map_receiver(first->fd, first_setup, getuid(), &first_map) == 0);
    CHECK(t736_shm_map_receiver(second.fd, &second_setup, getuid(), &second_map) == 0);
    memset(first->data, 0x42, T736_MAX_FRAME);
    memset(second.data, 0x73, T736_MAX_FRAME);
    CHECK(first_map[0] == 0x42 && second_map[0] == 0x73);
    CHECK(first_map[T736_MAX_FRAME - 1] == 0x42 && second_map[T736_MAX_FRAME - 1] == 0x73);
    void *old_map = second.data;
    t736_shm_destroy(&second);
    CHECK(second.fd == -1 && !second.data);
    unmapped(old_map);
    CHECK(second_map[0] == 0x73 && second_map[T736_MAX_FRAME - 1] == 0x73);
    first->data[0] = 0x64;
    CHECK(first_map[0] == 0x64 && second_map[0] == 0x73);
    CHECK(munmap((void *)first_map, T736_MAX_FRAME) == 0);
    CHECK(munmap((void *)second_map, T736_MAX_FRAME) == 0);
    CHECK(fd_count() == baseline);
    puts("PASS peers: separate inodes/content; receiver survives producer fd/map close");
    cases++;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int baseline = fd_count();
    create_failures();
    struct t736_shm shm = { .fd = -1 };
    struct t736_setup setup;
    int result = t736_shm_create(&shm, &setup);
    if (result) fprintf(stderr, "Production create failed: %d; Linux MFD_NOEXEC_SEAL is required, no downgrade.\n", result);
    CHECK(result == 0);
    seal_enforcement(&shm, &setup);
    send_failures(shm.fd);
    receive_ok(shm.fd, &setup, 0);
    receive_ok(shm.fd, &setup, 1);
    for (enum bad_rights scenario = NO_RIGHTS; scenario <= REQUEST_NO_RIGHTS; scenario++)
        receive_bad(shm.fd, scenario);
    puts("PASS SCM_RIGHTS: fragments/CLOEXEC/absent/multiple/late-extra/truncated/EOF/credentials/request-no-fd and fd counts");
    receive_timeouts(shm.fd);
    malformed_metadata(&shm, &setup);
    receiver_failures(&shm, &setup);
    independent_peers(&shm, &setup);
    void *old_map = shm.data;
    t736_shm_destroy(&shm);
    unmapped(old_map);
    t736_shm_destroy(&shm);
    CHECK(fd_count() == baseline);
    printf("PASS cedar_shm_seals: %u cases, no fd leaks, helper timeout=%d ms\n", cases, T736_SETUP_TIMEOUT_MS);
    return 0;
}
