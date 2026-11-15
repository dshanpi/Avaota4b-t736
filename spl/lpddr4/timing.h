/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_TIMING_H
#define T736_LP4_TIMING_H

#include "controller.h"

/* This block is separate from CTL0, unlike the older H616/A523 layout. */
#define T736_TIMING0_BASE UINT32_C(0x0a100000)
#define T736_CTL_DFIMISC 0x510u
#define T736_CTL_DFISTAT 0x514u
#define T736_CTL_PWRCTL 0x180u
#define T736_LP4_TIMING_WORDS 21u

struct t736_lp4_timing_word {
	uint32_t offset;
	uint32_t mask;
	uint32_t value;
};

struct t736_lp4_timing {
	uint32_t ck_mhz;
	uint32_t count;
	struct t736_lp4_timing_word words[T736_LP4_TIMING_WORDS];
};

/* ceil(ns * MHz / 1000), followed by the specified minimum CK cycles.
 * On invalid input/overflow, output is unchanged. No MMIO is performed.
 */
int t736_lp4_ns_to_ck(uint32_t nanoseconds, uint32_t ck_mhz,
		      uint32_t minimum_ck, uint32_t *cycles);

/* Candidate high-speed LPDDR4 profile for the observed 1800 MHz board.
 * The fixed RL/WL/DFI pipeline cannot be scaled by changing the clock alone.
 * Other frequencies and low-speed 1:2 mode are deliberately unsupported.
 * This is not a complete controller or PHY initializer. No MMIO is performed.
 */
int t736_lp4_build_timing(uint32_t ck_mhz, struct t736_lp4_timing *timing);

/* SRAM-only, clocks stable, clients stopped, both controllers in init mode.
 * IO callbacks must implement ordered device MMIO accesses.
 * Programs only the generated timing subset. Does not program mode registers,
 * reset/enable clocks, train the PHY, or declare any DRAM capacity usable.
 */
int t736_lp4_apply_timing(const struct t736_lp4_io *io, uint32_t ck_mhz);

/* SRAM-only post-training controller handoff. Both channels must still be
 * in init mode, match the observed LP4 geometry, and have PWRCTL bit 11
 * asserted. Caller must already have completed source-built PHY training and
 * released PRCM memory isolation; this function does neither. Every wait is
 * bounded. On failure the caller must abort initialization, not access DRAM.
 * Success establishes controller normal mode only, not tested memory.
 */
int t736_lp4_dfi_handoff(const struct t736_lp4_io *io, uint32_t timeout_us);

#endif
