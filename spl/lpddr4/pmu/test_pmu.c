/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "common_message.h"
#include "eye.h"
#include "mailbox.h"
#include "reference/phy-training-params-lpddr4.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_common_message(void)
{
    struct t736_lp4_common_config config = {
        .data_rate_mts = 2400, .sequence = 0x131f,
        .dq_width = {16, 16}, .rank_mask = {3, 3}
    };
    uint8_t message[0x100], expected[0x100], before[0x100];
    const size_t mr_base[2][2] = {
        {offsetof(PMU_SMB_LPDDR4_1D_t, MR1_A0), offsetof(PMU_SMB_LPDDR4_1D_t, MR1_A1)},
        {offsetof(PMU_SMB_LPDDR4_1D_t, MR1_B0), offsetof(PMU_SMB_LPDDR4_1D_t, MR1_B1)}
    };
    unsigned int channel, rank, mr;
    memset(expected, 0xa5, sizeof(expected));
    memcpy(message, expected, sizeof(message));
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, DRAMFreq)] = 0x60;
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, DRAMFreq) + 1] = 0x09;
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, SequenceCtrl)] = 0x1f;
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, SequenceCtrl) + 1] = 0x13;
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, EnabledDQsChA)] = 16;
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, CsPresentChA)] = 3;
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, EnabledDQsChB)] = 16;
    expected[offsetof(PMU_SMB_LPDDR4_1D_t, CsPresentChB)] = 3;
    for (channel = 0; channel < 2; ++channel)
        for (rank = 0; rank < 2; ++rank)
            for (mr = 0; mr < T736_LP4_MR_COUNT; ++mr) {
                uint8_t value = (uint8_t)(1 + 30 * channel + 12 * rank + mr);
                config.mode_register[channel][rank][mr] = value;
                expected[mr_base[channel][rank] + mr] = value;
            }
    assert(t736_lp4_patch_common(message, sizeof(message), &config) == 0);
    assert(memcmp(message, expected, sizeof(message)) == 0);
    memcpy(before, message, sizeof(message));
    config.sequence |= 0x40; /* Reserved for LP4; enabled by the LP5 full mask. */
    assert(t736_lp4_patch_common(message, sizeof(message), &config) == -1);
    assert(memcmp(before, message, sizeof(message)) == 0);
    config.sequence = 0x131f;
    config.rank_mask[1] = 2;
    assert(t736_lp4_patch_common(message, sizeof(message), &config) == -1);
    assert(memcmp(before, message, sizeof(message)) == 0);
    config.rank_mask[1] = 3;
    assert(t736_lp4_patch_common(message, T736_LP4_COMMON_END - 1u, &config) == -1);
    assert(memcmp(before, message, sizeof(message)) == 0);
    assert(t736_lp4_patch_common(NULL, sizeof(message), &config) == -1);
    assert(t736_lp4_patch_common(message, sizeof(message), NULL) == -1);
    config.rank_mask[1] = 0;
    config.dq_width[1] = 0;
    assert(t736_lp4_patch_common(message, sizeof(message), &config) == 0);
    assert(message[0x57] == 0 && message[0x58] == 0);
    config.data_rate_mts = 0;
    assert(t736_lp4_patch_common(message, sizeof(message), &config) == -1);
}

struct fake_mailbox {
    const uint32_t *words;
    size_t count, position;
    unsigned int phase, calls, fail_call, stall_ready, stall_ack;
    unsigned int major_traces, header_traces, argument_traces;
};

static int fake_read(void *context, uint16_t offset, uint16_t *value)
{
    struct fake_mailbox *fake = context;
    if (++fake->calls == fake->fail_call)
        return -1;
    if (offset == T736_PMU_POLL) {
        if (fake->phase == 0) {
            *value = (uint16_t)(fake->stall_ready || fake->position >= fake->count);
            if (*value == 0)
                fake->phase = 1;
        } else {
            assert(fake->phase == 4);
            *value = (uint16_t)!fake->stall_ack;
            if (*value == 1)
                fake->phase = 5;
        }
    } else if (offset == T736_PMU_STATUS_LO) {
        assert(fake->phase == 1);
        *value = (uint16_t)fake->words[fake->position];
        fake->phase = 2;
    } else {
        assert(offset == T736_PMU_STATUS_HI && fake->phase == 2);
        *value = (uint16_t)(fake->words[fake->position] >> 16);
        fake->phase = 3;
    }
    return 0;
}

static int fake_write(void *context, uint16_t offset, uint16_t value)
{
    struct fake_mailbox *fake = context;
    if (++fake->calls == fake->fail_call)
        return -1;
    assert(offset == T736_PMU_ACK);
    if (value == 0) {
        assert(fake->phase == 2 || fake->phase == 3);
        fake->phase = 4;
    } else {
        assert(value == 1 && fake->phase == 5);
        fake->phase = 0;
        ++fake->position;
    }
    return 0;
}

static void fake_trace(void *context, enum t736_pmu_word_kind kind, uint32_t word)
{
    struct fake_mailbox *fake = context;
    assert(fake->position != 0);
    if (kind == T736_PMU_MAJOR) {
        assert((fake->words[fake->position - 1] & 0xffffu) == word);
        ++fake->major_traces;
    } else if (kind == T736_PMU_STREAM_HEADER) {
        assert(fake->words[fake->position - 1] == word);
        ++fake->header_traces;
    } else {
        assert(kind == T736_PMU_STREAM_ARGUMENT);
        assert(fake->words[fake->position - 1] == word);
        ++fake->argument_traces;
    }
}

static void test_mailbox(void)
{
    const uint32_t words[] = {1, 8, 0xabcd0003, 7, 255, 0xffff00fe, 4, 7};
    const uint32_t fatal[] = {0xabcd00ff};
    const uint32_t major_high[] = {0x10007, 7};
    const uint32_t bad_stream[] = {8, 0xabcd0021};
    struct fake_mailbox fake = {.words = words, .count = 8};
    struct t736_pmu_io io = {&fake, fake_read, fake_write, fake_trace};
    struct t736_pmu_report report;
    unsigned int fault;
    assert(t736_pmu_wait(&io, 16, 8, &report) == T736_PMU_COMPLETE);
    assert(fake.position == 8 && fake.phase == 0 && report.status_reads == 16);
    assert(report.major_count == 4 && report.stream_arguments == 3);
    assert(fake.major_traces == 4 && fake.header_traces == 1 && fake.argument_traces == 3);
    fake = (struct fake_mailbox){.words = words, .count = 8};
    assert(t736_pmu_wait(&io, 15, 8, &report) == T736_PMU_TIMEOUT);
    assert(report.status_reads == 15 && fake.position == 7 && fake.phase == 4);
    fake = (struct fake_mailbox){.words = words, .count = 8, .stall_ready = 1};
    assert(t736_pmu_wait(&io, 9, 8, &report) == T736_PMU_TIMEOUT);
    assert(report.status_reads == 9 && fake.position == 0 && fake.phase == 0);
    fake = (struct fake_mailbox){.words = words, .count = 8, .stall_ack = 1};
    assert(t736_pmu_wait(&io, 9, 8, &report) == T736_PMU_TIMEOUT);
    assert(report.status_reads == 9 && fake.position == 0 && fake.phase == 4);
    fake = (struct fake_mailbox){.words = fatal, .count = 1};
    assert(t736_pmu_wait(&io, 2, 1, &report) == T736_PMU_TRAINING_FAILED);
    assert(fake.position == 1 && fake.phase == 0);
    fake = (struct fake_mailbox){.words = major_high, .count = 2};
    assert(t736_pmu_wait(&io, 4, 2, &report) == T736_PMU_COMPLETE);
    assert(fake.position == 1 && report.last_word == 7);
    fake = (struct fake_mailbox){.words = bad_stream, .count = 2};
    assert(t736_pmu_wait(&io, 10, 10, &report) == T736_PMU_BAD_STREAM);
    assert(fake.position == 2);
    fake = (struct fake_mailbox){.words = words, .count = 8};
    assert(t736_pmu_wait(&io, 99, 1, &report) == T736_PMU_MESSAGE_LIMIT);
    assert(fake.position == 1);
    for (fault = 1; fault <= 5; ++fault) {
        fake = (struct fake_mailbox){.words = fatal, .count = 1, .fail_call = fault};
        assert(t736_pmu_wait(&io, 100, 100, &report) == T736_PMU_IO_ERROR);
    }
    assert(t736_pmu_wait(NULL, 1, 1, &report) == T736_PMU_INVALID);
    assert(t736_pmu_wait(&io, 0, 1, &report) == T736_PMU_INVALID);
    assert(t736_pmu_wait(&io, 1, 0, &report) == T736_PMU_INVALID);
    assert(t736_pmu_wait(&io, 1, 1, NULL) == T736_PMU_INVALID);
}

/* Slow exhaustive interval oracle, intentionally separate from the linear scan. */
static int reference_eye(const uint32_t *errors, size_t n, unsigned int guard,
                         unsigned int minimum, int closed, struct t736_lp4_eye *eye)
{
    size_t first, last, tap, best_width = 0, best_first = 0;
    for (first = 0; first < n; ++first)
        for (last = first; last < n; ++last) {
            size_t width = last - first + 1;
            int pass = 1;
            if ((first > 0 && errors[first - 1] == 0) ||
                (last + 1 < n && errors[last + 1] == 0))
                continue;
            if (closed && (first == 0 || last + 1 == n))
                continue;
            for (tap = first; tap <= last; ++tap)
                if (errors[tap] != 0)
                    pass = 0;
            if (!pass || width <= 2 * guard)
                continue;
            width -= 2 * guard;
            if (width < minimum || width <= best_width)
                continue;
            best_width = width;
            best_first = first + guard;
        }
    if (best_width == 0)
        return -2;
    *eye = (struct t736_lp4_eye){(uint16_t)best_first,
        (uint16_t)(best_first + best_width - 1),
        (uint16_t)(best_first + (best_width - 1) / 2), (uint16_t)best_width};
    return 0;
}

static void test_eye(void)
{
    uint32_t errors[T736_LP4_MAX_TAPS];
    unsigned int pattern, guard, minimum, closed, tap;
    struct t736_lp4_eye actual, expected;
    for (pattern = 0; pattern < 1024; ++pattern) {
        for (tap = 0; tap < 10; ++tap)
            errors[tap] = (pattern >> tap) & 1u;
        for (guard = 0; guard < 4; ++guard)
            for (minimum = 1; minimum <= 5; ++minimum)
                for (closed = 0; closed < 2; ++closed) {
                    int a, b;
                    actual = expected = (struct t736_lp4_eye){101, 102, 103, 104};
                    a = t736_lp4_eye_select(errors, 10, 1, (uint16_t)guard,
                        (uint16_t)minimum, (int)closed, &actual);
                    b = reference_eye(errors, 10, guard, minimum, (int)closed, &expected);
                    assert(a == b && memcmp(&actual, &expected, sizeof(actual)) == 0);
                }
    }
    for (tap = 0; tap < T736_LP4_MAX_TAPS; ++tap)
        errors[tap] = 2;
    assert(t736_lp4_eye_select(errors, T736_LP4_MAX_TAPS, 1, 1, 1, 0, &actual) == 0);
    assert(actual.first == 1 && actual.last == 1022 && actual.center == 511);
    assert(t736_lp4_eye_select(errors, T736_LP4_MAX_TAPS, 3, 0, 1, 0, &actual) == -2);
    assert(t736_lp4_eye_select(errors, T736_LP4_MAX_TAPS, 1, UINT16_MAX, 1, 0, &actual) == -2);
    assert(t736_lp4_eye_select(errors, T736_LP4_MAX_TAPS + 1, 1, 0, 1, 0, &actual) == -1);
    assert(t736_lp4_eye_select(errors, 1, 0, 0, 1, 0, &actual) == -1);
    assert(t736_lp4_eye_select(errors, 1, 1, 0, 0, 0, &actual) == -1);
    assert(t736_lp4_eye_select(NULL, 1, 1, 0, 1, 0, &actual) == -1);
}

int main(void)
{
    test_common_message();
    test_mailbox();
    test_eye();
    puts("PASS: LP4 common offsets, bounded mailbox, 40960 eye cases; no hardware training");
    return 0;
}
