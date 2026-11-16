// SPDX-License-Identifier: GPL-2.0-only
/* C preserves B1/B2 as separately buildable artifacts. The included B1 glue
 * owns the inherited TWI callbacks; this file adds no second bus owner. */
#define t736_uboot_cpuclock_init t736_uboot_cpuclock_b1
#include "t736-uboot-cpuclock.c"
#undef t736_uboot_cpuclock_init
#include "a55clock_d.h"
#include "cpuclock_c.h"

static void cpu_c_trace(void *ctx, const struct t736_cpuclock_c_trace *t)
{
 (void)ctx;
 printf("CPU-D A76 step=%u reg=%04x before=%08x desired=%08x actual=%08x CTRL=%08x STAT=%08x CLK=%08x rc=%d\n",
  t->step,t->reg,t->before,t->desired,t->actual,t->ctrl,t->stat,t->clk,t->error);
}
static int cpu_c_a76(void)
{
 struct t736_twi bus={NULL,twi_read,twi_write,counter_low,COUNTER_HZ/100U,POLL_LIMIT,0,0};
 struct t736_cpuclock_io io={&bus,mmio_read,mmio_write,delay_checked,pmic_read,0,0};
 struct t736_cpuclock_c_report r;
 uint64_t midr,mpidr,frequency;
 int rc;
 asm volatile("mrs %0, midr_el1":"=r"(midr));
 asm volatile("mrs %0, mpidr_el1":"=r"(mpidr));
 asm volatile("mrs %0, cntfrq_el0":"=r"(frequency));
 io.midr=(uint32_t)midr;io.mpidr=(uint32_t)mpidr;
 if(frequency!=COUNTER_HZ || delay_checked(NULL,20)) {
  puts("CPU-D: A76 timer invalid; unchanged\n");return 0;
 }
 rc=t736_cpuclock_c_prepare(&io,&r,cpu_c_trace);
 printf("CPU-D: A76 rc=%d stage=%u CTRL=%08x STAT=%08x CLK=%08x mutation=%u rollback=%u/%d slow=%u guard=%u trace=%u\n",
  rc,r.base.stage,r.base.final_ctrl,r.base.final_stat,r.base.final_clk,
  r.base.mutation_attempted,r.base.rollback_attempted,r.base.rollback_error,
  r.base.slow_verified,r.base.guard_failure,r.trace_count);
 if(r.failure_recorded) {
  puts("CPU-D first failing capture (before rollback):\n");cpu_c_trace(NULL,&r.failed);
 }
 printf("CPU-D: A76 PMIC %02x/%02x/%02x/%02x/%02x/%02x -> %02x/%02x/%02x/%02x/%02x/%02x\n",
  r.base.supply_before.id,r.base.supply_before.enable,r.base.supply_before.dcdc3,
  r.base.supply_before.dcdc4,r.base.supply_before.phase,r.base.supply_before.feedback,
  r.base.supply_after.id,r.base.supply_after.enable,r.base.supply_after.dcdc3,
  r.base.supply_after.dcdc4,r.base.supply_after.phase,r.base.supply_after.feedback);
 if(!rc) {puts("CPU-D: A76 cold416 then warm1014 PASS\n");return 0;}
 if(!r.base.mutation_attempted && !r.base.guard_failure) {
  puts("CPU-D: A76 precondition rejected; unchanged\n");return 0;
 }
 uint32_t n=r.base.final_ctrl&0xff00U;
 if(!r.base.rollback_error && !r.base.guard_failure && r.base.slow_verified &&
    r.base.final_ctrl==r.base.final_stat &&
    (r.base.final_ctrl&~0xff00U)==(T736_CPUCLOCK_ORIGINAL&~0xff00U) &&
    (n==0x1400U || n==0x2000U || n==0x2700U)) {
  puts("CPU-D: A76 safe slow/PLL-off; committed N retained\n");return 0;
 }
 puts("CPU-D: A76 unsafe restoration; halt before secondary release\n");return -EIO;
}

static void a55_mmio_write(void *ctx, uint32_t addr, uint32_t value)
{
	mmio_write(ctx, addr, value);
	/* CPU0 itself switches clock source; synchronize subsequent execution. */
	asm volatile("dsb sy; isb" ::: "memory");
}

int t736_uboot_cpuclock_init(void)
{
	struct t736_twi bus = {NULL, twi_read, twi_write, counter_low,
		COUNTER_HZ / 100U, POLL_LIMIT, 0, 0};
	struct t736_cpuclock_io io = {&bus, mmio_read, a55_mmio_write,
		delay_checked, pmic_read, 0, 0};
	struct t736_a55clock_d_report report;
	struct t736_a55clock_report *r = &report.base;
	uint64_t midr, mpidr, frequency, saved_daif, masked_daif;
	int rc = cpu_c_a76();
	if (rc) return rc;
	asm volatile("mrs %0, midr_el1" : "=r"(midr));
	asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	io.midr = (uint32_t)midr;
	io.mpidr = (uint32_t)mpidr;
	if (frequency != COUNTER_HZ || delay_checked(NULL, 20)) {
		puts("CPU-D: A55 timer invalid; unchanged\n");
		return 0;
	}
	/* AArch64 U-Boot disable_interrupts() is a stub in this configuration.
 * Save DAIF explicitly and mask IRQ/FIQ. CNT is independent of CPU PLL. */
	asm volatile("mrs %0, daif; msr daifset, #3; isb; mrs %1, daif"
		     : "=&r"(saved_daif), "=&r"(masked_daif) :: "memory");
	if ((masked_daif & 0xc0U) != 0xc0U) {
		asm volatile("msr daif, %0; isb" :: "r"(saved_daif) : "memory");
		puts("CPU-D: interrupt mask failed; A55 unchanged\n");
		return 0;
	}
	rc = t736_a55clock_d_prepare(&io, &report);
	/* Serial output starts only after the complete A55 transaction returns. */
	for (unsigned int i=0;i<report.trace_count;i++) {
	 const struct t736_a55clock_d_trace *t=&report.trace[i];
	 printf("CPU-D A55 step=%u reg=%04x before=%08x desired=%08x actual=%08x CTRL=%08x STAT=%08x CLK=%08x rc=%d\n",
	  t->step,t->reg,t->before,t->desired,t->actual,t->ctrl,t->stat,t->clk,t->error);
	}
	/* Preserve the interrupt mask on any failed mutation until INITCALL
 * halts. Never resume boot or release secondaries in that case. */
	if (!rc || (!r->mutation_attempted && !r->guard_failure))
		asm volatile("dsb sy; msr daif, %0; isb" :: "r"(saved_daif) : "memory");
	printf("CPU-D: A55 rc=%d stage=%u core=%08x/%08x CNT=%llu DAIF=%llx/%llx\n",
	       rc, r->stage, io.midr, io.mpidr, (unsigned long long)frequency,
	       (unsigned long long)saved_daif, (unsigned long long)masked_daif);
	printf("CPU-D: A55 %08x/%08x/%08x -> %08x/%08x/%08x mutation=%u rollback=%d slow=%u guard=%u\n",
	       r->before_ctrl, r->before_stat, r->before_clk, r->final_ctrl, r->final_stat,
	       r->final_clk, r->mutation_attempted, r->rollback_error, r->slow_verified, r->guard_failure);
	printf("CPU-D: PMIC %02x/%02x/%02x/%02x/%02x/%02x -> %02x/%02x/%02x/%02x/%02x/%02x\n",
	       r->supply_before.id, r->supply_before.enable, r->supply_before.dcdc3,
	       r->supply_before.dcdc4, r->supply_before.phase, r->supply_before.feedback,
	       r->supply_after.id, r->supply_after.enable, r->supply_after.dcdc3,
	       r->supply_after.dcdc4, r->supply_after.phase, r->supply_after.feedback);
	if (!rc) {
		puts("CPU-D: A55/A76 configured N39/1=1014MHz; DSU and PMIC unchanged\n");
		return 0;
	}
	if (!r->mutation_attempted && !r->guard_failure) {
		puts("CPU-D: A55 precondition rejected; unchanged\n");
		return 0;
	}
	puts("CPU-D: A55 mutation or guard failed; IRQ/FIQ masked, halt before secondary release\n");
	return -EIO;
}
