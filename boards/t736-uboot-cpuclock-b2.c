// SPDX-License-Identifier: GPL-2.0-only
/* B2 preserves B1 as a separately buildable artifact. The included B1 glue
 * owns the inherited TWI callbacks; this file adds no second bus owner. */
#define t736_uboot_cpuclock_init t736_uboot_cpuclock_b1
#include "t736-uboot-cpuclock.c"
#undef t736_uboot_cpuclock_init
#include "a55clock.h"

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
	struct t736_a55clock_report r;
	uint64_t midr, mpidr, frequency, saved_daif, masked_daif;
	int rc = t736_uboot_cpuclock_b1();
	if (rc) return rc;
	asm volatile("mrs %0, midr_el1" : "=r"(midr));
	asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	io.midr = (uint32_t)midr;
	io.mpidr = (uint32_t)mpidr;
	if (frequency != COUNTER_HZ || delay_checked(NULL, 20)) {
		puts("CPU-B2: A55 timer invalid; unchanged\n");
		return 0;
	}
	/* AArch64 U-Boot disable_interrupts() is a stub in this configuration.
 * Save DAIF explicitly and mask IRQ/FIQ. CNT is independent of CPU PLL. */
	asm volatile("mrs %0, daif; msr daifset, #3; isb; mrs %1, daif"
		     : "=&r"(saved_daif), "=&r"(masked_daif) :: "memory");
	if ((masked_daif & 0xc0U) != 0xc0U) {
		asm volatile("msr daif, %0; isb" :: "r"(saved_daif) : "memory");
		puts("CPU-B2: interrupt mask failed; A55 unchanged\n");
		return 0;
	}
	rc = t736_a55clock_prepare(&io, &r);
	/* Preserve the interrupt mask on any failed mutation until INITCALL
 * halts. Never resume boot or release secondaries in that case. */
	if (!rc || (!r.mutation_attempted && !r.guard_failure))
		asm volatile("dsb sy; msr daif, %0; isb" :: "r"(saved_daif) : "memory");
	printf("CPU-B2: A55 rc=%d stage=%u core=%08x/%08x CNT=%llu DAIF=%llx/%llx\n",
	       rc, r.stage, io.midr, io.mpidr, (unsigned long long)frequency,
	       (unsigned long long)saved_daif, (unsigned long long)masked_daif);
	printf("CPU-B2: A55 %08x/%08x/%08x -> %08x/%08x/%08x mutation=%u rollback=%d slow=%u guard=%u\n",
	       r.before_ctrl, r.before_stat, r.before_clk, r.final_ctrl, r.final_stat,
	       r.final_clk, r.mutation_attempted, r.rollback_error, r.slow_verified, r.guard_failure);
	printf("CPU-B2: PMIC %02x/%02x/%02x/%02x/%02x/%02x -> %02x/%02x/%02x/%02x/%02x/%02x\n",
	       r.supply_before.id, r.supply_before.enable, r.supply_before.dcdc3,
	       r.supply_before.dcdc4, r.supply_before.phase, r.supply_before.feedback,
	       r.supply_after.id, r.supply_after.enable, r.supply_after.dcdc3,
	       r.supply_after.dcdc4, r.supply_after.phase, r.supply_after.feedback);
	if (!rc) {
		puts("CPU-B2: A55/A76 configured N39/1=1014MHz; DSU and PMIC unchanged\n");
		return 0;
	}
	if (!r.mutation_attempted && !r.guard_failure) {
		puts("CPU-B2: A55 precondition rejected; unchanged\n");
		return 0;
	}
	puts("CPU-B2: A55 mutation or guard failed; IRQ/FIQ masked, halt before secondary release\n");
	return -EIO;
}
