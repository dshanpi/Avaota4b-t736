// SPDX-License-Identifier: GPL-2.0-only
/* SRAM-only CCU diagnostic. Source-owned callbacks deny every non-CCU access. */
#include "../early/platform.h"
#include "../lpddr4/phy/clocks.h"

struct report {
	uint32_t magic, version, status, sctlr;
	uint32_t counter_hz, timer_delta, completed_stage, result;
	uint32_t before[6], after[6], writes[3], denied_address;
	uint32_t before_valid, after_valid, raw_cntfrq, timestamp_ctrl, timestamp_freqid;
	uint32_t reserved[3];
};
_Static_assert(sizeof(struct report) == 128, "CCU report ABI");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t addresses[6] = {
	T736_PLL_DDR, T736_DRAM0_CLK, T736_DRAM0_BGR,
	T736_NSI_CLK, T736_NSI_BGR, T736_MBUS_CLK
};

static int ccu_read(void *context, uint32_t address, uint32_t *value)
{
	uint32_t i;
	(void)context;
	for (i = 0; i < 6u; i++)
		if (address == addresses[i]) {
			*value = t736_read32(address);
			return 0;
		}
	probe_report.denied_address = address;
	return -1;
}

static int ccu_write(void *context, uint32_t address, uint32_t value)
{
	uint32_t i;
	(void)context;
	/* Only DDRPLL and DRAM0 clock/reset, never any NSI/MBUS register. */
	for (i = 0; i < 3u; i++)
		if (address == addresses[i] && probe_report.writes[i] < 32u) {
			probe_report.writes[i]++;
			t736_write32(address, value);
			return 0;
		}
	probe_report.denied_address = address;
	return -1;
}

static int delay(void *context, uint32_t us)
{
	(void)context;
	return t736_delay_us(us);
}

static int snapshot(volatile uint32_t values[6], volatile uint32_t *valid)
{
	uint32_t i, value;
	for (i = 0; i < 6u; i++) {
		if (ccu_read(0, addresses[i], &value))
			return -1;
		values[i] = value;
		*valid |= 1u << i;
	}
	return 0;
}

int probe_main(void)
{
	const struct t736_phy_io io = {
		.context = 0, .read16 = 0, .write16 = 0,
		.read32 = ccu_read, .write32 = ccu_write, .delay_us = delay
	};
	struct t736_phy_ccu_report ccu;
	volatile uint32_t *words = (volatile uint32_t *)&probe_report;
	uint32_t i, begin;
	int rc;
	for (i = 0; i < 32u; i++) words[i] = 0;
	probe_report.magic = 0x54374343; /* T7CC */
	probe_report.version = 1;
	probe_report.status = 1;
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(i));
	probe_report.sctlr = i;
	rc = t736_timer_init();
	probe_report.counter_hz = t736_timer_observation.effective_hz;
	probe_report.raw_cntfrq = t736_timer_observation.raw_cntfrq;
	probe_report.timestamp_ctrl = t736_timer_observation.ctrl_after;
	probe_report.timestamp_freqid = t736_timer_observation.freqid_after;
	if (rc) {
		probe_report.status = 0x101;
		probe_report.result = (uint32_t)rc;
		return rc;
	}
	begin = t736_counter_low();
	rc = t736_delay_us(10);
	probe_report.timer_delta = t736_counter_low() - begin;
	if (rc) {
		probe_report.status = 0x102;
		probe_report.result = (uint32_t)rc;
		return rc;
	}
	if (snapshot(probe_report.before, &probe_report.before_valid)) {
		probe_report.status = 0x202;
		return -1;
	}
	rc = t736_phy_ccu_cold_start(&io, 100000u, &ccu);
	probe_report.completed_stage = (uint32_t)ccu.completed_stage;
	probe_report.result = (uint32_t)rc;
	probe_report.status = rc ? 0x201 : 2;
	if (snapshot(probe_report.after, &probe_report.after_valid))
		probe_report.status = 0x202;
	__asm__ volatile("dsb sy" ::: "memory");
	return rc;
}
