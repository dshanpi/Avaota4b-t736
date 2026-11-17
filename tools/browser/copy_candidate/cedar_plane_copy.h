/* SPDX-License-Identifier: MIT
 * Isolated ordinary-copy experiment; not included by the production backend.
 * Source must be an already readable, stable CPU mapping. No mapping/cache/
 * synchronization/ownership changes occur here. Source/destination disjoint.
 */
#ifndef CEDAR_PLANE_COPY_CANDIDATE_H
#define CEDAR_PLANE_COPY_CANDIDATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef void (*cedar_copy_bytes_fn)(void *, const void *, size_t);

static void cedar_copy_bytes_libc(void *destination, const void *source, size_t size)
{ memcpy(destination, source, size); }

/* Explicit NEON comparison only: prior A76 WC measurements favored libc.
 * Exact tails do not reread the first/last vector or touch padding. */
static void cedar_copy_bytes_ldp128(void *destination, const void *source, size_t size)
{
    uint8_t *d = destination;
    const uint8_t *s = source;
#ifdef __aarch64__
    while (size >= 128) {
        __asm__ volatile(
            "ldp q0, q1, [%[s]]\n\tldp q2, q3, [%[s], #32]\n\t"
            "ldp q4, q5, [%[s], #64]\n\tldp q6, q7, [%[s], #96]\n\t"
            "stp q0, q1, [%[d]]\n\tstp q2, q3, [%[d], #32]\n\t"
            "stp q4, q5, [%[d], #64]\n\tstp q6, q7, [%[d], #96]"
            : : [s]"r"(s), [d]"r"(d)
            : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory");
        s += 128; d += 128; size -= 128;
    }
    while (size >= 16) {
        __asm__ volatile("ldr q0, [%[s]]\n\tstr q0, [%[d]]"
            : : [s]"r"(s), [d]"r"(d) : "v0", "memory");
        s += 16; d += 16; size -= 16;
    }
#endif
    /* Volatile loads/stores prevent compiler vectorization of a short exact
     * tail into speculative wider accesses. No barriers/cache ops implied. */
    while (size--) *(volatile uint8_t *)d++ = *(const volatile uint8_t *)s++;
}

static void cedar_copy_bytes_ldp256(void *destination, const void *source, size_t size)
{
    uint8_t *d = destination;
    const uint8_t *s = source;
#ifdef __aarch64__
    while (size >= 256) {
        __asm__ volatile(
            "ldp q0, q1, [%[s]]\n\tldp q2, q3, [%[s], #32]\n\t"
            "ldp q4, q5, [%[s], #64]\n\tldp q6, q7, [%[s], #96]\n\t"
            "ldp q16, q17, [%[s], #128]\n\tldp q18, q19, [%[s], #160]\n\t"
            "ldp q20, q21, [%[s], #192]\n\tldp q22, q23, [%[s], #224]\n\t"
            "stp q0, q1, [%[d]]\n\tstp q2, q3, [%[d], #32]\n\t"
            "stp q4, q5, [%[d], #64]\n\tstp q6, q7, [%[d], #96]\n\t"
            "stp q16, q17, [%[d], #128]\n\tstp q18, q19, [%[d], #160]\n\t"
            "stp q20, q21, [%[d], #192]\n\tstp q22, q23, [%[d], #224]"
            : : [s]"r"(s), [d]"r"(d)
            : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
              "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "memory");
        s += 256; d += 256; size -= 256;
    }
#endif
    cedar_copy_bytes_ldp128(d, s, size);
}

/* Validate before any access. For contiguous visible rows, a single copy is
 * equivalent to the old row loop. Padded/cropped rows retain that loop. */
static int cedar_copy_plane_candidate(uint8_t *destination, size_t destination_size,
    const uint8_t *source, size_t source_size, size_t row_bytes, size_t rows,
    size_t stride, cedar_copy_bytes_fn copy)
{
    if (!row_bytes || !rows) return 0;
    if (!destination || !source || !copy || stride < row_bytes ||
        rows > SIZE_MAX / row_bytes ||
        rows - 1 > (SIZE_MAX - row_bytes) / stride) return -1;
    size_t bytes = row_bytes * rows;
    size_t span = (rows - 1) * stride + row_bytes;
    if (bytes > destination_size || span > source_size) return -1;
    if (stride == row_bytes) copy(destination, source, bytes);
    else for (size_t row = 0; row < rows; row++)
        copy(destination + row * row_bytes, source + row * stride, row_bytes);
    return 0;
}
#endif
