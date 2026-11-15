// SPDX-License-Identifier: GPL-2.0-only
#include "board_power.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

struct mock { uint8_t reg[256]; unsigned writes; int drop; };
static int read8(void *ctx, uint8_t reg, uint8_t *v)
{ *v = ((struct mock *)ctx)->reg[reg]; return 0; }
static int write8(void *ctx, uint8_t reg, uint8_t v)
{
	struct mock *m = ctx;
	assert(reg == 0x11 || reg == 0x22 || reg == 0x35);
	m->writes++;
	if (m->drop != reg) m->reg[reg] = v;
	return 0;
}
static void init(struct mock *m)
{
	memset(m, 0, sizeof(*m)); m->drop = -1;
	m->reg[0x0e] = 3; m->reg[0x10] = 0xff; m->reg[0x12] = 23;
	m->reg[0x11] = 0x11; m->reg[0x22] = 0x50; m->reg[0x35] = 0xa0;
}
int main(void)
{
	struct mock m; struct t736_board_power_report r;
	const struct t736_axp_bus bus = { &m, read8, write8 };
	init(&m); assert(t736_board_power_enable(&bus, &r) == 0);
	assert(m.reg[0x11] == 0x19 && m.reg[0x22] == 0x52 && m.reg[0x35] == 0xad);
	assert(m.reg[0x10] == 0xff && m.reg[0x12] == 23 && m.writes == 3);
	assert(t736_board_power_enable(&bus, &r) == 0 && m.writes == 3);
	init(&m); m.reg[0x12] = 18;
	assert(t736_board_power_enable(&bus, &r) == T736_AXP_RANGE && !m.writes);
	init(&m); m.reg[0x10] &= ~1;
	assert(t736_board_power_enable(&bus, &r) == T736_AXP_RANGE && !m.writes);
	init(&m); m.reg[0x22] |= 2;
	assert(t736_board_power_enable(&bus, &r) == T736_AXP_LIVE_RANGE_CHANGE && !m.writes);
	init(&m); m.reg[0x0e] = 0;
	assert(t736_board_power_enable(&bus, &r) == T736_AXP_ID && !m.writes);
	init(&m); m.drop = 0x35;
	assert(t736_board_power_enable(&bus, &r) == T736_AXP_VERIFY && m.writes == 1);
	assert(m.reg[0x22] == 0x50 && m.reg[0x11] == 0x11);
	puts("Board power guard, readback, DDR-bit preservation and idempotence passed");
}
