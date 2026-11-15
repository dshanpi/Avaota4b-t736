// SPDX-License-Identifier: GPL-2.0-only
#include "checked_controller.h"

#define CTL_MRSTAT UINT32_C(0x90)
#define CONFIGURED_MSTR UINT32_C(0x03081002)

static int read_word(const struct t736_phy_io *io, uint32_t address, uint32_t *value)
{
	return io->read32(io->context, address, value) ? T736_CTL_CHECK_IO : T736_CTL_CHECK_OK;
}

int t736_ctl_observe_checked(const struct t736_phy_io *io, unsigned int channel,
			    volatile struct t736_ctl_observation *report)
{
	uint32_t base, value;
	int ret;
	if (!io || !io->read32 || !report || channel > 1u)
		return T736_CTL_CHECK_INVALID;
	base = T736_CTL0_BASE + channel * T736_CHANNEL_STRIDE;
	report->channel = channel;
	report->valid_mask = 0;
	report->mstr = report->stat = report->mrstat = 0;
	report->attempted_address = base + T736_CTL_MSTR;
	ret = read_word(io, report->attempted_address, &value);
	if (ret) return ret;
	report->mstr = value;
	report->valid_mask = 1;
	report->attempted_address = base + T736_CTL_STAT;
	ret = read_word(io, report->attempted_address, &value);
	if (ret) return ret;
	report->stat = value;
	report->valid_mask = 3;
	report->attempted_address = base + CTL_MRSTAT;
	ret = read_word(io, report->attempted_address, &value);
	if (ret) return ret;
	report->mrstat = value;
	report->valid_mask = 7;
	return T736_CTL_CHECK_OK;
}

static int valid_write(const struct t736_phy_io *io, unsigned int observed,
		       unsigned int exclusive, volatile struct t736_ctl_write_report *r)
{
	if (!io || !io->read32 || !io->write32 || !r || observed != 3u || exclusive != 1u)
		return 0;
	r->attempted_address = r->attempted_value = 0;
	r->writes_attempted = r->write_callbacks_completed = r->verified_writes = 0;
	r->channels_prechecked = 0;
	return 1;
}

static int precheck(const struct t736_phy_io *io, int require_layout,
		    volatile struct t736_ctl_write_report *report)
{
	static const struct { uint32_t address, expected; } prerequisite[] = {
		{T736_PLL_DDR, UINT32_C(0xf9004a00)},
		{T736_DRAM0_CLK, UINT32_C(0x80000003)},
		{T736_DRAM0_BGR, UINT32_C(0x00010001)},
		{UINT32_C(0x0a020010), UINT32_C(0x2728)},
		{UINT32_C(0x0a020014), UINT32_C(2)},
		{UINT32_C(0x07010250), UINT32_C(1)}
	};
	uint32_t ch, value, i;
	int ret;
	for (i = 0; i < sizeof(prerequisite) / sizeof(prerequisite[0]); i++) {
		report->attempted_address = prerequisite[i].address;
		ret = read_word(io, report->attempted_address, &value);
		if (ret) return ret;
		if (value != prerequisite[i].expected) return T736_CTL_CHECK_BUSY;
	}
	for (ch = 0; ch < 2u; ch++) {
		uint32_t base = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		report->attempted_address = base + T736_CTL_STAT;
		ret = read_word(io, report->attempted_address, &value);
		if (ret) return ret;
		if (value & 7u) return T736_CTL_CHECK_BUSY;
		if (require_layout) {
			report->attempted_address = base + T736_CTL_MSTR;
			ret = read_word(io, report->attempted_address, &value);
			if (ret) return ret;
			if (value != CONFIGURED_MSTR) return T736_CTL_CHECK_UNSUPPORTED;
		}
		report->channels_prechecked |= 1u << ch;
	}
	return T736_CTL_CHECK_OK;
}

static int write_verify(const struct t736_phy_io *io, uint32_t address, uint32_t value,
			volatile struct t736_ctl_write_report *report)
{
	uint32_t actual;
	int ret;
	report->attempted_address = address;
	report->attempted_value = value;
	report->writes_attempted++;
	if (io->write32(io->context, address, value)) return T736_CTL_CHECK_IO;
	report->write_callbacks_completed++;
	ret = read_word(io, address, &actual);
	if (ret) return ret;
	if (actual != value) return T736_CTL_CHECK_VERIFY;
	report->verified_writes++;
	return T736_CTL_CHECK_OK;
}

int t736_ctl_layout_checked(const struct t736_phy_io *io,
			   unsigned int observed_channels, unsigned int sram_exclusive,
			   const struct t736_lp4_geometry *geometry,
			   volatile struct t736_ctl_write_report *report)
{
	struct t736_lp4_layout layout;
	uint32_t ch, i;
	int ret;
	if (!valid_write(io, observed_channels, sram_exclusive, report))
		return T736_CTL_CHECK_INVALID;
	ret = t736_lp4_build_layout(geometry, &layout);
	if (ret) return ret == T736_LP4_UNSUPPORTED ? T736_CTL_CHECK_UNSUPPORTED : T736_CTL_CHECK_INVALID;
	ret = precheck(io, 0, report);
	if (ret) return ret;
	for (ch = 0; ch < 2u; ch++) {
		ret = write_verify(io, T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE + T736_CTL_MSTR,
				   layout.mstr, report);
		if (ret) return ret;
		for (i = 0; i < T736_LP4_MAP_WORDS; i++) {
			ret = write_verify(io, T736_MAP0_BASE + ch * T736_CHANNEL_STRIDE + layout.map[i].offset,
					   layout.map[i].value, report);
			if (ret) return ret;
		}
	}
	return T736_CTL_CHECK_OK;
}

int t736_ctl_timing_checked(const struct t736_phy_io *io,
			   unsigned int observed_channels, unsigned int sram_exclusive,
			   uint32_t ck_mhz, volatile struct t736_ctl_write_report *report)
{
	struct t736_lp4_timing timing;
	uint32_t ch, i, value;
	int ret;
	if (!valid_write(io, observed_channels, sram_exclusive, report))
		return T736_CTL_CHECK_INVALID;
	ret = t736_lp4_build_timing(ck_mhz, &timing);
	if (ret) return ret == T736_LP4_UNSUPPORTED ? T736_CTL_CHECK_UNSUPPORTED : T736_CTL_CHECK_INVALID;
	ret = precheck(io, 1, report);
	if (ret) return ret;
	for (ch = 0; ch < 2u; ch++) {
		for (i = 0; i < timing.count; i++) {
			const struct t736_lp4_timing_word *w = &timing.words[i];
			uint32_t address = T736_TIMING0_BASE + ch * T736_CHANNEL_STRIDE + w->offset;
			report->attempted_address = address;
			ret = read_word(io, address, &value);
			if (ret) return ret;
			value = (value & ~w->mask) | w->value;
			ret = write_verify(io, address, value, report);
			if (ret) return ret;
		}
	}
	return T736_CTL_CHECK_OK;
}
