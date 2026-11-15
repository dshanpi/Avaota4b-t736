/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_MODE_REGISTER_H
#define T736_LP4_MODE_REGISTER_H

#include <stdint.h>

/* Register facts: see pmu/mode-register-audit.json. */
#define T736_LP4_MRR_CTRL0 0x080u
#define T736_LP4_MRR_CTRL1 0x084u
#define T736_LP4_MRR_STAT 0x090u
#define T736_LP4_MRR_RESULT_CH0 UINT32_C(0x0a02002c)
#define T736_LP4_MRR_RESULT_CH1 UINT32_C(0x0a020040)
#define T736_LP4_MRR_MAX_WAIT_US 10000u

enum t736_lp4_mrr_error {
	T736_LP4_MRR_OK = 0,
	T736_LP4_MRR_INVALID = -1,
	T736_LP4_MRR_UNSUPPORTED = -2,
	T736_LP4_MRR_BUSY = -3,
	T736_LP4_MRR_TIMEOUT = -4,
	T736_LP4_MRR_VERIFY = -5,
	T736_LP4_MRR_IO = -6,
};

/* Callbacks return zero on success. Accesses must be ordered device MMIO. */
struct t736_lp4_mrr_io {
	void *ctx;
	int (*read32)(void *ctx, uint32_t address, uint32_t *value);
	int (*write32)(void *ctx, uint32_t address, uint32_t value);
	int (*delay_us)(void *ctx, uint32_t delay);
};

struct t736_lp4_mrr_result {
	uint32_t raw;
	uint8_t value;
};

/*
 * Diagnostic MRR only: MR5 (manufacturer), MR6 (revision), MR8 (geometry).
 * LP4, two-channel board/type8 receive-byte wiring only. No MRW interface.
 *
 * Requires initialized, trained DRAM and exclusive ownership of the software
 * MR interface (including firmware/DVFS). Checks STAT normal and idle before
 * writing; it does not disable normal memory traffic or change PHY mux/MR13.
 * The caller must prevent controller reconfiguration during this transaction.
 *
 * Bounded completion observes MRCTRL0 trigger clear and MRSTAT busy clear.
 * T736 has no independently identified receive-valid bit: the returned latch
 * is diagnostic evidence, not proof of a fresh or error-free DRAM response.
 * Cross-check repeated MR5/6/8 reads and raw lane bytes before trusting IDs.
 *
 * On failure the output is unchanged; no reset, retry, or speculative cleanup
 * is attempted. A timeout after triggering may leave a transaction in flight.
 */
int t736_lp4_mrr_read_id(const struct t736_lp4_mrr_io *io,
		       unsigned int channel, unsigned int rank,
		       unsigned int mr, uint32_t timeout_us,
		       struct t736_lp4_mrr_result *result);

#endif
