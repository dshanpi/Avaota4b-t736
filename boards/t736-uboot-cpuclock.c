// SPDX-License-Identifier: GPL-2.0-only
/* Fixed T736 board candidate: A76 only, before any secondary CPU release.
 * Inherit the SPL's initialized S_TWI0 byte engine. No PMIC configuration
 * writes, no bus initialization, and no A55/DSU clock writes are permitted.
 */
#include <stdio.h>
#include <asm/io.h>
#include <linux/errno.h>
#include "cpuclock.h"
#include "twi.h"

#define TWI_BASE 0x07083000U
#define POLL_LIMIT 1000000U
#define COUNTER_HZ 24000000U

static uint32_t counter_low(void *ctx)
{
	uint64_t ticks;
	(void)ctx;
	asm volatile("isb; mrs %0, cntpct_el0" : "=r"(ticks) :: "memory");
	return (uint32_t)ticks;
}

static uint32_t mmio_read(void *ctx, uint32_t addr)
{
	(void)ctx;
	return readl((void __iomem *)(uintptr_t)addr);
}

static void mmio_write(void *ctx, uint32_t addr, uint32_t value)
{
	(void)ctx;
	writel(value, (void __iomem *)(uintptr_t)addr);
	asm volatile("dsb sy" ::: "memory");
}

static int delay_checked(void *ctx, uint32_t us)
{
	uint32_t begin = counter_low(ctx);
	if (us > 0x7fffffffU / 24U)
		return -EINVAL;
	for (unsigned int i = 0; i < POLL_LIMIT; i++)
		if ((uint32_t)(counter_low(ctx) - begin) >= us * 24U)
			return 0;
	return -ETIMEDOUT;
}

static uint32_t twi_read(void *ctx, uint32_t off)
{
	return mmio_read(ctx, TWI_BASE + off);
}

static void twi_write(void *ctx, uint32_t off, uint32_t value)
{
	mmio_write(ctx, TWI_BASE + off, value);
}

/* All of these values come from the original SPL initialization. Refuse to
 * assume ownership if TF-A or another stage has changed that configuration.
 */
static int inherited_twi_valid(void)
{
	return (mmio_read(NULL, 0x07010010) & 0x0700001fU) == 0x04000000U &&
		(mmio_read(NULL, 0x0701019c) & 0x10001U) == 0x10001U &&
		(mmio_read(NULL, 0x07025000) & 0xffU) == 0x22U &&
		twi_read(NULL, TWI_CLOCK) == 0x59U &&
		twi_read(NULL, TWI_EFR) == 0 &&
		(twi_read(NULL, TWI_CTRL) & (TWI_EN | TWI_START | TWI_STOP)) == TWI_EN &&
		(twi_read(NULL, TWI_STATUS) & 0xffU) == 0xf8U &&
		(twi_read(NULL, TWI_LINE) & 0x30U) == 0x30U &&
		!(twi_read(NULL, TWI_DRV_CTRL) & 0x80000001U) &&
		!(twi_read(NULL, TWI_PMU_CTRL) & 0xffU);
}

static int pmic_read(void *ctx, uint8_t reg, uint8_t *value)
{
	struct t736_twi *bus = ctx;
	int rc;
	if (!inherited_twi_valid())
		return -EBUSY;
	rc = t736_twi_read8(bus, 0x36, reg, value);
	if (rc || !inherited_twi_valid())
		return -EIO;
	/* Fixed-board candidate is limited to the measured 900mV selectors.
 * Bit7 is DVM state, not part of the voltage selector. */
	if ((reg == 0x14 || reg == 0x15) && (*value & 0x7fU) != 40U)
		return -ERANGE;
	return 0;
}

int t736_uboot_cpuclock_init(void)
{
	struct t736_twi bus = {NULL, twi_read, twi_write, counter_low,
		COUNTER_HZ / 100U, POLL_LIMIT, 0, 0};
	struct t736_cpuclock_io io = {&bus, mmio_read, mmio_write, delay_checked,
		pmic_read, 0, 0};
	struct t736_cpuclock_report r;
	uint64_t midr, mpidr, frequency;
	int rc;
	asm volatile("mrs %0, midr_el1" : "=r"(midr));
	asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	io.midr = (uint32_t)midr;
	io.mpidr = (uint32_t)mpidr;
	if (frequency != COUNTER_HZ || delay_checked(NULL, 20)) {
		puts("CPU-B1: timer invalid; clocks unchanged, boot continues\n");
		return 0;
	}
	rc = t736_cpuclock_prepare(&io, &r);
	printf("CPU-B1: rc=%d stage=%u core=%08x/%08x xo=%08x ppu=%x/%x\n",
	       rc, r.stage, r.midr, r.mpidr, r.crystal, r.ppu6, r.ppu7);
	printf("CPU-B1: B %08x/%08x/%08x -> %08x/%08x/%08x mutation=%u rollback=%u/%d slow=%u guard=%u\n",
	       r.before_ctrl, r.before_stat, r.before_clk, r.final_ctrl, r.final_stat,
	       r.final_clk, r.mutation_attempted, r.rollback_attempted,
	       r.rollback_error, r.slow_verified, r.guard_failure);
	printf("CPU-B1: PMIC %02x/%02x/%02x/%02x/%02x/%02x -> %02x/%02x/%02x/%02x/%02x/%02x\n",
	       r.supply_before.id, r.supply_before.enable, r.supply_before.dcdc3,
	       r.supply_before.dcdc4, r.supply_before.phase, r.supply_before.feedback,
	       r.supply_after.id, r.supply_after.enable, r.supply_after.dcdc3,
	       r.supply_after.dcdc4, r.supply_after.phase, r.supply_after.feedback);
	if (!rc) {
		puts("CPU-B1: A76 configured N39/1=1014MHz, PMIC 900mV, guards unchanged\n");
		return 0;
	}
	if (!r.mutation_attempted && !r.guard_failure) {
		puts("CPU-B1: precondition rejected; clocks unchanged, boot continues\n");
		return 0;
	}
	if (!r.rollback_error && !r.guard_failure && r.slow_verified &&
	    r.final_ctrl == r.final_stat &&
	    (r.final_ctrl & ~0xff00U) == (T736_CPUCLOCK_ORIGINAL & ~0xff00U) &&
	    ((r.final_ctrl & 0xff00U) == 0x1400U || (r.final_ctrl & 0xff00U) == 0x2700U)) {
		puts("CPU-B1: safe slow/PLL-off fallback; committed N retained\n");
		return 0;
	}
	puts("CPU-B1: unsafe restoration; halt before releasing secondaries\n");
	return -EIO;
}
