// SPDX-License-Identifier: GPL-2.0-only
#include "controller.h"
#include "axp8191.h"
#include "board_observed.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const struct t736_lp4_geometry geometry = {2, 2, 16, 17, 10, 3};

struct mmio_mock {
	struct { uint32_t address, value; } word[64];
	unsigned int used, writes, delay;
	uint32_t drop_address;
	int ack_after;
};

static uint32_t read32(void *context, uint32_t address)
{
	struct mmio_mock *m = context;
	unsigned int i;
	if (address == T736_CTL0_BASE + T736_CTL_SWSTAT && m->ack_after >= 0)
		return m->delay >= (unsigned int)m->ack_after;
	for (i = 0; i < m->used; i++)
		if (m->word[i].address == address)
			return m->word[i].value;
	return 0;
}

static void write32(void *context, uint32_t address, uint32_t value)
{
	struct mmio_mock *m = context;
	unsigned int i;
	m->writes++;
	if (address == m->drop_address)
		return;
	for (i = 0; i < m->used; i++)
		if (m->word[i].address == address) {
			m->word[i].value = value;
			return;
		}
	assert(m->used < 64);
	m->word[m->used].address = address;
	m->word[m->used++].value = value;
}

static void delay_us(void *context, uint32_t us)
{
	struct mmio_mock *m = context;
	m->delay += us;
}

static void test_controller(void)
{
	struct t736_lp4_layout out;
	struct t736_lp4_geometry bad;
	struct mmio_mock m = {.ack_after = -1};
	struct t736_lp4_io io = {&m, read32, write32, delay_us};
	unsigned int i;

	assert(t736_lp4_build_layout(&geometry, &out) == T736_LP4_OK);
	assert(out.capacity_bytes == UINT64_C(8589934592));
	assert(t736_lp4_is_lpddr4(0x03081002u));
	assert(!t736_lp4_is_lpddr4(0x03081008u));
	assert(!t736_lp4_is_lpddr4(0x03081020u)); /* old H616 encoding */
	assert(t736_lp4_apply_layout(&io, &geometry) == T736_LP4_OK);
	assert(m.writes == 22u);
	/* The oracle is the UART observation, not constants copied from this C. */
	for (i = 0; i < sizeof(observed_layout) / sizeof(observed_layout[0]); i++)
		assert(read32(&m, observed_layout[i].address) == observed_layout[i].value);

	/* Each unsupported field is rejected before any register access/write. */
	for (i = 0; i < sizeof(geometry); i++) {
		bad = geometry;
		((uint8_t *)&bad)[i] = 0;
		m.writes = 0;
		assert(t736_lp4_apply_layout(&io, &bad) == T736_LP4_UNSUPPORTED);
		assert(m.writes == 0);
	}
	assert(t736_lp4_build_layout(NULL, &out) == T736_LP4_INVALID);
	assert(t736_lp4_build_layout(&geometry, NULL) == T736_LP4_INVALID);
	assert(t736_lp4_apply_layout(NULL, &geometry) == T736_LP4_INVALID);

	/* Busy channel 1 must prevent writes to channel 0 as well. */
	memset(&m, 0, sizeof(m)); m.ack_after = -1;
	write32(&m, T736_CTL0_BASE + T736_CHANNEL_STRIDE + T736_CTL_STAT, 1);
	m.writes = 0;
	assert(t736_lp4_apply_layout(&io, &geometry) == T736_LP4_BUSY);
	assert(m.writes == 0);

	memset(&m, 0, sizeof(m)); m.ack_after = -1;
	m.drop_address = T736_CTL0_BASE;
	assert(t736_lp4_apply_layout(&io, &geometry) == T736_LP4_VERIFY);
	assert(m.writes == 1);

	memset(&m, 0, sizeof(m)); m.ack_after = 3;
	assert(t736_lp4_set_update(&io, 0, 1, 5) == T736_LP4_OK);
	assert(m.delay == 3);
	memset(&m, 0, sizeof(m)); m.ack_after = 100;
	assert(t736_lp4_set_update(&io, 0, 1, 5) == T736_LP4_TIMEOUT);
	assert(m.delay == 5);
	m.writes = 0;
	assert(t736_lp4_set_update(&io, 2, 1, 5) == T736_LP4_INVALID);
	assert(t736_lp4_set_update(&io, 0, 2, 5) == T736_LP4_INVALID);
	assert(t736_lp4_set_update(&io, 0, 1, 0) == T736_LP4_INVALID);
	assert(m.writes == 0);
}

struct pmic_mock {
	uint8_t regs[256];
	unsigned int writes;
	int fail_read, fail_write, drop_write;
};

static int read8(void *context, uint8_t reg, uint8_t *value)
{
	struct pmic_mock *p = context;
	if (reg == p->fail_read)
		return -1;
	*value = p->regs[reg];
	return 0;
}

static int write8(void *context, uint8_t reg, uint8_t value)
{
	struct pmic_mock *p = context;
	p->writes++;
	if (reg == p->fail_write)
		return -1;
	if (reg != p->drop_write)
		p->regs[reg] = value;
	return 0;
}

static void init_pmic(struct pmic_mock *p)
{
	memset(p, 0, sizeof(*p));
	p->fail_read = p->fail_write = p->drop_write = -1;
	p->regs[0x0e] = 3;
	p->regs[0x19] = 0x80 | 40; /* preserve DVM; 900 mV */
	p->regs[0x10] = 0x25;      /* unrelated rails already on */
}

static void test_pmic(void)
{
	struct pmic_mock p;
	struct t736_axp_bus bus = {&p, read8, write8};
	uint8_t code;
	unsigned int mv, i;
	static const struct { unsigned int mv, code; } boundaries[] = {
		{500, 0}, {1100, 60}, {1200, 70}, {1220, 71}, {1540, 87},
		{1840, 102}, {1900, 103}, {3400, 118},
	};
	for (i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
		assert(t736_axp_encode_mv(T736_AXP_DCDC8, boundaries[i].mv, &code) == 0);
		assert(code == boundaries[i].code);
	}
	/* All documented selector codes round-trip; reserved selectors fail. */
	for (i = 0; i < 128; i++) {
		int ret = t736_axp_decode_mv(T736_AXP_DCDC8, (uint8_t)i, &mv);
		if (i <= 118) {
			assert(ret == 0);
			assert(t736_axp_encode_mv(T736_AXP_DCDC8, mv, &code) == 0);
			assert(code == i);
		} else {
			assert(ret == T736_AXP_RANGE);
		}
	}
	assert(t736_axp_encode_mv(T736_AXP_ELDO1, 800, &code) == 0 && code == 12);
	assert(t736_axp_encode_mv(T736_AXP_ELDO1, 660, &code) == T736_AXP_RANGE);
	assert(t736_axp_encode_mv(T736_AXP_DCDC8, 1210, &code) == T736_AXP_RANGE);
	assert(t736_axp_encode_mv(T736_AXP_DCDC8, 1860, &code) == T736_AXP_RANGE);
	assert(t736_axp_encode_mv(T736_AXP_ALDO4, 1800, &code) == 0 && code == 13);
	assert(t736_axp_decode_mv(T736_AXP_ELDO1, 63, &mv) == T736_AXP_RANGE);
	assert(t736_axp_encode_mv((enum t736_axp_rail)99, 1100, &code) == T736_AXP_INVALID);
	assert(t736_axp_encode_mv(T736_AXP_ELDO1, 800, NULL) == T736_AXP_INVALID);

	init_pmic(&p);
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1100) == T736_AXP_OK);
	assert(p.regs[0x19] == 0xbc && p.regs[0x10] == 0xa5);
	assert(p.writes == 2);
	p.writes = 0;
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1100) == T736_AXP_OK);
	assert(p.writes == 0);
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1900) == T736_AXP_LIVE_RANGE_CHANGE);
	assert(p.writes == 0);

	init_pmic(&p); p.regs[0x0e] = 0xff;
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1100) == T736_AXP_ID);
	assert(p.writes == 0);
	init_pmic(&p); p.fail_read = 0x10;
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1100) == T736_AXP_IO);
	assert(p.writes == 0);
	init_pmic(&p); p.fail_write = 0x19;
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1100) == T736_AXP_IO);
	assert(p.writes == 1 && p.regs[0x10] == 0x25);
	init_pmic(&p); p.drop_write = 0x19;
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1100) == T736_AXP_VERIFY);
	assert(p.writes == 1 && p.regs[0x10] == 0x25);
	init_pmic(&p); p.fail_write = 0x10;
	assert(t736_axp_set_rail(&bus, T736_AXP_DCDC8, 1100) == T736_AXP_IO);
	assert(p.regs[0x19] == 0xbc && p.regs[0x10] == 0x25);
}

int main(void)
{
	test_controller();
	test_pmic();
	puts("LPDDR4 component tests pass; not a DDR training/cold-boot result");
	return 0;
}
