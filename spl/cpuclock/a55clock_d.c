// SPDX-License-Identifier: GPL-2.0-only
/* Early single-core A55 warm frequency change, independent of CPU hotplug. */
#include "a55clock_d.h"

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

static int trace_step(const struct t736_cpuclock_io *io,
                      struct t736_a55clock_d_report *r, uint32_t step,
                      uint32_t off, uint32_t before, uint32_t desired,
                      uint32_t actual, int error)
{
 struct t736_a55clock_d_trace t = {step,off,before,desired,actual,
  rd(io,T736_A55_CTRL),rd(io,T736_A55_STAT),rd(io,T736_A55_CLK),error};
 if (r->trace_count < T736_A55_D_TRACES) r->trace[r->trace_count++] = t;
 else error = T736_CPUCLOCK_GUARD;
 if (error && !r->failure_recorded) {t.error=error;r->failed=t;r->failure_recorded=1;}
 return error;
}

static int mux(const struct t736_cpuclock_io *io,
               struct t736_a55clock_d_report *r, uint32_t step, uint32_t value)
{
 uint32_t before=rd(io,T736_A55_CLK),actual;
 io->write32(io->ctx,T736_CPUCLOCK_BASE+T736_A55_CLK,value);
 actual=rd(io,T736_A55_CLK);
 return trace_step(io,r,step,T736_A55_CLK,before,value,actual,
                   actual==value?0:T736_CPUCLOCK_VERIFY);
}

static int stage(const struct t736_cpuclock_io *io,
                 struct t736_a55clock_d_report *r, uint32_t step,
                 uint32_t desired, uint32_t old_n, int may_latch)
{
 uint32_t before=rd(io,T736_A55_CTRL),actual=before,expected_before;
 int rc=0;
 switch(step) {
 case 2: expected_before=T736_A55_ORIGINAL;break;
 case 3: expected_before=0xe0801300;break;
 case 4: expected_before=0xc0801300;break;
 case 5: expected_before=0xc0802700;break;
 case 8: expected_before=0xe0802700;break;
 default: return trace_step(io,r,step,T736_A55_CTRL,before,desired,actual,T736_CPUCLOCK_INVALID);
 }
 /* ENABLE/LDO never change. The only widened N observations are the
  * write-N and following LOCK_EN writes, both while OUTPUT is closed.
  * The manual makes N R/W and UPDATE WAC; it does not promise old-N readback.
  * No third N, unexpected gate, divider or reserved bit is accepted.
  */
 if ((desired&(ENABLE|LDO))!=(ENABLE|LDO) || (desired&(LOCK|UPDATE)) ||
     ((before^expected_before)&~(LOCK|N_MASK)) ||
     ((before&N_MASK)!=old_n && !(may_latch && (before&N_MASK)==0x2700)) ||
     (may_latch && ((step!=4 && step!=5) || old_n!=0x1300 ||
      (desired&N_MASK)!=0x2700 || (desired&OUTPUT) || (before&OUTPUT) ||
      (before&LOCK_EN) || (!!(desired&LOCK_EN)!=(step==5)))))
  return trace_step(io,r,step,T736_A55_CTRL,before,desired,actual,T736_CPUCLOCK_PRECONDITION);
 io->write32(io->ctx,T736_CPUCLOCK_BASE+T736_A55_CTRL,desired);
 actual=rd(io,T736_A55_CTRL);
 if (((actual^desired)&~(LOCK|N_MASK)) ||
     ((actual&N_MASK)!=old_n && !(may_latch && (actual&N_MASK)==(desired&N_MASK))))
  rc=T736_CPUCLOCK_VERIFY;
 return trace_step(io,r,step,T736_A55_CTRL,before,desired,actual,rc);
}

static int commit(const struct t736_cpuclock_io *io,
                  struct t736_a55clock_d_report *r, uint32_t desired)
{
 uint32_t before=rd(io,T736_A55_CTRL),actual=before;
 int rc=T736_CPUCLOCK_TIMEOUT;
 if ((desired&(ENABLE|LDO|LOCK_EN))!=(ENABLE|LDO|LOCK_EN) ||
     (desired&(LOCK|UPDATE|OUTPUT)) || (desired&N_MASK)!=0x2700 ||
     ((before^desired)&~(LOCK|N_MASK)) ||
     ((before&N_MASK)!=0x1300 && (before&N_MASK)!=0x2700))
  return trace_step(io,r,6,T736_A55_CTRL,before,desired,actual,T736_CPUCLOCK_PRECONDITION);
 if(delay(io,1)) return trace_step(io,r,6,T736_A55_CTRL,before,desired,actual,T736_CPUCLOCK_DELAY);
 io->write32(io->ctx,T736_CPUCLOCK_BASE+T736_A55_CTRL,desired|UPDATE);
 for(unsigned int i=0;i<1000;i++) {
  actual=rd(io,T736_A55_CTRL);
  if(!(actual&UPDATE)) {
   rc=((actual^desired)&~LOCK) || ((rd(io,T736_A55_STAT)^desired)&~LOCK)
      ?T736_CPUCLOCK_VERIFY:0;
   break;
  }
  if(delay(io,10)){rc=T736_CPUCLOCK_DELAY;break;}
 }
 return trace_step(io,r,6,T736_A55_CTRL,before,desired|UPDATE,actual,rc);
}

static int ready(const struct t736_cpuclock_io *io,
                 struct t736_a55clock_d_report *r, uint32_t step, uint32_t desired)
{
 unsigned int consecutive=0;
 uint32_t expected=desired|LOCK,before=rd(io,T736_A55_CTRL);
 int rc=T736_CPUCLOCK_TIMEOUT;
 for(unsigned int i=0;i<7000;i++) {
  consecutive=rd(io,T736_A55_CTRL)==expected && rd(io,T736_A55_STAT)==expected?consecutive+1:0;
  if(consecutive==3) {
   rc=delay(io,20);
   if(!rc && (rd(io,T736_A55_CTRL)!=expected || rd(io,T736_A55_STAT)!=expected))rc=T736_CPUCLOCK_VERIFY;
   break;
  }
  if(delay(io,10)){rc=T736_CPUCLOCK_DELAY;break;}
 }
 return trace_step(io,r,step,T736_A55_CTRL,before,expected,rd(io,T736_A55_CTRL),rc);
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

static void capture(const struct t736_cpuclock_io *io, struct t736_a55clock_d_report *r)
{
	r->base.final_ctrl = rd(io, T736_A55_CTRL);
	r->base.final_stat = rd(io, T736_A55_STAT);
	r->base.final_clk = rd(io, T736_A55_CLK);
	r->base.slow_verified = r->base.final_clk == 0 && rd(io, 0x3034) == 0;
}

static int guards(const struct t736_cpuclock_io *io, struct t736_a55clock_d_report *r)
{
	for (unsigned int i = 0; i < T736_A55_GUARDS; i++)
		if (rd(io, guard_offsets[i]) != r->base.guards_before[i]) r->base.guard_failure = 1;
	for (unsigned int i = 0; i < 8; i++)
		if ((io->read32(io->ctx, T736_A55_PPU(i)) & 0xf) != (i ? 0u : 8u))
			r->base.guard_failure = 1;
	if (io->read32(io->ctx, T736_CPUCLOCK_XO) != r->base.crystal) r->base.guard_failure = 1;
	return r->base.guard_failure ? T736_CPUCLOCK_GUARD : 0;
}

static int program(const struct t736_cpuclock_io *io, struct t736_a55clock_d_report *r)
{
	uint32_t desired = T736_A55_ORIGINAL & ~LOCK;
	int rc;
	r->base.mutation_attempted = 1;
	r->base.stage = 4;
	rc = mux(io, r, 1, 0); if (rc) return rc;
	rc = delay(io, 20); if (rc) return rc;
	desired &= ~OUTPUT;
	rc = stage(io, r, 2, desired, 0x1300, 0); if (rc) return rc;
	desired &= ~LOCK_EN;
	rc = stage(io, r, 3, desired, 0x1300, 0); if (rc) return rc;
	desired = (desired & ~N_MASK) | 0x2700;
	rc = stage(io, r, 4, desired, 0x1300, 1); if (rc) return rc;
	desired |= LOCK_EN;
	rc = stage(io, r, 5, desired, 0x1300, 1); if (rc) return rc;
	r->base.stage = 5;
	rc = commit(io, r, desired); if (rc) return rc;
	rc = ready(io, r, 7, desired); if (rc) return rc;
	desired |= OUTPUT;
	rc = stage(io, r, 8, desired, 0x2700, 0); if (rc) return rc;
	rc = ready(io, r, 9, desired); if (rc) return rc;
	r->base.stage = 6;
	rc = mux(io, r, 10, T736_A55_PLL_CLK); if (rc) return rc;
	rc=delay(io,20);
	return trace_step(io,r,11,T736_A55_CLK,T736_A55_PLL_CLK,T736_A55_PLL_CLK,rd(io,T736_A55_CLK),rc);
}

int t736_a55clock_d_prepare(const struct t736_cpuclock_io *io,
			 struct t736_a55clock_d_report *r)
{
	int rc;
	if (!io || !r || !io->read32 || !io->write32 || !io->delay_us || !io->pmic_read8)
		return T736_CPUCLOCK_INVALID;
	*r = (struct t736_a55clock_d_report){0};
	r->base.stage = 1;
	r->base.crystal = io->read32(io->ctx, T736_CPUCLOCK_XO);
	r->base.before_ctrl = rd(io, T736_A55_CTRL);
	r->base.before_stat = rd(io, T736_A55_STAT);
	r->base.before_clk = rd(io, T736_A55_CLK);
	for (unsigned int i = 0; i < 8; i++) r->base.ppu_before[i] = io->read32(io->ctx, T736_A55_PPU(i));
	for (unsigned int i = 0; i < T736_A55_GUARDS; i++) r->base.guards_before[i] = rd(io, guard_offsets[i]);
	capture(io, r);
	if (((io->midr >> 4) & 0xfff) != 0xd05 || (io->mpidr & 0x00ffffff) ||
	    (r->base.crystal & (BIT(14) | BIT(15))) != BIT(15) ||
	    r->base.before_ctrl != T736_A55_ORIGINAL || r->base.before_stat != T736_A55_ORIGINAL ||
	    r->base.before_clk != T736_A55_PLL_CLK || rd(io, 0x3034) || rd(io, 0x1004) ||
	    rd(io, 0x1008) != 0x30000000 || rd(io, 0x1018) != 0x40000001 ||
	    rd(io, 0x2000) != T736_CPUCLOCK_TARGET || rd(io, 0x202c) != T736_CPUCLOCK_TARGET ||
	    rd(io, 0x201c) != T736_CPUCLOCK_TARGET_CLK) {
		r->base.error = T736_CPUCLOCK_PRECONDITION;
		return r->base.error;
	}
	r->base.stage = 2;
	rc = supply(io, &r->base.supply_before);
	if (rc) { r->base.error = rc; return rc; }
	r->base.stage = 3;
	rc = guards(io, r);
	if (!rc) rc = delay(io, 20);
	if (!rc) rc = guards(io, r);
	if (!rc) rc = program(io, r);
	capture(io, r);
	if (!rc && (r->base.final_ctrl != T736_A55_TARGET || r->base.final_stat != T736_A55_TARGET ||
	    r->base.final_clk != T736_A55_PLL_CLK)) rc = T736_CPUCLOCK_VERIFY;
	if (!rc) { r->base.stage = 7; rc = supply(io, &r->base.supply_after); }
	if (!rc && !same_supply(&r->base.supply_before, &r->base.supply_after)) rc = T736_CPUCLOCK_SUPPLY;
	if (guards(io, r)) rc = T736_CPUCLOCK_GUARD;
	if (rc && !r->failure_recorded)
		trace_step(io,r,80,T736_A55_CTRL,r->base.before_ctrl,T736_A55_TARGET,rd(io,T736_A55_CTRL),rc);
	if (rc && r->base.mutation_attempted) {
		/* Never write CTRL on failure. CPU0 is running from the A55 clock. */
		r->base.rollback_error = mux(io, r, 90, 0);
		if (delay(io, 20)) r->base.rollback_error = T736_CPUCLOCK_DELAY;
		capture(io, r);
		if (!r->base.slow_verified || !(r->base.final_ctrl & ENABLE) || !(r->base.final_stat & ENABLE))
			r->base.rollback_error = T736_CPUCLOCK_VERIFY;
	}
	capture(io, r);
	if (guards(io, r)) rc = T736_CPUCLOCK_GUARD;
	r->base.error = rc;
	if (!rc) r->base.stage = 8;
	return rc;
}
