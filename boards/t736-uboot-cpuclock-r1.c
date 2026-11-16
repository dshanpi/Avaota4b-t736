// SPDX-License-Identifier: GPL-2.0-only
/* R1 rescue keeps the board-proven C A76 sequence and never calls A55 tuning.
 * The included B1 glue
 * owns the inherited TWI callbacks; this file adds no second bus owner. */
#define t736_uboot_cpuclock_init t736_uboot_cpuclock_b1
#include "t736-uboot-cpuclock.c"
#undef t736_uboot_cpuclock_init
#include "cpuclock_c.h"

static void cpu_c_trace(void *ctx, const struct t736_cpuclock_c_trace *t)
{
 (void)ctx;
 printf("CPU-C A76 step=%u reg=%04x before=%08x desired=%08x actual=%08x CTRL=%08x STAT=%08x CLK=%08x rc=%d\n",
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
  puts("CPU-C: A76 timer invalid; unchanged\n");return 0;
 }
 rc=t736_cpuclock_c_prepare(&io,&r,cpu_c_trace);
 printf("CPU-C: A76 rc=%d stage=%u CTRL=%08x STAT=%08x CLK=%08x mutation=%u rollback=%u/%d slow=%u guard=%u trace=%u\n",
  rc,r.base.stage,r.base.final_ctrl,r.base.final_stat,r.base.final_clk,
  r.base.mutation_attempted,r.base.rollback_attempted,r.base.rollback_error,
  r.base.slow_verified,r.base.guard_failure,r.trace_count);
 if(r.failure_recorded) {
  puts("CPU-C first failing capture (before rollback):\n");cpu_c_trace(NULL,&r.failed);
 }
 printf("CPU-C: A76 PMIC %02x/%02x/%02x/%02x/%02x/%02x -> %02x/%02x/%02x/%02x/%02x/%02x\n",
  r.base.supply_before.id,r.base.supply_before.enable,r.base.supply_before.dcdc3,
  r.base.supply_before.dcdc4,r.base.supply_before.phase,r.base.supply_before.feedback,
  r.base.supply_after.id,r.base.supply_after.enable,r.base.supply_after.dcdc3,
  r.base.supply_after.dcdc4,r.base.supply_after.phase,r.base.supply_after.feedback);
 if(!rc) {puts("CPU-C: A76 cold416 then warm1014 PASS\n");return 0;}
 if(!r.base.mutation_attempted && !r.base.guard_failure) {
  puts("CPU-C: A76 precondition rejected; unchanged\n");return 0;
 }
 uint32_t n=r.base.final_ctrl&0xff00U;
 if(!r.base.rollback_error && !r.base.guard_failure && r.base.slow_verified &&
    r.base.final_ctrl==r.base.final_stat &&
    (r.base.final_ctrl&~0xff00U)==(T736_CPUCLOCK_ORIGINAL&~0xff00U) &&
    (n==0x1400U || n==0x2000U || n==0x2700U)) {
  puts("CPU-C: A76 safe slow/PLL-off; committed N retained\n");return 0;
 }
 puts("CPU-C: A76 unsafe restoration; halt before secondary release\n");return -EIO;
}

int t736_uboot_cpuclock_init(void)
{
 puts("CPU-R1: A76-only rescue; A55 clock remains inherited, no A55 writes\n");
 return cpu_c_a76();
}
