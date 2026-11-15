/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mailbox.h"

static enum t736_pmu_result poll(const struct t736_pmu_io *io,
                                 uint16_t expected, uint32_t *remaining,
                                 struct t736_pmu_report *report)
{
    while (*remaining != 0u) {
        uint16_t value;
        --*remaining;
        ++report->status_reads;
        if (io->read16(io->context, T736_PMU_POLL, &value) != 0)
            return T736_PMU_IO_ERROR;
        if ((value & 1u) == expected)
            return T736_PMU_COMPLETE;
    }
    return T736_PMU_TIMEOUT;
}

static enum t736_pmu_result receive(const struct t736_pmu_io *io,
                                    uint32_t *remaining,
                                    struct t736_pmu_report *report,
                                    enum t736_pmu_word_kind kind)
{
    uint16_t low, high = 0;
    enum t736_pmu_result result = poll(io, 0u, remaining, report);
    if (result != T736_PMU_COMPLETE)
        return result;
    if (io->read16(io->context, T736_PMU_STATUS_LO, &low) != 0)
        return T736_PMU_IO_ERROR;
    /* Major IDs are 16 bits; only stream payloads own the high mailbox.
     * Never let a stale streaming high halfword mask completion or failure.
     */
    if (kind != T736_PMU_MAJOR &&
        io->read16(io->context, T736_PMU_STATUS_HI, &high) != 0)
        return T736_PMU_IO_ERROR;
    report->last_word = (uint32_t)low | ((uint32_t)high << 16);
    if (io->write16(io->context, T736_PMU_ACK, 0u) != 0)
        return T736_PMU_IO_ERROR;
    result = poll(io, 1u, remaining, report);
    if (result != T736_PMU_COMPLETE)
        return result;
    if (io->write16(io->context, T736_PMU_ACK, 1u) != 0)
        return T736_PMU_IO_ERROR;
    if (io->trace)
        io->trace(io->context, kind, report->last_word);
    return T736_PMU_COMPLETE;
}

enum t736_pmu_result t736_pmu_wait(const struct t736_pmu_io *io,
                                  uint32_t status_read_budget,
                                  uint32_t major_limit,
                                  struct t736_pmu_report *report)
{
    enum t736_pmu_result result;
    if (!io || !io->read16 || !io->write16 || !report ||
        status_read_budget == 0u || major_limit == 0u)
        return T736_PMU_INVALID;
    report->last_word = 0;
    report->major_count = 0;
    report->stream_arguments = 0;
    report->status_reads = 0;
    while (report->major_count < major_limit) {
        uint32_t count, argument;
        result = receive(io, &status_read_budget, report, T736_PMU_MAJOR);
        if (result != T736_PMU_COMPLETE)
            return result;
        ++report->major_count;
        if (report->last_word == T736_PMU_SUCCESS)
            return T736_PMU_COMPLETE;
        if (report->last_word == T736_PMU_FATAL)
            return T736_PMU_TRAINING_FAILED;
        if (report->last_word != T736_PMU_STREAM)
            continue;
        result = receive(io, &status_read_budget, report, T736_PMU_STREAM_HEADER);
        if (result != T736_PMU_COMPLETE)
            return result;
        count = report->last_word & 0xffffu;
        if (count > T736_PMU_MAX_STREAM_ARGS)
            return T736_PMU_BAD_STREAM;
        for (argument = 0; argument < count; ++argument) {
            result = receive(io, &status_read_budget, report,
                             T736_PMU_STREAM_ARGUMENT);
            if (result != T736_PMU_COMPLETE)
                return result;
            ++report->stream_arguments;
        }
    }
    return T736_PMU_MESSAGE_LIMIT;
}
