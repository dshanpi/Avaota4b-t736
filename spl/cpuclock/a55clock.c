// SPDX-License-Identifier: GPL-2.0-only
/* Early single-core A55 warm frequency change, independent of CPU hotplug. */
#include "a55clock.h"

#define BIT(n) (UINT32_C(1) << (n))
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define LDO BIT(30)
#define ENABLE BIT(31)
#define N_MASK UINT32_C(0xff00)

static const uint16_t guard_offsets[T736_A55_GUARDS] = {
	0x0000, 0x0008, 0x000c,
	0x1004, 0x1008, 0x1018,
	0x2000, 0x2004, 0x2008, 0x2018, 0x201c, 0x202c,
	0x3000, 0x3004, 0x3008, 0x3018, 0x301c, 0x302c, 0x3034,
};

static uint32_t rd(const struct t736_cpuclock_io *io, uint32_t off)
{
	return io->read32(io->ctx, T736_CPUCLOCK_BASE + off);
}

static int delay(const struct t736_cpuclock_io *io, uint32_t us)
{
	return io->delay_us(io->ctx, us) ? T736_CPUCLOCK_DELAY : 0;
}

static int mux(const struct t736_cpuclock_io *io, uint32_t value)
{
	io->write32(io->ctx, T736_CPUCLOCK_BASE + T736_A55_CLK, value);
	return rd(io, T736_A55_CLK) == value ? 0 : T736_CPUCLOCK_VERIFY;
}

static int stage(const struct t736_cpuclock_io *io, uint32_t desired,
		 uint32_t old_n, int may_latch)
{
	uint32_t before = rd(io, T736_A55_CTRL), actual;
	if ((desired & (ENABLE | LDO)) != (ENABLE | LDO) ||
	    (desired & (LOCK | UPDATE)) || !(before & ENABLE) ||
	    (before & UPDATE) ||
	    (may_latch && ((before & LOCK_EN) || !(desired & LOCK_EN))))
		return T736_CPUCLOCK_PRECONDITION;
	io->write32(io->ctx, T736_CPUCLOCK_BASE + T736_A55_CTRL, desired);
	actual = rd(io, T736_A55_CTRL);
	if (((actual ^ desired) & ~(LOCK | N_MASK)) ||
	    ((actual & N_MASK) != old_n &&
	     !(may_latch && (actual & N_MASK) == (desired & N_MASK))))
		return T736_CPUCLOCK_VERIFY;
	return 0;
}

static int commit(const struct t736_cpuclock_io *io, uint32_t desired)
{
	if ((desired & (ENABLE | LDO | LOCK_EN)) != (ENABLE | LDO | LOCK_EN) ||
	    (desired & (LOCK | UPDATE | OUTPUT)) ||
	    (desired & N_MASK) != 0x2700 ||
	    !(rd(io, T736_A55_CTRL) & ENABLE) || (rd(io, T736_A55_CTRL) & UPDATE))
		return T736_CPUCLOCK_PRECONDITION;
	if (delay(io, 1)) return T736_CPUCLOCK_DELAY;
	io->write32(io->ctx, T736_CPUCLOCK_BASE + T736_A55_CTRL, desired | UPDATE);
	for (unsigned int i = 0; i < 1000; i++) {
		uint32_t value = rd(io, T736_A55_CTRL);
		if (!(value & UPDATE))
			return ((value ^ desired) & ~LOCK) ||
				((rd(io, T736_A55_STAT) ^ desired) & ~LOCK) ?
				T736_CPUCLOCK_VERIFY : 0;
		if (delay(io, 10)) return T736_CPUCLOCK_DELAY;
	}
	return T736_CPUCLOCK_TIMEOUT;
}

static int ready(const struct t736_cpuclock_io *io, uint32_t desired)
{
	unsigned int consecutive = 0;
	uint32_t expected = desired | LOCK;
	for (unsigned int i = 0; i < 7000; i++) {
		consecutive = rd(io, T736_A55_CTRL) == expected &&
			rd(io, T736_A55_STAT) == expected ? consecutive + 1 : 0;
		if (consecutive == 3) {
			if (delay(io, 20)) return T736_CPUCLOCK_DELAY;
			return rd(io, T736_A55_CTRL) == expected &&
				rd(io, T736_A55_STAT) == expected ? 0 : T736_CPUCLOCK_VERIFY;
		}
		if (delay(io, 10)) return T736_CPUCLOCK_DELAY;
	}
	return T736_CPUCLOCK_TIMEOUT;
}

static int supply(const struct t736_cpuclock_io *io, struct t736_cpuclock_supply *s)
{
	static const uint8_t regs[] = {0x0e, 0x10, 0x14, 0x15, 0x1b, 0x1d};
	uint8_t *raw = (uint8_t *)s;
	_Static_assert(sizeof(*s) == sizeof(regs), "PMIC byte snapshot layout");
	for (unsigned int i = 0; i < sizeof(regs); i++)
		if (io->pmic_read8(io->ctx, regs[i], &raw[i])) return T736_CPUCLOCK_PMIC;
	return s->id == 3 && (s->enable & 0x0c) == 0x0c && !(s->phase & 0xc0) &&
		(s->dcdc3 & 0x7f) == 40 && (s->dcdc4 & 0x7f) == 40 ?
		0 : T736_CPUCLOCK_SUPPLY;
}

static int same_supply(const struct t736_cpuclock_supply *a,
		       const struct t736_cpuclock_supply *b)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	for (unsigned int i = 0; i < sizeof(*a); i++)
		if (x[i] != y[i]) return 0;
	return 1;
}

static void capture(const struct t736_cpuclock_io *io, struct t736_a55clock_report *r)
{
	r->final_ctrl = rd(io, T736_A55_CTRL);
	r->final_stat = rd(io, T736_A55_STAT);
	r->final_clk = rd(io, T736_A55_CLK);
	r->slow_verified = r->final_clk == 0 && rd(io, 0x3034) == 0;
}

static int guards(const struct t736_cpuclock_io *io, struct t736_a55clock_report *r)
{
	for (unsigned int i = 0; i < T736_A55_GUARDS; i++)
		if (rd(io, guard_offsets[i]) != r->guards_before[i]) r->guard_failure = 1;
	for (unsigned int i = 0; i < 8; i++)
		if ((io->read32(io->ctx, T736_A55_PPU(i)) & 0xf) != (i ? 0u : 8u))
			r->guard_failure = 1;
	if (io->read32(io->ctx, T736_CPUCLOCK_XO) != r->crystal) r->guard_failure = 1;
	return r->guard_failure ? T736_CPUCLOCK_GUARD : 0;
}

static int program(const struct t736_cpuclock_io *io, struct t736_a55clock_report *r)
{
	uint32_t desired = T736_A55_ORIGINAL & ~LOCK;
	int rc;
	r->mutation_attempted = 1;
	r->stage = 4;
	rc = mux(io, 0); if (rc) return rc;
	rc = delay(io, 20); if (rc) return rc;
	desired &= ~OUTPUT;
	rc = stage(io, desired, 0x1300, 0); if (rc) return rc;
	desired &= ~LOCK_EN;
	rc = stage(io, desired, 0x1300, 0); if (rc) return rc;
	desired = (desired & ~N_MASK) | 0x2700;
	rc = stage(io, desired, 0x1300, 0); if (rc) return rc;
	desired |= LOCK_EN;
	rc = stage(io, desired, 0x1300, 1); if (rc) return rc;
	r->stage = 5;
	rc = commit(io, desired); if (rc) return rc;
	rc = ready(io, desired); if (rc) return rc;
	desired |= OUTPUT;
	rc = stage(io, desired, 0x2700, 0); if (rc) return rc;
	rc = ready(io, desired); if (rc) return rc;
	r->stage = 6;
	rc = mux(io, T736_A55_PLL_CLK); if (rc) return rc;
	return delay(io, 20);
}

int t736_a55clock_prepare(const struct t736_cpuclock_io *io,
			 struct t736_a55clock_report *r)
{
	int rc;
	if (!io || !r || !io->read32 || !io->write32 || !io->delay_us || !io->pmic_read8)
		return T736_CPUCLOCK_INVALID;
	*r = (struct t736_a55clock_report){0};
	r->stage = 1;
	r->crystal = io->read32(io->ctx, T736_CPUCLOCK_XO);
	r->before_ctrl = rd(io, T736_A55_CTRL);
	r->before_stat = rd(io, T736_A55_STAT);
	r->before_clk = rd(io, T736_A55_CLK);
	for (unsigned int i = 0; i < 8; i++) r->ppu_before[i] = io->read32(io->ctx, T736_A55_PPU(i));
	for (unsigned int i = 0; i < T736_A55_GUARDS; i++) r->guards_before[i] = rd(io, guard_offsets[i]);
	capture(io, r);
	if (((io->midr >> 4) & 0xfff) != 0xd05 || (io->mpidr & 0x00ffffff) ||
	    (r->crystal & (BIT(14) | BIT(15))) != BIT(15) ||
	    r->before_ctrl != T736_A55_ORIGINAL || r->before_stat != T736_A55_ORIGINAL ||
	    r->before_clk != T736_A55_PLL_CLK || rd(io, 0x3034) || rd(io, 0x1004) ||
	    rd(io, 0x1008) != 0x30000000 || rd(io, 0x1018) != 0x40000001 ||
	    rd(io, 0x2000) != T736_CPUCLOCK_TARGET || rd(io, 0x202c) != T736_CPUCLOCK_TARGET ||
	    rd(io, 0x201c) != T736_CPUCLOCK_TARGET_CLK) {
		r->error = T736_CPUCLOCK_PRECONDITION;
		return r->error;
	}
	r->stage = 2;
	rc = supply(io, &r->supply_before);
	if (rc) { r->error = rc; return rc; }
	r->stage = 3;
	rc = guards(io, r);
	if (!rc) rc = delay(io, 20);
	if (!rc) rc = guards(io, r);
	if (!rc) rc = program(io, r);
	capture(io, r);
	if (!rc && (r->final_ctrl != T736_A55_TARGET || r->final_stat != T736_A55_TARGET ||
	    r->final_clk != T736_A55_PLL_CLK)) rc = T736_CPUCLOCK_VERIFY;
	if (!rc) { r->stage = 7; rc = supply(io, &r->supply_after); }
	if (!rc && !same_supply(&r->supply_before, &r->supply_after)) rc = T736_CPUCLOCK_SUPPLY;
	if (guards(io, r)) rc = T736_CPUCLOCK_GUARD;
	if (rc && r->mutation_attempted) {
		/* Never write CTRL on failure. CPU0 is running from the A55 clock. */
		r->rollback_error = mux(io, 0);
		if (delay(io, 20)) r->rollback_error = T736_CPUCLOCK_DELAY;
		capture(io, r);
		if (!r->slow_verified || !(r->final_ctrl & ENABLE) || !(r->final_stat & ENABLE))
			r->rollback_error = T736_CPUCLOCK_VERIFY;
	}
	capture(io, r);
	if (guards(io, r)) rc = T736_CPUCLOCK_GUARD;
	r->error = rc;
	if (!rc) r->stage = 8;
	return rc;
}
