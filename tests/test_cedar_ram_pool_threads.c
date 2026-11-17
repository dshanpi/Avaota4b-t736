/* SPDX-License-Identifier: MIT
 * Exercise the real backend's ordinary RAM pool without initializing Gst,
 * opening a decoder, touching a device or loading a vendor component.
 */
#include "../tools/browser/cedar_backend.c"
#include <assert.h>
#include <pthread.h>
#include <sched.h>

#define THREAD_TRIALS 512U
#define FRAME_BYTES 4096U

struct return_args {
    pthread_barrier_t *start;
    cedar_frame *frame;
    gboolean yield_after_start;
};

static void wait_start(pthread_barrier_t *barrier)
{
    int result = pthread_barrier_wait(barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *return_frame(void *opaque)
{
    struct return_args *args = opaque;
    wait_start(args->start);
    if (args->yield_after_start)
        sched_yield();
    cedar_frame_free(args->frame);
    return NULL;
}

static cedar_frame acquire_frame(cedar_decoder *decoder, unsigned value)
{
    cedar_frame frame = {.data = ram_acquire(decoder, FRAME_BYTES),
                         .size = FRAME_BYTES};
    assert(frame.data && !((uintptr_t)frame.data & 63));
    memset(frame.data, (uint8_t)value, frame.size);
    return frame; /* Ownership transfers to the caller; no second owner. */
}

static void concurrent_return_and_close(unsigned trial, gboolean keep_held)
{
    cedar_decoder *decoder = calloc(1, sizeof(*decoder));
    cedar_frame cached, first, second, held = {0};
    cedar_ram_pool *pool;
    pthread_barrier_t start;
    pthread_t threads[2];
    struct return_args args[2];
    unsigned marker = (trial * 17U + 3U) & 255U;

    assert(decoder);
    /* Allocate before returning any frame, so close always starts with one
     * separate cached block and two independent outstanding worker frames. */
    cached = acquire_frame(decoder, 0x31);
    first = acquire_frame(decoder, 0x42);
    second = acquire_frame(decoder, 0x53);
    if (keep_held)
        held = acquire_frame(decoder, marker);
    assert(cached.data != first.data && cached.data != second.data &&
           first.data != second.data);
    if (keep_held)
        assert(held.data != cached.data && held.data != first.data &&
               held.data != second.data);
    cedar_frame_free(&cached);
    pool = decoder->ram_pool;
    assert(pool->cached == 1);
    assert(pool->allocations == 3U + (unsigned)keep_held);
    assert(g_atomic_int_get(&pool->references) == 3 + (int)keep_held);

    assert(pthread_barrier_init(&start, NULL, 3) == 0);
    args[0] = (struct return_args){&start, &first, trial % 3U == 1};
    args[1] = (struct return_args){&start, &second, trial % 3U == 2};
    assert(pthread_create(&threads[0], NULL, return_frame, &args[0]) == 0);
    assert(pthread_create(&threads[1], NULL, return_frame, &args[1]) == 0);
    wait_start(&start);
    if (trial % 3U == 0)
        sched_yield();
    cedar_close(decoder);
    assert(pthread_join(threads[0], NULL) == 0);
    assert(pthread_join(threads[1], NULL) == 0);
    assert(pthread_barrier_destroy(&start) == 0);
    assert(!first.data && !first.size && !second.data && !second.size);

    if (keep_held) {
        /* The held frame, not the already freed decoder, owns this final
         * reference. It also makes these inspection reads safe. */
        assert(g_atomic_int_get(&pool->references) == 1);
        assert(pool->closed && pool->cached == 0 && pool->idle == NULL);
        for (size_t i = 0; i < held.size; i++)
            assert(held.data[i] == marker);
        cedar_frame_free(&held);
        assert(!held.data && !held.size);
    }
    /* Without keep_held the final release may be either worker or close.
     * Never inspect pool here: it must already have been destroyed. */
}

int main(void)
{
    for (unsigned trial = 0; trial < THREAD_TRIALS; trial++) {
        concurrent_return_and_close(trial, FALSE);
        concurrent_return_and_close(trial, TRUE);
    }
    printf("PASS: actual RAM pool, %u three-party-barrier trials, %u worker "
           "threads; pre-existing idle block, concurrent frame return/close, "
           "last-reference teardown and held-frame pixels after close; "
           "no Gst init, hardware, vendor component or decoder opening\n",
           THREAD_TRIALS * 2U, THREAD_TRIALS * 4U);
    return 0;
}
