/* SPDX-License-Identifier: GPL-2.0-only */
#include "mode_register.h"
#include "controller.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

struct fake {
	uint32_t base, mstr, stat, raw, control;
	unsigned int calls, fail_call, writes, result_reads;
	unsigned int elapsed, request_done, busy_done;
	int initially_busy, initially_requested, corrupt_select, bad_final_stat;
	uint32_t written_address[3], written_value[3];
};

static struct fake initial(unsigned int channel)
{
	struct fake f;
	memset(&f, 0, sizeof(f));
	f.base = T736_CTL0_BASE + (uint32_t)channel * T736_CHANNEL_STRIDE;
	f.mstr = UINT32_C(0x03081002);
	f.stat = 1;
	f.raw = UINT32_C(0xa1b2c3d4);
	f.request_done = 2;
	f.busy_done = 4;
	return f;
}

static int fails(struct fake *f)
{
	++f->calls;
	return f->fail_call != 0u && f->calls == f->fail_call;
}

static int rd(void *ctx, uint32_t address, uint32_t *value)
{
	struct fake *f = ctx;
	if (fails(f))
		return -1;
	if (address == f->base + T736_CTL_MSTR) {
		*value = f->mstr;
	} else if (address == f->base + T736_CTL_STAT) {
		*value = f->bad_final_stat && f->writes == 3u ? 3u : f->stat;
	} else if (address == f->base + T736_LP4_MRR_CTRL0) {
		*value = f->control;
		if ((f->writes == 0u && f->initially_requested) ||
		    (f->writes == 3u && f->elapsed < f->request_done))
			*value |= UINT32_C(0x80000000);
		if (f->writes == 3u && f->corrupt_select)
			*value ^= 1u;
	} else if (address == f->base + T736_LP4_MRR_STAT) {
		*value = (uint32_t)((f->writes == 0u && f->initially_busy) ||
			 (f->writes == 3u && f->elapsed < f->busy_done));
	} else {
		assert(address == (f->base == T736_CTL0_BASE ?
			T736_LP4_MRR_RESULT_CH0 : T736_LP4_MRR_RESULT_CH1));
		assert(f->writes == 3u && f->elapsed >= f->request_done &&
		       f->elapsed >= f->busy_done);
		++f->result_reads;
		*value = f->raw;
	}
	return 0;
}

static int wr(void *ctx, uint32_t address, uint32_t value)
{
	struct fake *f = ctx;
	if (fails(f))
		return -1;
	assert(f->writes < 3u);
	/* Safety oracle: only three controller writes; no physical MRW possible. */
	assert(address == f->base + T736_LP4_MRR_CTRL0 ||
	       address == f->base + T736_LP4_MRR_CTRL1);
	if (address == f->base + T736_LP4_MRR_CTRL0) {
		assert((value & 1u) == 1u);
		assert((value & ~UINT32_C(0x80000031)) == 0u);
		assert((value & 0x30u) == 0x10u || (value & 0x30u) == 0x20u);
		f->control = value & ~UINT32_C(0x80000000);
	} else {
		assert(value == 0x500u || value == 0x600u || value == 0x800u);
	}
	f->written_address[f->writes] = address;
	f->written_value[f->writes] = value;
	++f->writes;
	return 0;
}

static int delay(void *ctx, uint32_t us)
{
	struct fake *f = ctx;
	if (fails(f))
		return -1;
	assert(us == 1u);
	++f->elapsed;
	return 0;
}

static int invoke(struct fake *f, unsigned int channel, unsigned int rank,
		  unsigned int mr, uint32_t wait, struct t736_lp4_mrr_result *out)
{
	struct t736_lp4_mrr_io io = {f, rd, wr, delay};
	return t736_lp4_mrr_read_id(&io, channel, rank, mr, wait, out);
}

static void unchanged(const struct t736_lp4_mrr_result *out)
{
	assert(out->raw == 0x55aacc33u && out->value == 0x5au);
}

int main(void)
{
	struct fake f;
	struct t736_lp4_mrr_result out = {0x55aacc33u, 0x5au};
	unsigned int channel, rank, mr, byte, n, count;
	const unsigned int ids[] = {5, 6, 8};
	for (channel = 0; channel < 2u; ++channel) {
		for (rank = 0; rank < 2u; ++rank) {
			for (mr = 0; mr < 3u; ++mr) {
				for (byte = 0; byte < 256u; ++byte) {
					f = initial(channel);
					f.raw = channel == 0u ? 0xa5c3007eu | (byte << 8) :
						0xa5c3d200u | byte;
					assert(invoke(&f, channel, rank, ids[mr], 5, &out) == 0);
					assert(out.value == byte && out.raw == f.raw);
					assert(f.writes == 3u && f.result_reads == 1u && f.elapsed == 4u);
					assert(f.written_address[0] == f.base + 0x80u);
					assert(f.written_address[1] == f.base + 0x84u);
					assert(f.written_address[2] == f.base + 0x80u);
					assert(f.written_value[0] == (rank == 0u ? 0x11u : 0x21u));
					assert(f.written_value[1] == ids[mr] * 256u);
					assert(f.written_value[2] == (f.written_value[0] | 0x80000000u));
				}
			}
		}
	}
	/* All other MRs fail before even reading a controller register. */
	for (mr = 0; mr < 256u; ++mr) {
		if (mr == 5u || mr == 6u || mr == 8u)
			continue;
		f = initial(0);
		assert(invoke(&f, 0, 0, mr, 5, &out) == T736_LP4_MRR_INVALID);
		assert(f.calls == 0u);
	}
	out.raw = 0x55aacc33u;
	out.value = 0x5au;
	f = initial(0);
	assert(invoke(&f, 2, 0, 5, 5, &out) == T736_LP4_MRR_INVALID);
	assert(invoke(&f, 0, 2, 5, 5, &out) == T736_LP4_MRR_INVALID);
	assert(invoke(&f, UINT_MAX, UINT_MAX, 5, 5, &out) == T736_LP4_MRR_INVALID);
	assert(invoke(&f, 0, 0, 5, 0, &out) == T736_LP4_MRR_INVALID);
	assert(invoke(&f, 0, 0, 5, UINT32_MAX, &out) == T736_LP4_MRR_INVALID);
	assert(f.calls == 0u);
	unchanged(&out);
	for (n = 0; n < 16u; ++n) {
		if (n == 2u)
			continue;
		f = initial(0);
		f.mstr = 0x03081000u | n;
		assert(invoke(&f, 0, 0, 5, 5, &out) == T736_LP4_MRR_UNSUPPORTED);
		assert(f.writes == 0u);
		unchanged(&out);
	}
	f = initial(0);
	f.mstr &= ~UINT32_C(0x02000000);
	assert(invoke(&f, 0, 1, 5, 5, &out) == T736_LP4_MRR_UNSUPPORTED);
	assert(f.writes == 0u);
	for (n = 0; n < 8u; ++n) {
		if (n == 1u)
			continue;
		f = initial(0);
		f.stat = n;
		assert(invoke(&f, 0, 0, 5, 5, &out) == T736_LP4_MRR_BUSY);
		assert(f.writes == 0u);
	}
	f = initial(0);
	f.initially_busy = 1;
	assert(invoke(&f, 0, 0, 5, 5, &out) == T736_LP4_MRR_BUSY);
	assert(f.writes == 0u);
	f = initial(0);
	f.initially_requested = 1;
	assert(invoke(&f, 0, 0, 5, 5, &out) == T736_LP4_MRR_BUSY);
	assert(f.writes == 0u);
	unchanged(&out);
	/* Independently stall trigger and busy: never read stale latch on timeout. */
	for (n = 0; n < 2u; ++n) {
		f = initial(0);
		f.request_done = n == 0u ? UINT_MAX : 0u;
		f.busy_done = n == 1u ? UINT_MAX : 0u;
		assert(invoke(&f, 0, 0, 5, 3, &out) == T736_LP4_MRR_TIMEOUT);
		assert(f.elapsed == 3u && f.writes == 3u && f.result_reads == 0u);
		unchanged(&out);
	}
	f = initial(0);
	f.corrupt_select = 1;
	assert(invoke(&f, 0, 0, 5, 5, &out) == T736_LP4_MRR_VERIFY);
	assert(f.result_reads == 0u);
	unchanged(&out);
	f = initial(0);
	f.bad_final_stat = 1;
	assert(invoke(&f, 0, 0, 5, 5, &out) == T736_LP4_MRR_VERIFY);
	assert(f.result_reads == 0u);
	unchanged(&out);
	f = initial(0);
	assert(invoke(&f, 0, 0, 5, 5, &out) == 0);
	count = f.calls;
	/* Every MMIO and delay callback failure aborts immediately. */
	for (n = 1; n <= count; ++n) {
		f = initial(0);
		f.fail_call = n;
		out.raw = 0x55aacc33u;
		out.value = 0x5au;
		assert(invoke(&f, 0, 0, 5, 5, &out) == T736_LP4_MRR_IO);
		assert(f.calls == n);
		unchanged(&out);
	}
	{
		struct t736_lp4_mrr_io io = {&f, rd, wr, delay};
		assert(t736_lp4_mrr_read_id(NULL, 0, 0, 5, 5, &out) == T736_LP4_MRR_INVALID);
		assert(t736_lp4_mrr_read_id(&io, 0, 0, 5, 5, NULL) == T736_LP4_MRR_INVALID);
		io.read32 = NULL;
		assert(t736_lp4_mrr_read_id(&io, 0, 0, 5, 5, &out) == T736_LP4_MRR_INVALID);
		io.read32 = rd;
		io.write32 = NULL;
		assert(t736_lp4_mrr_read_id(&io, 0, 0, 5, 5, &out) == T736_LP4_MRR_INVALID);
		io.write32 = wr;
		io.delay_us = NULL;
		assert(t736_lp4_mrr_read_id(&io, 0, 0, 5, 5, &out) == T736_LP4_MRR_INVALID);
	}
	puts("PASS: 3072 lane/channel/rank/ID vectors; only read commands; every I/O failure; busy, state, timeout guards");
	return 0;
}
