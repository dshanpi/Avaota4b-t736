// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "twi.h"

struct fake {
	uint8_t states[8], sent[8];
	unsigned pos, state_count, sent_count, starts, stops, resets;
	uint32_t ctrl, status, time, step, clock, lines, drv, pmu, reads;
	int stall_at, stop_stuck, reset_stuck;
};
static uint32_t read_reg(void *p, uint32_t reg)
{
	struct fake *f = p;
	f->reads++;
	switch (reg) {
	case TWI_CTRL: return f->ctrl;
	case TWI_STATUS: return f->status;
	case TWI_RESET: return f->reset_stuck;
	case TWI_LINE: return f->lines;
	case TWI_DATA: return 3;
	case TWI_DRV_CTRL: return f->drv;
	case TWI_PMU_CTRL: return f->pmu;
	default: return 0;
	}
}
static void write_reg(void *p, uint32_t reg, uint32_t value)
{
	struct fake *f = p;
	if (reg == TWI_RESET) {
		f->resets++; f->status = 0xf8; f->ctrl = 0;
	} else if (reg == TWI_CLOCK) {
		f->clock = value;
	} else if (reg == TWI_DATA) {
		assert(f->sent_count < sizeof(f->sent));
		f->sent[f->sent_count++] = (uint8_t)value;
	} else if (reg == TWI_CTRL) {
		assert(!(value & TWI_ACK)); /* final byte must be NACKed */
		if (!(value & TWI_IF)) { f->ctrl = value; return; }
		if (value & TWI_STOP) {
			f->stops++;
			f->ctrl = f->stop_stuck ? value : TWI_EN;
			if (!f->stop_stuck) f->status = 0xf8;
			return;
		}
		assert(f->pos < f->state_count);
		if (value & TWI_START) f->starts++;
		f->ctrl = TWI_EN;
		if ((int)f->pos != f->stall_at) {
			f->status = f->states[f->pos]; f->ctrl |= TWI_IF;
		}
		f->pos++;
	}
}
static uint32_t ticks(void *p)
{
	struct fake *f = p;
	f->time += f->step;
	return f->time;
}
static struct t736_twi setup(struct fake *f)
{
	static const uint8_t read_states[] = {8, 0x18, 0x28, 0x10, 0x40, 0x58};
	memset(f, 0, sizeof(*f));
	memcpy(f->states, read_states, sizeof(read_states));
	f->state_count = sizeof(read_states); f->stall_at = -1;
	f->lines = 0x30; f->status = 0xf8; f->step = 1;
	return (struct t736_twi){.ctx = f, .read = read_reg, .write = write_reg,
		.ticks = ticks, .timeout_ticks = 10, .poll_limit = 20};
}
int main(void)
{
	struct fake f;
	struct t736_twi b = setup(&f);
	uint8_t value = 0xaa;
	unsigned i;
	assert(t736_twi_init(&b) == 0);
	assert(f.clock == 0x59 && f.resets == 1);
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == 0 && value == 3);
	assert(f.starts == 2 && f.stops == 1 && f.pos == 6);
	assert(f.sent_count == 3 && f.sent[0] == 0x6c && f.sent[1] == 0x0e && f.sent[2] == 0x6d);
	b = setup(&f); f.states[3] = 0x28; f.state_count = 4;
	assert(t736_twi_write8(&b, 0x36, 0x19, 0x55) == 0);
	assert(f.sent_count == 3 && f.sent[2] == 0x55 && f.starts == 1 && f.stops == 1);
	for (i = 0; i < 6; i++) {
		b = setup(&f); f.states[i] = 0x20; value = 0xaa;
		assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_NACK);
		assert(value == 0xaa && f.stops == 1 && f.pos == i + 1);
		b = setup(&f); f.states[i] = 0x38;
		assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_ARBITRATION);
		assert(f.stops == 0 && f.resets == 1);
		b = setup(&f); f.stall_at = (int)i;
		assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_TIMEOUT);
		assert(f.stops == 1 && f.reads < 100);
	}
	b = setup(&f); f.step = 0; f.stall_at = 0;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_TIMEOUT && f.reads < 100);
	b = setup(&f); f.stop_stuck = 1; value = 0xaa;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_TIMEOUT);
	assert(value == 0xaa && f.resets == 1 && b.cleanup_error == TWI_TIMEOUT);
	b = setup(&f); f.states[1] = 0x20; f.stop_stuck = 1;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_NACK && b.cleanup_error == TWI_TIMEOUT);
	b = setup(&f); f.reset_stuck = 1;
	assert(t736_twi_init(&b) == TWI_TIMEOUT && f.reads < 100);
	b = setup(&f); f.lines = 0x20;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_BUSY && !f.starts && !f.stops);
	b = setup(&f); f.drv = 1;
	assert(t736_twi_init(&b) == TWI_BUSY && !f.resets);
	b = setup(&f); f.pmu = 0x80;
	assert(t736_twi_init(&b) == TWI_BUSY && !f.resets);
	b = setup(&f); f.time = 0xfffffff8u; f.stall_at = 0;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_TIMEOUT && f.reads < 100);
	assert(t736_twi_read8(&b, 0x80, 0x0e, &value) == TWI_INVALID);
	b.read = 0;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_INVALID);
	b = setup(&f); f.states[0] = 0;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_BUS_ERROR);
	b = setup(&f); f.states[0] = 0x50;
	assert(t736_twi_read8(&b, 0x36, 0x0e, &value) == TWI_UNEXPECTED);
	puts("TWI tests PASS: read/write, W1C/repeated-start, NACK/arbitration/timeout at every phase, stopped counter, wraparound, STOP failure, busy engines");
	return 0;
}
