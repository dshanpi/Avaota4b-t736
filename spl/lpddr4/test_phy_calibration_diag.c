// SPDX-License-Identifier: GPL-2.0-only
#include "phy_calibration_diag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake {
	uint16_t mux, micro, clocks, reset, run, pu, pd;
	unsigned calls, fail_at, writes, delays, pulses;
	uint32_t elapsed, address[4];
	uint16_t value[4];
	int change_codes;
};

static int fail(struct fake *f)
{
	f->calls++;
	return f->fail_at == f->calls;
}

static int read16(void *context, uint32_t addr, uint16_t *value)
{
	struct fake *f = context;
	if (fail(f)) return -1;
	switch (addr) {
	case T736_PHY_MUX: *value = f->mux; break;
	case T736_PHY_MICRO_RESET: *value = f->micro; break;
	case T736_ZCAL_CLOCKS: *value = f->clocks; break;
	case T736_ZCAL_RESET: *value = f->reset; break;
	case T736_ZCAL_RUN: *value = f->run; break;
	case T736_ZCAL_PU: *value = f->pu; break;
	case T736_ZCAL_PD: *value = f->pd; break;
	default: assert(0); return -1;
	}
	return 0;
}

static int write16(void *context, uint32_t addr, uint16_t value)
{
	struct fake *f = context;
	if (fail(f)) return -1;
	assert(f->writes < 4);
	f->address[f->writes] = addr;
	f->value[f->writes++] = value;
	assert(value <= 1);
	if (addr == T736_ZCAL_RESET) {
		f->reset = value;
	} else {
		assert(addr == T736_ZCAL_RUN);
		f->run = value;
		if (value == 1) f->pulses++;
	}
	return 0;
}

static int delay_us(void *context, uint32_t delay)
{
	struct fake *f = context;
	if (fail(f)) return -1;
	f->elapsed += delay;
	f->delays++;
	if (f->run && f->change_codes) {
		f->pu = 0x3700;
		f->pd = 0x4200;
	}
	return 0;
}

static struct fake fresh(void)
{
	struct fake f = {0};
	f.micro = T736_PHY_RESET_STALL;
	f.clocks = 3;
	f.pu = 0x1200;
	f.pd = 0x2400;
	return f;
}

static struct t736_phy_io io_for(struct fake *f)
{
	struct t736_phy_io io = {0};
	io.context = f;
	io.read16 = read16;
	io.write16 = write16;
	io.delay_us = delay_us;
	return io;
}

static void check_observe(void)
{
	struct fake f = fresh();
	struct t736_phy_io io = io_for(&f);
	struct t736_zcal_snapshot out, sentinel;
	memset(&sentinel, 0xa5, sizeof(sentinel));
	out = sentinel;
	assert(t736_zcal_observe(&io, &out) == T736_ZCAL_OBSERVED_ONLY);
	assert(f.calls == 6 && f.writes == 0 && out.pu == 0x1200);
	for (unsigned i = 1; i <= 6; i++) {
		f = fresh(); f.fail_at = i; out = sentinel;
		assert(t736_zcal_observe(&io, &out) == T736_ZCAL_IO);
		assert(f.calls == i && f.writes == 0);
		assert(memcmp(&out, &sentinel, sizeof(out)) == 0);
	}
	f = fresh(); f.mux = 1;
	assert(t736_zcal_observe(&io, &out) == T736_ZCAL_PRECONDITION);
	assert(f.calls == 1);
	f = fresh(); f.micro = 1;
	assert(t736_zcal_observe(&io, &out) == T736_ZCAL_PRECONDITION);
	assert(f.calls == 2);
	assert(t736_zcal_observe(NULL, &out) == T736_ZCAL_INVALID);
	assert(t736_zcal_observe(&io, NULL) == T736_ZCAL_INVALID);
}

static void check_timed(void)
{
	const struct t736_zcal_experiment yes = {1, 1, 1};
	struct t736_zcal_experiment experiment;
	struct fake f = fresh();
	struct t736_phy_io io = io_for(&f);
	struct t736_zcal_report report;
	unsigned calls;
	for (unsigned change = 0; change < 2; change++) {
		for (uint32_t wait = 35; wait <= 10000; wait++) {
			f = fresh(); f.change_codes = (int)change;
			assert(t736_zcal_timed_experiment(&io, &yes, wait, &report) ==
			       T736_ZCAL_TIMED_ONLY);
			assert(f.calls == 17 && f.writes == 4 && f.delays == 2);
			assert(f.address[0] == T736_ZCAL_RESET && f.value[0] == 1);
			assert(f.address[1] == T736_ZCAL_RESET && f.value[1] == 0);
			assert(f.address[2] == T736_ZCAL_RUN && f.value[2] == 1);
			assert(f.address[3] == T736_ZCAL_RUN && f.value[3] == 0);
			assert(f.elapsed == wait + 1 && report.waited_us == wait + 1);
			assert(report.writes_attempted == 4 && report.writes_completed == 4);
			assert(report.before_valid && report.after_valid);
			assert(report.before.pu == 0x1200 && report.before.pd == 0x2400);
			assert(report.after.pu == (change ? 0x3700 : 0x1200));
			assert(report.after.pd == (change ? 0x4200 : 0x2400));
			assert(f.reset == 0 && f.run == 0 && f.pulses == 1);
		}
	}
	calls = f.calls;
	for (unsigned i = 1; i <= calls; i++) {
		f = fresh(); f.fail_at = i;
		int result = t736_zcal_timed_experiment(&io, &yes, 35, &report);
		assert(result == ((i == 9 || i == 12) ? T736_ZCAL_TIMER : T736_ZCAL_IO));
		assert(f.calls == i); /* Nothing, including cleanup, after failure. */
		assert(report.writes_completed == f.writes);
		assert(report.writes_attempted == f.writes +
		       ((i == 8 || i == 10 || i == 11 || i == 13) ? 1u : 0u));
		assert(!report.after_valid);
	}
	for (unsigned i = 0; i < 7; i++) {
		f = fresh();
		switch (i) {
		case 0: f.mux = 1; break;
		case 1: f.micro = 8; break;
		case 2: f.clocks = 2; break;
		case 3: f.reset = 2; break;
		case 4: f.run = 1; break;
		case 5: f.pu |= 1; break;
		default: f.pd |= 1; break;
		}
		assert(t736_zcal_timed_experiment(&io, &yes, 35, &report) ==
		       T736_ZCAL_PRECONDITION);
		assert(f.writes == 0 && f.delays == 0);
	}
	for (unsigned i = 0; i < 3; i++) {
		f = fresh(); experiment = yes;
		if (i == 0) experiment.sram_exclusive_cold_context = 0;
		if (i == 1) experiment.clocks_and_analog_setup_reviewed = 2;
		if (i == 2) experiment.candidate_mapping_reviewed = 0;
		assert(t736_zcal_timed_experiment(&io, &experiment, 35, &report) ==
		       T736_ZCAL_INVALID);
		assert(f.calls == 0);
	}
	f = fresh();
	assert(t736_zcal_timed_experiment(&io, &yes, 34, &report) == T736_ZCAL_INVALID);
	assert(t736_zcal_timed_experiment(&io, &yes, 10001, &report) == T736_ZCAL_INVALID);
	assert(t736_zcal_timed_experiment(NULL, &yes, 35, &report) == T736_ZCAL_INVALID);
	assert(t736_zcal_timed_experiment(&io, NULL, 35, &report) == T736_ZCAL_INVALID);
	assert(t736_zcal_timed_experiment(&io, &yes, 35, NULL) == T736_ZCAL_INVALID);
	assert(f.calls == 0);
}

int main(void)
{
	check_observe();
	check_timed();
	puts("PHY calibration diagnostic: 19932 timed vectors, read-only guards, all 23 callback failure points PASS; no hardware or calibration-success claim");
	return 0;
}
