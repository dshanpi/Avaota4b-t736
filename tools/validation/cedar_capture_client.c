/* SPDX-License-Identifier: MIT
 * DIAGNOSTIC ONLY: record at most 64 filtered access units, then deliberately
 * fail before producing a frame so the wrapper replays original packets in
 * its native software decoder. No socket, device, mmap, or DMA operation.
 * Must never be installed as the normal hardware client.
 */
#define _GNU_SOURCE
#include "cedar_client.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int output = -1, active, consumed;
static unsigned count;
static size_t total;
static int all(const void *buf, size_t length)
{
    const unsigned char *p = buf;
    while (length) {
        ssize_t n = write(output, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -EIO;
        p += n; length -= n;
    }
    return 0;
}
__attribute__((constructor)) static void init(int argc, char **argv, char **envp)
{
    (void)envp;
    const char *enabled = getenv("T736_CEDAR_CAPTURE");
    if (!enabled || strcmp(enabled,"1") || argc < 2 || strcmp(argv[argc-1],"rdd")) return;
    output = open("/home/t736/.local/state/t736-bili-filtered-v5.bin",
                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (output >= 0) fprintf(stderr,"CEDAR_CAPTURE_ONLY: no hardware access, bounded filtered AU recording\n");
}
int t736_client_available(void) { return output >= 0 && !consumed; }
int t736_client_open(int codec, unsigned width, unsigned height)
{
    if (output < 0 || active || consumed) return -ENOTCONN;
    uint32_t header[4] = { 0x35434150, (unsigned)codec, width, height };
    if (all(header,sizeof(header))) return -EIO;
    active = 1; consumed = 1;
    fprintf(stderr,"CEDAR_CAPTURE_ONLY: codec=%d %ux%u\n",codec,width,height);
    return 1;
}
int t736_client_push(int session, const uint8_t *data, size_t size, int64_t pts, int64_t dts)
{
    if (session != 1 || !active || !data || !size || count >= 64 ||
        size > T736_MAX_PACKET || total > T736_MAX_PACKET-size) return -EIO;
    uint32_t n = (uint32_t)size;
    if (all(&n,sizeof(n)) || all(&pts,sizeof(pts)) || all(&dts,sizeof(dts)) || all(data,size)) return -EIO;
    count++; total += size;
    return 0;
}
int t736_client_pull(int session, uint8_t *dst, size_t cap, struct t736_frame *frame, unsigned ms)
{
    (void)session;(void)dst;(void)cap;(void)frame;(void)ms;
    return count >= 64 ? -EIO : 0;
}
int t736_client_drain(int session) { (void)session; return -EIO; }
int t736_client_flush(int session) { (void)session; return -EIO; }
int t736_client_close(int session)
{
    (void)session;
    if (active) {
        fprintf(stderr,"CEDAR_CAPTURE_ONLY: recorded=%u bytes=%zu; native fallback expected\n",count,total);
        active=0;
    }
    if (output >= 0) { close(output);output=-1; }
    return 0;
}
