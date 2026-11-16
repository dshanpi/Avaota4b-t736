/* SPDX-License-Identifier: GPL-2.0-only */
/* A733 SMHC3 CCU: linear M and N, each 1..32; SDR card clock = module/2. */
#ifndef T736_MMC_CLOCK_HS_DRAFT_H
#define T736_MMC_CLOCK_HS_DRAFT_H

struct t736_clock_plan {
	u32 reg, card_hz, parent_hz;
	unsigned int m, n, source;
};

static inline u32 t736_ref24_hz(u32 reference, u32 dcxo_hz)
{
	u32 ref_ready = BIT(31) | BIT(28) | BIT(27);
	u32 denominator;

	if ((reference & ref_ready) != ref_ready || dcxo_hz != 26000000)
		return 0;
	denominator = (((reference >> 1) & 1) + 1) *
		      (((reference >> 16) & 0x7f) + 1);
	/* Exact rational comparison: do not round an unverified parent to 24M. */
	return (u64)dcxo_hz * (((reference >> 8) & 0xff) + 1) ==
	       (u64)24000000 * denominator ? 24000000 : 0;
}

static inline u32 t736_peri0_800_hz(u32 reference, u32 peripheral,
				   u32 pattern0, u32 pattern1, u32 dcxo_hz)
{
	u32 peri_ready = BIT(31) | BIT(28) | BIT(26);
	u32 ref_hz = t736_ref24_hz(reference, dcxo_hz);
	u32 denominator;

	/* Donor clock_autogen.h: FREF_SEL[24]=1 selects REFPLL, 0 selects
	 * DCXO. Reject direct 26M and any active/transitional SDM pattern.
	 * These are read-only checks; never retune a shared peripheral PLL.
	 */
	if (!ref_hz || (peripheral & peri_ready) != peri_ready ||
	    !(peripheral & BIT(24)) || (pattern0 & BIT(31)) ||
	    (pattern1 & (BIT(27) | BIT(26))))
		return 0;
	denominator = (((peripheral >> 1) & 1) + 1) *
		      (((peripheral >> 16) & 7) + 1);
	return (u64)ref_hz * (((peripheral >> 8) & 0xff) + 1) ==
	       (u64)800000000 * denominator ? 800000000 : 0;
}

static inline int t736_plan_sdr_clock(u32 target_hz, u32 sys24_hz, u32 peri800_hz,
				     struct t736_clock_plan *plan)
{
	u32 parents[2] = { sys24_hz, peri800_hz };
	unsigned int s, m, n;

	if (target_hz < 400000 || target_hz > 50000000 ||
	    sys24_hz != 24000000 || (peri800_hz && peri800_hz != 800000000))
		return -EINVAL;
	plan->card_hz = 0;
	for (s = 0; s < 2; s++) {
		if (!parents[s])
			continue;
		for (n = 1; n <= 32; n++)
			for (m = 1; m <= 32; m++) {
				u32 hz = parents[s] / (2 * m * n);

				if ((u64)parents[s] > (u64)target_hz * 2 * m * n ||
				    hz <= plan->card_hz)
					continue;
				plan->card_hz = hz;
				plan->parent_hz = parents[s];
				plan->m = m;
				plan->n = n;
				plan->source = s ? 1 : 0;
				plan->reg = BIT(31) | (plan->source << 24) |
					    ((n - 1) << 8) | (m - 1);
			}
	}
	return plan->card_hz ? 0 : -ERANGE;
}

#endif
