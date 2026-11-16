/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_MMC_TUNING_DRAFT_H
#define T736_MMC_TUNING_DRAFT_H

#define T736_TUNING_TAPS 64
/* Match the actual T736 Tina tuning policy, with explicit failure reporting. */
#define T736_TUNING_MIN_WINDOW 12
#define T736_TUNING_TRIALS 20

struct t736_tuning_window {
	unsigned int first, count, tap;
};

/* SAMP_DL is a physical delay chain. There is no evidence that tap63 wraps
 * continuously to tap0; do not join the two end windows into a false eye.
 */
static inline int t736_tuning_select(u64 passing, struct t736_tuning_window *w)
{
	unsigned int first = 0, count = 0, i;

	w->first = w->count = w->tap = 0;
	for (i = 0; i <= T736_TUNING_TAPS; i++) {
		if (i < T736_TUNING_TAPS && (passing & (1ULL << i))) {
			if (!count)
				first = i;
			count++;
		} else {
			if (count > w->count) {
				w->first = first;
				w->count = count;
			}
			count = 0;
		}
	}
	if (w->count < T736_TUNING_MIN_WINDOW)
		return -EIO;
	w->tap = w->first + (w->count - 1) / 2;
	return 0;
}

/* Actual board sdc_tm4_sm3_freq0=2020ffff/freq1=ffff3128. These are
 * initialization seeds only; a seed never substitutes for CMD21 validation.
 */
static inline unsigned int t736_hs200_seed(u32 card_hz)
{
	if (card_hz <= 25000000)
		return 0;
	if (card_hz <= 100000000)
		return 32;
	return card_hz <= 150000000 ? 40 : 49;
}

#endif
