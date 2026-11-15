// SPDX-License-Identifier: GPL-2.0-only
#include "domain_prepare_diag.h"

static int read_values(const struct t736_phy_io *io,
		       struct t736_domain_values *values)
{
	struct t736_domain_values sample;
	if (io->read32(io->context, T736_DOMAIN_SYS, &sample.sys) ||
	    io->read32(io->context, T736_DOMAIN_CHANNEL_TYPE, &sample.channel_type) ||
	    io->read32(io->context, T736_DOMAIN_ISOLATION, &sample.isolation))
		return T736_DOMAIN_IO;
	values->sys = sample.sys;
	values->channel_type = sample.channel_type;
	values->isolation = sample.isolation;
	return 0;
}

static int rmw_verify(const struct t736_phy_io *io,
		      struct t736_domain_report *report, uint32_t address,
		      uint32_t expected, uint32_t mask, uint32_t value)
{
	uint32_t original, target, readback;
	if (io->read32(io->context, address, &original))
		return T736_DOMAIN_IO;
	if (original != expected)
		return T736_DOMAIN_PRECONDITION;
	target = (original & ~mask) | (value & mask);
	report->last_write_address = address;
	report->last_write_value = target;
	report->writes_attempted++;
	if (io->write32(io->context, address, target))
		return T736_DOMAIN_IO;
	report->writes_completed++;
	if (io->read32(io->context, address, &readback))
		return T736_DOMAIN_IO;
	return readback == target ? 0 : T736_DOMAIN_VERIFY;
}

int t736_domain_prepare_experiment(const struct t736_phy_io *io,
				  unsigned int cold_sram_exclusive,
				  struct t736_domain_report *report)
{
	uint32_t pll, clock, bus;
	int ret;
	if (!io || !io->read32 || !io->write32 || !io->delay_us ||
	    !report || cold_sram_exclusive != 1)
		return T736_DOMAIN_INVALID;
	/* Scalar stores avoid an implicit freestanding memset dependency. */
	report->before.sys = 0;
	report->before.channel_type = 0;
	report->before.isolation = 0;
	report->after.sys = 0;
	report->after.channel_type = 0;
	report->after.isolation = 0;
	report->waited_us = 0;
	report->last_write_address = 0;
	report->last_write_value = 0;
	report->before_valid = 0;
	report->after_valid = 0;
	report->writes_attempted = 0;
	report->writes_completed = 0;
	if (io->read32(io->context, T736_PLL_DDR, &pll) ||
	    io->read32(io->context, T736_DRAM0_CLK, &clock) ||
	    io->read32(io->context, T736_DRAM0_BGR, &bus))
		return T736_DOMAIN_IO;
	if (pll != UINT32_C(0xf9004a00) || clock != UINT32_C(0x80000003) ||
	    bus != UINT32_C(0x00010001))
		return T736_DOMAIN_PRECONDITION;
	ret = read_values(io, &report->before);
	if (ret)
		return ret;
	report->before_valid = 1;
	if (report->before.sys != UINT32_C(0x3200) ||
	    report->before.channel_type != 0 || report->before.isolation != 0)
		return T736_DOMAIN_PRECONDITION;
	/* Initial low SYS_CFG bits are already zero. SDK clear14/clear28
	 * phases would be no-ops here; no extra writes are introduced.
	 */
	ret = rmw_verify(io, report, T736_DOMAIN_SYS, report->before.sys,
			 UINT32_C(0x3f3f), UINT32_C(0x2728));
	if (ret)
		return ret;
	ret = rmw_verify(io, report, T736_DOMAIN_CHANNEL_TYPE,
			 report->before.channel_type, UINT32_C(3), UINT32_C(2));
	if (ret)
		return ret;
	ret = rmw_verify(io, report, T736_DOMAIN_ISOLATION,
			 report->before.isolation, UINT32_C(1), UINT32_C(1));
	if (ret)
		return ret;
	if (io->delay_us(io->context, T736_DOMAIN_WAIT_US))
		return T736_DOMAIN_TIMER;
	report->waited_us = T736_DOMAIN_WAIT_US;
	ret = read_values(io, &report->after);
	if (ret)
		return ret;
	report->after_valid = 1;
	if (report->after.sys != UINT32_C(0x2728) ||
	    report->after.channel_type != 2 || report->after.isolation != 1)
		return T736_DOMAIN_VERIFY;
	return T736_DOMAIN_WRITES_OBSERVED_ONLY;
}
