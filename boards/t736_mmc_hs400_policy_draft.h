/* SPDX-License-Identifier: GPL-2.0-only */
/* Experimental HS400 policy, separate from every frozen HS200 candidate. */
#ifndef T736_MMC_HS400_POLICY_DRAFT_H
#define T736_MMC_HS400_POLICY_DRAFT_H

struct t736_hs400_seed {
	unsigned int command, strobe;
};

/* Actual eMMC DT sm4_cmd=2020ffff/ffff0e24, sm4=1a21ffff/ffff0d11.
 * Seeds are not tuning evidence. CMD and DS need separate read verification.
 */
static inline struct t736_hs400_seed t736_hs400_seed_for_rate(u32 card_hz)
{
	struct t736_hs400_seed seed;

	if (card_hz <= 25000000) {
		seed.command = 0;
		seed.strobe = 0;
	} else if (card_hz <= 50000000) {
		seed.command = 32;
		seed.strobe = 33;
	} else if (card_hz <= 100000000) {
		seed.command = 32;
		seed.strobe = 26;
	} else if (card_hz <= 150000000) {
		seed.command = 36;
		seed.strobe = 17;
	} else {
		seed.command = 14;
		seed.strobe = 13;
	}
	return seed;
}

/* The card's HS400 bus is DDR, but the controller's generic DDR bit is NOT
 * its HS400 enable. Actual T736 sunxi_mmc_ddr_timing excludes HS400.
 * NTSR[31:mode,0:new_HS400] remain0, EDSD[31] selects dedicated HS400.
 */
#define T736_HS400_CTRL_DDR_VALUE 0
#define T736_HS400_NTSR_VALUE 0
#define T736_HS400_EDSD_VALUE BIT(31)
#define T736_HS400_CRC_VALUE 6
#define T736_HS400_DRIVE_VALUE BIT(16)
#define T736_HS400_INTERNAL_DIVIDER 0
#define T736_HS400_MODULE_PER_CARD 2

/* Core runtime retuning passes through MMC_TIMING_MMC_DDR52 with8bit bus.
 * This transition must be implemented even when DDR52 is not advertised as
 * an independently selectable operating capability. At50MHz, sm2=25.
 */
#define T736_DDR8_TRANSITION_CTRL_VALUE BIT(10)
#define T736_DDR8_TRANSITION_NTSR_VALUE 0
#define T736_DDR8_TRANSITION_EDSD_VALUE 0
#define T736_DDR8_TRANSITION_CRC_VALUE 3
#define T736_DDR8_TRANSITION_DRIVE_VALUE (BIT(16) | BIT(17))
#define T736_DDR8_TRANSITION_INTERNAL_DIVIDER 1
#define T736_DDR8_TRANSITION_MODULE_PER_CARD 4
#define T736_DDR8_TRANSITION_SAMPLE_50M 25

static inline int t736_plan_ddr8_transition_clock(u32 target_hz, u32 sys24_hz,
						 u32 peri800_hz, struct t736_clock_plan *plan)
{
	int ret;

	if (target_hz < 400000 || target_hz > 50000000)
		return -EINVAL;
	ret = t736_plan_sdr_clock(target_hz * 2, sys24_hz, peri800_hz, plan);
	if (!ret)
		plan->card_hz /= 2;
	return ret;
}

#endif
