/* SPDX-License-Identifier: GPL-2.0-only */
#include "cold_mr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct event { unsigned int width, write; uint32_t address, value; };
struct fake {
	struct event event[256];
	unsigned int count, fail_at, failed, writes;
	unsigned int command, stuck_done, no_done, stuck_request, stuck_busy;
	unsigned int clear_self_clears, bad_select, bad_data, early_done;
	unsigned int pending[2], ticks[2];
	uint32_t ctrl[2], data[2], stat[2];
	uint32_t override_address, override_value;
};

static int event(struct fake *f, unsigned int w, unsigned int wr,
		 uint32_t a, uint32_t v)
{
	assert(!f->failed); /* Any callback after a failure is a test failure. */
	assert(f->count < 256);
	f->event[f->count++] = (struct event){w, wr, a, v};
	if (wr) ++f->writes;
	if (f->fail_at && f->count == f->fail_at) {
		f->failed = 1;
		return -1;
	}
	return 0;
}

static void init(struct fake *f, int command)
{
	memset(f, 0, sizeof(*f));
	f->command = (unsigned int)command;
	f->ctrl[0] = f->ctrl[1] = command ? 0x38 : 0x30;
	f->stat[0] = f->stat[1] = command ? 0x10000 : 0;
}

static int rd16(void *ctx, uint32_t a, uint16_t *v)
{
	struct fake *f = ctx;
	uint32_t x;
	switch (a) {
	case 0x0aaa0000: x = 0; break;
	case 0x0aaa0132: x = 9; break;
	case 0x0aa80100: x = 3; break;
	case 0x0a9400c0: x = f->command ? 3 : 2; break;
	case 0x0a94014a: x = 0; break;
	default: assert(0); return -1;
	}
	if (a == f->override_address) x = f->override_value;
	if (event(f, 16, 0, a, x)) return -1;
	*v = (uint16_t)x;
	return 0;
}

static int rd32(void *ctx, uint32_t a, uint32_t *v)
{
	struct fake *f = ctx;
	unsigned int ch;
	uint32_t o, x = 0;
	if (a == 0x0a020010) x = 0x273f;
	else if (a == 0x0a02002c) x = 0xabcdef01;
	else if (a == 0x0a020040) x = 0x12345678;
	else {
		assert((a >= 0x0a110000 && a < 0x0a111000) ||
		       (a >= 0x0a510000 && a < 0x0a511000));
		ch = a >= 0x0a510000;
		o = a - (ch ? 0x0a510000 : 0x0a110000);
		switch (o) {
		case 0: x = 0x03081002; break;
		case 0x14: x = 0; break;
		case 0x80: x = f->ctrl[ch]; break;
		case 0x84: x = f->data[ch]; break;
		case 0x90: x = f->stat[ch]; break;
		case 0x94: x = 0x10203040 + ch; break;
		case 0x98: x = 0x50607080 + ch; break;
		case 0x100: x = 0; break;
		case 0x180: x = f->command ? 0 : 0x800; break;
		case 0x510: x = f->command ? 0x10015 : 0x10014; break;
		case 0x514: x = f->command ? 1 : 0; break;
		case 0xc80: case 0xc84: x = 1; break;
		default: assert(0); return -1;
		}
	}
	if (a == f->override_address) x = f->override_value;
	if (event(f, 32, 0, a, x)) return -1;
	*v = x;
	return 0;
}

static int wr32(void *ctx, uint32_t a, uint32_t v)
{
	struct fake *f = ctx;
	unsigned int ch = a >= 0x0a510000;
	uint32_t o = a - (ch ? 0x0a510000 : 0x0a110000);
	assert(o == 0x80 || o == 0x84); /* All writes confined to MRCTRL0/1. */
	assert(!f->command || (o != 0x80 || (v & 8)));
	if (event(f, 32, 1, a, v)) return -1;
	if (o == 0x84) {
		f->data[ch] = v ^ (f->bad_data ? 1u : 0u);
		if (f->early_done) f->stat[ch] |= 0x10000;
		return 0;
	}
	f->ctrl[ch] = v ^ (f->bad_select ? 8u : 0u);
	if (v & 0x01000000) {
		if (!f->stuck_done) f->stat[ch] &= ~0x10000u;
		if (f->clear_self_clears) f->ctrl[ch] &= ~0x01000000u;
	}
	if (v & 0x80000000) {
		f->pending[ch] = 1;
		f->stat[ch] |= 1;
	}
	return 0;
}

static int delay(void *ctx, uint32_t us)
{
	struct fake *f = ctx;
	unsigned int ch;
	assert(us == 1);
	if (event(f, 0, 0, 0, us)) return -1;
	for (ch = 0; ch < 2; ++ch) {
		if (f->pending[ch] && ++f->ticks[ch] >= 2) {
			if (!f->stuck_request) f->ctrl[ch] &= ~0x80000000u;
			if (!f->stuck_busy) f->stat[ch] &= ~1u;
			if ((f->ctrl[ch] & 1) && !f->no_done) f->stat[ch] |= 0x10000;
		}
	}
	return 0;
}

static const struct t736_cold_mr_context context = {1, 1, 1, 9};
static struct t736_phy_io io(struct fake *f)
{
	struct t736_phy_io x = {f, rd16, NULL, rd32, wr32, delay};
	return x;
}

static int run(struct fake *f, unsigned int kind,
	       const struct t736_cold_mr_request *q,
	       struct t736_cold_mr_report *r)
{
	struct t736_phy_io x = io(f);
	return kind ? t736_cold_mr_command(&x, &context, q, r) :
		t736_cold_mr_arm(&x, &context, r);
}

static void successful_and_faults(unsigned int kind)
{
	struct fake f, baseline;
	struct t736_cold_mr_report r;
	struct t736_cold_mr_request q = {1, 1, 8, 0, kind == 2, 5};
	unsigned int count, i, first_write = 0, expected_writes = kind == 2 ? 4 : kind ? 3 : 2;
	int expected = kind == 2 ? T736_COLD_MR_RECEIVED_CANDIDATE :
		kind ? T736_COLD_MR_ISSUED_ONLY : T736_COLD_MR_ARMED_ONLY;
	init(&f, kind != 0);
	assert(run(&f, kind, &q, &r) == expected);
	assert(r.writes_attempted == expected_writes && r.writes_completed == expected_writes);
	assert(r.channels_armed == 3 && r.stage == T736_COLD_MR_FINISHED);
	assert((f.ctrl[0] & 8) && (f.ctrl[1] & 8));
	if (kind == 2) {
		assert(r.done_was_cleared && r.fresh_done_observed && r.data_valid);
		assert(r.mrr_data[0] == 0x10203041 && r.mrr_data[1] == 0x50607081);
		assert(r.common_latch == 0x12345678);
	}
	count = f.count;
	baseline = f;
	for (i = 0; i < count; ++i) if (f.event[i].write) {first_write = i; break;}
	for (i = 1; i <= count; ++i) {
		init(&f, kind != 0); f.fail_at = i;
		assert(run(&f, kind, &q, &r) < 0);
		assert(f.failed && f.count == i);
		assert(r.writes_attempted == f.writes);
		assert(r.writes_completed + (baseline.event[i-1].write ? 1u : 0u) == f.writes);
	}
	/* Every initially checked physical field is independently corrupted. */
	for (i = 0; i < first_write; ++i) {
		const struct event *e = &baseline.event[i];
		if (!e->width) continue;
		init(&f, kind != 0);
		f.override_address = e->address;
		f.override_value = e->value ^ (e->address % 0x10000 == 0x80 ?
			0x80000000u : e->address % 0x10000 == 0x180 && kind ? 0x800u : 1u);
		assert(run(&f, kind, &q, &r) == T736_COLD_MR_PRECONDITION);
		assert(!f.writes);
	}
	printf("cold MR kind %u: %u callback faults, %u prewrite operations checked\n", kind, count, first_write);
}

static void protocol_failures(void)
{
	struct fake f;
	struct t736_cold_mr_report r;
	struct t736_cold_mr_request q = {0, 0, 5, 0, 1, 3};
	init(&f, 1); f.stuck_done = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_TIMEOUT);
	assert(f.writes == 1 && !r.done_was_cleared && !r.data_valid);
	init(&f, 1); f.no_done = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_TIMEOUT);
	assert(f.writes == 4 && r.done_was_cleared && !r.fresh_done_observed);
	init(&f, 1); f.stuck_request = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_TIMEOUT);
	assert(!r.data_valid);
	init(&f, 1); f.stuck_busy = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_TIMEOUT);
	assert(!r.data_valid);
	init(&f, 1); f.clear_self_clears = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_RECEIVED_CANDIDATE);
	assert(r.common_latch == 0xabcdef01 && r.mrr_data[0] == 0x10203040);
	init(&f, 1); f.bad_select = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_VERIFY);
	assert(f.writes == 1);
	init(&f, 1); f.bad_data = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_VERIFY);
	assert(f.writes == 3);
	init(&f, 1); f.early_done = 1;
	assert(run(&f, 2, &q, &r) == T736_COLD_MR_VERIFY);
	assert(f.writes == 3 && !r.data_valid);
	/* MRW is not gated by stale read-done and never reads MRR result. */
	init(&f, 1); q.read = 0; q.mr = 13; q.data = 0x40;
	assert(run(&f, 1, &q, &r) == T736_COLD_MR_ISSUED_ONLY);
	assert(f.data[0] == 0x0d40 && !r.fresh_done_observed && !r.data_valid);
	init(&f, 0); f.bad_select = 1;
	assert(run(&f, 0, &q, &r) == T736_COLD_MR_VERIFY);
	assert(f.writes == 1 && !r.channels_armed);
}

static void invalid_inputs(void)
{
	struct fake f;
	struct t736_phy_io x;
	struct t736_cold_mr_report r, before;
	struct t736_cold_mr_context c;
	struct t736_cold_mr_request q, original = {0, 0, 8, 0, 1, 3};
	unsigned int i;
	init(&f, 1); x = io(&f);
	memset(&r, 0x57, sizeof(r)); before = r;
	for (i = 0; i < 12; ++i) {
		q = original; c = context;
		switch (i) {
		case 0: q.channel = 2; break;
		case 1: q.rank = 2; break;
		case 2: q.mr = 64; break;
		case 3: q.data = 256; break;
		case 4: q.read = 2; break;
		case 5: q.data = 1; break;
		case 6: q.timeout_us = 0; break;
		case 7: q.timeout_us = 10001; break;
		case 8: c.sram_exclusive_cold_context = 0; break;
		case 9: c.same_generation_mapping_reviewed = 0; break;
		case 10: c.command_path_and_timing_ready = 0; break;
		case 11: x.delay_us = NULL; break;
		}
		assert(t736_cold_mr_command(&x, &c, &q, &r) == T736_COLD_MR_INVALID);
		assert(!f.count && !memcmp(&r, &before, sizeof(r)));
	}
	assert(t736_cold_mr_arm(NULL, &context, &r) == T736_COLD_MR_INVALID);
	assert(t736_cold_mr_arm(&x, NULL, &r) == T736_COLD_MR_INVALID);
	assert(t736_cold_mr_arm(&x, &context, NULL) == T736_COLD_MR_INVALID);
}

static void frozen_micro_state(void)
{
	struct fake f;
	struct t736_phy_io x;
	struct t736_cold_mr_report r;
	struct t736_cold_mr_context c = context;
	struct t736_cold_mr_request q = {0, 0, 8, 0, 1, 3};
	unsigned int mode, kind, observed;
	for (mode = 1; mode <= 9; mode += 8) {
		c.expected_micro_reset = (uint8_t)mode;
		for (kind = 0; kind < 3; ++kind) {
			for (observed = 0; observed <= 9; ++observed) {
				int rc;
				init(&f, kind != 0); x = io(&f); q.read = kind == 2;
				f.override_address = 0x0aaa0132; f.override_value = observed;
				rc = kind ? t736_cold_mr_command(&x, &c, &q, &r) :
					t736_cold_mr_arm(&x, &c, &r);
				if (observed == mode) assert(rc > 0);
				else {
					assert(rc == T736_COLD_MR_PRECONDITION);
					assert(!f.writes && f.count == 3);
					/* SYS_CFG, mux, micro only: no internal PHY read. */
				}
			}
		}
	}
	for (mode = 0; mode <= 10; ++mode) if (mode != 1 && mode != 9) {
		init(&f, 1); x = io(&f); c.expected_micro_reset = (uint8_t)mode;
		assert(t736_cold_mr_arm(&x, &c, &r) == T736_COLD_MR_INVALID);
		assert(t736_cold_mr_command(&x, &c, &q, &r) == T736_COLD_MR_INVALID);
		assert(!f.count);
	}
}

int main(void)
{
	successful_and_faults(0);
	successful_and_faults(1);
	successful_and_faults(2);
	protocol_failures();
	invalid_inputs();
	frozen_micro_state();
	puts("cold MR source tests PASS (no hardware)");
	return 0;
}
