// SPDX-License-Identifier: GPL-2.0-only
/*
 * T736 LPDDR4 timing components. Register locations and CK-unit semantics
 * were established by read-only inspection of the existing board library;
 * source/proof boundaries are recorded in evidence/lpddr4-timing-contract.json.
 * No executable vendor data or instruction stream is included here.
 */
#include "timing.h"

int t736_lp4_ns_to_ck(uint32_t ns, uint32_t mhz,
		      uint32_t minimum, uint32_t *cycles)
{
	uint64_t value;

	if (!cycles || !mhz)
		return T736_LP4_INVALID;
	/* UINT32_MAX squared plus 999 still fits in uint64_t. */
	value = ((uint64_t)ns * mhz + 999u) / 1000u;
	if (value > UINT32_MAX)
		return T736_LP4_UNSUPPORTED;
	if (value < minimum)
		value = minimum;
	*cycles = (uint32_t)value;
	return T736_LP4_OK;
}

static uint32_t ck(uint32_t ns, uint32_t minimum)
{
	uint32_t result = 0;

	/* All call-site constants fit at the single supported board frequency. */
	(void)t736_lp4_ns_to_ck(ns, 1800u, minimum, &result);
	return result;
}

static void word(struct t736_lp4_timing *t, uint32_t offset,
		 uint32_t mask, uint32_t value)
{
	t->words[t->count++] = (struct t736_lp4_timing_word){offset, mask, value};
}

int t736_lp4_build_timing(uint32_t mhz, struct t736_lp4_timing *out)
{
	struct t736_lp4_timing *t = out;
	uint32_t trp, tras, txp, twtr, trefi, rd_to_wr;

	if (!out)
		return T736_LP4_INVALID;
	if (mhz != 1800u)
		return T736_LP4_UNSUPPORTED;
	t->ck_mhz = mhz;
	t->count = 0u;
	trp = ck(21u, 4u);
	tras = ck(42u, 3u);
	txp = ck(8u, 5u);
	twtr = ck(10u, 8u);
	/* Refresh counters use 32 CK ticks, unlike the single-CK DRAMTMGs. */
	trefi = ck(3904u, 0u) / 32u;
	rd_to_wr = ck(3u, 0u) + 52u - ck(2u, 0u) - 6u;

	word(t, 0x000u, UINT32_MAX,
	     (67u << 24) | (ck(40u, 0u) << 16) |
	     ((trefi * 9u / 32u) << 8) | tras);
	word(t, 0x004u, UINT32_MAX, (txp << 16) | (16u << 8) | (tras + trp));
	/* RL=36, WL=18; LP4 burst-16 turnaround at the verified clock. */
	word(t, 0x008u, UINT32_MAX,
	     (18u << 24) | (36u << 16) | (rd_to_wr << 8) | (twtr + 27u));
	word(t, 0x00cu, UINT32_MAX,
	     (ck(14u, 10u) << 16) | ((ck(3u, 0u) + 54u + ck(8u, 8u)) << 8) |
	     (ck(8u, 8u) + 67u));
	word(t, 0x010u, UINT32_MAX,
	     (ck(18u, 4u) << 24) | (8u << 16) | (ck(10u, 4u) << 8) | trp);
	word(t, 0x014u, UINT32_MAX,
	     (ck(2u, 3u) << 24) | (ck(8u, 5u) << 16) |
	     (ck(15u, 4u) << 8) | ck(8u, 4u));
	word(t, 0x018u, UINT32_MAX, txp + 2u);
	word(t, 0x01cu, UINT32_MAX, 2u);
	word(t, 0x024u, UINT32_MAX, (2u << 16) | (2u << 8) | 8u);
	word(t, 0x030u, UINT32_MAX, ck(2u, 3u) << 16);
	word(t, 0x034u, UINT32_MAX, (26u << 24) | (32u << 16) | 4u);
	word(t, 0x038u, UINT32_MAX, (ck(40u, 8u) << 16) | ck(388u, 2u));
	word(t, 0x05cu, UINT32_MAX, (100u << 16) | 2u);
	word(t, 0x060u, UINT32_MAX, (8u << 16) | (15u << 8) | 15u);
	word(t, 0x064u, UINT32_MAX, (2u << 16) | (67u << 8) | 16u);
	/* DFI pipeline is controller/PHY-specific; not JEDEC RL/WL in isolation. */
	word(t, 0x580u, UINT32_MAX, (2u << 24) | (31u << 16) | (2u << 8) | 14u);
	word(t, 0x584u, UINT32_MAX, (6u << 16) | (2u << 8) | 2u);
	word(t, 0x588u, UINT32_MAX, (31u << 16) | (31u << 8) | 14u);
	/* Per-bank refresh interval; high bits/reserved fields are preserved. */
	word(t, 0x600u, UINT32_C(0x8000ffff), ck(488u, 0u) / 32u);
	word(t, 0x604u, UINT32_C(0x0fff0fff), (ck(380u, 0u) << 16) | ck(190u, 0u));
	word(t, 0x608u, UINT32_C(0xffff0000), (10u << 24) | (ck(90u, 0u) << 16));
	return T736_LP4_OK;
}

static int valid_io(const struct t736_lp4_io *io)
{
	return io && io->read32 && io->write32;
}

static int both_initializing(const struct t736_lp4_io *io)
{
	unsigned int ch;

	for (ch = 0; ch < 2; ch++) {
		uint32_t base = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		if ((io->read32(io->ctx, base + T736_CTL_STAT) & 7u) != 0u)
			return T736_LP4_BUSY;
		if (io->read32(io->ctx, base + T736_CTL_MSTR) != UINT32_C(0x03081002))
			return T736_LP4_UNSUPPORTED;
	}
	return T736_LP4_OK;
}

int t736_lp4_apply_timing(const struct t736_lp4_io *io, uint32_t mhz)
{
	struct t736_lp4_timing timing;
	unsigned int ch, i;
	int ret;

	if (!valid_io(io))
		return T736_LP4_INVALID;
	ret = t736_lp4_build_timing(mhz, &timing);
	if (ret)
		return ret;
	ret = both_initializing(io);
	if (ret)
		return ret;
	for (ch = 0; ch < 2; ch++) {
		for (i = 0; i < timing.count; i++) {
			const struct t736_lp4_timing_word *w = &timing.words[i];
			uint32_t addr = T736_TIMING0_BASE + ch * T736_CHANNEL_STRIDE + w->offset;
			uint32_t value = (io->read32(io->ctx, addr) & ~w->mask) | w->value;
			io->write32(io->ctx, addr, value);
			if (io->read32(io->ctx, addr) != value)
				return T736_LP4_VERIFY;
		}
	}
	return T736_LP4_OK;
}

static int poll(const struct t736_lp4_io *io, uint32_t addr, uint32_t mask,
		uint32_t value, uint32_t timeout)
{
	uint32_t elapsed;

	for (elapsed = 0; ; elapsed++) {
		if ((io->read32(io->ctx, addr) & mask) == value)
			return T736_LP4_OK;
		if (elapsed == timeout)
			return T736_LP4_TIMEOUT;
		io->delay_us(io->ctx, 1u);
	}
}

static int modify(const struct t736_lp4_io *io, uint32_t addr,
		  uint32_t clear, uint32_t set)
{
	uint32_t value = (io->read32(io->ctx, addr) & ~clear) | set;
	io->write32(io->ctx, addr, value);
	return ((io->read32(io->ctx, addr) ^ value) & (clear | set)) ?
		T736_LP4_VERIFY : T736_LP4_OK;
}

static int updates(const struct t736_lp4_io *io, int commit, uint32_t timeout)
{
	unsigned int ch;
	int ret;

	for (ch = 0; ch < 2; ch++) {
		ret = t736_lp4_set_update(io, ch, commit, timeout);
		if (ret)
			return ret;
	}
	return T736_LP4_OK;
}

int t736_lp4_dfi_handoff(const struct t736_lp4_io *io, uint32_t timeout)
{
	unsigned int ch;
	int ret;

	if (!valid_io(io) || !io->delay_us || !timeout || timeout > 1000000u)
		return T736_LP4_INVALID;
	ret = both_initializing(io);
	if (ret)
		return ret;
	/* Validate both channels before opening either update transaction. */
	for (ch = 0; ch < 2; ch++) {
		uint32_t base = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		if (!(io->read32(io->ctx, base + T736_CTL_PWRCTL) & (1u << 11)))
			return T736_LP4_BUSY;
	}
	ret = updates(io, 0, timeout);
	if (ret)
		return ret;
	for (ch = 0; ch < 2; ch++) {
		ret = modify(io, T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE + T736_CTL_DFIMISC,
			     0u, (1u << 12) | (1u << 5) | 1u);
		if (ret)
			return ret;
	}
	ret = updates(io, 1, timeout);
	if (ret)
		return ret;
	for (ch = 0; ch < 2; ch++) {
		ret = poll(io, T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE + T736_CTL_DFISTAT,
			   1u, 1u, timeout);
		if (ret)
			return ret;
	}
	io->delay_us(io->ctx, 50u);
	ret = updates(io, 0, timeout);
	if (ret)
		return ret;
	for (ch = 0; ch < 2; ch++) {
		uint32_t base = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		ret = modify(io, base + T736_CTL_DFIMISC, 1u << 5, 0u);
		if (ret)
			return ret;
		ret = modify(io, base + T736_CTL_PWRCTL, 1u << 11, 0u);
		if (ret)
			return ret;
	}
	for (ch = 0; ch < 2; ch++) {
		ret = poll(io, T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE + T736_CTL_STAT,
			   0x37u, 1u, timeout);
		if (ret)
			return ret;
	}
	io->delay_us(io->ctx, 50u);
	for (ch = 0; ch < 2; ch++) {
		ret = modify(io, T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE + T736_CTL_DFIMISC,
			     1u, 0u);
		if (ret)
			return ret;
	}
	ret = updates(io, 1, timeout);
	if (!ret)
		io->delay_us(io->ctx, 2u);
	return ret;
}
