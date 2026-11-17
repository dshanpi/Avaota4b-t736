/* SPDX-License-Identifier: MIT
 * Minimal native-versus-linux-user-emulator probe. No Cedar production code,
 * service or hardware is used. A child access fault is caught and reported.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void access_fault(int number)
{
    _exit(100 + number);
}

static void residency(const char *kind, const char *process, void *map, size_t size)
{
    unsigned char resident = 0xff;
    errno = 0;
    int result = mincore(map, size, &resident);
    int error = errno;
    printf("%s %s mincore: rc=%d errno=%d resident=%u\n",
           kind, process, result, error, resident);
}

static void probe(const char *kind, int file_backed, int dontfork)
{
    const size_t size = (size_t)sysconf(_SC_PAGESIZE);
    int fd = -1;
    if (file_backed) {
        fd = memfd_create("dontfork-minimal-probe", MFD_CLOEXEC);
        unsigned char value = 0x5a;
        if (fd < 0 || ftruncate(fd, (off_t)size) || pwrite(fd, &value, 1, 0) != 1) {
            perror("memfd setup"); exit(2);
        }
    }
    void *map = mmap(NULL, size, file_backed ? PROT_READ : PROT_READ | PROT_WRITE,
                     file_backed ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); exit(2); }
    if (!file_backed) *(unsigned char *)map = 0x5a;
    if (dontfork) {
        errno = 0;
        int result = madvise(map, size, MADV_DONTFORK);
        int error = errno;
        printf("%s parent madvise(DONTFORK): rc=%d errno=%d address=%p size=%zu\n",
               kind, result, error, map, size);
    } else printf("%s parent no advice: address=%p size=%zu\n", kind, map, size);
    residency(kind, "parent-before-fork", map, size);
    pid_t child = fork();
    if (child < 0) { perror("fork"); exit(2); }
    if (!child) {
        residency(kind, "child", map, size);
        /* This independently distinguishes a real mapping from mincore alone.
         * The handler exits111 if the expected DONTFORK access fault occurs. */
        unsigned char value = *(volatile unsigned char *)map;
        printf("%s child read: value=%u\n", kind, value);
        _exit(value == 0x5a ? 0 : 2);
    }
    int status;
    if (waitpid(child, &status, 0) != child) { perror("waitpid"); exit(2); }
    printf("%s parent child-status: exited=%d code=%d signal=%d\n", kind,
           WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    residency(kind, "parent-after-child", map, size);
    if (munmap(map, size) || (fd >= 0 && close(fd))) { perror("cleanup"); exit(2); }
}

int main(void)
{
    struct rlimit no_core = {0, 0};
    if (setrlimit(RLIMIT_CORE, &no_core)) { perror("setrlimit"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (signal(SIGSEGV, access_fault) == SIG_ERR) { perror("signal"); return 2; }
    probe("anonymous-control", 0, 0);
    probe("anonymous-dontfork", 0, 1);
    probe("memfd-readonly-dontfork", 1, 1);
    return 0; /* Observation probe: conclusions come from recorded results. */
}
