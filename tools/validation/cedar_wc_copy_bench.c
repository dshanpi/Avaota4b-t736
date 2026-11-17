/* SPDX-License-Identifier: MIT
 * Read-only copy benchmark for an EXISTING, IDLE I420/YV12 dma-buf.
 * No DMA allocation, import, device ioctl, firmware call or pipeline creation.
 * pidfd_getfd duplicates a selected export fd; mmap is strictly PROT_READ.
 * Use --self-test on ordinary memory to verify routines without board access.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

typedef void (*copy_fn)(void *, const void *, size_t);
static void libc_copy(void *d, const void *s, size_t n) { memcpy(d,s,n); }

#ifdef __aarch64__
static void neon_ld1_64(void *dest, const void *source, size_t size)
{
    uint8_t *d=dest; const uint8_t *s=source;
    while (size >= 64) {
        __asm__ volatile("ld1 {v0.16b-v3.16b}, [%[s]]\n\t"
                         "st1 {v0.16b-v3.16b}, [%[d]]"
                         : : [s]"r"(s), [d]"r"(d)
                         : "v0","v1","v2","v3","memory");
        s+=64; d+=64; size-=64;
    }
    if (size) memcpy(d,s,size);
}
static void neon_ldp_64(void *dest, const void *source, size_t size)
{
    uint8_t *d=dest; const uint8_t *s=source;
    while (size >= 64) {
        __asm__ volatile("ldp q0, q1, [%[s]]\n\tldp q2, q3, [%[s], #32]\n\t"
                         "stp q0, q1, [%[d]]\n\tstp q2, q3, [%[d], #32]"
                         : : [s]"r"(s), [d]"r"(d)
                         : "v0","v1","v2","v3","memory");
        s+=64; d+=64; size-=64;
    }
    if (size) memcpy(d,s,size);
}
static void neon_ldp_128(void *dest, const void *source, size_t size)
{
    uint8_t *d=dest; const uint8_t *s=source;
    while (size >= 128) {
        __asm__ volatile("ldp q0, q1, [%[s]]\n\tldp q2, q3, [%[s], #32]\n\t"
                         "ldp q4, q5, [%[s], #64]\n\tldp q6, q7, [%[s], #96]\n\t"
                         "stp q0, q1, [%[d]]\n\tstp q2, q3, [%[d], #32]\n\t"
                         "stp q4, q5, [%[d], #64]\n\tstp q6, q7, [%[d], #96]"
                         : : [s]"r"(s), [d]"r"(d)
                         : "v0","v1","v2","v3","v4","v5","v6","v7","memory");
        s+=128; d+=128; size-=128;
    }
    neon_ldp_64(d,s,size);
}
static void neon_ldp_256(void *dest, const void *source, size_t size)
{
    uint8_t *d=dest; const uint8_t *s=source;
    while (size >= 256) {
        __asm__ volatile("ldp q0, q1, [%[s]]\n\tldp q2, q3, [%[s], #32]\n\t"
                         "ldp q4, q5, [%[s], #64]\n\tldp q6, q7, [%[s], #96]\n\t"
                         "ldp q16, q17, [%[s], #128]\n\tldp q18, q19, [%[s], #160]\n\t"
                         "ldp q20, q21, [%[s], #192]\n\tldp q22, q23, [%[s], #224]\n\t"
                         "stp q0, q1, [%[d]]\n\tstp q2, q3, [%[d], #32]\n\t"
                         "stp q4, q5, [%[d], #64]\n\tstp q6, q7, [%[d], #96]\n\t"
                         "stp q16, q17, [%[d], #128]\n\tstp q18, q19, [%[d], #160]\n\t"
                         "stp q20, q21, [%[d], #192]\n\tstp q22, q23, [%[d], #224]"
                         : : [s]"r"(s), [d]"r"(d)
                         : "v0","v1","v2","v3","v4","v5","v6","v7",
                           "v16","v17","v18","v19","v20","v21","v22","v23","memory");
        s+=256; d+=256; size-=256;
    }
    neon_ldp_128(d,s,size);
}
#endif

static uint64_t now_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC_RAW,&t)) { perror("clock_gettime"); exit(2); }
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static unsigned number(const char *s)
{
    char *end; errno=0; unsigned long n=strtoul(s,&end,10);
    if (errno || *end || end==s || n>10000000) { fprintf(stderr,"invalid integer\n"); exit(2); }
    return (unsigned)n;
}
static void frame_copy(copy_fn fn, int rows, uint8_t *dst, const uint8_t *src,
                       unsigned width, unsigned height, unsigned storage_height)
{
    size_t y_storage=(size_t)width*storage_height, c_storage=y_storage/4;
    for (unsigned p=0;p<3;p++) {
        unsigned w=width>>(p!=0), h=height>>(p!=0);
        const uint8_t *plane=src+(p==0 ? 0 : y_storage+(p-1)*c_storage);
        if (rows) for (unsigned y=0;y<h;y++) fn(dst+(size_t)y*w,plane+(size_t)y*w,w);
        else fn(dst,plane,(size_t)w*h);
        dst+=(size_t)w*h;
    }
}

int main(int argc,char **argv)
{
    unsigned width=1920,height=1080,storage_height=1088,iterations=12;
    int self_test=0,pid=-1,remote_fd=-1,fd=-1,pidfd=-1;
    for (int i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--self-test")) { self_test=1; continue; }
        if (i+1>=argc) { fprintf(stderr,"missing option value\n"); return 2; }
        unsigned value=number(argv[++i]); const char *option=argv[i-1];
        if (!strcmp(option,"--pid")) pid=value;
        else if (!strcmp(option,"--fd")) remote_fd=value;
        else if (!strcmp(option,"--width")) width=value;
        else if (!strcmp(option,"--height")) height=value;
        else if (!strcmp(option,"--storage-height")) storage_height=value;
        else if (!strcmp(option,"--iterations")) iterations=value;
        else { fprintf(stderr,"unknown option: %s\n",option); return 2; }
    }
    if (!width || !height || width>1920 || height>1080 || storage_height<height ||
        storage_height>4096 || (width|height|storage_height)&1 ||
        !iterations || iterations>100 || (!self_test && (pid<=0 || remote_fd<0))) {
        fprintf(stderr,"usage: %s --self-test | --pid PID --fd ID [--width W --height H --storage-height H --iterations N]\n",argv[0]);
        return 2;
    }
    size_t span=(size_t)width*storage_height*3/2,visible=(size_t)width*height*3/2;
    uint8_t *source,*dest,*reference;
    if (posix_memalign((void **)&dest,64,visible) || posix_memalign((void **)&reference,64,visible)) {
        fprintf(stderr,"destination allocation failed\n"); return 2;
    }
    memset(dest,0,visible); memset(reference,0,visible);
    if (self_test) {
        if (posix_memalign((void **)&source,64,span)) return 2;
        for (size_t i=0;i<span;i++) source[i]=(uint8_t)(i*31+(i>>12));
    } else {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_getfd)
        pidfd=(int)syscall(SYS_pidfd_open,pid,0);
        if (pidfd<0) { perror("pidfd_open"); return 2; }
        fd=(int)syscall(SYS_pidfd_getfd,pidfd,remote_fd,0);
        if (fd<0) { perror("pidfd_getfd (requires permission to inspect target process)"); return 2; }
#else
        fprintf(stderr,"build headers lack pidfd support\n"); return 2;
#endif
        struct stat statbuf;
        if (fstat(fd,&statbuf)) { perror("fstat"); return 2; }
        if (statbuf.st_size<0 || (uint64_t)statbuf.st_size<span) {
            fprintf(stderr,"fd size=%jd is smaller than required plane span=%zu\n",(intmax_t)statbuf.st_size,span); return 2;
        }
        source=mmap(NULL,span,PROT_READ,MAP_SHARED,fd,0);
        if (source==MAP_FAILED) { perror("read-only mmap"); return 2; }
    }
    struct { const char *name; copy_fn fn; int rows; } variants[]={
        {"libc-row",libc_copy,1}, {"libc-plane",libc_copy,0},
#ifdef __aarch64__
        {"neon-ld1-64-plane",neon_ld1_64,0}, {"neon-ldp-64-plane",neon_ldp_64,0},
        {"neon-ldp-128-plane",neon_ldp_128,0}, {"neon-ldp-256-plane",neon_ldp_256,0},
#endif
    };
    frame_copy(libc_copy,1,reference,source,width,height,storage_height);
    printf("{\"source\":\"%s\",\"pid\":%d,\"fd\":%d,\"mapped_bytes\":%zu,\"visible_bytes\":%zu,\"width\":%u,\"height\":%u,\"storage_height\":%u,\"iterations\":%u,\"results\":[",
           self_test?"ordinary-memory-self-test":"borrowed-existing-dmabuf-readonly",pid,remote_fd,span,visible,width,height,storage_height,iterations);
    for (size_t v=0;v<sizeof(variants)/sizeof(variants[0]);v++) {
        uint64_t total=0,min=UINT64_MAX,max=0;
        frame_copy(variants[v].fn,variants[v].rows,dest,source,width,height,storage_height);
        if (memcmp(reference,dest,visible)) { fprintf(stderr,"copy mismatch or source changed: %s\n",variants[v].name); return 1; }
        for (unsigned n=0;n<iterations;n++) {
            uint64_t start=now_ns();
            frame_copy(variants[v].fn,variants[v].rows,dest,source,width,height,storage_height);
            uint64_t elapsed=now_ns()-start;
            total+=elapsed; if (elapsed<min) min=elapsed; if (elapsed>max) max=elapsed;
        }
        if (memcmp(reference,dest,visible)) { fprintf(stderr,"copy mismatch or source changed: %s\n",variants[v].name); return 1; }
        printf("%s{\"name\":\"%s\",\"avg_us\":%.3f,\"min_us\":%.3f,\"max_us\":%.3f,\"MB_s\":%.3f,\"exact\":true}",
               v?",":"",variants[v].name,(double)total/iterations/1000.0,
               (double)min/1000.0,(double)max/1000.0,(double)visible*iterations*1000.0/total);
        fflush(stdout);
    }
    puts("],\"passed\":true,\"dma_allocations\":0,\"source_writes\":0}");
    if (self_test) free(source); else munmap(source,span);
    if (fd>=0) close(fd);
    if (pidfd>=0) close(pidfd);
    free(reference); free(dest); return 0;
}
