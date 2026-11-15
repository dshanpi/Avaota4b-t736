// SPDX-License-Identifier: GPL-2.0-only
/* Source-authored host components; evidence and scope in provenance.json. */
#include "host.h"
#include "../controller.h"
#include "../timing.h"

#define BIT(n) (UINT32_C(1) << (n))
#define PLL_ENABLE BIT(31)
#define PLL_LDO BIT(30)
#define PLL_LOCK_ENABLE BIT(29)
#define PLL_LOCK BIT(28)
#define PLL_OUTPUT BIT(27)
#define PLL_T736_OPAQUE24 BIT(24)
#define PLL_P_MASK (UINT32_C(7) << 20)
#define PLL_N_MASK (UINT32_C(255) << 8)
#define PLL_M_MASK BIT(1)
#define PLL_READY (PLL_ENABLE | PLL_LDO | PLL_LOCK_ENABLE | PLL_LOCK | PLL_OUTPUT)
#define PLL_PARAMETER_MASK (PLL_P_MASK | PLL_N_MASK | PLL_M_MASK | PLL_T736_OPAQUE24)
#define CLK_GATE BIT(31)
#define CLK_UPDATE BIT(27)
#define CLK_SOURCE_MASK (UINT32_C(7) << 24)
#define CLK_LOW_SPEED BIT(16)
#define CLK_DFS BIT(28)
#define CLK_DIV_MASK UINT32_C(0x1f)
#define BUS_READY (BIT(16) | BIT(0))
#define OBSERVED_MSTR UINT32_C(0x03081002)
#define DFIMISC_PSTATE_MASK UINT32_C(0x1f00)

static int valid32(const struct t736_phy_io *io)
{
	return io && io->read32 && io->write32 && io->delay_us;
}

static int valid_all(const struct t736_phy_io *io)
{
	return valid32(io) && io->read16 && io->write16;
}

static int timeout_valid(uint32_t timeout)
{
	return timeout && timeout <= 1000000u;
}

static int read32(const struct t736_phy_io *io, uint32_t addr, uint32_t *v)
{
	return io->read32(io->context, addr, v) ? T736_PHY_IO : T736_PHY_OK;
}

static int write32(const struct t736_phy_io *io, uint32_t addr, uint32_t v)
{
	return io->write32(io->context, addr, v) ? T736_PHY_IO : T736_PHY_OK;
}

static int write_verify32(const struct t736_phy_io *io, uint32_t addr,
			  uint32_t value, uint32_t mask)
{
	uint32_t readback;
	int ret = write32(io, addr, value);

	if (ret)
		return ret;
	ret = read32(io, addr, &readback);
	if (ret)
		return ret;
	return (readback & mask) == (value & mask) ? T736_PHY_OK : T736_PHY_VERIFY;
}

static int poll32(const struct t736_phy_io *io, uint32_t addr,
		  uint32_t mask, uint32_t expected, uint32_t timeout)
{
	uint32_t elapsed = 0, value;
	int ret;

	for (;;) {
		ret = read32(io, addr, &value);
		if (ret)
			return ret;
		if ((value & mask) == expected)
			return T736_PHY_OK;
		if (elapsed == timeout)
			return T736_PHY_TIMEOUT;
		if (io->delay_us(io->context, 1u))
			return T736_PHY_TIMER;
		elapsed++;
	}
}

static int qualify_pll_lock(const struct t736_phy_io *io, uint32_t timeout)
{
	uint32_t elapsed = 0, stable = 0, value;
	int ret;

	/* Manual 4.1.4.1 calls for checking lock three times. Require three
	 * consecutive observations separated by confirmed 1us intervals. This
	 * qualifies status only; no undocumented analog adjustment is guessed.
	 */
	for (;;) {
		ret = read32(io, T736_PLL_DDR, &value);
		if (ret)
			return ret;
		stable = (value & PLL_LOCK) ? stable + 1u : 0u;
		if (stable == 3u)
			return T736_PHY_OK;
		if (elapsed == timeout)
			return T736_PHY_TIMEOUT;
		if (io->delay_us(io->context, 1u))
			return T736_PHY_TIMER;
		elapsed++;
	}
}

int t736_phy_observe_host(const struct t736_phy_io *io,
			 struct t736_phy_host_state *state)
{
	if (!io || !io->read16 || !state)
		return T736_PHY_INVALID;
	*state = (struct t736_phy_host_state){0, 0, 0, 0};
	if (io->read16(io->context, T736_PHY_MUX, &state->mux))
		return T736_PHY_IO;
	state->mux_valid = 1;
	if (state->mux != 0)
		return T736_PHY_NOT_OWNER;
	if (io->read16(io->context, T736_PHY_MICRO_RESET, &state->micro_reset))
		return T736_PHY_IO;
	state->micro_reset_valid = 1;
	return T736_PHY_OK;
}

int t736_phy_pll_frequency(uint32_t pll, uint32_t *mhz)
{
	uint32_t n, divisor, numerator;

	if (!mhz)
		return T736_PHY_INVALID;
	if ((pll & PLL_READY) != PLL_READY)
		return T736_PHY_BUSY;
	n = ((pll & PLL_N_MASK) >> 8) + 1u;
	if (n < 12u || n > 255u)
		return T736_PHY_UNSUPPORTED;
	divisor = (((pll & PLL_P_MASK) >> 20) + 1u) *
		  (((pll & PLL_M_MASK) >> 1) + 1u);
	numerator = 24u * n;
	if (numerator % divisor)
		return T736_PHY_UNSUPPORTED;
	*mhz = numerator / divisor;
	return T736_PHY_OK;
}

int t736_phy_start_pll_1800(const struct t736_phy_io *io, uint32_t timeout)
{
	uint32_t value, pll;
	int ret;

	if (!valid32(io) || !timeout_valid(timeout))
		return T736_PHY_INVALID;
	/* Read only CCU registers while the DRAM0 domain is stopped. */
	ret = read32(io, T736_DRAM0_CLK, &value);
	if (ret)
		return ret;
	if (value & (CLK_GATE | CLK_UPDATE))
		return T736_PHY_BUSY;
	ret = read32(io, T736_DRAM0_BGR, &value);
	if (ret)
		return ret;
	if (value & BIT(0))
		return T736_PHY_BUSY;
	/* The main NSI clock also serves the interconnect. BootROM's SYS24M
	 * and the observed PERI0_600M source do not consume DDRPLL. Keep NSI
	 * and its configuration gate/reset untouched. The same applies to
	 * MBUS SYS24M/observed PERI0_400M. This does not initialize PERI0 PLL.
	 */
	ret = read32(io, T736_NSI_CLK, &value);
	if (ret)
		return ret;
	if (value & (CLK_UPDATE | CLK_DFS))
		return T736_PHY_BUSY;
	if ((value & CLK_SOURCE_MASK) != 0 &&
	    (value & CLK_SOURCE_MASK) != (3u << 24))
		return T736_PHY_UNSUPPORTED;
	ret = read32(io, T736_MBUS_CLK, &value);
	if (ret)
		return ret;
	if (value & (CLK_UPDATE | CLK_DFS))
		return T736_PHY_BUSY;
	if ((value & CLK_SOURCE_MASK) != 0 &&
	    (value & CLK_SOURCE_MASK) != (4u << 24))
		return T736_PHY_UNSUPPORTED;
	ret = read32(io, T736_PLL_DDR, &pll);
	if (ret)
		return ret;
	/* Manual disable order: output first, then PLL/lock detection. Never
	 * forward the read-only lock bit in a write. Keep other fields.
	 */
	pll &= ~(PLL_OUTPUT | PLL_LOCK);
	ret = write_verify32(io, T736_PLL_DDR, pll, PLL_OUTPUT);
	if (ret)
		return ret;
	pll &= ~(PLL_ENABLE | PLL_LOCK_ENABLE);
	ret = write_verify32(io, T736_PLL_DDR, pll,
			     PLL_ENABLE | PLL_OUTPUT | PLL_LOCK_ENABLE);
	if (ret)
		return ret;
	ret = poll32(io, T736_PLL_DDR, PLL_LOCK, 0, timeout);
	if (ret)
		return ret;
	/* Enable LDO before changing divider parameters and enabling the PLL. */
	pll |= PLL_LDO;
	ret = write_verify32(io, T736_PLL_DDR, pll, PLL_LDO | PLL_ENABLE | PLL_OUTPUT);
	if (ret)
		return ret;
	if (io->delay_us(io->context, 5u))
		return T736_PHY_TIMER;
	/* 1800 = 24 * 75 / 1 / 1. No LP5 frequency/latency preset is used. */
	pll = (pll & ~PLL_PARAMETER_MASK) | PLL_T736_OPAQUE24 | (74u << 8);
	ret = write_verify32(io, T736_PLL_DDR, pll, PLL_PARAMETER_MASK);
	if (ret)
		return ret;
	pll |= PLL_ENABLE;
	ret = write_verify32(io, T736_PLL_DDR, pll, PLL_LDO | PLL_ENABLE | PLL_OUTPUT);
	if (ret)
		return ret;
	pll |= PLL_LOCK_ENABLE;
	ret = write_verify32(io, T736_PLL_DDR, pll, PLL_LOCK_ENABLE | PLL_OUTPUT);
	if (ret)
		return ret;
	ret = qualify_pll_lock(io, timeout);
	if (ret)
		return ret;
	/* Method 2 requires 20us AFTER lock, even if acquisition took longer
	 * than 20us. Downstream clocks remain gated through output enable.
	 */
	if (io->delay_us(io->context, 20u))
		return T736_PHY_TIMER;
	ret = read32(io, T736_PLL_DDR, &value);
	if (ret)
		return ret;
	if (!(value & PLL_LOCK))
		return T736_PHY_VERIFY;
	pll |= PLL_OUTPUT;
	return write_verify32(io, T736_PLL_DDR, pll,
			      (PLL_READY & ~PLL_LOCK) | PLL_PARAMETER_MASK);
}

static int cold_controller(const struct t736_phy_io *io)
{
	uint32_t value, mhz, ch;
	int ret;

	ret = read32(io, T736_DRAM0_BGR, &value);
	if (ret)
		return ret;
	if ((value & BUS_READY) != BUS_READY)
		return T736_PHY_BUSY;
	ret = read32(io, T736_PLL_DDR, &value);
	if (ret)
		return ret;
	ret = t736_phy_pll_frequency(value, &mhz);
	if (ret)
		return ret;
	if (mhz != 1800u)
		return T736_PHY_UNSUPPORTED;
	ret = read32(io, T736_DRAM0_CLK, &value);
	if (ret)
		return ret;
	if (!(value & CLK_GATE) || (value & CLK_UPDATE))
		return T736_PHY_BUSY;
	if (value & CLK_LOW_SPEED)
		return T736_PHY_UNSUPPORTED;
	for (ch = 0; ch < 2u; ch++) {
		uint32_t ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		ret = read32(io, ctl + T736_CTL_STAT, &value);
		if (ret)
			return ret;
		if ((value & 7u) != 0)
			return T736_PHY_BUSY;
		ret = read32(io, ctl + T736_CTL_MSTR, &value);
		if (ret)
			return ret;
		if (value != OBSERVED_MSTR)
			return T736_PHY_UNSUPPORTED;
	}
	return T736_PHY_OK;
}

int t736_phy_cold_claim(const struct t736_phy_io *io)
{
	struct t736_phy_host_state state;
	uint16_t owner;
	int ret;

	if (!valid_all(io))
		return T736_PHY_INVALID;
	ret = cold_controller(io);
	if (ret)
		return ret;
	if (io->read16(io->context, T736_PHY_MUX, &owner))
		return T736_PHY_IO;
	if (owner > 1u)
		return T736_PHY_UNSUPPORTED;
	/* MicroReset is in the host control aperture and writable with mux=1,
	 * as corroborated by both host drivers' reset/start/abort sequences.
	 * Do not steal the PHY CSR bus from a core that can still run.
	 */
	if (io->write16(io->context, T736_PHY_MICRO_RESET, T736_PHY_RESET_STALL))
		return T736_PHY_IO;
	/* This cold-only read verifies halt before changing bus ownership. The
	 * conservative live observer above intentionally does not do this read
	 * when mux=1, since it is never allowed to disrupt a running system.
	 */
	if (io->read16(io->context, T736_PHY_MICRO_RESET, &owner))
		return T736_PHY_IO;
	if (owner != T736_PHY_RESET_STALL)
		return T736_PHY_VERIFY;
	if (io->write16(io->context, T736_PHY_MUX, 0))
		return T736_PHY_IO;
	ret = t736_phy_observe_host(io, &state);
	if (ret)
		return ret;
	return state.micro_reset == T736_PHY_RESET_STALL ? T736_PHY_OK : T736_PHY_VERIFY;
}

static int prepared(const struct t736_phy_io *io)
{
	struct t736_phy_host_state state;
	int ret = cold_controller(io);

	if (ret)
		return ret;
	ret = t736_phy_observe_host(io, &state);
	if (ret)
		return ret;
	if (state.micro_reset != T736_PHY_RESET_STALL)
		return T736_PHY_BUSY;
	return T736_PHY_OK;
}

int t736_phy_set_dq_selectors(const struct t736_phy_io *io,
			    const uint8_t selectors[4][8])
{
	uint32_t slice, bit;
	int ret;

	if (!valid_all(io) || !selectors)
		return T736_PHY_INVALID;
	for (slice = 0; slice < 4u; slice++) {
		uint32_t used = 0;
		for (bit = 0; bit < 8u; bit++) {
			uint32_t selector = selectors[slice][bit];
			if (selector > 7u || (used & BIT(selector)))
				return T736_PHY_INVALID;
			used |= BIT(selector);
		}
	}
	ret = prepared(io);
	if (ret)
		return ret;
	for (slice = 0; slice < 4u; slice++) {
		for (bit = 0; bit < 8u; bit++) {
			uint32_t addr = T736_PHY_DQ_SELECTOR0 +
				slice * T736_PHY_DBYTE_STRIDE + bit * 2u;
			uint16_t value;
			if (io->write16(io->context, addr, selectors[slice][bit]) ||
			    io->read16(io->context, addr, &value))
				return T736_PHY_IO;
			if (value != selectors[slice][bit])
				return T736_PHY_VERIFY;
		}
	}
	return T736_PHY_OK;
}

static int swctl(const struct t736_phy_io *io, uint32_t ctl,
		 uint32_t commit, uint32_t timeout)
{
	int ret = write32(io, ctl + T736_CTL_SWCTL, commit);

	if (ret)
		return ret;
	return poll32(io, ctl + T736_CTL_SWSTAT, 1u, commit, timeout);
}

int t736_phy_select_pstate0(const struct t736_phy_io *io, uint32_t timeout)
{
	uint32_t ch, value, expected;
	int ret;

	if (!valid_all(io) || !timeout_valid(timeout))
		return T736_PHY_INVALID;
	ret = prepared(io);
	if (ret)
		return ret;
	for (ch = 0; ch < 2u; ch++) {
		uint32_t ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		ret = swctl(io, ctl, 0, timeout);
		if (ret)
			return ret;
		ret = read32(io, ctl + T736_CTL_DFIMISC, &value);
		if (ret)
			return ret;
		value &= ~DFIMISC_PSTATE_MASK;
		ret = write_verify32(io, ctl + T736_CTL_DFIMISC, value, UINT32_MAX);
		if (ret)
			return ret;
		ret = swctl(io, ctl, 1, timeout);
		if (ret)
			return ret;
	}
	ret = read32(io, T736_DRAM0_CLK, &value);
	if (ret)
		return ret;
	expected = (value & ~(CLK_SOURCE_MASK | CLK_DIV_MASK | CLK_UPDATE)) | 3u;
	ret = write32(io, T736_DRAM0_CLK, expected | CLK_UPDATE);
	if (ret)
		return ret;
	ret = poll32(io, T736_DRAM0_CLK, CLK_UPDATE, 0, timeout);
	if (ret)
		return ret;
	ret = read32(io, T736_DRAM0_CLK, &value);
	if (ret)
		return ret;
	return value == expected ? T736_PHY_OK : T736_PHY_VERIFY;
}
