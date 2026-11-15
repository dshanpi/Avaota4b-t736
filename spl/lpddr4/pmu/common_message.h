/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef T736_LP4_COMMON_MESSAGE_H
#define T736_LP4_COMMON_MESSAGE_H

#include <stddef.h>
#include <stdint.h>

/* Only fields corroborated for T736 are exposed. This is NOT a complete DMEM
 * image: caller must supply an independently implemented firmware's buffer.
 * In particular, offsets 0x08..0x0f and the extended block are not described by
 * the older public LPDDR4 header, and must not be guessed from LPDDR5.
 */
#define T736_LP4_COMMON_END 0x7du
#define T736_LP4_MR_COUNT 12u
#define T736_LP4_SEQ_DEVICE_INIT (1u << 0)
#define T736_LP4_SEQ_WRITE_LEVEL (1u << 1)
#define T736_LP4_SEQ_READ_GATE (1u << 2)
#define T736_LP4_SEQ_READ_DQS (1u << 3)
#define T736_LP4_SEQ_WRITE_DQ (1u << 4)
#define T736_LP4_SEQ_READ_DESKEW (1u << 8)
#define T736_LP4_SEQ_MAX_READ_LATENCY (1u << 9)
#define T736_LP4_SEQ_CA (1u << 12)
#define T736_LP4_SEQ_VALID 0x131fu

struct t736_lp4_common_config {
    uint16_t data_rate_mts;
    uint16_t sequence;
    uint8_t dq_width[2];
    uint8_t rank_mask[2];
    /* MR order: 1, 2, 3, 4, 11, 12, 13, 14, 16, 17, 22, 24. */
    uint8_t mode_register[2][2][T736_LP4_MR_COUNT];
};

/* Returns 0 or -1 (invalid arguments). No byte changes on validation failure.
 * Writes common fields only; caller owns validation of MR electrical settings.
 * This performs no MMIO and does not assert compatibility of a full firmware.
 */
int t736_lp4_patch_common(uint8_t *message, size_t length,
                         const struct t736_lp4_common_config *config);

#endif
