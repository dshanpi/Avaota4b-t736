// SPDX-License-Identifier: GPL-2.0-only
/* Fixed board A76 sequence derived from the separately reviewed offline-core
 * trial. A733 CPU PLL manual: N raw (no +1), VCO 650..2260MHz; Tina's 26MHz
 * A76 1014MHz OPP is 800mV for every listed VF bin. No donor probe is called.
 */
#include "cpuclock_c.h"

#define BIT(n) (UINT32_C(1) << (n))
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define LDO BIT(30)
#define ENABLE BIT(31)
#define N_MASK UINT32_C(0xff00)

static const uint16_t guard_offsets[T736_CPUCLOCK_GUARDS] = {
	0x0000, 0x0008, 0x000c,
	0x1000, 0x1004, 0x1008, 0x1018, 0x101c, 0x102c,
	0x3000, 0x3004, 0x3008, 0x3018, 0x301c, 0x302c,
	0x3034, 0x2004, 0x2008, 0x2018,
};

static uint32_t read_reg(const struct t736_cpuclock_io *io, uint32_t off)
{
	return io->read32(io->ctx, T736_CPUCLOCK_BASE + off);
}

static int delay(const struct t736_cpuclock_io *io, uint32_t us)
{
	return io->delay_us(io->ctx, us) ? T736_CPUCLOCK_DELAY : 0;
}

static int trace_step(const struct t736_cpuclock_io *io,
                      struct t736_cpuclock_c_report *r, uint32_t step,
                      uint32_t off, uint32_t before, uint32_t desired,
                      uint32_t actual, int error)
{
 struct t736_cpuclock_c_trace t = {step, off, before, desired, actual,
  read_reg(io, T736_CPUCLOCK_CTRL), read_reg(io, T736_CPUCLOCK_STAT),
  read_reg(io, T736_CPUCLOCK_CLK), error};
 r->last = t; r->trace_count++;
 if (error && !r->failure_recorded) {r->failed = t; r->failure_recorded = 1;}
 if (r->trace) r->trace(io->ctx, &t);
 return error;
}
static int write_mux(const struct t736_cpuclock_io *io,
                     struct t736_cpuclock_c_report *r, uint32_t step, uint32_t value)
{
 uint32_t before=read_reg(io,T736_CPUCLOCK_CLK), actual;
 io->write32(io->ctx,T736_CPUCLOCK_BASE+T736_CPUCLOCK_CLK,value);
 actual=read_reg(io,T736_CPUCLOCK_CLK);
 return trace_step(io,r,step,T736_CPUCLOCK_CLK,before,value,actual,
                   actual==value?0:T736_CPUCLOCK_VERIFY);
}
/* The cold ENABLE/LOCK_EN and warm LOCK_EN stages accept only the original
 * committed N or the requested N. Match runtime-v4, without suppressing any
 * gate checks or accepting any third N. Capture before rollback overwrites it.
 */
static int stage_control(const struct t736_cpuclock_io *io,
                         struct t736_cpuclock_c_report *r, uint32_t step,
                         uint32_t desired, uint32_t old_n, int may_latch)
{
 uint32_t before=read_reg(io,T736_CPUCLOCK_CTRL), actual;
 int rc=0;
 if (desired & (LOCK|UPDATE)) return T736_CPUCLOCK_INVALID;
 io->write32(io->ctx,T736_CPUCLOCK_BASE+T736_CPUCLOCK_CTRL,desired);
 actual=read_reg(io,T736_CPUCLOCK_CTRL);
 if (((actual^desired)&~(LOCK|N_MASK)) ||
     ((actual&N_MASK)!=old_n &&
      !(may_latch && (desired&ENABLE) && (actual&N_MASK)==(desired&N_MASK))))
  rc=T736_CPUCLOCK_VERIFY;
 return trace_step(io,r,step,T736_CPUCLOCK_CTRL,before,desired,actual,rc);
}
static int update(const struct t736_cpuclock_io *io,
                  struct t736_cpuclock_c_report *r, uint32_t step, uint32_t desired)
{
 uint32_t before=read_reg(io,T736_CPUCLOCK_CTRL),actual=before;
 int rc=T736_CPUCLOCK_TIMEOUT;
 if ((desired&(ENABLE|LDO|LOCK_EN))!=(ENABLE|LDO|LOCK_EN) ||
     (desired&(LOCK|UPDATE)) ||
     ((desired&N_MASK)!=0x2000 && (desired&N_MASK)!=0x2700) || (before&UPDATE))
  return trace_step(io,r,step,T736_CPUCLOCK_CTRL,before,desired,actual,T736_CPUCLOCK_PRECONDITION);
 if(delay(io,1)) return trace_step(io,r,step,T736_CPUCLOCK_CTRL,before,desired,actual,T736_CPUCLOCK_DELAY);
 io->write32(io->ctx,T736_CPUCLOCK_BASE+T736_CPUCLOCK_CTRL,desired|UPDATE);
 for(unsigned int i=0;i<1000;i++) {
  actual=read_reg(io,T736_CPUCLOCK_CTRL);
  if(!(actual&UPDATE)) {
   rc=((actual^desired)&~LOCK) || ((read_reg(io,T736_CPUCLOCK_STAT)^desired)&~LOCK)
      ?T736_CPUCLOCK_VERIFY:0;
   break;
  }
  if(delay(io,10)){rc=T736_CPUCLOCK_DELAY;break;}
 }
 return trace_step(io,r,step,T736_CPUCLOCK_CTRL,before,desired|UPDATE,actual,rc);
}

static int stopped_valid(uint32_t ctrl, uint32_t stat)
{
	uint32_t n = ctrl & N_MASK;
	return ctrl == stat &&
		(ctrl & ~N_MASK) == (T736_CPUCLOCK_ORIGINAL & ~N_MASK) &&
		(n == (T736_CPUCLOCK_ORIGINAL & N_MASK) ||
		 n == UINT32_C(0x2000) ||
		 n == (T736_CPUCLOCK_TARGET & N_MASK));
}

static int wait_ready(const struct t736_cpuclock_io *io,
                      struct t736_cpuclock_c_report *r, uint32_t step, uint32_t desired)
{
 unsigned int consecutive=0;
 uint32_t expected=desired|LOCK, before=read_reg(io,T736_CPUCLOCK_CTRL);
 int rc=T736_CPUCLOCK_TIMEOUT;
 for(unsigned int i=0;i<7000;i++) {
  consecutive=read_reg(io,T736_CPUCLOCK_CTRL)==expected &&
    read_reg(io,T736_CPUCLOCK_STAT)==expected ? consecutive+1:0;
  if(consecutive==3) {
   rc=delay(io,20);
   if(!rc && (read_reg(io,T736_CPUCLOCK_CTRL)!=expected ||
              read_reg(io,T736_CPUCLOCK_STAT)!=expected)) rc=T736_CPUCLOCK_VERIFY;
   break;
  }
  if(delay(io,10)){rc=T736_CPUCLOCK_DELAY;break;}
 }
 return trace_step(io,r,step,T736_CPUCLOCK_CTRL,before,expected,
                   read_reg(io,T736_CPUCLOCK_CTRL),rc);
}

static int read_supply(const struct t736_cpuclock_io *io,
		       struct t736_cpuclock_supply *s)
{
	static const uint8_t regs[] = {0x0e, 0x10, 0x14, 0x15, 0x1b, 0x1d};
	uint8_t *raw = (uint8_t *)s;
	_Static_assert(sizeof(*s) == sizeof(regs), "PMIC byte snapshot layout");
	for (unsigned int i = 0; i < sizeof(regs); i++)
		if (io->pmic_read8(io->ctx, regs[i], &raw[i]))
			return T736_CPUCLOCK_PMIC;
	/* Selectors 30..60 are 800..1100mV; bit7 is preserved DVM state. */
	if (s->id != 3 || (s->enable & 0x0c) != 0x0c || (s->phase & 0xc0) ||
	    (s->dcdc3 & 0x7f) < 30 || (s->dcdc3 & 0x7f) > 60 ||
	    (s->dcdc4 & 0x7f) < 30 || (s->dcdc4 & 0x7f) > 60)
		return T736_CPUCLOCK_SUPPLY;
	return 0;
}

static int same_supply(const struct t736_cpuclock_supply *a,
		       const struct t736_cpuclock_supply *b)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	for (unsigned int i = 0; i < sizeof(*a); i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static void capture_final(const struct t736_cpuclock_io *io,
			  struct t736_cpuclock_c_report *r)
{
	r->base.final_ctrl = read_reg(io, T736_CPUCLOCK_CTRL);
	r->base.final_stat = read_reg(io, T736_CPUCLOCK_STAT);
	r->base.final_clk = read_reg(io, T736_CPUCLOCK_CLK);
	r->base.slow_verified = r->base.final_clk == 0 && read_reg(io, 0x3034) == 0;
}

static int guard_check(const struct t736_cpuclock_io *io,
		       struct t736_cpuclock_c_report *r)
{
	for (unsigned int i = 0; i < T736_CPUCLOCK_GUARDS; i++)
		if (read_reg(io, guard_offsets[i]) != r->base.guards_before[i])
			r->base.guard_failure = 1;
	if (io->read32(io->ctx, T736_CPUCLOCK_XO) != r->base.crystal ||
	    (io->read32(io->ctx, T736_CPUCLOCK_PPU6) & 0xf) != 0 ||
	    (io->read32(io->ctx, T736_CPUCLOCK_PPU7) & 0xf) != 0)
		r->base.guard_failure = 1;
	return r->base.guard_failure ? T736_CPUCLOCK_GUARD : 0;
}

static int restore_slow(const struct t736_cpuclock_io *io,
                        struct t736_cpuclock_c_report *r)
{
 uint32_t value,before,actual;
 int rc;
 r->base.rollback_attempted=1;
 if((io->read32(io->ctx,T736_CPUCLOCK_PPU6)&0xf) ||
    (io->read32(io->ctx,T736_CPUCLOCK_PPU7)&0xf)) return T736_CPUCLOCK_PRECONDITION;
 rc=write_mux(io,r,90,0);if(rc)return rc;
 if(delay(io,20))return T736_CPUCLOCK_DELAY;
 const uint32_t clear[]={OUTPUT,LOCK_EN,ENABLE};
 for(unsigned int i=0;i<3;i++) {
  before=read_reg(io,T736_CPUCLOCK_CTRL);
  value=before&~(LOCK|UPDATE|clear[i]);
  io->write32(io->ctx,T736_CPUCLOCK_BASE+T736_CPUCLOCK_CTRL,value);
  actual=read_reg(io,T736_CPUCLOCK_CTRL);
  trace_step(io,r,91+i,T736_CPUCLOCK_CTRL,before,value,actual,0);
 }
 /* No UPDATE in rollback; retain N20/N32/N39 exactly as runtime-v4. */
 for(unsigned int i=0;i<1000;i++) {
  capture_final(io,r);
  if(r->base.slow_verified && stopped_valid(r->base.final_ctrl,r->base.final_stat))return 0;
  if(delay(io,10))return T736_CPUCLOCK_DELAY;
 }
 return T736_CPUCLOCK_VERIFY;
}
static int program_1014(const struct t736_cpuclock_io *io,
                        struct t736_cpuclock_c_report *r)
{
 uint32_t desired=T736_CPUCLOCK_ORIGINAL;
 int rc;
 r->base.stage=4;r->base.mutation_attempted=1;
 /* Cold416: exactly the board-tested runtime-v4 order and external /2. */
 rc=write_mux(io,r,1,0x00010000);if(rc)return rc;
 rc=stage_control(io,r,2,desired|LDO,0x1400,0);if(rc)return rc;
 desired|=OUTPUT;
 rc=stage_control(io,r,3,desired,0x1400,0);if(rc)return rc;
 desired=(desired&~N_MASK)|0x2000;
 rc=stage_control(io,r,4,desired,0x1400,0);if(rc)return rc;
 desired|=ENABLE;
 rc=stage_control(io,r,5,desired,0x1400,1);if(rc)return rc;
 desired|=LOCK_EN;
 rc=stage_control(io,r,6,desired,0x1400,1);if(rc)return rc;
 rc=update(io,r,7,desired);if(rc)return rc;
 rc=wait_ready(io,r,8,desired);if(rc)return rc;
 rc=write_mux(io,r,9,0x03010000);if(rc)return rc;
 /* Warm1014: PLL EN/LDO stay on, output closed while factor changes. */
 r->base.stage=5;
 rc=write_mux(io,r,10,0);if(rc)return rc;
 rc=delay(io,20);if(rc)return rc;
 desired&=~OUTPUT;
 rc=stage_control(io,r,11,desired,0x2000,0);if(rc)return rc;
 desired&=~LOCK_EN;
 rc=stage_control(io,r,12,desired,0x2000,0);if(rc)return rc;
 desired=(desired&~N_MASK)|0x2700;
 rc=stage_control(io,r,13,desired,0x2000,0);if(rc)return rc;
 desired|=LOCK_EN;
 rc=stage_control(io,r,14,desired,0x2000,1);if(rc)return rc;
 rc=update(io,r,15,desired);if(rc)return rc;
 rc=wait_ready(io,r,16,desired);if(rc)return rc;
 desired|=OUTPUT;
 rc=stage_control(io,r,17,desired,0x2700,0);if(rc)return rc;
 rc=wait_ready(io,r,18,desired);if(rc)return rc;
 r->base.stage=6;
 rc=write_mux(io,r,19,T736_CPUCLOCK_TARGET_CLK);if(rc)return rc;
 return delay(io,20);
}

int t736_cpuclock_c_prepare(const struct t736_cpuclock_io *io,
			  struct t736_cpuclock_c_report *r, t736_cpuclock_c_trace_fn trace)
{
	int rc;
	if (!io || !r || !io->read32 || !io->write32 || !io->delay_us ||
	    !io->pmic_read8)
		return T736_CPUCLOCK_INVALID;
	*r = (struct t736_cpuclock_c_report){0};
	r->trace = trace;
	r->base.stage = 1;
	r->base.midr = io->midr;
	r->base.mpidr = io->mpidr;
	r->base.crystal = io->read32(io->ctx, T736_CPUCLOCK_XO);
	r->base.ppu6 = io->read32(io->ctx, T736_CPUCLOCK_PPU6);
	r->base.ppu7 = io->read32(io->ctx, T736_CPUCLOCK_PPU7);
	r->base.before_ctrl = read_reg(io, T736_CPUCLOCK_CTRL);
	r->base.before_stat = read_reg(io, T736_CPUCLOCK_STAT);
	r->base.before_clk = read_reg(io, T736_CPUCLOCK_CLK);
	for (unsigned int i = 0; i < T736_CPUCLOCK_GUARDS; i++)
		r->base.guards_before[i] = read_reg(io, guard_offsets[i]);
	capture_final(io, r);
	/* NCAT3 uses Aff1 for core index; bootstrap must be a little core. */
	if (((io->midr >> 4) & 0xfff) != 0xd05 || (io->mpidr & 0xff) ||
	    ((io->mpidr >> 8) & 0xff) >= 6 || (r->base.ppu6 & 0xf) || (r->base.ppu7 & 0xf) ||
	    (r->base.crystal & (BIT(14) | BIT(15))) != BIT(15) ||
	    r->base.before_ctrl != T736_CPUCLOCK_ORIGINAL ||
	    r->base.before_stat != T736_CPUCLOCK_ORIGINAL || r->base.before_clk != 0 ||
	    read_reg(io, 0x3034) != 0 || read_reg(io, 0x2004) != 0 ||
	    read_reg(io, 0x2008) != UINT32_C(0x30000000) ||
	    read_reg(io, 0x2018) != UINT32_C(0x40000001)) {
		r->base.error = T736_CPUCLOCK_PRECONDITION;
		return r->base.error;
	}
	r->base.stage = 2;
	rc = read_supply(io, &r->base.supply_before);
	if (rc) {
		r->base.error = rc;
		return rc;
	}
	r->base.stage = 3;
	rc = guard_check(io, r);
	if (!rc)
		rc = program_1014(io, r);
	if (!rc) {
		r->base.stage = 7;
		capture_final(io, r);
		if (r->base.final_ctrl != T736_CPUCLOCK_TARGET ||
		    r->base.final_stat != T736_CPUCLOCK_TARGET ||
		    r->base.final_clk != T736_CPUCLOCK_TARGET_CLK)
			rc = T736_CPUCLOCK_VERIFY;
	}
	if (!rc)
		rc = read_supply(io, &r->base.supply_after);
	if (!rc && !same_supply(&r->base.supply_before, &r->base.supply_after))
		rc = T736_CPUCLOCK_SUPPLY;
	if (guard_check(io, r))
		rc = T736_CPUCLOCK_GUARD;
	if (rc && r->base.mutation_attempted && !r->failure_recorded) {
		uint32_t value = read_reg(io, T736_CPUCLOCK_CTRL);
		trace_step(io, r, 80, T736_CPUCLOCK_CTRL, value, value, value, rc);
	}
	if (rc && r->base.mutation_attempted)
		r->base.rollback_error = restore_slow(io, r);
	capture_final(io, r);
	if (guard_check(io, r))
		rc = T736_CPUCLOCK_GUARD;
	r->base.error = rc;
	if (!rc)
		r->base.stage = 8;
	return rc;
}

