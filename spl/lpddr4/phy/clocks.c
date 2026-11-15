// SPDX-License-Identifier: GPL-2.0-only
/* T736 CCU register semantics from User Manual v0.90, sections 4.1.4.5,
 * 4.1.6.73--75 and 4.1.6.122--123. No table replay or executable firmware.
 */
#include "clocks.h"
#include "../controller.h"

#define BIT(n) (UINT32_C(1) << (n))
#define CLK_GATE BIT(31)
#define NSI_RESET_RELEASE BIT(30)
#define CLK_DFS BIT(28)
#define CLK_UPDATE BIT(27)
#define CLK_SOURCE (UINT32_C(7) << 24)
#define CLK_DIV UINT32_C(0x1f)
#define DRAM_LOW_SPEED BIT(16)
#define BUS_GATE BIT(0)
#define BUS_RESET_RELEASE BIT(16)
#define BUS_READY (BUS_GATE | BUS_RESET_RELEASE)

static int rd(const struct t736_phy_io *io, uint32_t addr, uint32_t *value)
{
	return io->read32(io->context, addr, value) ? T736_PHY_IO : T736_PHY_OK;
}

static int wr(const struct t736_phy_io *io, uint32_t addr, uint32_t value)
{
	return io->write32(io->context, addr, value) ? T736_PHY_IO : T736_PHY_OK;
}

static int bus_modify(const struct t736_phy_io *io, uint32_t addr,
		      uint32_t clear, uint32_t set)
{
	uint32_t value, expected;
	int ret = rd(io, addr, &value);

	if (ret)
		return ret;
	expected = (value & ~clear) | set;
	ret = wr(io, addr, expected);
	if (ret)
		return ret;
	ret = rd(io, addr, &value);
	if (ret)
		return ret;
	return (value & BUS_READY) == (expected & BUS_READY) ? T736_PHY_OK : T736_PHY_VERIFY;
}

/* Setting UPD is also required for GATING changes, not just SRC/DIV.
 * Section 4.1.4.5 explicitly says readback otherwise returns old parameters.
 */
static int clock_modify(const struct t736_phy_io *io, uint32_t addr,
			uint32_t clear, uint32_t set, uint32_t timeout)
{
	uint32_t value, expected, elapsed = 0;
	int ret = rd(io, addr, &value);

	if (ret)
		return ret;
	if (value & CLK_UPDATE)
		return T736_PHY_BUSY;
	expected = (value & ~clear) | set;
	/* The readback already describes the active configuration. In particular,
	 * cold DRAM0 may already be gated while its selected DDRPLL is stopped.
	 * Do not start a redundant req/ack transaction with an unavailable source.
	 */
	if (expected == value)
		return T736_PHY_OK;
	ret = wr(io, addr, expected | CLK_UPDATE);
	if (ret)
		return ret;
	for (;;) {
		ret = rd(io, addr, &value);
		if (ret)
			return ret;
		if (!(value & CLK_UPDATE))
			break;
		if (elapsed == timeout)
			return T736_PHY_TIMEOUT;
		if (io->delay_us(io->context, 1u))
			return T736_PHY_TIMER;
		elapsed++;
	}
	return value == expected ? T736_PHY_OK : T736_PHY_VERIFY;
}

static int preflight(const struct t736_phy_io *io)
{
	uint32_t nsi, mbus, dram, bgr, ch, value;
	int ret;

	ret = rd(io, T736_NSI_CLK, &nsi);
	if (ret)
		return ret;
	ret = rd(io, T736_MBUS_CLK, &mbus);
	if (ret)
		return ret;
	ret = rd(io, T736_DRAM0_CLK, &dram);
	if (ret)
		return ret;
	ret = rd(io, T736_DRAM0_BGR, &bgr);
	if (ret)
		return ret;
	if ((nsi | mbus | dram) & CLK_UPDATE)
		return T736_PHY_BUSY;
	if ((nsi | mbus) & CLK_DFS)
		return T736_PHY_UNSUPPORTED;
	if ((nsi & (CLK_GATE | NSI_RESET_RELEASE)) != (CLK_GATE | NSI_RESET_RELEASE))
		return T736_PHY_BUSY;
	if (!(mbus & CLK_GATE))
		return T736_PHY_BUSY;
	if ((nsi & CLK_SOURCE) != 0 && (nsi & CLK_SOURCE) != (3u << 24))
		return T736_PHY_UNSUPPORTED;
	if ((mbus & CLK_SOURCE) != 0 && (mbus & CLK_SOURCE) != (4u << 24))
		return T736_PHY_UNSUPPORTED;
	ret = rd(io, T736_NSI_BGR, &value);
	if (ret)
		return ret;
	if ((value & BUS_READY) != BUS_READY)
		return T736_PHY_BUSY;
	/* Avoid touching the controller aperture when its bus is off/in reset. */
	if ((bgr & BUS_READY) == BUS_READY && (dram & CLK_GATE)) {
		for (ch = 0; ch < 2u; ch++) {
			ret = rd(io, T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE +
				 T736_CTL_STAT, &value);
			if (ret)
				return ret;
			if (value & 7u)
				return T736_PHY_BUSY;
		}
	}
	return T736_PHY_OK;
}

static int isolate(const struct t736_phy_io *io, uint32_t timeout)
{
	int ret;

	ret = clock_modify(io, T736_DRAM0_CLK, CLK_GATE, 0, timeout);
	if (ret)
		return ret;
	ret = bus_modify(io, T736_DRAM0_BGR, BUS_GATE, 0);
	if (ret)
		return ret;
	return bus_modify(io, T736_DRAM0_BGR, BUS_RESET_RELEASE, 0);
}

static int configure(const struct t736_phy_io *io, uint32_t timeout)
{
	/* DRAM0 stays gated while source/divider changes. Independent fabric
	 * sources/dividers/gates/resets are all preserved, not reconfigured.
	 */
	return clock_modify(io, T736_DRAM0_CLK,
			    CLK_SOURCE | CLK_DIV | DRAM_LOW_SPEED, 3u, timeout);
}

static int enable_buses(const struct t736_phy_io *io)
{
	int ret;

	/* Manual 4.1.4.4: release reset before passing the bus gate. */
	ret = bus_modify(io, T736_DRAM0_BGR, 0, BUS_RESET_RELEASE);
	if (ret)
		return ret;
	return bus_modify(io, T736_DRAM0_BGR, 0, BUS_GATE);
}

int t736_phy_ccu_cold_start(const struct t736_phy_io *io, uint32_t timeout,
			    struct t736_phy_ccu_report *report)
{
	int ret;

	if (!report)
		return T736_PHY_INVALID;
	*report = (struct t736_phy_ccu_report){T736_CCU_NOT_STARTED, T736_PHY_INVALID};
	if (!io || !io->read32 || !io->write32 || !io->delay_us ||
	    !timeout || timeout > 1000000u)
		return T736_PHY_INVALID;
	ret = preflight(io);
	if (ret)
		goto out;
	report->completed_stage = T736_CCU_PREFLIGHT_PASSED;
	ret = isolate(io, timeout);
	if (ret)
		goto out;
	report->completed_stage = T736_CCU_CONSUMERS_ISOLATED;
	ret = t736_phy_start_pll_1800(io, timeout);
	if (ret)
		goto out;
	report->completed_stage = T736_CCU_PLL_LOCKED;
	ret = configure(io, timeout);
	if (ret)
		goto out;
	report->completed_stage = T736_CCU_CLOCK_SOURCES_CONFIGURED;
	ret = enable_buses(io);
	if (ret)
		goto out;
	report->completed_stage = T736_CCU_CFG_BUSES_ENABLED;
	ret = clock_modify(io, T736_DRAM0_CLK, 0, CLK_GATE, timeout);
	if (ret)
		goto out;
	report->completed_stage = T736_CCU_CLOCKS_ENABLED;
out:
	report->result = ret;
	return ret;
}
