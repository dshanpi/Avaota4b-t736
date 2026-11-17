/* Exercise the actual ARM64 candidate MemAdapter without VE or dma-heap hardware.
 * Executable symbols replace only the allocator/VE boundaries with memfd mocks.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sc_interface.h"
#include "memoryAdapter.h"
#include "veAdapter.h"
#include "veInterface.h"

static unsigned int offset;
static int map_failure, high_address, maps, releases, last_fd = -1;
static int heap_open_failure;
static int context_marker;
static int active[4096];

int CdcIonOpen(void)
{
    if (heap_open_failure) { errno = ENOENT; return -1; }
    return memfd_create("mock-heap-control", MFD_CLOEXEC);
}

int CdcDmaheapAllocFd(int unused, size_t size)
{
    int fd;
    (void)unused;
    fd = memfd_create("mock-wc-backing", MFD_CLOEXEC);
    assert(fd >= 0 && fd < (int)(sizeof(active)/sizeof(active[0])));
    assert(ftruncate(fd, (off_t)size) == 0);
    last_fd = fd;
    return fd;
}

static void *mock_init(VeConfig *config)
{
    assert(config->nDecoderFlag == 1);
    return &context_marker;
}
static void mock_release(void *self) { assert(self == &context_marker); releases++; }
static unsigned int mock_offset(void *self) { assert(self == &context_marker); return offset; }
static uint64_t mock_version(void *self) { assert(self == &context_marker); return UINT64_C(0x300003321021321); }
static int mock_map(void *self, struct user_iommu_param *p)
{
    assert(self == &context_marker);
    if (map_failure) { errno = EIO; return -1; }
    assert(p->fd >= 0 && p->fd < (int)(sizeof(active)/sizeof(active[0])));
    active[p->fd]++;
    maps++;
    p->iommu_addr = high_address ? UINT32_C(0xfffff000) : UINT32_C(0x90000000) + p->fd * 0x10000u;
    return 0;
}
static int mock_unmap(void *self, struct user_iommu_param *p)
{
    assert(self == &context_marker);
    assert(p->fd >= 0 && p->fd < (int)(sizeof(active)/sizeof(active[0])));
    assert(active[p->fd] > 0 && maps > 0);
    active[p->fd]--;
    maps--;
    return 0;
}
static VeOpsS mock_ops = {
    .init = mock_init, .release = mock_release, .getPhyOffset = mock_offset,
    .getIcVeVersion = mock_version, .getIommuAddr = mock_map, .freeIommuAddr = mock_unmap,
};
VeOpsS *GetVeOpsS(int type) { assert(type == VE_OPS_TYPE_NORMAL); return &mock_ops; }

static void fd_closed(int fd)
{
    errno = 0;
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
}

int main(void)
{
    struct ScMemOpsS *ops = MemAdapterGetOpsS();
    struct rlimit no_core = {0, 0};
    void *p;
    unsigned int addr;
    int fd, status;
    pid_t child;
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    assert(ops->palloc(4096, NULL, NULL) == NULL);
    heap_open_failure = 1;
    assert(ops->open() != 0);
    heap_open_failure = 0;
    offset = 0x40000000;
    assert(ops->open() != 0);
    offset = 0;
    assert(ops->open() == 0);
    assert(ops->palloc(0, NULL, NULL) == NULL);
    p = ops->palloc(8192, NULL, NULL);
    assert(p && maps == 1);
    fd = last_fd;
    memset(p, 0x35, 8192);
    ops->flush_cache(p, 8192);
    ops->flush_cache((char *)p + 8191, 1);
    assert((uintptr_t)ops->ve_get_phyaddr((char *)p + 17) == (uintptr_t)ops->cpu_get_phyaddr(p) + 17);
    child = fork();
    assert(child >= 0);
    if (!child) { ops->flush_cache((char *)p + 8191, 2); _exit(99); }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    ops->pfree(p, NULL, NULL);
    assert(maps == 0);
    fd_closed(fd);
    map_failure = 1;
    assert(ops->palloc(4096, NULL, NULL) == NULL && maps == 0);
    fd_closed(last_fd);
    addr = 0x1234;
    assert(ops->get_phy_by_fd(123, &addr) != 0 && addr == 0);
    map_failure = 0;
    high_address = 1;
    assert(ops->palloc(8192, NULL, NULL) == NULL && maps == 0);
    fd_closed(last_fd);
    high_address = 0;
    p = ops->palloc_no_cache(4096, NULL, NULL);
    assert(p && maps == 1);
    fd = last_fd;
    ops->close(); /* Closing with a residual owned allocation must release mapping/FD. */
    assert(maps == 0);
    fd_closed(fd);
    assert(releases == 3);
    puts("PASS: actual candidate; missing heap, phy offset, allocation/import errors, WC range/barrier, overflow and lifetime");
    return 0;
}
