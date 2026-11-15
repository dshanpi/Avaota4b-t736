/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_PHY_CALIBRATION_DIAG_H
#define T736_PHY_CALIBRATION_DIAG_H

#include "phy/host.h"

/* Experimental local CSR mapping; see pmu/phy-calibration-audit.json.
 * These are absolute byte addresses, accessed as ordered halfwords.
 */
#define T736_ZCAL_RESET UINT32_C(0x0a940620)
#define T736_ZCAL_RUN UINT32_C(0x0a940622)
#define T736_ZCAL_PU UINT32_C(0x0a94064c)
#define T736_ZCAL_PD UINT32_C(0x0a94064e)
#define T736_ZCAL_CLOCKS UINT32_C(0x0aa80100)
#define T736_ZCAL_MIN_WAIT_US 35u
#define T736_ZCAL_MAX_WAIT_US 10000u

enum t736_zcal_result {
	/* Positive: only callback sequence and timed interval completed.
	 * No value of this API means calibration succeeded or DRAM is usable.
	 */
	T736_ZCAL_OBSERVED_ONLY = 1,
	T736_ZCAL_TIMED_ONLY = 2,
	T736_ZCAL_INVALID = -1,
	T736_ZCAL_IO = -2,
	T736_ZCAL_PRECONDITION = -3,
	T736_ZCAL_TIMER = -4
};

struct t736_zcal_snapshot {
	uint16_t reset;
	uint16_t run;
	uint16_t pu;
	uint16_t pd;
};

struct t736_zcal_report {
	struct t736_zcal_snapshot before;
	struct t736_zcal_snapshot after;
	uint32_t waited_us;
	uint32_t last_write_address;
	uint16_t last_write_value;
	uint8_t writes_attempted;
	uint8_t writes_completed;
	uint8_t before_valid;
	uint8_t after_valid;
};

/* Caller acknowledgements, not hardware proofs. Require each exactly 1.
 * This makes experimental bring-up distinct from an ordinary runtime API.
 */
struct t736_zcal_experiment {
	uint8_t sram_exclusive_cold_context;
	uint8_t clocks_and_analog_setup_reviewed;
	uint8_t candidate_mapping_reviewed;
};

/* Read-only observation. Requires MuxSel=0 and PMU reset+stall asserted;
 * refuses before internal CSR reads otherwise. Output unchanged on failure.
 * Caller must already have a clocked, accessible PHY domain and run from SRAM
 * with DMA/interrupts/other masters quiesced. The function does not claim APB.
 * Raw code fields are diagnostic: bits15:8 are NXP's PU/PD codes, bit0 is its
 * override enable; target validity/freshness/error semantics are unconfirmed.
 */
int t736_zcal_observe(const struct t736_phy_io *io,
		     struct t736_zcal_snapshot *snapshot);

/* Experimental bounded sequence, not a training-success API.
 * Requires caller-reviewed cold-state analog and DFI clock prerequisites.
 * Additionally checks host ownership, PMU reset+stall, Hclk/Ucclk enabled,
 * reset in {0,1}, run=0 and PU/PD override bit0 clear before any write.
 * Does not configure voltage, impedance range, PState, DFI tick count, clock
 * gates, unknown companion controls, code override, or result-latch strobes.
 * Writes only reset1, reset0, run1, run0 with 1us reset and wait_us run time.
 * The 1us hold is a local minimum-hold diagnostic policy, not a specified
 * hardware reset pulse width. run0 comes from public ARC host-interface use,
 * not an inferred cleanup; no cleanup write is performed after failure.
 * NXP's 35us retention wait supplies a diagnostic lower bound; a T736 cold
 * calibration worst-case completion time has NOT been established.
 *
 * Callbacks must terminate and delay_us must confirm elapsed time. A bus hang
 * is not bounded by this function. Failure aborts immediately without retry or
 * speculative cleanup. Even a failed callback may have changed hardware;
 * inspect writes_attempted and power-cycle before another experiment.
 * On valid invocation report is progressively updated, including on failure.
 * Neither changed nor unchanged PU/PD values prove calibration success.
 */
int t736_zcal_timed_experiment(const struct t736_phy_io *io,
			     const struct t736_zcal_experiment *experiment,
			     uint32_t wait_us, struct t736_zcal_report *report);

#endif
