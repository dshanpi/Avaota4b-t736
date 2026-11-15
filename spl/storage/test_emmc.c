// SPDX-License-Identifier: GPL-2.0-only
#include "emmc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct sim {
	uint32_t regs[129], fifo_word, cmd, commands, writes, clock_updates;
	uint32_t fail_cmd, irq_error, r1_error, argument;
	uint64_t delay;
	unsigned int fifo_reads;
	int stalled_reset, stalled_clock, stuck_busy, no_ocr, byte_addressed;
	int short_data, early_done;
	int module_enabled, timer_failed;
	uint8_t extcsd[512];
};

static uint32_t read32(void *ctx, uint32_t addr)
{
	struct sim *s = ctx;
	uint32_t offset;
	assert(addr >= 0x04023000u && addr <= 0x04023200u);
	offset = addr - 0x04023000u;
	if (offset == 0)
		return s->stalled_reset ? 7u : (s->regs[0] & ~7u);
	if (offset == 0x18)
		return s->stalled_clock ? 0x80000000u : 0;
	if (offset == 0x20) {
		if (s->cmd == 1)
			return (s->no_ocr ? 0 : 0x80000000u) |
			       (s->byte_addressed ? 0 : 0x40000000u) | 0xff8000u;
		return s->cmd == s->fail_cmd ? s->r1_error : 0x900u;
	}
	if (offset == 0x38)
		return (s->cmd == s->fail_cmd ? s->irq_error : 0) |
		       (s->early_done || s->fifo_reads == 128 ? 12u : 4u);
	if (offset == 0x3c)
		return (s->stuck_busy ? 0x200u : 0) |
		       (s->short_data && s->fifo_reads >= 127 ? 4u : 0);
	if (offset == 0x200) {
		unsigned int i, at = s->fifo_reads++ * 4;
		uint32_t word = 0;
		assert(at < 512);
		for (i = 0; i < 4; i++)
			word |= (uint32_t)(s->cmd == 8 ? s->extcsd[at + i] :
				(uint8_t)((s->argument + at + i) & 255u)) << (i * 8);
		return word;
	}
	return s->regs[offset / 4];
}

static void write32(void *ctx, uint32_t addr, uint32_t value)
{
	struct sim *s = ctx;
	s->writes++;
	if (addr == 0x02002d30u) {
		assert(value == 0x8000001du || value == 0x80000000u);
		s->module_enabled = 1;
		return;
	}
	assert(addr >= 0x04023000u && addr < 0x04023200u);
	assert(s->module_enabled);
	s->regs[(addr - 0x04023000u) / 4] = value;
	if (addr == 0x0402301cu)
		s->argument = value;
	if (addr == 0x04023018u) {
		if (value & (1u << 21)) {
			s->clock_updates++;
			return;
		}
		s->cmd = value & 63u;
		/* A media/EXT_CSD write command must never be issued. */
		assert(s->cmd == 0 || s->cmd == 1 || s->cmd == 2 ||
		       s->cmd == 3 || s->cmd == 7 || s->cmd == 8 || s->cmd == 17);
		assert(!(value & (1u << 10)));
		s->commands++;
		s->fifo_reads = 0;
	}
}

static int delay_us(void *ctx, uint32_t delay)
{
	((struct sim *)ctx)->delay += delay;
	return ((struct sim *)ctx)->timer_failed;
}

static void reset(struct sim *s)
{
	memset(s, 0, sizeof(*s));
	s->fail_cmd = 63;
	s->extcsd[192] = 8;
	s->extcsd[214] = 0x40; /* 2 GiB, 0x00400000 sectors */
	s->regs[0x5c / 4] = 0x01710000u; /* Existing RX phase-clear fields. */
}

int main(void)
{
	struct sim s;
	struct t736_emmc c;
	struct t736_emmc_io io = { &s, read32, write32, delay_us };
	uint8_t scratch[514];
	unsigned int i;
	uint32_t before;
	reset(&s);
	memset(scratch, 0xa5, sizeof(scratch));
	assert(t736_emmc_open(&c, &io, scratch + 1) == 0);
	assert(c.sectors == 0x00400000u && c.initialized && s.clock_updates == 4);
	assert(s.regs[0x5c / 4] == 0x81710000u);
	assert(scratch[0] == 0xa5 && scratch[513] == 0xa5);
	assert(t736_emmc_read(&c, 123, scratch + 1) == 0);
	for (i = 0; i < 512; i++)
		assert(scratch[i + 1] == (uint8_t)((123 + i) & 255));
	assert(scratch[0] == 0xa5 && scratch[513] == 0xa5 && s.fifo_reads == 128);
	before = s.writes;
	assert(t736_emmc_read(&c, c.sectors, scratch) == T736_EMMC_INVALID);
	assert(t736_emmc_read(&c, 0, NULL) == T736_EMMC_INVALID && s.writes == before);
	s.fail_cmd = 17; s.irq_error = 1u << 6;
	assert(t736_emmc_read(&c, 0, scratch) == T736_EMMC_IO && !c.initialized);
	before = s.writes;
	assert(t736_emmc_read(&c, 0, scratch) == T736_EMMC_INVALID && s.writes == before);
	reset(&s); s.stalled_reset = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_TIMEOUT && !s.commands);
	reset(&s); s.stalled_clock = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_TIMEOUT && !s.commands);
	reset(&s); s.no_ocr = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_TIMEOUT);
	assert(s.commands == 1001);
	reset(&s); s.byte_addressed = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_UNSUPPORTED);
	reset(&s); s.extcsd[179] = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_UNSUPPORTED);
	reset(&s); s.extcsd[214] = 0;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_UNSUPPORTED);
	reset(&s); s.fail_cmd = 3; s.r1_error = 1u << 22;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_IO);
	reset(&s); s.fail_cmd = 8; s.r1_error = 1u << 22;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_IO && s.fifo_reads == 0);
	reset(&s); s.timer_failed = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_TIMER && !c.initialized);
	reset(&s); s.stuck_busy = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_TIMEOUT);
	reset(&s); s.short_data = 1; s.early_done = 1;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_TIMEOUT);
	assert(s.fifo_reads == 127);
	reset(&s); s.fail_cmd = 8; s.irq_error = 1u << 9;
	assert(t736_emmc_open(&c, &io, scratch) == T736_EMMC_TIMEOUT);
	before = s.writes;
	assert(t736_emmc_open(&c, NULL, scratch) == T736_EMMC_INVALID);
	assert(s.writes == before && !c.initialized);
	puts("PASS: eMMC PIO protocol, bounds, no-write allowlist and failure paths; no hardware test");
	return 0;
}
