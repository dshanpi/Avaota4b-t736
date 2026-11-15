// SPDX-License-Identifier: GPL-2.0-only
#include "checked_controller.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct reg { uint32_t address, value; };
struct mock {
	struct reg regs[128];
	unsigned int count, calls, fail_at, writes, drop_write;
	uint32_t last_address;
};
static const struct t736_lp4_geometry geometry = {2, 2, 16, 17, 10, 3};

static uint32_t *slot(struct mock *m, uint32_t address)
{
	unsigned int i;
	for (i = 0; i < m->count; i++)
		if (m->regs[i].address == address) return &m->regs[i].value;
	assert(m->count < 128);
	m->regs[m->count].address = address;
	m->regs[m->count].value = 0xa5a55a5au;
	return &m->regs[m->count++].value;
}

static int allowed(uint32_t address)
{
	uint32_t ch;
	if (address == T736_PLL_DDR || address == T736_DRAM0_CLK || address == T736_DRAM0_BGR ||
	    address == 0x0a020010u || address == 0x0a020014u || address == 0x07010250u)
		return 1;
	for (ch = 0; ch < 2; ch++) {
		uint32_t d = ch * T736_CHANNEL_STRIDE;
		if (address == T736_CTL0_BASE + d || address == T736_CTL0_BASE + d + 0x14u ||
		    address == T736_CTL0_BASE + d + 0x90u ||
		    (address >= T736_MAP0_BASE + d && address < T736_MAP0_BASE + d + 0x30u) ||
		    (address >= T736_TIMING0_BASE + d && address < T736_TIMING0_BASE + d + 0x1000u))
			return 1;
	}
	return 0;
}

static int read32(void *context, uint32_t address, uint32_t *value)
{
	struct mock *m = context;
	assert(allowed(address));
	m->last_address = address;
	m->calls++;
	if (m->calls == m->fail_at) return -1;
	*value = *slot(m, address);
	return 0;
}

static int write32(void *context, uint32_t address, uint32_t value)
{
	struct mock *m = context;
	assert(allowed(address));
	assert(address >= T736_TIMING0_BASE && address < T736_MAP0_BASE + T736_CHANNEL_STRIDE + 0x30u);
	assert(address != T736_CTL0_BASE + 0x14u && address != T736_CTL0_BASE + 0x90u);
	assert(address != T736_CTL0_BASE + T736_CHANNEL_STRIDE + 0x14u &&
	       address != T736_CTL0_BASE + T736_CHANNEL_STRIDE + 0x90u);
	m->last_address = address;
	m->calls++;
	if (m->calls == m->fail_at) return -1;
	m->writes++;
	*slot(m, address) = value ^ (m->writes == m->drop_write ? UINT32_C(0x80000000) : 0u);
	return 0;
}

static struct t736_phy_io reset(struct mock *m, int configured)
{
	unsigned int ch;
	memset(m, 0, sizeof(*m));
	*slot(m, T736_PLL_DDR) = 0xf9004a00u;
	*slot(m, T736_DRAM0_CLK) = 0x80000003u;
	*slot(m, T736_DRAM0_BGR) = 0x10001u;
	*slot(m, 0x0a020010u) = 0x2728u;
	*slot(m, 0x0a020014u) = 2u;
	*slot(m, 0x07010250u) = 1u;
	for (ch = 0; ch < 2; ch++) {
		uint32_t base = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		*slot(m, base) = configured ? 0x03081002u : 0x03040000u;
		*slot(m, base + 0x14u) = 0;
		*slot(m, base + 0x90u) = 0;
	}
	return (struct t736_phy_io){m, 0, 0, read32, write32, 0};
}

static void observations(void)
{
	struct mock m;
	struct t736_phy_io io;
	struct t736_ctl_observation r;
	unsigned int ch, n;
	for (ch = 0; ch < 2; ch++) {
		io = reset(&m, 0);
		assert(t736_ctl_observe_checked(&io, ch, &r) == 0);
		assert(m.calls == 3 && !m.writes && r.valid_mask == 7);
		assert(r.mstr == 0x03040000u && r.stat == 0 && r.mrstat == 0);
		assert(r.attempted_address == T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE + 0x90u);
		for (n = 1; n <= 3; n++) {
			io = reset(&m, 0); m.fail_at = n;
			assert(t736_ctl_observe_checked(&io, ch, &r) == T736_CTL_CHECK_IO);
			assert(m.calls == n && !m.writes && r.valid_mask == (1u << (n-1u)) - 1u);
			assert(r.attempted_address == m.last_address);
		}
	}
	io = reset(&m, 0);
	assert(t736_ctl_observe_checked(&io, 2, &r) == T736_CTL_CHECK_INVALID && !m.calls);
}

static int configure(const struct t736_phy_io *io, int timing,
		     struct t736_ctl_write_report *r)
{
	return timing ? t736_ctl_timing_checked(io, 3, 1, 1800, r) :
		t736_ctl_layout_checked(io, 3, 1, &geometry, r);
}

static void configurations(int timing)
{
	struct mock m;
	struct t736_phy_io io = reset(&m, timing);
	struct t736_ctl_write_report r;
	unsigned int calls, writes, n;
	assert(configure(&io, timing, &r) == 0);
	calls = m.calls; writes = m.writes;
	assert(writes == (timing ? 42u : 22u));
	assert(r.channels_prechecked == 3 && r.writes_attempted == writes &&
	       r.write_callbacks_completed == writes && r.verified_writes == writes);
	if (timing) {
		for (n = 0; n < 2; n++) {
			uint32_t base = T736_TIMING0_BASE + n * T736_CHANNEL_STRIDE;
			assert((*slot(&m, base + 0x600u) & 0x7fff0000u) == 0x25a50000u);
			assert((*slot(&m, base + 0x604u) & 0xf000f000u) == 0xa0005000u);
			assert((*slot(&m, base + 0x608u) & 0x0000ffffu) == 0x00005a5au);
		}
	}
	for (n = 1; n <= calls; n++) {
		io = reset(&m, timing); m.fail_at = n;
		assert(configure(&io, timing, &r) == T736_CTL_CHECK_IO);
		assert(m.calls == n && r.write_callbacks_completed == m.writes);
		assert(r.verified_writes <= r.write_callbacks_completed &&
		       r.write_callbacks_completed <= r.writes_attempted && r.writes_attempted <= writes);
		assert(r.attempted_address == m.last_address);
	}
	for (n = 1; n <= writes; n++) {
		io = reset(&m, timing); m.drop_write = n;
		assert(configure(&io, timing, &r) == T736_CTL_CHECK_VERIFY);
		assert(m.writes == n && r.verified_writes == n-1u);
	}
	io = reset(&m, timing);
	*slot(&m, T736_CTL0_BASE + T736_CHANNEL_STRIDE + T736_CTL_STAT) = 1;
	assert(configure(&io, timing, &r) == T736_CTL_CHECK_BUSY && !m.writes);
	{
		static const uint32_t prerequisites[] = {T736_PLL_DDR, T736_DRAM0_CLK, T736_DRAM0_BGR,
			0x0a020010u, 0x0a020014u, 0x07010250u};
		unsigned int j;
		for (j = 0; j < 6; j++) for (n = 0; n < 32; n++) {
			io = reset(&m, timing);
			*slot(&m, prerequisites[j]) ^= UINT32_C(1) << n;
			assert(configure(&io, timing, &r) == T736_CTL_CHECK_BUSY && !m.writes);
			assert(m.calls == j+1u); /* No controller access after a failed common prerequisite. */
		}
	}
	printf("Checked %s: %u IO fault positions, %u corrupted readbacks; immediate stop\n",
	       timing ? "timing" : "layout", calls, writes);
}

int main(void)
{
	struct mock m;
	struct t736_phy_io io = reset(&m, 0);
	struct t736_ctl_write_report r;
	observations(); configurations(0); configurations(1);
	assert(t736_ctl_timing_checked(&io, 3, 1, 1800, &r) == T736_CTL_CHECK_UNSUPPORTED && !m.writes);
	io = reset(&m, 0);
	assert(t736_ctl_layout_checked(&io, 1, 1, &geometry, &r) == T736_CTL_CHECK_INVALID && !m.calls);
	assert(t736_ctl_layout_checked(&io, 3, 0, &geometry, &r) == T736_CTL_CHECK_INVALID && !m.calls);
	assert(t736_ctl_layout_checked(&io, 3, 1, 0, &r) == T736_CTL_CHECK_INVALID && !m.calls);
	assert(t736_ctl_timing_checked(&io, 3, 1, 800, &r) == T736_CTL_CHECK_UNSUPPORTED && !m.calls);
	puts("Checked controller: cold reset MSTR accepted by observation/layout; no PHY, clock, domain, port, DFI-init handshake, training or normal-mode entry");
	return 0;
}
