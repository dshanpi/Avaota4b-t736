/* SPDX-License-Identifier: MIT
 * Independent allocator-failure audit of the actual Cedar RAM pool.
 * Link with --wrap=calloc --wrap=posix_memalign --wrap=free.
 * No hardware, vendor plugin, or DMA allocation is used.
 */
#define main canonical_backend_tests_main
#include "test_cedar_backend.c"
#undef main
#include <errno.h>

void *__real_calloc(size_t count, size_t size);
int __real_posix_memalign(void **result, size_t alignment, size_t size);
void __real_free(void *pointer);

static unsigned fail_calloc_once, fail_aligned_once;
static unsigned pool_allocations, block_allocations, outstanding;
static void *tracked[128];

static void track(void *pointer)
{
    assert(pointer);
    for (unsigned i = 0; i < 128; i++) {
        if (!tracked[i]) {
            tracked[i] = pointer;
            outstanding++;
            return;
        }
    }
    assert(!"allocation tracker exhausted");
}

void *__wrap_calloc(size_t count, size_t size)
{
    void *pointer;
    if (fail_calloc_once) {
        fail_calloc_once--;
        return NULL;
    }
    pointer = __real_calloc(count, size);
    if (pointer) {
        pool_allocations++;
        track(pointer);
    }
    return pointer;
}

int __wrap_posix_memalign(void **result, size_t alignment, size_t size)
{
    int status;
    if (fail_aligned_once) {
        fail_aligned_once--;
        return ENOMEM;
    }
    status = __real_posix_memalign(result, alignment, size);
    if (!status) {
        block_allocations++;
        track(*result);
    }
    return status;
}

void __wrap_free(void *pointer)
{
    if (pointer) {
        for (unsigned i = 0; i < 128; i++) {
            if (tracked[i] == pointer) {
                tracked[i] = NULL;
                assert(outstanding);
                outstanding--;
                break;
            }
        }
    }
    __real_free(pointer);
}

static void clean_state(void)
{
    assert(!outstanding && !fail_calloc_once && !fail_aligned_once);
    for (unsigned i = 0; i < 128; i++) assert(!tracked[i]);
}

int main(int argc, char **argv)
{
    cedar_decoder d = {0};
    cedar_frame a = {0}, b = {0}, c = {0};
    unsigned before;
    gst_init(&argc, &argv);

    /* Invalid sizes must not create a pool or overflow allocation arithmetic. */
    before = pool_allocations;
    assert(!ram_acquire(&d, 0));
    assert(!ram_acquire(&d, (size_t)CEDAR_MAX_OUTPUT + 1));
    assert(!ram_acquire(&d, SIZE_MAX));
    assert(!d.ram_pool && pool_allocations == before);
    cedar_frame_free(NULL);
    cedar_frame_free(&a);
    cedar_frame_free(&a);
    ram_pool_close(&d);
    clean_state();

    /* First pool allocation fails without publishing a dangling owner. */
    fail_calloc_once = 1;
    assert(!ram_acquire(&d, 4096) && !d.ram_pool);
    clean_state();

    /* A block allocation failure leaves exactly the decoder owner's ref. */
    fail_aligned_once = 1;
    assert(!ram_acquire(&d, 4096) && d.ram_pool);
    assert(g_atomic_int_get(&d.ram_pool->references) == 1);
    assert(!d.ram_pool->cached && outstanding == 1);
    ram_pool_close(&d);
    clean_state();

    /* An outstanding frame remains unchanged when a second allocation fails. */
    a.data = ram_acquire(&d, 4096);
    assert(a.data);
    memset(a.data, 0xa7, 4096);
    fail_aligned_once = 1;
    assert(!ram_acquire(&d, 4096));
    assert(g_atomic_int_get(&d.ram_pool->references) == 2);
    for (unsigned i = 0; i < 4096; i++) assert(a.data[i] == 0xa7);
    cedar_frame_free(&a);
    assert(d.ram_pool->cached == 1);

    /* A wrong-size idle buffer is neither reused nor lost on OOM. */
    before = block_allocations;
    fail_aligned_once = 1;
    assert(!ram_acquire(&d, 8192));
    assert(d.ram_pool->cached == 1 && block_allocations == before);
    a.data = ram_acquire(&d, 4096);
    assert(a.data && d.ram_pool->cached == 0 && block_allocations == before);
    for (unsigned i = 0; i < 4096; i++) assert(a.data[i] == 0xa7);
    b.data = ram_acquire(&d, 8192);
    assert(b.data && b.data != a.data);
    memset(b.data, 0x39, 8192);
    c.data = ram_acquire(&d, CEDAR_MAX_OUTPUT);
    assert(c.data && !((uintptr_t)c.data & 63));
    c.data[CEDAR_MAX_OUTPUT - 1] = 0x61;
    cedar_frame_free(&c);
    ram_pool_close(&d);
    assert(!d.ram_pool && outstanding == 3); /* Pool plus two live blocks. */
    for (unsigned i = 0; i < 4096; i++) assert(a.data[i] == 0xa7);
    for (unsigned i = 0; i < 8192; i++) assert(b.data[i] == 0x39);
    cedar_frame_free(&a);
    assert(outstanding == 2);
    cedar_frame_free(&b);
    cedar_frame_free(&b);
    clean_state();

    /* Exercise copy_sample's error/unmap path with each real allocator failing. */
    for (unsigned kind = 0; kind < 2; kind++) {
        cedar_decoder decoder = {.width = 4, .height = 4};
        cedar_frame output = {0};
        GstSample *sample = sample_for(GST_VIDEO_FORMAT_YV12, TRUE, 2, 2, 4, 4);
        if (!kind) fail_calloc_once = 1;
        else fail_aligned_once = 1;
        assert(copy_sample(&decoder, sample, &output) == CEDAR_ERROR);
        assert(decoder.failed && !output.data);
        assert(strstr(cedar_error(&decoder), "I420 output allocation failed"));
        cedar_frame_free(&output);
        ram_pool_close(&decoder);
        gst_sample_unref(sample);
        clean_state();
    }
    printf("PASS: actual RAM pool allocator failures, invalid/max sizes, mixed-size "
           "cache, held frames after failure/close, empty free, copy_sample OOM; "
           "tracked allocation balance=0, pools=%u blocks=%u\n",
           pool_allocations, block_allocations);
    return 0;
}
