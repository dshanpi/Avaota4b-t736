// SPDX-License-Identifier: GPL-2.0-only
#include "phy_calibration_diag.h"

static int owned(const struct t736_phy_io *io)
{
	uint16_t value;
	if (io->read16(io->context, T736_PHY_MUX, &value))
		return T736_ZCAL_IO;
	if (value != 0)
		return T736_ZCAL_PRECONDITION;
	if (io->read16(io->context, T736_PHY_MICRO_RESET, &value))
		return T736_ZCAL_IO;
	if ((value & T736_PHY_RESET_STALL) != T736_PHY_RESET_STALL)
		return T736_ZCAL_PRECONDITION;
	return 0;
}

static int snapshot_read(const struct t736_phy_io *io,
			 struct t736_zcal_snapshot *snapshot)
{
	struct t736_zcal_snapshot value;
	if (io->read16(io->context, T736_ZCAL_RESET, &value.reset) ||
	    io->read16(io->context, T736_ZCAL_RUN, &value.run) ||
	    io->read16(io->context, T736_ZCAL_PU, &value.pu) ||
	    io->read16(io->context, T736_ZCAL_PD, &value.pd))
		return T736_ZCAL_IO;
	/* MMU-off SRAM must not rely on a compiler-generated unaligned word
	 * copy. Keep the halfword ABI; strict-alignment ARM builds need no libc. */
	snapshot->reset = value.reset;
	snapshot->run = value.run;
	snapshot->pu = value.pu;
	snapshot->pd = value.pd;
	return 0;
}

int t736_zcal_observe(const struct t736_phy_io *io,
		     struct t736_zcal_snapshot *snapshot)
{
	int ret;
	if (!io || !io->read16 || !snapshot)
		return T736_ZCAL_INVALID;
	ret = owned(io);
	if (ret)
		return ret;
	ret = snapshot_read(io, snapshot);
	return ret ? ret : T736_ZCAL_OBSERVED_ONLY;
}

static int write_recorded(const struct t736_phy_io *io,
			  struct t736_zcal_report *report,
			  uint32_t address, uint16_t value)
{
	report->last_write_address = address;
	report->last_write_value = value;
	report->writes_attempted++;
	if (io->write16(io->context, address, value))
		return T736_ZCAL_IO;
	report->writes_completed++;
	return 0;
}

int t736_zcal_timed_experiment(const struct t736_phy_io *io,
			     const struct t736_zcal_experiment *experiment,
			     uint32_t wait_us, struct t736_zcal_report *report)
{
	uint16_t clocks;
	int ret;
	if (!io || !io->read16 || !io->write16 || !io->delay_us ||
	    !experiment || !report || wait_us < T736_ZCAL_MIN_WAIT_US ||
	    wait_us > T736_ZCAL_MAX_WAIT_US ||
	    experiment->sram_exclusive_cold_context != 1 ||
	    experiment->clocks_and_analog_setup_reviewed != 1 ||
	    experiment->candidate_mapping_reviewed != 1)
		return T736_ZCAL_INVALID;
	/* Explicit scalar stores keep this freestanding without a libc memset. */
	report->before.reset = 0;
	report->before.run = 0;
	report->before.pu = 0;
	report->before.pd = 0;
	report->after.reset = 0;
	report->after.run = 0;
	report->after.pu = 0;
	report->after.pd = 0;
	report->waited_us = 0;
	report->last_write_address = 0;
	report->last_write_value = 0;
	report->writes_attempted = 0;
	report->writes_completed = 0;
	report->before_valid = 0;
	report->after_valid = 0;
	ret = owned(io);
	if (ret)
		return ret;
	if (io->read16(io->context, T736_ZCAL_CLOCKS, &clocks))
		return T736_ZCAL_IO;
	if (clocks != 3)
		return T736_ZCAL_PRECONDITION;
	ret = snapshot_read(io, &report->before);
	if (ret)
		return ret;
	report->before_valid = 1;
	if (report->before.reset > 1 || report->before.run != 0 ||
	    ((report->before.pu | report->before.pd) & 1u))
		return T736_ZCAL_PRECONDITION;
	ret = write_recorded(io, report, T736_ZCAL_RESET, 1);
	if (ret)
		return ret;
	if (io->delay_us(io->context, 1))
		return T736_ZCAL_TIMER;
	report->waited_us = 1;
	ret = write_recorded(io, report, T736_ZCAL_RESET, 0);
	if (ret)
		return ret;
	ret = write_recorded(io, report, T736_ZCAL_RUN, 1);
	if (ret)
		return ret;
	if (io->delay_us(io->context, wait_us))
		return T736_ZCAL_TIMER;
	report->waited_us += wait_us;
	ret = write_recorded(io, report, T736_ZCAL_RUN, 0);
	if (ret)
		return ret;
	ret = snapshot_read(io, &report->after);
	if (ret)
		return ret;
	report->after_valid = 1;
	return T736_ZCAL_TIMED_ONLY;
}
