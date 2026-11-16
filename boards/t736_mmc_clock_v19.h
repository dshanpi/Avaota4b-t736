/* SPDX-License-Identifier: GPL-2.0-only */
/* A733 SMHC3 CCU: linear M and N, each 1..32; SDR card clock = module/2. */
#ifndef T736_MMC_CLOCK_V19_H
#define T736_MMC_CLOCK_V19_H

struct t736_clock_plan {
	u32 reg, card_hz, parent_hz;
	unsigned int m, n, source;
};

static inline u32 t736_peri1_800_hz(u32 reference, u32 peripheral, u32 dcxo_hz)
{
	u32 ref_ready = BIT(31) | BIT(28) | BIT(27);
	u32 peri_ready = BIT(31) | BIT(28) | BIT(26);
	u64 hz;

	if ((reference & ref_ready) != ref_ready ||
	    (peripheral & peri_ready) != peri_ready || dcxo_hz != 26000000)
		return 0;
	hz = (u64)dcxo_hz * (((reference >> 8) & 0xff) + 1);
	hz = div_u64(hz, (((reference >> 1) & 1) + 1) *
		     (((reference >> 16) & 0x7f) + 1));
	if (hz != 24000000)
		return 0;
	hz *= ((peripheral >> 8) & 0xff) + 1;
	hz = div_u64(hz, (((peripheral >> 1) & 1) + 1) *
		     (((peripheral >> 16) & 7) + 1));
	return hz == 800000000 ? hz : 0;
}

static inline int t736_plan_sdr_clock(u32 target_hz, u32 peri800_hz,
				     struct t736_clock_plan *plan)
{
	u32 parents[2] = { 24000000, peri800_hz };
	unsigned int s, m, n;

	if (target_hz < 400000 || target_hz > 50000000)
		return -EINVAL;
	plan->card_hz = 0;
	for (s = 0; s < 2; s++) {
		if (!parents[s])
			continue;
		for (n = 1; n <= 32; n++)
			for (m = 1; m <= 32; m++) {
				u32 hz = parents[s] / (2 * m * n);

				if (hz > target_hz || hz <= plan->card_hz)
					continue;
				plan->card_hz = hz;
				plan->parent_hz = parents[s];
				plan->m = m;
				plan->n = n;
				plan->source = s ? 3 : 0;
				plan->reg = BIT(31) | (plan->source << 24) |
					    ((n - 1) << 8) | (m - 1);
			}
	}
	return plan->card_hz ? 0 : -ERANGE;
}

#endif
