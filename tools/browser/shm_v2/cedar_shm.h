/* SPDX-License-Identifier: MIT
 * Linux ordinary memfd setup helpers; no device, DMA fd or hardware address.
 */
#ifndef T736_CEDAR_SHM_H
#define T736_CEDAR_SHM_H
#include "cedar_protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
#ifndef F_SEAL_EXEC
#define F_SEAL_EXEC 0x0020
#endif
#define T736_SHM_SEALS (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL)
#ifndef T736_SETUP_TIMEOUT_MS
#define T736_SETUP_TIMEOUT_MS 5000
#endif

struct t736_shm {
    int fd;
    uint8_t *data;
};

static inline void t736_shm_destroy(struct t736_shm *shm)
{
    if (shm->data) munmap(shm->data, T736_MAX_FRAME);
    if (shm->fd >= 0) close(shm->fd);
    shm->data = NULL;
    shm->fd = -1;
}

static inline int t736_shm_create(struct t736_shm *shm, struct t736_setup *setup)
{
    struct stat st;
    int error, seals;
    *shm = (struct t736_shm){ .fd = -1 };
    shm->fd = memfd_create("t736-ordinary-i420-v2",
                          MFD_CLOEXEC | MFD_ALLOW_SEALING | MFD_NOEXEC_SEAL);
    if (shm->fd < 0) return -errno;
    if (fchmod(shm->fd, 0600) || ftruncate(shm->fd, T736_MAX_FRAME)) goto fail;
    void *map = mmap(NULL, T736_MAX_FRAME, PROT_READ | PROT_WRITE, MAP_SHARED, shm->fd, 0);
    if (map == MAP_FAILED) goto fail;
    shm->data = map;
    /* FUTURE_WRITE deliberately retains this existing server RW mapping. */
    if (fcntl(shm->fd, F_ADD_SEALS, T736_SHM_SEALS)) goto fail;
    seals = fcntl(shm->fd, F_GET_SEALS);
    if (seals < 0 || fstat(shm->fd, &st)) goto fail;
    if ((seals & (T736_SHM_SEALS | F_SEAL_EXEC)) != (T736_SHM_SEALS | F_SEAL_EXEC) ||
        !S_ISREG(st.st_mode) || (st.st_mode & 0111) || st.st_size != T736_MAX_FRAME) {
        errno = EPROTO; goto fail;
    }
    *setup = (struct t736_setup){ .capacity = T736_MAX_FRAME, .format = T736_SHM_I420,
        .seals = (uint32_t)seals, .device = (uint64_t)st.st_dev, .inode = (uint64_t)st.st_ino };
    return 0;
fail:
    error = -errno;
    t736_shm_destroy(shm);
    return error;
}

/* Send the descriptor once with the first positive byte count. The caller
 * sends any remaining ordinary bytes separately after a short sendmsg. */
static inline ssize_t t736_send_descriptor(int socket, int descriptor,
                                           const void *data, size_t bytes)
{
    union { struct cmsghdr alignment; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } control;
    struct iovec iov = { .iov_base = (void *)data, .iov_len = bytes };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
    memset(&control, 0, sizeof(control));
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &descriptor, sizeof(descriptor));
    ssize_t result;
    do { result = sendmsg(socket, &msg, MSG_NOSIGNAL); } while (result < 0 && errno == EINTR);
    return result;
}

/* recvmsg is used for every setup fragment, so extra rights in later fragments
 * cannot be silently discarded. Every received fd is closed on rejection. */
static inline int t736_receive_descriptor(int socket, void *data, size_t bytes,
                                          int *descriptor)
{
    size_t received = 0;
    int kept = -1, error = -EPROTO;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -errno;
    int64_t deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + T736_SETUP_TIMEOUT_MS;
    while (received < bytes) {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) { error = -errno; goto fail; }
        int64_t remaining = deadline - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        if (remaining <= 0) { error = -ETIMEDOUT; goto fail; }
        struct pollfd wait = { .fd = socket, .events = POLLIN };
        int ready = poll(&wait, 1, (int)remaining);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) { error = ready < 0 ? -errno : -ETIMEDOUT; goto fail; }
        union { struct cmsghdr alignment; unsigned char bytes[CMSG_SPACE(8 * sizeof(int))]; } control;
        struct iovec iov = { .iov_base = (uint8_t *)data + received, .iov_len = bytes - received };
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes) };
        memset(&control, 0, sizeof(control));
        ssize_t count;
        count = recvmsg(socket, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        int malformed = !!(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC));
        if (count >= 0) {
            for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
                if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
                    cmsg->cmsg_len < CMSG_LEN(0)) { malformed = 1; continue; }
                if ((uint8_t *)cmsg + cmsg->cmsg_len >
                    (uint8_t *)msg.msg_control + msg.msg_controllen) { malformed = 1; continue; }
                size_t length = cmsg->cmsg_len - CMSG_LEN(0);
                if (length != sizeof(int)) malformed = 1;
                for (size_t offset = 0; offset + sizeof(int) <= length; offset += sizeof(int)) {
                    int fd;
                    memcpy(&fd, (uint8_t *)CMSG_DATA(cmsg) + offset, sizeof(fd));
                    if (descriptor && kept < 0 && !malformed) kept = fd;
                    else { close(fd); malformed = 1; }
                }
            }
        }
        if (count <= 0) { error = count < 0 ? -errno : -EPIPE; goto fail; }
        if (malformed) goto fail;
        received += (size_t)count;
    }
    if (descriptor && kept < 0) goto fail;
    if (descriptor) *descriptor = kept;
    return 0;
fail:
    if (kept >= 0) close(kept);
    return error;
}

static inline int t736_shm_map_receiver(int fd, const struct t736_setup *setup,
                                        uid_t expected_owner, const uint8_t **map)
{
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    int flags = fcntl(fd, F_GETFD);
    if (seals < 0 || flags < 0 || fstat(fd, &st)) return -errno;
    if ((seals & (T736_SHM_SEALS | F_SEAL_EXEC)) != (T736_SHM_SEALS | F_SEAL_EXEC) ||
        (uint32_t)seals != setup->seals || !S_ISREG(st.st_mode) || st.st_nlink != 0 ||
        st.st_uid != expected_owner || (st.st_mode & 0111) ||
        st.st_size != T736_MAX_FRAME || setup->capacity != T736_MAX_FRAME ||
        setup->format != T736_SHM_I420 || setup->reserved ||
        (uint64_t)st.st_dev != setup->device || (uint64_t)st.st_ino != setup->inode ||
        !(flags & FD_CLOEXEC)) return -EPROTO;
    void *data = mmap(NULL, T736_MAX_FRAME, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) return -errno;
    if (madvise(data, T736_MAX_FRAME, MADV_DONTFORK)) {
        int error = -errno; munmap(data, T736_MAX_FRAME); return error;
    }
    *map = data;
    return 0;
}
#endif
