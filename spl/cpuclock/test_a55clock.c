// SPDX-License-Identifier: GPL-2.0-only
/* Fault injection for local safety invariants; not a physical PLL model. */
#include "a55clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define BIT(n) (UINT32_C(1) << (n))
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define LDO BIT(30)
#define ENABLE BIT(31)
#define NM UINT32_C(0xff00)
struct fake {
	uint32_t regs[0x4000/4], ppu[8], xo;
	uint8_t pmic[256];
	unsigned int writes, ctrl_writes, updates, delays, reads;
	unsigned int early_latch, bad_n, stuck_update, no_lock, frozen_stat;
	unsigned int pmic_fail, supply_drift, guard_drift, ppu_drift;
	unsigned int delay_fail_after, initial_mux_fail, rollback_mux_fail, forbid_ctrl;
};
static uint32_t rd(void *ctx, uint32_t addr)
{
	struct fake *f = ctx;
	if (addr == T736_CPUCLOCK_XO) return f->xo;
	for (unsigned int i=0;i<8;i++) if (addr == T736_A55_PPU(i)) return f->ppu[i];
	assert(addr>=T736_CPUCLOCK_BASE && addr<T736_CPUCLOCK_BASE+0x4000 && !(addr&3));
	return f->regs[(addr-T736_CPUCLOCK_BASE)/4];
}
static void wr(void *ctx, uint32_t addr, uint32_t value)
{
	struct fake *f=ctx;
	uint32_t off=addr-T736_CPUCLOCK_BASE;
	uint32_t *ctrl=&f->regs[T736_A55_CTRL/4], old=*ctrl;
	f->writes++;
	assert(off==T736_A55_CTRL || off==T736_A55_CLK);
	if(off==T736_A55_CLK) {
		if (!value && f->initial_mux_fail && !f->ctrl_writes) return;
		if (!value && f->rollback_mux_fail && f->forbid_ctrl) return;
		if(value==T736_A55_PLL_CLK) {
			assert(*ctrl==T736_A55_TARGET && f->regs[T736_A55_STAT/4]==T736_A55_TARGET);
		}
		f->regs[off/4]=value;return;
	}
	assert(!f->forbid_ctrl);
	f->ctrl_writes++;
	assert((value&(ENABLE|LDO))==(ENABLE|LDO)); /* Never turn off a live A55 PLL. */
	assert(!(value&LOCK));
	assert(f->regs[T736_A55_CLK/4]==0); /* All PLL changes occur under slow. */
	*ctrl=(value&~(NM|LOCK|UPDATE))|(old&(NM|LOCK|UPDATE));
	if(!(value&LOCK_EN)) *ctrl&=~LOCK;
	if(f->early_latch && !(old&LOCK_EN) && (value&LOCK_EN))
		*ctrl=(*ctrl&~NM)|(f->bad_n?0x3500:(value&NM));
	if(value&UPDATE) {
		f->updates++;
		assert((value&NM)==0x2700); /* Never commit inherited below-spec N19. */
		assert(!(value&OUTPUT));
		*ctrl|=UPDATE;
		if(!f->stuck_update) {
			*ctrl=(*ctrl&~(NM|LOCK|UPDATE))|(value&NM);
			if(!f->no_lock) *ctrl|=LOCK;
		}
	}
	if(!f->frozen_stat) f->regs[T736_A55_STAT/4]=*ctrl;
	if(f->guard_drift && (value&NM)==0x2700) f->regs[0x301c/4]=0x03000104;
}
static int delay(void *ctx, uint32_t us)
{
	struct fake *f=ctx;
	assert(us==1||us==10||us==20);
	f->delays++;
	assert(f->delays<16000);
	return f->delay_fail_after && f->delays>=f->delay_fail_after ? -1:0;
}
static int pmic(void *ctx,uint8_t reg,uint8_t *v)
{
	struct fake *f=ctx;
	f->reads++;
	assert(reg==0x0e||reg==0x10||reg==0x14||reg==0x15||reg==0x1b||reg==0x1d);
	if(f->reads>6) {
		if(f->pmic_fail) { f->forbid_ctrl=1;return -1; }
		if(f->ppu_drift) f->ppu[1]=8;
	}
	*v=f->pmic[reg];
	if(f->supply_drift && f->reads>6 && reg==0x14) {(*v)++;f->forbid_ctrl=1;}
	return 0;
}
static struct t736_cpuclock_io init(struct fake *f)
{
	memset(f,0,sizeof(*f));f->xo=1u<<15;f->ppu[0]=0x108;
	f->regs[T736_A55_CTRL/4]=f->regs[T736_A55_STAT/4]=T736_A55_ORIGINAL;
	f->regs[T736_A55_CLK/4]=T736_A55_PLL_CLK;
	f->regs[0x1008/4]=0x30000000;f->regs[0x1018/4]=0x40000001;
	f->regs[0x2000/4]=f->regs[0x202c/4]=T736_CPUCLOCK_TARGET;
	f->regs[0x201c/4]=T736_CPUCLOCK_TARGET_CLK;
	f->regs[0x3000/4]=f->regs[0x302c/4]=0xf8801300;f->regs[0x301c/4]=0x03000105;
	f->pmic[0x0e]=3;f->pmic[0x10]=0x7f;f->pmic[0x14]=f->pmic[0x15]=40;
	return (struct t736_cpuclock_io){f,rd,wr,delay,pmic,0x412fd050,0x80000000};
}
static void failed_slow(struct fake *f,struct t736_a55clock_report *r)
{
	assert(r->error && r->mutation_attempted && r->slow_verified);
	assert(r->final_ctrl&ENABLE);assert(r->final_stat&ENABLE);
	assert(!r->rollback_error);assert(f->regs[T736_A55_CLK/4]==0);
	assert(f->updates<=1);
}
int main(void)
{
	struct fake f;struct t736_a55clock_report r;struct t736_cpuclock_io io;
	for(unsigned int early=0;early<2;early++) {
		io=init(&f);f.early_latch=early;
		assert(t736_a55clock_prepare(&io,&r)==0);
		assert(r.stage==8 && f.updates==1 && f.reads==12 && !r.guard_failure);
		assert(r.final_ctrl==T736_A55_TARGET && r.final_stat==T736_A55_TARGET && r.final_clk==T736_A55_PLL_CLK);
	}
	io=init(&f);f.pmic[0x14]|=0x80;f.pmic[0x15]|=0x80;
	assert(!t736_a55clock_prepare(&io,&r));
	for(unsigned int cpu=0;cpu<8;cpu++) {
		io=init(&f);f.ppu[cpu]=cpu?8:0;
		assert(t736_a55clock_prepare(&io,&r)==T736_CPUCLOCK_GUARD && !f.writes);
	}
	io=init(&f);io.mpidr|=0x100;
	assert(t736_a55clock_prepare(&io,&r)==T736_CPUCLOCK_PRECONDITION&&!f.writes);
	io=init(&f);io.midr=0x410fd0b0;
	assert(t736_a55clock_prepare(&io,&r)==T736_CPUCLOCK_PRECONDITION&&!f.writes);
	io=init(&f);f.pmic[0x14]=39;
	assert(t736_a55clock_prepare(&io,&r)==T736_CPUCLOCK_SUPPLY&&!f.writes);
	io=init(&f);f.regs[0x2000/4]=0xf8802000;
	assert(t736_a55clock_prepare(&io,&r)==T736_CPUCLOCK_PRECONDITION&&!f.writes);
	io=init(&f);f.initial_mux_fail=1;
	assert(t736_a55clock_prepare(&io,&r) && !f.ctrl_writes && r.rollback_error);
	io=init(&f);f.no_lock=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);
	io=init(&f);f.stuck_update=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);
	io=init(&f);f.frozen_stat=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);
	io=init(&f);f.early_latch=1;f.bad_n=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);assert(!f.updates);
	io=init(&f);f.pmic_fail=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);
	io=init(&f);f.supply_drift=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);
	io=init(&f);f.guard_drift=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);assert(r.guard_failure);
	io=init(&f);f.ppu_drift=1;assert(t736_a55clock_prepare(&io,&r));failed_slow(&f,&r);assert(r.guard_failure);
	io=init(&f);f.pmic_fail=1;f.rollback_mux_fail=1;
	assert(t736_a55clock_prepare(&io,&r) && r.rollback_error && !r.slow_verified);
	io=init(&f);f.delay_fail_after=1;
	assert(t736_a55clock_prepare(&io,&r)==T736_CPUCLOCK_DELAY&&!f.writes);
	io=init(&f);f.delay_fail_after=2;
	assert(t736_a55clock_prepare(&io,&r)==T736_CPUCLOCK_DELAY && r.mutation_attempted && r.rollback_error && !f.ctrl_writes);
	puts("A55 host tests PASS: delayed/early N, single bootstrap, EN retained, no N19 commit, bounded faults and mux-only failure");
	return 0;
}
