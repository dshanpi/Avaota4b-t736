// SPDX-License-Identifier: GPL-2.0-only
#include "timing.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef T736_TIMING_BOARD_OBSERVATION
#include "timing_board_observed.h"
#endif

struct mock {
	uint32_t ctl[2][0x1000u / 4u];
	uint32_t tmg[2][0x1000u / 4u];
	uint32_t writes, elapsed;
	uint32_t lost_address;
	int lost_write, stuck_update, stuck_dfi, stuck_normal;
};

static uint32_t *location(struct mock *m, uint32_t address)
{
	unsigned int ch;
	for (ch = 0; ch < 2; ch++) {
		uint32_t base = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		uint32_t tmg = T736_TIMING0_BASE + ch * T736_CHANNEL_STRIDE;
		assert((address & 3u) == 0u);
		if (address >= base && address < base + 0x1000u)
			return &m->ctl[ch][(address - base) / 4u];
		if (address >= tmg && address < tmg + 0x1000u)
			return &m->tmg[ch][(address - tmg) / 4u];
	}
	assert(!"Unexpected MMIO region: timing must not touch clocks/PHY/PMIC");
	return NULL;
}

static uint32_t read32(void *ctx, uint32_t address)
{
	return *location(ctx, address);
}

static void write32(void *ctx, uint32_t address, uint32_t value)
{
	struct mock *m = ctx;
	unsigned int ch;
	m->writes++;
	if (m->lost_write || address == m->lost_address)
		return;
	*location(m, address) = value;
	for (ch = 0; ch < 2; ch++) {
		uint32_t base = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		if (address == base + T736_CTL_SWCTL && !m->stuck_update)
			m->ctl[ch][T736_CTL_SWSTAT / 4u] = value & 1u;
		if (address == base + T736_CTL_DFIMISC && (value & 0x20u) && !m->stuck_dfi)
			m->ctl[ch][T736_CTL_DFISTAT / 4u] = 1u;
		if (address == base + T736_CTL_PWRCTL && !(value & 0x800u) && !m->stuck_normal)
			m->ctl[ch][T736_CTL_STAT / 4u] = 1u;
	}
}

static void delay_us(void *ctx, uint32_t us)
{
	struct mock *m = ctx;
	m->elapsed += us;
}

static struct t736_lp4_io setup(struct mock *m)
{
	unsigned int ch;
	memset(m, 0, sizeof(*m));
	for (ch = 0; ch < 2; ch++) {
		m->ctl[ch][T736_CTL_MSTR / 4u] = 0x03081002u;
		m->ctl[ch][T736_CTL_SWSTAT / 4u] = 1u;
		m->ctl[ch][T736_CTL_PWRCTL / 4u] = 0x800u;
	}
	return (struct t736_lp4_io){m, read32, write32, delay_us};
}

static void arithmetic(void)
{
	uint32_t value = 99u;
	assert(t736_lp4_ns_to_ck(18u, 1800u, 4u, &value) == 0 && value == 33u);
	assert(t736_lp4_ns_to_ck(10u, 1800u, 4u, &value) == 0 && value == 18u);
	assert(t736_lp4_ns_to_ck(1u, 1u, 4u, &value) == 0 && value == 4u);
	assert(t736_lp4_ns_to_ck(0u, 1800u, 0u, &value) == 0 && value == 0u);
	assert(t736_lp4_ns_to_ck(UINT32_MAX, 1000u, 0u, &value) == 0 && value == UINT32_MAX);
	assert(t736_lp4_ns_to_ck(UINT32_MAX, UINT32_MAX, 0u, &value) == T736_LP4_UNSUPPORTED);
	assert(value == UINT32_MAX);
	assert(t736_lp4_ns_to_ck(1u, 0u, 0u, &value) == T736_LP4_INVALID);
	assert(t736_lp4_ns_to_ck(1u, 1u, 0u, NULL) == T736_LP4_INVALID);
}

static void generated_profile(void)
{
	struct t736_lp4_timing t, unchanged;
	unsigned int i;
	assert(t736_lp4_build_timing(1800u, &t) == 0);
	assert(t.count == T736_LP4_TIMING_WORDS);
	assert(t.words[0].value == 0x43483d4cu);
	assert(t.words[1].value == 0x000f1072u);
	assert(t.words[2].value == 0x1224302du);
	assert(t.words[3].value == 0x001a4b52u);
	assert(t.words[4].value == 0x21081226u);
	assert(t.words[5].value == 0x040f1b0fu);
	assert(t.words[18].value == 27u);
	assert(t.words[19].value == 0x02ac0156u);
	for (i = 0; i < t.count; i++) {
		assert(t.words[i].offset < 0x1000u && (t.words[i].offset & 3u) == 0u);
		assert((t.words[i].value & ~t.words[i].mask) == 0u);
	}
	unchanged = t;
	assert(t736_lp4_build_timing(900u, &t) == T736_LP4_UNSUPPORTED);
	assert(memcmp(&unchanged, &t, sizeof(t)) == 0);
	assert(t736_lp4_build_timing(UINT32_MAX, &t) == T736_LP4_UNSUPPORTED);
	assert(t736_lp4_build_timing(1800u, NULL) == T736_LP4_INVALID);
#ifdef T736_TIMING_BOARD_OBSERVATION
	for (i = 0; i < sizeof(timing_observed) / sizeof(timing_observed[0]); i++) {
		unsigned int w;
		for (w = 0; w < t.count; w++)
			if (t.words[w].offset == timing_observed[i].offset)
				break;
		assert(w < t.count);
		assert((timing_observed[i].value & t.words[w].mask) == t.words[w].value);
	}
#endif
}

static void programming(void)
{
	struct mock m;
	struct t736_lp4_io io = setup(&m);
	struct t736_lp4_timing t;
	unsigned int ch, i;
	assert(t736_lp4_build_timing(1800u, &t) == 0);
	for (ch = 0; ch < 2; ch++)
		for (i = 0; i < 0x1000u / 4u; i++)
			m.tmg[ch][i] = 0xa5a5a5a5u;
	assert(t736_lp4_apply_timing(&io, 1800u) == 0);
	assert(m.writes == T736_LP4_TIMING_WORDS * 2u);
	for (ch = 0; ch < 2; ch++)
		for (i = 0; i < t.count; i++) {
			uint32_t expected = (0xa5a5a5a5u & ~t.words[i].mask) | t.words[i].value;
			assert(m.tmg[ch][t.words[i].offset / 4u] == expected);
		}
	io = setup(&m);
	m.ctl[1][T736_CTL_STAT / 4u] = 1u;
	assert(t736_lp4_apply_timing(&io, 1800u) == T736_LP4_BUSY && m.writes == 0u);
	io = setup(&m);
	m.ctl[1][T736_CTL_MSTR / 4u] = 0x03081003u;
	assert(t736_lp4_apply_timing(&io, 1800u) == T736_LP4_UNSUPPORTED && m.writes == 0u);
	io = setup(&m);
	m.lost_write = 1;
	assert(t736_lp4_apply_timing(&io, 1800u) == T736_LP4_VERIFY && m.writes == 1u);
	io = setup(&m);
	assert(t736_lp4_apply_timing(&io, 800u) == T736_LP4_UNSUPPORTED && m.writes == 0u);
	assert(t736_lp4_apply_timing(NULL, 1800u) == T736_LP4_INVALID);
}

static void handoff(void)
{
	struct mock m;
	struct t736_lp4_io io = setup(&m);
	unsigned int ch;
	for (ch = 0; ch < 2; ch++)
		m.ctl[ch][T736_CTL_DFIMISC / 4u] = 0xabc00114u;
	assert(t736_lp4_dfi_handoff(&io, 10u) == 0);
	assert(m.elapsed == 102u);
	for (ch = 0; ch < 2; ch++) {
		assert(m.ctl[ch][T736_CTL_DFIMISC / 4u] == 0xabc01114u);
		assert(m.ctl[ch][T736_CTL_PWRCTL / 4u] == 0u);
		assert(m.ctl[ch][T736_CTL_STAT / 4u] == 1u);
		assert(m.ctl[ch][T736_CTL_SWSTAT / 4u] == 1u);
	}
	assert(t736_lp4_dfi_handoff(&io, 10u) == T736_LP4_BUSY);
	io = setup(&m);
	m.ctl[1][T736_CTL_PWRCTL / 4u] = 0u;
	assert(t736_lp4_dfi_handoff(&io, 10u) == T736_LP4_BUSY && m.writes == 0u);
	io = setup(&m);
	m.stuck_update = 1;
	assert(t736_lp4_dfi_handoff(&io, 10u) == T736_LP4_TIMEOUT);
	assert(m.elapsed == 10u && m.writes == 1u);
	io = setup(&m);
	m.lost_address = T736_CTL0_BASE + T736_CHANNEL_STRIDE + T736_CTL_DFIMISC;
	assert(t736_lp4_dfi_handoff(&io, 10u) == T736_LP4_VERIFY);
	assert(m.elapsed == 0u && m.ctl[0][T736_CTL_PWRCTL / 4u] == 0x800u);
	io = setup(&m);
	m.stuck_dfi = 1;
	assert(t736_lp4_dfi_handoff(&io, 10u) == T736_LP4_TIMEOUT);
	assert(m.elapsed == 10u);
	assert(m.ctl[0][T736_CTL_PWRCTL / 4u] == 0x800u);
	io = setup(&m);
	m.stuck_normal = 1;
	assert(t736_lp4_dfi_handoff(&io, 10u) == T736_LP4_TIMEOUT);
	assert(m.elapsed == 60u);
	io = setup(&m);
	assert(t736_lp4_dfi_handoff(&io, 0u) == T736_LP4_INVALID && m.writes == 0u);
	assert(t736_lp4_dfi_handoff(&io, 1000001u) == T736_LP4_INVALID && m.writes == 0u);
	io.delay_us = NULL;
	assert(t736_lp4_dfi_handoff(&io, 10u) == T736_LP4_INVALID && m.writes == 0u);
}

int main(void)
{
	arithmetic();
	generated_profile();
	programming();
	handoff();
	puts("PASS: LPDDR4 timing candidate arithmetic, MMIO boundaries, failure handling; no hardware initialization performed");
	return 0;
}
