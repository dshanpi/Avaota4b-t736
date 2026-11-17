/* SPDX-License-Identifier: MIT
 * Ordinary-memory correctness only. Guard pages exercise the actual ARM64
 * instructions under QEMU; host sanitizers check C geometry/guard semantics.
 * No device, fd import, DMA allocation or producer pipeline is accessed.
 */
#define _GNU_SOURCE
#include "../tools/browser/copy_candidate/cedar_plane_copy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;
static size_t calls;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void counting_copy(void *d, const void *s, size_t n)
{ calls++; memcpy(d, s, n); }

static void alignment_and_tails(cedar_copy_bytes_fn copy)
{
    uint8_t source[640], output[640], expected[640];
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 17 + 11);
    for (size_t n = 0; n <= 512; n++) {
        for (unsigned sa = 0; sa < 32; sa++) {
            unsigned da = (sa * 13 + (unsigned)n) & 63;
            memset(output, 0xa9, sizeof(output));
            memset(expected, 0xa9, sizeof(expected));
            memcpy(expected + da, source + sa, n);
            copy(output + da, source + sa, n);
            CHECK(!memcmp(output, expected, sizeof(output)));
        }
    }
}

static void guards(cedar_copy_bytes_fn copy)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    CHECK(page >= 4096);
    uint8_t *source = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint8_t *output = mmap(NULL, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(source != MAP_FAILED && output != MAP_FAILED);
    CHECK(!mprotect(source + page, page, PROT_READ | PROT_WRITE));
    CHECK(!mprotect(output + page, page, PROT_READ | PROT_WRITE));
    for (size_t i = 0; i < page; i++) source[page + i] = (uint8_t)(i * 11 + 3);
    CHECK(!mprotect(source + page, page, PROT_READ));
    /* Every tail size0..512 and larger row sizes ends immediately at a guard.
     * Zero-length calls intentionally point at PROT_NONE memory. */
    for (size_t n = 0; n <= page; n++) {
        if (n > 512 && n != 959 && n != 960 && n != 961 &&
            n != 1919 && n != 1920 && n != 1921 && n != page - 1 && n != page) continue;
        memset(output + page, 0xd3, page);
        copy(output + page * 2 - n, source + page * 2 - n, n);
        CHECK(!memcmp(output + page * 2 - n, source + page * 2 - n, n));
        for (size_t i = 0; i < page - n; i++) CHECK(output[page + i] == 0xd3);
        /* Beginning-of-object variant detects negative-address accesses. */
        copy(output + page, source + page, n);
        CHECK(!memcmp(output + page, source + page, n));
    }
    CHECK(!munmap(source, page * 3) && !munmap(output, page * 3));
}

static void invalid_geometry(void)
{
    uint8_t source[64], output[64], before[64];
    memset(source, 0x3c, sizeof(source)); memset(output, 0xa7, sizeof(output));
    memcpy(before, output, sizeof(output));
    CHECK(!cedar_copy_plane_candidate(NULL, 0, NULL, 0, 0, SIZE_MAX, 0, NULL));
    CHECK(!cedar_copy_plane_candidate(NULL, 0, NULL, 0, SIZE_MAX, 0, 0, NULL));
    struct geometry { size_t width, rows, stride, src, dst; } bad[] = {
        {8,2,7,64,64}, {8,2,8,15,64}, {8,2,8,64,15},
        {SIZE_MAX,2,SIZE_MAX,SIZE_MAX,SIZE_MAX},
        {1,SIZE_MAX,SIZE_MAX,SIZE_MAX,SIZE_MAX}, {16,4,17,64,64}
    };
    for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
        calls = 0;
        CHECK(cedar_copy_plane_candidate(output,bad[i].dst,source,bad[i].src,
            bad[i].width,bad[i].rows,bad[i].stride,counting_copy) == -1);
        CHECK(calls == 0 && !memcmp(output,before,sizeof(output)));
    }
}

static void planar_frame(cedar_copy_bytes_fn copy, unsigned width, unsigned height,
                         unsigned padding, unsigned crop_x, unsigned crop_y,
                         int yv12, unsigned chroma_extra)
{
    size_t plane_sizes[3], strides[3], offsets[3], starts[3], src_size = 0, dst_size = 0;
    for (unsigned plane = 0; plane < 3; plane++) {
        size_t w = plane ? (width + 1)/2 : width;
        size_t h = plane ? (height + 1)/2 : height;
        size_t x = plane ? crop_x/2 : crop_x;
        size_t y = plane ? crop_y/2 : crop_y;
        strides[plane] = w + x + padding + (plane == 1 ? chroma_extra : 0);
        offsets[plane] = src_size;
        plane_sizes[plane] = strides[plane] * (h + y + 3);
        starts[plane] = src_size + y * strides[plane] + x;
        src_size += plane_sizes[plane]; dst_size += w * h;
    }
    uint8_t *source = malloc(src_size), *output = malloc(dst_size + 32), *expected = malloc(dst_size);
    CHECK(source && output && expected);
    memset(source, 0xef, src_size); memset(output, 0xa3, dst_size + 32);
    size_t destination = 0;
    for (unsigned logical = 0; logical < 3; logical++) {
        unsigned physical = logical && yv12 ? 3 - logical : logical;
        size_t w = logical ? (width + 1)/2 : width;
        size_t h = logical ? (height + 1)/2 : height;
        for (size_t y = 0; y < h; y++) for (size_t x = 0; x < w; x++) {
            uint8_t value = (uint8_t)(logical * 67 + x * 11 + y * 3);
            source[starts[physical] + y * strides[physical] + x] = value;
            expected[destination + y * w + x] = value;
        }
        destination += w * h;
    }
    destination = 0;
    for (unsigned logical = 0; logical < 3; logical++) {
        unsigned physical = logical && yv12 ? 3 - logical : logical;
        size_t w = logical ? (width + 1)/2 : width;
        size_t h = logical ? (height + 1)/2 : height;
        size_t remaining = offsets[physical] + plane_sizes[physical] - starts[physical];
        CHECK(!cedar_copy_plane_candidate(output + 16 + destination, dst_size - destination,
            source + starts[physical], remaining, w, h, strides[physical], copy));
        destination += w * h;
    }
    CHECK(!memcmp(output + 16, expected, dst_size));
    for (unsigned i = 0; i < 16; i++) CHECK(output[i] == 0xa3 && output[16 + dst_size + i] == 0xa3);
    free(source); free(output); free(expected);
}

int main(void)
{
    cedar_copy_bytes_fn variants[] = {cedar_copy_bytes_libc, cedar_copy_bytes_ldp128,
                                     cedar_copy_bytes_ldp256};
    invalid_geometry();
    for (unsigned v = 0; v < sizeof(variants)/sizeof(variants[0]); v++) {
        alignment_and_tails(variants[v]); guards(variants[v]);
        unsigned geometry[][2] = {{1,1},{18,1},{4,4},{18,14},{317,177},{318,178},{1920,1080}};
        for (unsigned g = 0; g < sizeof(geometry)/sizeof(geometry[0]); g++)
            for (unsigned padding = 0; padding <= 16; padding += 16)
                for (unsigned crop = 0; crop <= 2; crop += 2)
                    for (int yv12 = 0; yv12 <= 1; yv12++)
                        planar_frame(variants[v],geometry[g][0],geometry[g][1],padding,crop,crop,yv12,0);
        for (int yv12 = 0; yv12 <= 1; yv12++) {
            /* A nonzero vertical crop is still packed when horizontal crop0.
             * Chroma planes are tested independently, with unequal strides. */
            planar_frame(variants[v],1920,1080,0,0,2,yv12,0);
            planar_frame(variants[v],18,14,0,0,2,yv12,16);
            planar_frame(variants[v],317,177,0,0,2,yv12,1);
        }
    }
    uint8_t source[128], destination[128]; memset(source, 4, sizeof(source));
    calls = 0;
    CHECK(!cedar_copy_plane_candidate(destination,128,source,128,16,4,16,counting_copy));
    CHECK(calls == 1);
    calls = 0;
    CHECK(!cedar_copy_plane_candidate(destination,128,source,128,16,4,20,counting_copy));
    CHECK(calls == 4);
    printf("PASS checks=%u source=ordinary-memory hardware_access=0 production_modified=0\n",checks);
    return 0;
}
