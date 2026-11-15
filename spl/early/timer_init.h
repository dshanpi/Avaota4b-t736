/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_EARLY_TIMER_INIT_H
#define T736_EARLY_TIMER_INIT_H
#include <stdint.h>
#define T736_TIMESTAMP_CTRL UINT32_C(0x08020000)
#define T736_TIMESTAMP_FREQID UINT32_C(0x08020020)
#define T736_TIMESTAMP_HZ UINT32_C(24000000)
struct t736_timer_io {
	void *context;
	uint32_t (*read32)(void *, uint32_t);
	void (*write32)(void *, uint32_t, uint32_t);
	uint32_t (*counter)(void *);
	uint32_t (*cntfrq)(void *);
};
struct t736_timer_state {
	uint32_t ctrl_before, freqid_before, raw_cntfrq, counter_before;
	uint32_t advanced_before, ctrl_after, freqid_after, effective_hz;
};
/* T736 manual 5.1.8: +0 bit0 enables; +0x20 only records the fixed clock
 * frequency, it does not set/divide it. SyterKit ARM timer uses 24 MHz.
 * No counter-value or CNTFRQ writes. Reject conflicting nonzero metadata.
 */
int t736_timestamp_prepare(const struct t736_timer_io *, struct t736_timer_state *, uint32_t);
extern struct t736_timer_state t736_timer_observation;
#endif
