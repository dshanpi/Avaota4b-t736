// SPDX-License-Identifier: GPL-2.0-only
#include "domain_prepare_diag.h"
#include <assert.h>
#include <stdio.h>

struct fake {
	uint32_t value[6], written_address[3], written_value[3];
	unsigned calls, fail_at, corrupt_at, writes, delays;
};

static const uint32_t addresses[6] = {
	T736_PLL_DDR, T736_DRAM0_CLK, T736_DRAM0_BGR,
	T736_DOMAIN_SYS, T736_DOMAIN_CHANNEL_TYPE, T736_DOMAIN_ISOLATION
};

static struct fake fresh(void)
{
	struct fake f = {0};
	f.value[0] = UINT32_C(0xf9004a00);
	f.value[1] = UINT32_C(0x80000003);
	f.value[2] = UINT32_C(0x10001);
	f.value[3] = UINT32_C(0x3200);
	return f;
}

static unsigned index_for(uint32_t address)
{
	for (unsigned i = 0; i < 6; i++)
		if (addresses[i] == address) return i;
	assert(0); /* No controller, PHY, ports, SMC, PMIC or storage access. */
	return 0;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct fake *f = context;
	f->calls++;
	if (f->calls == f->fail_at) return -1;
	*value = f->value[index_for(address)];
	if (f->calls == f->corrupt_at) *value ^= UINT32_C(0x80000000);
	return 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct fake *f = context;
	unsigned i = index_for(address);
	assert(i >= 3 && f->writes < 3);
	f->calls++;
	if (f->calls == f->fail_at) return -1;
	f->written_address[f->writes] = address;
	f->written_value[f->writes++] = value;
	f->value[i] = value;
	return 0;
}

static int delay_us(void *context, uint32_t delay)
{
	struct fake *f = context;
	assert(delay == 10000 && f->writes == 3);
	f->calls++;
	if (f->calls == f->fail_at) return -1;
	f->delays++;
	return 0;
}

static struct t736_phy_io io_for(struct fake *f)
{
	struct t736_phy_io io = {0};
	io.context = f;
	io.read32 = read32;
	io.write32 = write32;
	io.delay_us = delay_us;
	return io;
}

int main(void)
{
	struct fake f = fresh();
	struct t736_phy_io io = io_for(&f);
	struct t736_domain_report report;
	assert(t736_domain_prepare_experiment(&io, 1, &report) == T736_DOMAIN_WRITES_OBSERVED_ONLY);
	assert(f.calls == 19 && f.writes == 3 && f.delays == 1);
	assert(report.before_valid && report.after_valid && report.waited_us == 10000);
	assert(report.before.sys == 0x3200 && report.before.channel_type == 0 && report.before.isolation == 0);
	assert(report.after.sys == 0x2728 && report.after.channel_type == 2 && report.after.isolation == 1);
	assert(f.written_address[0] == T736_DOMAIN_SYS && f.written_value[0] == 0x2728);
	assert(f.written_address[1] == T736_DOMAIN_CHANNEL_TYPE && f.written_value[1] == 2);
	assert(f.written_address[2] == T736_DOMAIN_ISOLATION && f.written_value[2] == 1);
	assert(report.writes_attempted == 3 && report.writes_completed == 3);
	for (unsigned i = 1; i <= 19; i++) {
		f = fresh(); f.fail_at = i;
		int result = t736_domain_prepare_experiment(&io, 1, &report);
		assert(result == (i == 16 ? T736_DOMAIN_TIMER : T736_DOMAIN_IO));
		assert(f.calls == i); /* No retry, cleanup or speculative reads. */
		assert(report.writes_completed == f.writes);
		assert(report.writes_attempted == f.writes + ((i == 8 || i == 11 || i == 14) ? 1u : 0u));
		assert(!report.after_valid);
	}
	for (unsigned index = 0; index < 6; index++) {
		for (unsigned bit = 0; bit < 32; bit++) {
			f = fresh(); f.value[index] ^= UINT32_C(1) << bit;
			assert(t736_domain_prepare_experiment(&io, 1, &report) == T736_DOMAIN_PRECONDITION);
			assert(f.writes == 0 && f.delays == 0);
		}
	}
	for (unsigned i = 7; i <= 19; i++) {
		if (i == 8 || i == 11 || i == 14 || i == 16) continue;
		f = fresh(); f.corrupt_at = i;
		int result = t736_domain_prepare_experiment(&io, 1, &report);
		assert(result == ((i == 7 || i == 10 || i == 13) ?
		       T736_DOMAIN_PRECONDITION : T736_DOMAIN_VERIFY));
		if (i < 17) assert(f.calls == i);
		else assert(f.calls == 19 && report.after_valid);
	}
	f = fresh();
	assert(t736_domain_prepare_experiment(&io, 0, &report) == T736_DOMAIN_INVALID);
	assert(t736_domain_prepare_experiment(&io, 2, &report) == T736_DOMAIN_INVALID);
	assert(t736_domain_prepare_experiment(NULL, 1, &report) == T736_DOMAIN_INVALID);
	assert(t736_domain_prepare_experiment(&io, 1, NULL) == T736_DOMAIN_INVALID);
	assert(f.calls == 0);
	puts("Domain preparation diagnostic PASS: 192 initial-state bit guards, 19 callback failures, 9 drift/readback failures; no controller/PHY access or readiness claim");
	return 0;
}
