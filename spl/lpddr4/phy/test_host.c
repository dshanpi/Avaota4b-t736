// SPDX-License-Identifier: GPL-2.0-only
#include "host.h"
#include "clocks.h"
#include "../controller.h"
#include "../timing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef T736_CCU_BOARD_OBSERVATION
#include "ccu_board_observed.h"
#endif

#define B(n) (UINT32_C(1) << (n))
struct reg { uint32_t address, value; };
struct event { uint32_t address, value; unsigned int width, write; };
struct mock {
	struct reg regs[64];
	struct event events[1024];
	unsigned int count, event_count, io_count, fail_io;
	unsigned int delay_count, fail_delay;
	uint32_t elapsed, pll_started, update_started, pll_delay, pll_locked_at;
	int pll_never_locks, pll_lock_stuck, update_stuck, swstat_stuck;
	int update_requires_pll;
	int lose_lock_after_settle;
	uint32_t drop_write_address;
};

static uint32_t *slot(struct mock *m, uint32_t address)
{
	unsigned int i;
	for (i = 0; i < m->count; i++)
		if (m->regs[i].address == address)
			return &m->regs[i].value;
	assert(m->count < 64u);
	m->regs[m->count].address = address;
	m->regs[m->count].value = 0;
	return &m->regs[m->count++].value;
}

static int event(struct mock *m, uint32_t addr, uint32_t value,
		 unsigned int width, unsigned int write)
{
	assert(m->event_count < 1024u);
	m->events[m->event_count++] = (struct event){addr, value, width, write};
	m->io_count++;
	return m->fail_io && m->io_count == m->fail_io ? -1 : 0;
}

static int r16(void *ctx, uint32_t addr, uint16_t *value)
{
	struct mock *m = ctx;
	assert((addr & 1u) == 0);
	if (event(m, addr, 0, 16, 0))
		return -1;
	*value = (uint16_t)*slot(m, addr);
	return 0;
}

static int w16(void *ctx, uint32_t addr, uint16_t value)
{
	struct mock *m = ctx;
	assert((addr & 1u) == 0);
	if (event(m, addr, value, 16, 1))
		return -1;
	if (addr == m->drop_write_address)
		return 0;
	if (addr == T736_PHY_MUX && value == 0)
		assert(*slot(m, T736_PHY_MICRO_RESET) == T736_PHY_RESET_STALL);
	*slot(m, addr) = value;
	return 0;
}

static int r32(void *ctx, uint32_t addr, uint32_t *value)
{
	struct mock *m = ctx;
	uint32_t *p;
	assert((addr & 3u) == 0);
	if (event(m, addr, 0, 32, 0))
		return -1;
	p = slot(m, addr);
	if (addr == T736_PLL_DDR) {
		if (m->pll_lock_stuck)
			*p |= B(28);
		else if ((*p & (B(31) | B(29))) == (B(31) | B(29)) &&
			 !m->pll_never_locks && m->elapsed - m->pll_started >= m->pll_delay) {
			if (!(*p & B(28)))
				m->pll_locked_at = m->elapsed;
			*p |= B(28);
		}
	}
	if ((addr == T736_DRAM0_CLK || addr == T736_MBUS_CLK || addr == T736_NSI_CLK) &&
	    !m->update_stuck &&
	    (!m->update_requires_pll || addr != T736_DRAM0_CLK ||
	     (*slot(m, T736_PLL_DDR) & (B(31) | B(27))) == (B(31) | B(27))) &&
	    m->elapsed - m->update_started >= 3u)
		*p &= ~B(27);
	*value = *p;
	return 0;
}

static int w32(void *ctx, uint32_t addr, uint32_t value)
{
	struct mock *m = ctx;
	uint32_t prior = *slot(m, addr);
	unsigned int ch;
	assert((addr & 3u) == 0);
	if (event(m, addr, value, 32, 1))
		return -1;
	if (addr == m->drop_write_address)
		return 0;
	if (addr == T736_PLL_DDR) {
		assert((value & B(28)) == 0); /* RO lock must not be written. */
		if ((value ^ prior) & (UINT32_C(0x0070ff02) | B(24))) {
			assert(prior & B(30));
			assert(!(value & (B(31) | B(27))));
		}
		if (value & B(27)) {
			assert(prior & B(28)); /* Output cannot precede real lock. */
			assert(m->pll_locked_at != UINT32_MAX);
			assert(m->elapsed - m->pll_locked_at >= 20u);
		}
		if ((value & B(31)) && !(prior & B(31)))
			assert(prior & B(30)); /* LDO precedes PLL enable. */
		if (!(value & B(31))) {
			value &= ~B(28);
			m->pll_locked_at = UINT32_MAX;
		} else
			value |= prior & B(28);
		if (!(prior & B(29)) && (value & B(29)))
			m->pll_started = m->elapsed;
	}
	if (addr == T736_NSI_CLK) {
		/* The complete CCU path may never gate/reset the main interconnect. */
		assert((value & (B(31) | B(30))) == (B(31) | B(30)));
		assert((value & (7u << 24)) == 0);
	}
	*slot(m, addr) = value;
	if ((addr == T736_DRAM0_CLK || addr == T736_MBUS_CLK || addr == T736_NSI_CLK) &&
	    (value & B(27)))
		m->update_started = m->elapsed;
	for (ch = 0; ch < 2; ch++) {
		uint32_t ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		if (addr == ctl + T736_CTL_SWCTL && !m->swstat_stuck)
			*slot(m, ctl + T736_CTL_SWSTAT) = value;
	}
	return 0;
}

static int delay(void *ctx, uint32_t us)
{
	struct mock *m = ctx;
	m->delay_count++;
	if (m->delay_count == m->fail_delay)
		return -1;
	m->elapsed += us;
	if (us == 20u && m->lose_lock_after_settle) {
		*slot(m, T736_PLL_DDR) &= ~B(28);
		m->pll_never_locks = 1;
	}
	return 0;
}

static struct t736_phy_io io_for(struct mock *m)
{
	return (struct t736_phy_io){m, r16, w16, r32, w32, delay};
}

static void setup(struct mock *m, int gated)
{
	unsigned int ch;
	memset(m, 0, sizeof(*m));
	m->pll_delay = 3;
	*slot(m, T736_PLL_DDR) = UINT32_C(0xf9004a00);
	*slot(m, T736_DRAM0_CLK) = gated ? 0 : UINT32_C(0x80000003);
	*slot(m, T736_DRAM0_BGR) = gated ? 0 : UINT32_C(0x10001);
	*slot(m, T736_NSI_CLK) = UINT32_C(0xc0000000);
	*slot(m, T736_NSI_BGR) = 0;
	*slot(m, T736_MBUS_CLK) = 0;
	*slot(m, T736_PHY_MUX) = 0;
	*slot(m, T736_PHY_MICRO_RESET) = T736_PHY_RESET_STALL;
	for (ch = 0; ch < 2; ch++) {
		uint32_t ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		*slot(m, ctl + T736_CTL_MSTR) = UINT32_C(0x03081002);
		*slot(m, ctl + T736_CTL_STAT) = 0;
		*slot(m, ctl + T736_CTL_SWSTAT) = 1;
		*slot(m, ctl + T736_CTL_DFIMISC) = UINT32_C(0xa5a55f21);
	}
}

static unsigned int writes(const struct mock *m)
{
	unsigned int i, n = 0;
	for (i = 0; i < m->event_count; i++)
		n += m->events[i].write;
	return n;
}

static void test_observe(void)
{
	struct mock m;
	struct t736_phy_io io = io_for(&m);
	struct t736_phy_host_state state;
	uint32_t owner;
	for (owner = 0; owner < 4; owner++) {
		setup(&m, 0);
		*slot(&m, T736_PHY_MUX) = owner;
		assert(t736_phy_observe_host(&io, &state) ==
		       (owner ? T736_PHY_NOT_OWNER : T736_PHY_OK));
		assert(state.mux_valid == 1 && state.mux == owner);
		assert(state.micro_reset_valid == (owner ? 0 : 1));
		assert(m.event_count == (owner ? 1u : 2u));
		assert(writes(&m) == 0);
	}
	setup(&m, 0);
	m.fail_io = 1;
	assert(t736_phy_observe_host(&io, &state) == T736_PHY_IO);
	assert(state.mux_valid == 0 && state.micro_reset_valid == 0);
	setup(&m, 0);
	m.fail_io = 2;
	assert(t736_phy_observe_host(&io, &state) == T736_PHY_IO);
	assert(state.mux_valid == 1 && state.micro_reset_valid == 0);
	assert(t736_phy_observe_host(0, &state) == T736_PHY_INVALID);
}

static void test_decode(void)
{
	uint32_t mhz = 99;
	assert(t736_phy_pll_frequency(UINT32_C(0xf9004a00), &mhz) == 0 && mhz == 1800);
	/* Opaque bit24 is not falsely used as a multiplier/divider. */
	assert(t736_phy_pll_frequency(UINT32_C(0xf8004a00), &mhz) == 0 && mhz == 1800);
	assert(t736_phy_pll_frequency(UINT32_C(0xf9104a02), &mhz) == 0 && mhz == 450);
	assert(t736_phy_pll_frequency(UINT32_C(0xf8604a02), &mhz) == T736_PHY_UNSUPPORTED);
	assert(mhz == 450);
	assert(t736_phy_pll_frequency(UINT32_C(0xe9004a00), &mhz) == T736_PHY_BUSY);
	assert(t736_phy_pll_frequency(UINT32_C(0xf900ff00), &mhz) == T736_PHY_UNSUPPORTED);
	assert(t736_phy_pll_frequency(UINT32_C(0xf9000a00), &mhz) == T736_PHY_UNSUPPORTED);
	assert(t736_phy_pll_frequency(UINT32_C(0xf9004a00), 0) == T736_PHY_INVALID);
}

static void test_pll(void)
{
	struct mock m;
	struct t736_phy_io io = io_for(&m);
	unsigned int operations, n, delays;
	uint32_t mhz;
	const uint32_t guards[] = {T736_DRAM0_CLK, T736_DRAM0_BGR};
	setup(&m, 1);
	*slot(&m, T736_PLL_DDR) |= B(23); /* Unrelated field must be preserved. */
	assert(t736_phy_start_pll_1800(&io, 10) == 0);
	assert(m.elapsed == 30 && writes(&m) == 7);
	assert(*slot(&m, T736_PLL_DDR) & B(23));
	assert(t736_phy_pll_frequency(*slot(&m, T736_PLL_DDR), &mhz) == 0 && mhz == 1800);
	operations = m.io_count;
	delays = m.delay_count;
	for (n = 1; n <= operations; n++) {
		setup(&m, 1);
		m.fail_io = n;
		assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_IO);
		assert(m.io_count == n); /* Never execute later writes after IO error. */
	}
	for (n = 0; n < 2; n++) {
		setup(&m, 1);
		*slot(&m, guards[n]) = n == 0 ? B(31) : 1u;
		assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_BUSY);
		assert(writes(&m) == 0);
	}
	setup(&m, 1);
	*slot(&m, T736_NSI_CLK) |= B(24); /* NSI using the PLL being changed. */
	assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_UNSUPPORTED);
	assert(writes(&m) == 0);
	setup(&m, 1);
	*slot(&m, T736_MBUS_CLK) = UINT32_C(0x82000000);
	assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_UNSUPPORTED);
	assert(writes(&m) == 0);
	setup(&m, 1);
	*slot(&m, T736_NSI_CLK) = UINT32_C(0xc3000000);
	*slot(&m, T736_NSI_BGR) = UINT32_C(0x00010001);
	*slot(&m, T736_MBUS_CLK) = UINT32_C(0x84000000);
	assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_OK);
	assert(*slot(&m, T736_NSI_CLK) == UINT32_C(0xc3000000));
	assert(*slot(&m, T736_NSI_BGR) == UINT32_C(0x00010001));
	assert(*slot(&m, T736_MBUS_CLK) == UINT32_C(0x84000000));
	setup(&m, 1);
	*slot(&m, T736_NSI_CLK) |= B(28);
	assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_BUSY);
	assert(writes(&m) == 0);
	setup(&m, 1);
	*slot(&m, T736_MBUS_CLK) |= B(28);
	assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_BUSY);
	assert(writes(&m) == 0);
	setup(&m, 1);
	m.pll_never_locks = 1;
	assert(t736_phy_start_pll_1800(&io, 7) == T736_PHY_TIMEOUT);
	assert(m.elapsed == 12 && !(*slot(&m, T736_PLL_DDR) & B(27)));
	setup(&m, 1);
	m.pll_lock_stuck = 1;
	assert(t736_phy_start_pll_1800(&io, 7) == T736_PHY_TIMEOUT);
	assert(m.elapsed == 7 && writes(&m) == 2);
	/* LDO settling, each lock qualification wait and post-lock 20us. */
	for (n = 1; n <= delays; n++) {
		unsigned int expected_writes = n == 1 ? 3u : 6u;
		setup(&m, 1);
		m.fail_delay = n;
		assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_TIMER);
		assert(m.delay_count == n && writes(&m) == expected_writes);
		assert(!(*slot(&m, T736_PLL_DDR) & B(27)));
	}
	setup(&m, 1);
	m.pll_delay = 23;
	assert(t736_phy_start_pll_1800(&io, 30) == T736_PHY_OK);
	assert(m.elapsed == 50 && m.elapsed - m.pll_locked_at >= 20u);
	setup(&m, 1);
	m.lose_lock_after_settle = 1;
	assert(t736_phy_start_pll_1800(&io, 10) == T736_PHY_VERIFY);
	assert(writes(&m) == 6 && !(*slot(&m, T736_PLL_DDR) & B(27)));
	setup(&m, 1);
	assert(t736_phy_start_pll_1800(&io, 4) == T736_PHY_TIMEOUT);
	assert(!(*slot(&m, T736_PLL_DDR) & B(27))); /* Only two lock samples. */
	setup(&m, 1);
	m.drop_write_address = T736_PLL_DDR;
	assert(t736_phy_start_pll_1800(&io, 7) == T736_PHY_VERIFY);
	setup(&m, 1);
	assert(t736_phy_start_pll_1800(&io, 0) == T736_PHY_INVALID);
	assert(t736_phy_start_pll_1800(&io, 1000001) == T736_PHY_INVALID);
	assert(m.event_count == 0);
}

static void test_claim(void)
{
	struct mock m;
	struct t736_phy_io io = io_for(&m);
	unsigned int n, operations;
	setup(&m, 0);
	*slot(&m, T736_PHY_MUX) = 1;
	*slot(&m, T736_PHY_MICRO_RESET) = 0;
	assert(t736_phy_cold_claim(&io) == 0);
	assert(*slot(&m, T736_PHY_MUX) == 0 && *slot(&m, T736_PHY_MICRO_RESET) == 9);
	assert(writes(&m) == 2);
	operations = m.io_count;
	for (n = 1; n <= operations; n++) {
		setup(&m, 0);
		m.fail_io = n;
		assert(t736_phy_cold_claim(&io) == T736_PHY_IO);
		assert(m.io_count == n);
	}
	setup(&m, 0);
	*slot(&m, T736_PHY_MUX) = 1;
	*slot(&m, T736_PHY_MICRO_RESET) = 0;
	m.drop_write_address = T736_PHY_MICRO_RESET;
	assert(t736_phy_cold_claim(&io) == T736_PHY_VERIFY);
	assert(*slot(&m, T736_PHY_MUX) == 1 && writes(&m) == 1);
	setup(&m, 0);
	*slot(&m, T736_PHY_MUX) = 1;
	m.drop_write_address = T736_PHY_MUX;
	assert(t736_phy_cold_claim(&io) == T736_PHY_NOT_OWNER);
	setup(&m, 0);
	*slot(&m, T736_CTL0_BASE + T736_CHANNEL_STRIDE + T736_CTL_STAT) = 1;
	assert(t736_phy_cold_claim(&io) == T736_PHY_BUSY && writes(&m) == 0);
	setup(&m, 0);
	*slot(&m, T736_CTL0_BASE + T736_CTL_MSTR) = UINT32_C(0x03080010);
	assert(t736_phy_cold_claim(&io) == T736_PHY_UNSUPPORTED && writes(&m) == 0);
}

static void test_dq(void)
{
	struct mock m;
	struct t736_phy_io io = io_for(&m);
	uint8_t mapping[4][8];
	unsigned int s, b, n, operations;
	for (s = 0; s < 4; s++)
		for (b = 0; b < 8; b++)
			mapping[s][b] = (uint8_t)((b + s) % 8);
	setup(&m, 0);
	assert(t736_phy_set_dq_selectors(&io, mapping) == 0);
	assert(writes(&m) == 32);
	for (s = 0; s < 4; s++)
		for (b = 0; b < 8; b++)
			assert(*slot(&m, T736_PHY_DQ_SELECTOR0 + s * 0x2000u + b * 2u) == mapping[s][b]);
	for (n = 0; n < m.event_count; n++)
		if (m.events[n].write)
			assert(m.events[n].width == 16);
	operations = m.io_count;
	for (n = 1; n <= operations; n++) {
		setup(&m, 0);
		m.fail_io = n;
		assert(t736_phy_set_dq_selectors(&io, mapping) == T736_PHY_IO);
		assert(m.io_count == n);
	}
	setup(&m, 0);
	mapping[3][7] = mapping[3][6];
	assert(t736_phy_set_dq_selectors(&io, mapping) == T736_PHY_INVALID);
	assert(m.event_count == 0);
	mapping[3][7] = 8;
	assert(t736_phy_set_dq_selectors(&io, mapping) == T736_PHY_INVALID);
	assert(m.event_count == 0);
	mapping[3][7] = 2;
	setup(&m, 0);
	*slot(&m, T736_PHY_MUX) = 1;
	assert(t736_phy_set_dq_selectors(&io, mapping) == T736_PHY_NOT_OWNER && writes(&m) == 0);
	setup(&m, 0);
	*slot(&m, T736_PHY_MICRO_RESET) = 1;
	assert(t736_phy_set_dq_selectors(&io, mapping) == T736_PHY_BUSY && writes(&m) == 0);
	setup(&m, 0);
	m.drop_write_address = T736_PHY_DQ_SELECTOR0 + 2u;
	assert(t736_phy_set_dq_selectors(&io, mapping) == T736_PHY_VERIFY);
	assert(writes(&m) == 2);
}

static void test_pstate(void)
{
	struct mock m;
	struct t736_phy_io io = io_for(&m);
	unsigned int ch, n, operations;
	setup(&m, 0);
	*slot(&m, T736_DRAM0_CLK) = UINT32_C(0x82000011);
	assert(t736_phy_select_pstate0(&io, 7) == 0);
	assert(*slot(&m, T736_DRAM0_CLK) == UINT32_C(0x80000003));
	for (ch = 0; ch < 2; ch++) {
		uint32_t ctl = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		assert(*slot(&m, ctl + T736_CTL_DFIMISC) == UINT32_C(0xa5a54021));
		assert(*slot(&m, ctl + T736_CTL_SWSTAT) == 1);
	}
	assert(writes(&m) == 7 && m.elapsed == 3);
	operations = m.io_count;
	for (n = 1; n <= operations; n++) {
		setup(&m, 0);
		m.fail_io = n;
		assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_IO);
		assert(m.io_count == n);
	}
	setup(&m, 0);
	m.swstat_stuck = 1;
	assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_TIMEOUT);
	assert(m.elapsed == 7 && writes(&m) == 1);
	setup(&m, 0);
	m.swstat_stuck = 1;
	m.fail_delay = 1;
	assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_TIMER);
	assert(m.elapsed == 0 && writes(&m) == 1);
	setup(&m, 0);
	m.update_stuck = 1;
	assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_TIMEOUT);
	assert(m.elapsed == 7);
	for (n = 1; n <= 3; n++) {
		setup(&m, 0);
		m.fail_delay = n;
		assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_TIMER);
		assert(m.delay_count == n && writes(&m) == 7);
	}
	setup(&m, 0);
	*slot(&m, T736_DRAM0_CLK) |= B(16);
	assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_UNSUPPORTED && writes(&m) == 0);
	setup(&m, 0);
	*slot(&m, T736_PLL_DDR) = UINT32_C(0xf9003100);
	assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_UNSUPPORTED && writes(&m) == 0);
	setup(&m, 0);
	m.drop_write_address = T736_CTL0_BASE + T736_CTL_DFIMISC;
	assert(t736_phy_select_pstate0(&io, 7) == T736_PHY_VERIFY);
	assert(writes(&m) == 2);
}

static void setup_reset_ccu(struct mock *m)
{
	setup(m, 1);
	*slot(m, T736_NSI_CLK) = UINT32_C(0xc0000000);
	*slot(m, T736_NSI_BGR) = UINT32_C(0x00010001);
	*slot(m, T736_MBUS_CLK) = UINT32_C(0x80000000);
	*slot(m, T736_DRAM0_BGR) = UINT32_C(0x00000001);
	*slot(m, T736_PLL_DDR) = 0;
	/* Conservative req/ack model: a stopped DDRPLL cannot acknowledge a
	 * DRAM0 update. No redundant transaction is needed for the gated state.
	 */
	m->update_requires_pll = 1;
}

static void test_ccu(void)
{
	struct mock m;
	struct t736_phy_io io = io_for(&m);
	struct t736_phy_ccu_report report;
	unsigned int n, operations, delays;
	uint32_t mhz;
	setup_reset_ccu(&m);
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_OK);
	assert(report.completed_stage == T736_CCU_CLOCKS_ENABLED && report.result == 0);
	assert(*slot(&m, T736_NSI_CLK) == UINT32_C(0xc0000000));
	assert(*slot(&m, T736_MBUS_CLK) == UINT32_C(0x80000000));
	assert(*slot(&m, T736_DRAM0_CLK) == UINT32_C(0x80000003));
	assert(*slot(&m, T736_DRAM0_BGR) == UINT32_C(0x00010001));
	assert(*slot(&m, T736_NSI_BGR) == UINT32_C(0x00010001));
	assert(t736_phy_pll_frequency(*slot(&m, T736_PLL_DDR), &mhz) == 0 && mhz == 1800);
	assert(writes(&m) == 13u && m.elapsed == 36u);
	/* No PHY, DRAM/SMC, PMIC, memory, or storage write is part of CCU. */
	for (n = 0; n < m.event_count; n++) {
		const struct event *e = &m.events[n];
		if (!e->write)
			continue;
		assert(e->width == 32);
		assert(e->address == T736_DRAM0_CLK ||
		       e->address == T736_DRAM0_BGR || e->address == T736_PLL_DDR);
	}
	operations = m.io_count;
	delays = m.delay_count;
	printf("DDR CCU startup: %u IO fault positions, %u timer fault positions; 13 writes to three CCU registers; fabric unchanged\n",
	       operations, delays);
	for (n = 1; n <= operations; n++) {
		setup_reset_ccu(&m);
		m.fail_io = n;
		assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_IO);
		assert(report.result == T736_PHY_IO && report.completed_stage != T736_CCU_CLOCKS_ENABLED);
		assert(m.io_count == n);
	}
	for (n = 1; n <= delays; n++) {
		setup_reset_ccu(&m);
		m.fail_delay = n;
		assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_TIMER);
		assert(report.result == T736_PHY_TIMER && report.completed_stage != T736_CCU_CLOCKS_ENABLED);
		assert(m.delay_count == n);
	}
	setup_reset_ccu(&m);
	*slot(&m, T736_NSI_CLK) |= B(24);
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_UNSUPPORTED);
	assert(writes(&m) == 0 && report.completed_stage == T736_CCU_NOT_STARTED);
	setup_reset_ccu(&m);
	*slot(&m, T736_MBUS_CLK) = UINT32_C(0x82000000);
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_UNSUPPORTED && writes(&m) == 0);
	setup_reset_ccu(&m);
	*slot(&m, T736_NSI_BGR) = 1;
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_BUSY && writes(&m) == 0);
	setup_reset_ccu(&m);
	*slot(&m, T736_MBUS_CLK) &= ~B(31);
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_BUSY && writes(&m) == 0);
	setup_reset_ccu(&m);
	*slot(&m, T736_NSI_CLK) &= ~B(30);
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_BUSY && writes(&m) == 0);
	setup_reset_ccu(&m);
	*slot(&m, T736_MBUS_CLK) |= B(28);
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_UNSUPPORTED && writes(&m) == 0);
	setup_reset_ccu(&m);
	*slot(&m, T736_DRAM0_CLK) |= B(27);
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_BUSY && writes(&m) == 0);
	setup_reset_ccu(&m);
	*slot(&m, T736_DRAM0_BGR) = UINT32_C(0x10001);
	*slot(&m, T736_DRAM0_CLK) = UINT32_C(0x80000003);
	*slot(&m, T736_CTL0_BASE + T736_CHANNEL_STRIDE + T736_CTL_STAT) = 1;
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_BUSY && writes(&m) == 0);
	setup_reset_ccu(&m);
	m.update_stuck = 1;
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_TIMEOUT);
	assert(report.completed_stage == T736_CCU_PLL_LOCKED && writes(&m) == 10);
	setup_reset_ccu(&m);
	m.pll_never_locks = 1;
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_TIMEOUT);
	assert(report.completed_stage == T736_CCU_CONSUMERS_ISOLATED);
	assert(*slot(&m, T736_MBUS_CLK) == UINT32_C(0x80000000));
	assert(!(*slot(&m, T736_DRAM0_CLK) & B(31)));
	setup_reset_ccu(&m);
	m.drop_write_address = T736_DRAM0_BGR;
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_VERIFY);
	assert(report.completed_stage == T736_CCU_PREFLIGHT_PASSED);
	setup_reset_ccu(&m);
	m.drop_write_address = T736_DRAM0_CLK;
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_VERIFY);
	assert(report.completed_stage == T736_CCU_PLL_LOCKED);
	assert(t736_phy_ccu_cold_start(&io, 10, 0) == T736_PHY_INVALID);
	setup_reset_ccu(&m);
	assert(t736_phy_ccu_cold_start(&io, 0, &report) == T736_PHY_INVALID && m.io_count == 0);
#ifdef T736_CCU_BOARD_OBSERVATION
	setup_reset_ccu(&m);
	for (n = 0; n < sizeof(ccu_board_observed) / sizeof(ccu_board_observed[0]); n++)
		*slot(&m, ccu_board_observed[n].address) = ccu_board_observed[n].value;
	assert(t736_phy_ccu_cold_start(&io, 10, &report) == T736_PHY_OK);
	assert(report.completed_stage == T736_CCU_CLOCKS_ENABLED);
	assert(*slot(&m, T736_PLL_DDR) == UINT32_C(0xf9004a00));
	assert(*slot(&m, T736_DRAM0_CLK) == UINT32_C(0x80000003));
	assert(*slot(&m, T736_DRAM0_BGR) == UINT32_C(0x00010001));
	for (n = 0; n < sizeof(ccu_board_observed) / sizeof(ccu_board_observed[0]); n++) {
		uint32_t addr = ccu_board_observed[n].address;
		if (addr == T736_NSI_CLK || addr == T736_NSI_BGR || addr == T736_MBUS_CLK)
			assert(*slot(&m, addr) == ccu_board_observed[n].value);
	}
	puts("DDR CCU startup: actual 0x190300 early-FEL register state accepted; all three fabric values preserved");
#endif
}

int main(void)
{
	test_observe();
	test_decode();
	test_pll();
	test_claim();
	test_dq();
	test_pstate();
	test_ccu();
	puts("PHY host: cold CCU/PLL sequencing, ownership, reset ordering, DQ mapping, DFI selection, finite timeouts and exhaustive IO/timer fault positions passed");
	return 0;
}
