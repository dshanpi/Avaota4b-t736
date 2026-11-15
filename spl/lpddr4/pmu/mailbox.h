/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef T736_LP4_MAILBOX_H
#define T736_LP4_MAILBOX_H

#include <stdint.h>

#define T736_PMU_POLL 0x0008u
#define T736_PMU_ACK 0x0062u
#define T736_PMU_STATUS_LO 0x0064u
#define T736_PMU_STATUS_HI 0x0068u
#define T736_PMU_SUCCESS 0x07u
#define T736_PMU_STREAM 0x08u
#define T736_PMU_FATAL 0xffu
#define T736_PMU_MAX_STREAM_ARGS 32u

enum t736_pmu_result {
    T736_PMU_COMPLETE = 0,
    T736_PMU_INVALID = -1,
    T736_PMU_IO_ERROR = -2,
    T736_PMU_TIMEOUT = -3,
    T736_PMU_TRAINING_FAILED = -4,
    T736_PMU_BAD_STREAM = -5,
    T736_PMU_MESSAGE_LIMIT = -6
};

enum t736_pmu_word_kind {
    T736_PMU_MAJOR,
    T736_PMU_STREAM_HEADER,
    T736_PMU_STREAM_ARGUMENT
};

struct t736_pmu_io {
    void *context;
    /* Offsets are bytes relative to the PHY control APB at 0x0aaa0000.
     * Implement with ordered 16-bit MMIO, returning zero on success.
     * The callbacks themselves must terminate; budget bounds status reads.
     */
    int (*read16)(void *context, uint16_t offset, uint16_t *value);
    int (*write16)(void *context, uint16_t offset, uint16_t value);
    void (*trace)(void *context, enum t736_pmu_word_kind kind, uint32_t word);
};

struct t736_pmu_report {
    uint32_t last_word;
    uint32_t major_count;
    uint32_t stream_arguments;
    uint32_t status_reads;
};

/* Receive already-running firmware's messages. Does not start or reset PMU.
 * Both ready and acknowledgement polls share one finite budget. Values in
 * streaming data are never interpreted as completion/fatal major messages.
 * Major IDs use only STATUS_LO; a stale STATUS_HI is ignored for major IDs.
 * A failed handshake is not recoverable here: caller must stop/reset PMU.
 * Return COMPLETE only reports firmware's claim; memory tests remain required.
 */
enum t736_pmu_result t736_pmu_wait(const struct t736_pmu_io *io,
                                  uint32_t status_read_budget,
                                  uint32_t major_limit,
                                  struct t736_pmu_report *report);

#endif
