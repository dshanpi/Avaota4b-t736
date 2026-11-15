// SPDX-License-Identifier: GPL-2.0-only
#include "platform.h"

struct t736_timer_state t736_timer_observation;

uint32_t t736_counter_frequency(void)
{
	uint32_t value;
	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(value));
	return value;
}

uint32_t t736_counter_low(void)
{
	uint32_t lo, hi;
	__asm__ volatile("isb\n\tmrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi) :: "memory");
	return lo;
}

static uint32_t timestamp_read(void *context, uint32_t address)
{
	(void)context;
	return t736_read32(address);
}

static void timestamp_write(void *context, uint32_t address, uint32_t value)
{
	(void)context;
	t736_write32(address, value);
}

static uint32_t timestamp_counter(void *context)
{
	(void)context;
	return t736_counter_low();
}

static uint32_t timestamp_cntfrq(void *context)
{
	(void)context;
	return t736_counter_frequency();
}

int t736_timer_init(void)
{
	const struct t736_timer_io io = {0, timestamp_read, timestamp_write,
		timestamp_counter, timestamp_cntfrq};
	return t736_timestamp_prepare(&io, &t736_timer_observation, T736_POLL_LIMIT);
}

int t736_delay_us(uint32_t usec)
{
	uint32_t begin, i;
	if (t736_timer_observation.effective_hz != T736_COUNTER_HZ || usec > 0x7fffffffu / 24u)
		return -1;
	begin = t736_counter_low();
	for (i = 0; i < T736_POLL_LIMIT; i++)
		if ((uint32_t)(t736_counter_low() - begin) >= usec * 24u)
			return 0;
	return -1;
}

int t736_wait32(uintptr_t address, uint32_t mask, uint32_t value, uint32_t usec)
{
	uint32_t begin, i;
	if (t736_timer_observation.effective_hz != T736_COUNTER_HZ || usec > 0x7fffffffu / 24u || (value & ~mask))
		return -1;
	begin = t736_counter_low();
	for (i = 0; i < T736_POLL_LIMIT; i++) {
		if ((t736_read32(address) & mask) == value)
			return 0;
		if ((uint32_t)(t736_counter_low() - begin) >= usec * 24u)
			break;
	}
	return -1;
}
