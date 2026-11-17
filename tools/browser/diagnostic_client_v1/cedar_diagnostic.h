#ifndef T736_CLIENT_DIAGNOSTIC_H
#define T736_CLIENT_DIAGNOSTIC_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static inline void diag(const char *stage, long rc, int error, long a, long b)
{
    int saved = errno;
    char line[256];
    int n = snprintf(line, sizeof(line), "T736_CEDAR_DIAG pid=%ld uid=%ld euid=%ld stage=%s rc=%ld errno=%d a=%ld b=%ld\n",
                     (long)getpid(), (long)getuid(), (long)geteuid(), stage, rc, error, a, b);
    if (n > 0 && (size_t)n < sizeof(line)) { ssize_t ignored = write(STDERR_FILENO, line, (size_t)n); (void)ignored; }
    errno = saved;
}
static inline void diag_namespace(void)
{
    int saved = errno;
    char buf[4097];
    int fd = open("/proc/self/uid_map", O_RDONLY | O_CLOEXEC);
    if (fd < 0) diag("uid_map.open", -1, errno, 0, 0);
    else {
        ssize_t n = read(fd, buf, sizeof(buf)-1); int err = n < 0 ? errno : 0; close(fd);
        if (n < 0) diag("uid_map.read", n, err, 0, 0);
        else {
            buf[n] = 0; char *cursor = buf; unsigned count = 0;
            while (*cursor && count < 16) {
                unsigned long inside, outside, length; int used = 0;
                if (sscanf(cursor, " %lu %lu %lu%n", &inside, &outside, &length, &used) != 3 || !used) break;
                diag("uid_map.range", (long)length, 0, (long)inside, (long)outside);
                cursor += used; count++;
            }
            diag("uid_map.done", count, 0, n, n == (ssize_t)sizeof(buf)-1);
        }
    }
    fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) diag("status.open", -1, errno, 0, 0);
    else {
        ssize_t n = read(fd, buf, sizeof(buf)-1); int err = n < 0 ? errno : 0; close(fd);
        if (n < 0) diag("status.read", n, err, 0, 0);
        else {
            buf[n] = 0;
            char *cursor = !strncmp(buf,"NSpid:",6) ? buf : strstr(buf,"\nNSpid:");
            if (!cursor) diag("NSpid.absent", 0, 0, 0, 0);
            else {
                if (*cursor == '\n') cursor++;
                cursor += 6; unsigned index = 0;
                while (*cursor && *cursor != '\n' && index < 16) {
                    while (*cursor == ' ' || *cursor == '\t') cursor++;
                    if (*cursor < '0' || *cursor > '9') break;
                    unsigned long value = 0;
                    while (*cursor >= '0' && *cursor <= '9') value = value*10 + (unsigned)(*cursor++ - '0');
                    diag("NSpid.value", index++, 0, (long)value, 0);
                }
            }
        }
    }
    errno = saved;
}
#endif
