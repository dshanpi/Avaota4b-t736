// SPDX-License-Identifier: GPL-2.0-only
/* Fixed common-domain write observation. Never accesses controller/PHY/DDR. */
#include "../early/platform.h"
#include "../lpddr4/domain_prepare_diag.h"

struct report {
	uint32_t magic, version, status, sctlr;
	uint32_t effective_hz, raw_cntfrq, pretest_ticks, pretest_result;
	uint32_t phase, component_result, ccu[3], before[3], after[3], writes[3];
	uint32_t denied_address, validity_counts, waited_us, last_write_address, last_write_value;
	uint32_t component_delay_ticks, component_delay_result, component_delay_calls;
	uint32_t timestamp_ctrl, timestamp_freqid;
};
_Static_assert(sizeof(struct report) == 128, "Domain setup report ABI");
volatile struct report probe_report __attribute__((section(".report")));
static const uint32_t ccu_addresses[3] = {T736_PLL_DDR, T736_DRAM0_CLK, T736_DRAM0_BGR};
static const uint32_t ccu_expected[3] = {0xf9004a00u, 0x80000003u, 0x10001u};
static const uint32_t addresses[3] = {T736_DOMAIN_SYS, T736_DOMAIN_CHANNEL_TYPE, T736_DOMAIN_ISOLATION};
static const uint32_t targets[3] = {0x2728u, 2u, 1u};

static int fixed_read(void *context, uint32_t address, uint32_t *value)
{
	uint32_t i;
	(void)context;
	for (i = 0; i < 3u; i++)
		if (address == addresses[i] || address == ccu_addresses[i]) {
			*value = t736_read32(address);
			return 0;
		}
	probe_report.denied_address = address;
	return -1;
}

static int fixed_write(void *context, uint32_t address, uint32_t value)
{
	uint32_t next = probe_report.writes[0] + probe_report.writes[1] + probe_report.writes[2];
	(void)context;
	/* A second local gate prevents writes even if the caller sequencing changes. */
	if (probe_report.phase != 5u || probe_report.pretest_result != 0 ||
	    probe_report.pretest_ticks < 240000u || next >= 3u ||
	    address != addresses[next] || value != targets[next]) {
		probe_report.denied_address = address;
		return -1;
	}
	probe_report.writes[next]++;
	t736_write32(address, value);
	return 0;
}

static int fixed_delay(void *context, uint32_t us)
{
	uint32_t begin;
	int rc;
	(void)context;
	probe_report.component_delay_calls++;
	if (us != 10000u || probe_report.component_delay_calls != 1u) {
		probe_report.component_delay_result = (uint32_t)-1;
		return -1;
	}
	begin = t736_counter_low();
	rc = t736_delay_us(us);
	probe_report.component_delay_ticks = t736_counter_low() - begin;
	if (!rc && probe_report.component_delay_ticks < 240000u) rc = -1;
	probe_report.component_delay_result = (uint32_t)rc;
	return rc;
}

static void after_snapshot(void)
{
	uint32_t i, value;
	for (i = 0; i < 3u; i++) {
		if (fixed_read(0, addresses[i], &value)) return;
		probe_report.after[i] = value;
		probe_report.validity_counts |= 1u << (8u+i);
	}
}

int probe_main(void)
{
	const struct t736_phy_io io = {0, 0, 0, fixed_read, fixed_write, fixed_delay};
	struct t736_domain_report component;
	volatile uint32_t *words = (volatile uint32_t *)&probe_report;
	uint32_t i, value, begin;
	int rc;
	for (i = 0; i < 32u; i++) words[i] = 0;
	probe_report.magic = 0x54374457; /* T7DW */
	probe_report.version = 1;
	probe_report.status = 1;
	probe_report.phase = 1;
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(value));
	probe_report.sctlr = value;
	rc = t736_timer_init();
	probe_report.effective_hz = t736_timer_observation.effective_hz;
	probe_report.raw_cntfrq = t736_timer_observation.raw_cntfrq;
	probe_report.timestamp_ctrl = t736_timer_observation.ctrl_after;
	probe_report.timestamp_freqid = t736_timer_observation.freqid_after;
	if (rc) { probe_report.status = 0x101; return rc; }
	probe_report.phase = 2;
	for (i = 0; i < 3u; i++) {
		if (fixed_read(0, ccu_addresses[i], &value)) { probe_report.status = 0x103; return -1; }
		probe_report.ccu[i] = value;
		probe_report.validity_counts |= 1u << (16u+i);
		if (value != ccu_expected[i]) { probe_report.status = 0x103; return -1; }
	}
	probe_report.phase = 3;
	for (i = 0; i < 3u; i++) {
		if (fixed_read(0, addresses[i], &value)) { probe_report.status = 0x104; return -1; }
		probe_report.before[i] = value;
		probe_report.validity_counts |= 1u << i;
	}
	if (probe_report.before[0] != 0x3200u || probe_report.before[1] || probe_report.before[2]) {
		probe_report.status = 0x104; after_snapshot(); return -1;
	}
	probe_report.phase = 4;
	begin = t736_counter_low();
	rc = t736_delay_us(10000u);
	probe_report.pretest_ticks = t736_counter_low() - begin;
	if (!rc && probe_report.pretest_ticks < 240000u) rc = -1;
	probe_report.pretest_result = (uint32_t)rc;
	if (rc) { probe_report.status = 0x102; after_snapshot(); return rc; }
	probe_report.phase = 5;
	rc = t736_domain_prepare_experiment(&io, 1, &component);
	probe_report.component_result = (uint32_t)rc;
	probe_report.waited_us = component.waited_us;
	probe_report.last_write_address = component.last_write_address;
	probe_report.last_write_value = component.last_write_value;
	probe_report.validity_counts |= (uint32_t)component.writes_attempted << 24;
	probe_report.validity_counts |= (uint32_t)component.writes_completed << 28;
	if (component.before_valid) probe_report.validity_counts |= 1u << 3;
	if (component.after_valid) probe_report.validity_counts |= 1u << 11;
	after_snapshot();
	probe_report.status = rc == T736_DOMAIN_WRITES_OBSERVED_ONLY ? 2u : 0x201u;
	if (rc == T736_DOMAIN_WRITES_OBSERVED_ONLY) probe_report.phase = 6;
	__asm__ volatile("dsb sy" ::: "memory");
	/* Report preserves OBSERVED_ONLY; match the tested zero-success ROM return. */
	return rc == T736_DOMAIN_WRITES_OBSERVED_ONLY ? 0 : rc;
}
