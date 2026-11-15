/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_EARLY_PLATFORM_H
#define T736_EARLY_PLATFORM_H
#include <stdint.h>
#include "timer_init.h"
#define T736_COUNTER_HZ 24000000u
#define T736_POLL_LIMIT 1000000u

static inline uint32_t t736_read32(uintptr_t address)
{
	uint32_t value = *(volatile uint32_t *)address;
	__asm__ volatile("dmb sy" ::: "memory");
	return value;
}
static inline void t736_write32(uintptr_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline void t736_clrset32(uintptr_t address, uint32_t mask, uint32_t value)
{
	t736_write32(address, (t736_read32(address) & ~mask) | value);
}
uint32_t t736_counter_frequency(void);
uint32_t t736_counter_low(void);
/* Validate/init T736 fixed-rate timestamp metadata/EN, then require CNTPCT advance.
 * CNTFRQ may be zero in BootROM; this does not write the CP15 CNTFRQ register. */
int t736_timer_init(void);
/* All waits also terminate if the physical counter stalls. */
int t736_delay_us(uint32_t usec);
int t736_wait32(uintptr_t address, uint32_t mask, uint32_t value, uint32_t usec);
#endif
