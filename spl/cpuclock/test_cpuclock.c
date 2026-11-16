// SPDX-License-Identifier: GPL-2.0-only
/* Host fault injection for safety properties, not a hardware model or proof
 * of the physical PLL/PMIC sequence. The board trials remain mandatory.
 */
#include "cpuclock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BIT(n) (UINT32_C(1) << (n))
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define ENABLE BIT(31)
#define N_MASK UINT32_C(0xff00)

struct fake {
	uint32_t regs[0x4000 / 4], xo, ppu6, ppu7;
	uint8_t pmic[256];
	unsigned int writes, pmic_reads, delays, bad_write, mux_fail_seen;
	unsigned int no_lock, stuck_update, frozen_status, post_pmic_fail;
	unsigned int supply_drift, guard_drift, delay_fail, rollback_mux_fail;
	unsigned int post_ppu_drift;
	unsigned int updates, disabled_updates;
	unsigned int early_latch, unexpected_latch;
};

static uint32_t read32(void *ctx, uint32_t addr)
{
	struct fake *f = ctx;
	if (addr == T736_CPUCLOCK_XO) return f->xo;
	if (addr == T736_CPUCLOCK_PPU6) return f->ppu6;
	if (addr == T736_CPUCLOCK_PPU7) return f->ppu7;
	assert(addr >= T736_CPUCLOCK_BASE && addr < T736_CPUCLOCK_BASE + 0x4000);
	assert(!(addr & 3));
	return f->regs[(addr - T736_CPUCLOCK_BASE) / 4];
}

static void write32(void *ctx, uint32_t addr, uint32_t value)
{
	struct fake *f = ctx;
	unsigned int off = addr - T736_CPUCLOCK_BASE;
	uint32_t *ctrl = &f->regs[T736_CPUCLOCK_CTRL / 4];
	uint32_t before = *ctrl;
	f->writes++;
	assert(!f->mux_fail_seen); /* No PLL shutdown after unconfirmed slow mux. */
	assert(!(f->ppu6 & 0xf) && !(f->ppu7 & 0xf));
	if (off != T736_CPUCLOCK_CTRL && off != T736_CPUCLOCK_CLK) {
		f->bad_write++;
		assert(0);
	}
	if (off == T736_CPUCLOCK_CLK) {
		if (value == T736_CPUCLOCK_TARGET_CLK) {
			assert(*ctrl == T736_CPUCLOCK_TARGET);
			assert(f->regs[T736_CPUCLOCK_STAT / 4] == T736_CPUCLOCK_TARGET);
		}
		if (!value && f->rollback_mux_fail &&
		    f->regs[T736_CPUCLOCK_CLK / 4] == T736_CPUCLOCK_TARGET_CLK) {
			f->mux_fail_seen = 1;
			return;
		}
		f->regs[off / 4] = value;
		return;
	}
	assert(!(value & LOCK)); /* Never echo the read-only LOCK field. */
	/* Linear CPU PLL semantics: gates are immediate, N remains the committed
	 * old value until an enabled PLL receives UPDATE carrying the desired N.
	 * This specifically models the first real-board trial's stale N read. */
	*ctrl = (value & ~(N_MASK | UPDATE)) | (*ctrl & (N_MASK | UPDATE | LOCK));
	if ((f->early_latch == 1 && (~before & value & ENABLE)) ||
	    (f->early_latch == 2 && (~before & value & LOCK_EN)))
		*ctrl = (*ctrl & ~N_MASK) | (f->unexpected_latch ? 0x3500 : (value & N_MASK));
	if (!(value & ENABLE) || !(value & LOCK_EN)) *ctrl &= ~LOCK;
	if (!f->frozen_status)
		f->regs[T736_CPUCLOCK_STAT / 4] = *ctrl;
	if (f->guard_drift && (value & 0xff00) == 0x2700)
		f->regs[0x101c / 4] = 0x03000001;
	if (value & UPDATE) {
		f->updates++;
		if (!(value & ENABLE)) {
			f->disabled_updates++;
			return;
		}
		*ctrl |= UPDATE;
		if (f->stuck_update) return;
		*ctrl &= ~(UPDATE | LOCK);
		*ctrl = (*ctrl & ~N_MASK) | (value & N_MASK);
		if ((value & (ENABLE | LOCK_EN)) == (ENABLE | LOCK_EN) && !f->no_lock)
			*ctrl |= LOCK;
		if (!f->frozen_status)
			f->regs[T736_CPUCLOCK_STAT / 4] = *ctrl;
	}
}

static int delay_us(void *ctx, uint32_t us)
{
	struct fake *f = ctx;
	assert(us == 1 || us == 10 || us == 20);
	f->delays++;
	assert(f->delays <= 16000); /* A stuck counter cannot make an endless wait. */
	return f->delay_fail ? -1 : 0;
}

static int pmic_read8(void *ctx, uint8_t reg, uint8_t *value)
{
	struct fake *f = ctx;
	assert(reg == 0x0e || reg == 0x10 || reg == 0x14 || reg == 0x15 ||
	       reg == 0x1b || reg == 0x1d);
	f->pmic_reads++;
	if (f->post_ppu_drift && f->pmic_reads == 12) f->ppu7 = 8;
	if (f->post_pmic_fail && f->pmic_reads > 6) return -1;
	*value = f->pmic[reg];
	if (f->supply_drift && reg == 0x15 && f->pmic_reads > 6)
		(*value)++;
	return 0;
}

static struct t736_cpuclock_io init(struct fake *f)
{
	memset(f, 0, sizeof(*f));
	f->regs[T736_CPUCLOCK_CTRL / 4] = T736_CPUCLOCK_ORIGINAL;
	f->regs[T736_CPUCLOCK_STAT / 4] = T736_CPUCLOCK_ORIGINAL;
	f->regs[0x2008 / 4] = 0x30000000;
	f->regs[0x2018 / 4] = 0x40000001;
	f->regs[0x1000 / 4] = f->regs[0x102c / 4] = 0xf8801300;
	f->regs[0x101c / 4] = 0x03000000;
	f->regs[0x3000 / 4] = f->regs[0x302c / 4] = 0xf8801300;
	f->regs[0x301c / 4] = 0x03000105;
	f->xo = 1u << 15;
	f->pmic[0x0e] = 3;
	f->pmic[0x10] = 0x7f;
	f->pmic[0x14] = f->pmic[0x15] = 40;
	return (struct t736_cpuclock_io){ f, read32, write32, delay_us,
		pmic_read8, 0x412fd050, 0x80000000 };
}

static void assert_restored(struct fake *f, struct t736_cpuclock_report *r)
{
	assert(r->rollback_attempted && !r->rollback_error && r->slow_verified);
	assert((r->final_ctrl & ~N_MASK) == (T736_CPUCLOCK_ORIGINAL & ~N_MASK));
	assert((r->final_ctrl & N_MASK) == (T736_CPUCLOCK_ORIGINAL & N_MASK) ||
	       (r->final_ctrl & N_MASK) == (T736_CPUCLOCK_TARGET & N_MASK));
	assert(r->final_stat == r->final_ctrl);
	assert(f->regs[T736_CPUCLOCK_CLK / 4] == 0);
	assert(!f->bad_write);
	assert(!f->disabled_updates);
}

int main(void)
{
	struct fake f;
	struct t736_cpuclock_report r;
	struct t736_cpuclock_io io;
	int rc;
	io = init(&f);
	assert(t736_cpuclock_prepare(&io, &r) == 0);
	assert(r.stage == 8 && !r.guard_failure && !r.rollback_attempted);
	assert(r.final_ctrl == T736_CPUCLOCK_TARGET && r.final_clk == T736_CPUCLOCK_TARGET_CLK);
	assert(f.pmic_reads == 12 && !f.bad_write);
	assert(f.updates == 1 && !f.disabled_updates);
	for (unsigned int latch = 1; latch <= 2; latch++) {
		io = init(&f); f.early_latch = latch;
		assert(t736_cpuclock_prepare(&io, &r) == 0);
		assert(r.final_ctrl == T736_CPUCLOCK_TARGET && f.updates == 1);
		assert(!f.disabled_updates && !r.rollback_attempted);
	}
	io = init(&f); f.early_latch = 1; f.unexpected_latch = 1;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_VERIFY);
	assert(r.rollback_attempted && r.rollback_error && r.slow_verified);
	assert(!f.updates); /* Unrecognized N fails closed; no target commit. */

	io = init(&f); f.pmic[0x14] |= 0x80; f.pmic[0x15] |= 0x80;
	assert(t736_cpuclock_prepare(&io, &r) == 0); /* DVM is not voltage code. */
	io = init(&f); f.pmic[0x15] = 29;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_SUPPLY && !f.writes);
	io = init(&f); f.pmic[0x1b] = 0x80;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_SUPPLY && !f.writes);
	io = init(&f); f.pmic[0x0e] = 2;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_SUPPLY && !f.writes);
	io = init(&f); f.ppu6 = 8;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_PRECONDITION && !f.writes);
	io = init(&f); f.ppu7 = 8;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_PRECONDITION && !f.writes);
	io = init(&f); io.midr = 0x410fd0b0;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_PRECONDITION && !f.writes);
	io = init(&f); f.xo = 0;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_PRECONDITION && !f.writes);
	io = init(&f); f.regs[T736_CPUCLOCK_STAT / 4] ^= 0x100;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_PRECONDITION && !f.writes);

	io = init(&f); f.no_lock = 1;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_TIMEOUT);
	assert_restored(&f, &r);
	io = init(&f); f.frozen_status = 1;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_TIMEOUT);
	assert(r.rollback_attempted && r.rollback_error && r.slow_verified);
	io = init(&f); f.post_pmic_fail = 1;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_PMIC);
	assert_restored(&f, &r);
	io = init(&f); f.supply_drift = 1;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_SUPPLY);
	assert_restored(&f, &r);
	io = init(&f); f.guard_drift = 1;
	assert(t736_cpuclock_prepare(&io, &r) == T736_CPUCLOCK_GUARD && r.guard_failure);
	assert_restored(&f, &r);

	io = init(&f); f.stuck_update = 1;
	rc = t736_cpuclock_prepare(&io, &r);
	assert(rc && r.rollback_attempted && r.rollback_error && r.slow_verified);
	io = init(&f); f.delay_fail = 1;
	rc = t736_cpuclock_prepare(&io, &r);
	assert(rc == T736_CPUCLOCK_DELAY && r.rollback_error && r.slow_verified);
	io = init(&f); f.post_pmic_fail = 1; f.rollback_mux_fail = 1;
	rc = t736_cpuclock_prepare(&io, &r);
	assert(rc && r.rollback_error && !r.slow_verified && f.mux_fail_seen);
	assert(r.final_ctrl == T736_CPUCLOCK_TARGET); /* Leave PLL when mux uncertain. */
	io = init(&f); f.post_ppu_drift = 1;
	rc = t736_cpuclock_prepare(&io, &r);
	assert(rc == T736_CPUCLOCK_GUARD && r.rollback_error && r.guard_failure);
	assert(r.final_ctrl == T736_CPUCLOCK_TARGET); /* Never retune a newly live core. */

	puts("CPUCLK host tests PASS: buffered N/active UPDATE, read-only refusals/DVM, bounded waits and rollback/PPU guards");
	return 0;
}
