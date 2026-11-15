// SPDX-License-Identifier: GPL-2.0-only
/*
 * Project-local LPDDR4 controller building blocks. Register facts are checked
 * against the board's read-only observations; see implementation.json.
 * No vendor library, instruction array or DRAM-training binary is included.
 */
#include "controller.h"

int t736_lp4_is_lpddr4(uint32_t mstr)
{
	return (mstr & T736_MSTR_TYPE_MASK) == T736_MSTR_LPDDR4;
}

int t736_lp4_build_layout(const struct t736_lp4_geometry *g,
			 struct t736_lp4_layout *out)
{
	struct t736_lp4_layout layout;
	uint32_t bank, row, repeated_row;

	if (!g || !out)
		return T736_LP4_INVALID;
	/* Do not generalize the observed map to an unverified memory topology. */
	if (g->channels != 2 || g->ranks_per_channel != 2 ||
	    g->channel_width_bits != 16 || g->row_bits != 17 ||
	    g->column_bits != 10 || g->bank_bits != 3)
		return T736_LP4_UNSUPPORTED;

	/* rank mask 3; burst length 16/2; half of 32-bit controller bus; LP4. */
	layout.mstr = (3u << 24) | (8u << 16) | (1u << 12) |
		      T736_MSTR_LPDDR4;
	layout.capacity_bytes = (UINT64_C(1) <<
		(g->row_bits + g->column_bits + g->bank_bits)) *
		(g->channel_width_bits / 8u) * g->ranks_per_channel * g->channels;
	bank = g->column_bits - 3u;
	row = g->column_bits + g->bank_bits - 6u;
	repeated_row = row * UINT32_C(0x01010101);
	layout.map[0] = (struct t736_lp4_map_word){0x004u,
		(uint32_t)g->row_bits + g->column_bits + g->bank_bits - 6u};
	layout.map[1] = (struct t736_lp4_map_word){0x00cu,
		bank * UINT32_C(0x00010101)};
	layout.map[2] = (struct t736_lp4_map_word){0x010u, 0x3f3fu};
	layout.map[3] = (struct t736_lp4_map_word){0x014u, 0x1f000000u};
	layout.map[4] = (struct t736_lp4_map_word){0x018u, 0u};
	layout.map[5] = (struct t736_lp4_map_word){0x01cu,
		(repeated_row & 0x00ffffffu) | 0x1f000000u};
	layout.map[6] = (struct t736_lp4_map_word){0x020u, repeated_row};
	layout.map[7] = (struct t736_lp4_map_word){0x024u, repeated_row};
	layout.map[8] = (struct t736_lp4_map_word){0x028u, repeated_row};
	layout.map[9] = (struct t736_lp4_map_word){0x02cu,
		repeated_row & 0x0000ffffu};
	*out = layout;
	return T736_LP4_OK;
}

static int io_valid(const struct t736_lp4_io *io)
{
	return io && io->read32 && io->write32;
}

int t736_lp4_apply_layout(const struct t736_lp4_io *io,
			 const struct t736_lp4_geometry *geometry)
{
	struct t736_lp4_layout layout;
	unsigned int ch, i;
	int ret;

	if (!io_valid(io))
		return T736_LP4_INVALID;
	ret = t736_lp4_build_layout(geometry, &layout);
	if (ret)
		return ret;
	/* Check both channels before touching either one. */
	for (ch = 0; ch < geometry->channels; ch++) {
		uint32_t ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		if ((io->read32(io->ctx, ctl + T736_CTL_STAT) & 7u) != 0)
			return T736_LP4_BUSY;
	}
	for (ch = 0; ch < geometry->channels; ch++) {
		uint32_t ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		uint32_t map = T736_MAP0_BASE + ch * T736_CHANNEL_STRIDE;
		io->write32(io->ctx, ctl + T736_CTL_MSTR, layout.mstr);
		if (io->read32(io->ctx, ctl + T736_CTL_MSTR) != layout.mstr)
			return T736_LP4_VERIFY;
		for (i = 0; i < T736_LP4_MAP_WORDS; i++) {
			uint32_t address = map + layout.map[i].offset;
			io->write32(io->ctx, address, layout.map[i].value);
			if (io->read32(io->ctx, address) != layout.map[i].value)
				return T736_LP4_VERIFY;
		}
	}
	return T736_LP4_OK;
}

int t736_lp4_set_update(const struct t736_lp4_io *io, unsigned int ch,
		       int commit, uint32_t timeout_us)
{
	uint32_t ctl, elapsed = 0;

	if (!io_valid(io) || !io->delay_us || ch >= 2 ||
	    (commit != 0 && commit != 1) || !timeout_us || timeout_us > 1000000u)
		return T736_LP4_INVALID;
	ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
	io->write32(io->ctx, ctl + T736_CTL_SWCTL, (uint32_t)commit);
	for (;;) {
		if ((io->read32(io->ctx, ctl + T736_CTL_SWSTAT) & 1u) ==
		    (uint32_t)commit)
			return T736_LP4_OK;
		if (elapsed == timeout_us)
			return T736_LP4_TIMEOUT;
		io->delay_us(io->ctx, 1u);
		elapsed++;
	}
}
