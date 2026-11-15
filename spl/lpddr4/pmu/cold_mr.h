/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_COLD_MR_H
#define T736_LP4_COLD_MR_H

#include "../phy/host.h"

/* Candidate same-generation interface. See cold-mr-interface-audit.json. */
#define T736_COLD_MR_MAX_WAIT_US 10000u
enum t736_cold_mr_status {
	T736_COLD_MR_ARMED_ONLY = 1,
	T736_COLD_MR_ISSUED_ONLY = 2,
	T736_COLD_MR_RECEIVED_CANDIDATE = 3,
	T736_COLD_MR_INVALID = -1,
	T736_COLD_MR_IO = -2,
	T736_COLD_MR_PRECONDITION = -3,
	T736_COLD_MR_VERIFY = -4,
	T736_COLD_MR_TIMEOUT = -5,
	T736_COLD_MR_TIMER = -6
};

enum t736_cold_mr_stage {
	T736_COLD_MR_CHECK = 1,
	T736_COLD_MR_ARM,
	T736_COLD_MR_CLEAR_DONE,
	T736_COLD_MR_SELECT,
	T736_COLD_MR_TRIGGER,
	T736_COLD_MR_WAIT,
	T736_COLD_MR_DATA,
	T736_COLD_MR_FINISHED
};

struct t736_cold_mr_context {
	/* Each acknowledgement must equal one. These do not prove pin state. */
	uint8_t sram_exclusive_cold_context;
	uint8_t same_generation_mapping_reviewed;
	/* Required only for a command: reset/CKE/clock/tINIT/DFI setup and MR
	 * selection/data must have been independently reviewed by the caller.
	 */
	uint8_t command_path_and_timing_ready;
	/* Frozen experimental precondition: exactly 1 (stall) or 9 (reset and
	 * stall). Never permits 0; no ARC execution is started by this API.
	 */
	uint8_t expected_micro_reset;
};

struct t736_cold_mr_request {
	unsigned int channel;
	unsigned int rank;
	unsigned int mr;
	unsigned int data;
	unsigned int read;
	uint32_t timeout_us;
};

struct t736_cold_mr_report {
	uint32_t stage;
	uint32_t last_address;
	uint32_t last_value;
	uint32_t last_ctrl0;
	uint32_t last_mrstat;
	uint32_t mrr_data[2];
	uint32_t common_latch;
	uint32_t waited_us;
	uint32_t writes_attempted;
	uint32_t writes_completed;
	uint8_t last_width;
	uint8_t last_is_write;
	uint8_t channels_armed;
	uint8_t done_was_cleared;
	uint8_t fresh_done_observed;
	uint8_t data_valid;
};

/* Two 32-bit writes, setting sw_init_int on BOTH controllers. Does not
 * release memory reset or trigger MR/DFI; leaves sw_init_int set. Checks the
 * observed held state on both channels before the first write. Caller must
 * already have the domain accessible. No PHY write callback is used.
 */
int t736_cold_mr_arm(const struct t736_phy_io *io,
		    const struct t736_cold_mr_context *context,
		    struct t736_cold_mr_report *report);

/* One explicit MRW/MRR while software initialization is armed on both
 * channels and both controllers remain INIT. Does not establish reset/CKE,
 * init timings, DFI/PHY readiness, or a trained command/data path.
 * MRW completion means issued only, never device acknowledgement.
 * MRR requires done=0 after explicit clear and then done=1 after triggering;
 * its raw controller/common latches remain experimental T736 observations.
 * Does not infer lane mapping or compare bytes. No returned status proves
 * DDR trained or memory usable. Preserves sw_init_int even on success.
 *
 * Ordered callbacks must terminate; bus hangs are not bounded here.
 * delay_us must verify elapsed time. Report advances on valid invocation,
 * including failure; failed writes may already have taken effect. Aborts
 * without cleanup/retry. Recovery and clearing sw_init_int are external.
 */
int t736_cold_mr_command(const struct t736_phy_io *io,
			const struct t736_cold_mr_context *context,
			const struct t736_cold_mr_request *request,
			struct t736_cold_mr_report *report);

#endif
