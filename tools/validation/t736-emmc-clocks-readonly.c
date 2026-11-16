// SPDX-License-Identifier: GPL-2.0
/*
 * One-shot T736/A733 eMMC parent-clock evidence collector.
 * All seven targets are ordinary CCU R/RW configuration registers, with
 * read-only lock status in the PLL controls. No interrupt/lock-clear register
 * is sampled. This module performs no MMIO writes, CCF registration, clock
 * operations, reset, regulator, DMA, MMC request or /dev/mem access.
 *
 * Three bounded back-to-back passes expose changing values; they are not an
 * atomic snapshot or a physical frequency measurement. Keep the raw source
 * selection and pattern words: a nominal integer PLL decode can be invalid
 * when the input source differs or sigma-delta modulation is active.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

#define T736_CCU_BASE 0x02002000UL
#define T736_CCU_SIZE 0x1000
#define T736_CLOCK_SAMPLES 3

struct t736_emmc_clock_reg {
	u16 offset;
	const char *name;
};

static const struct t736_emmc_clock_reg t736_emmc_clock_regs[] = {
	{ 0x0000, "PLL_REF_CTRL" },
	{ 0x00c0, "PLL_PERI1_CTRL" },
	{ 0x00c8, "PLL_PERI1_PAT0" },
	{ 0x00cc, "PLL_PERI1_PAT1" },
	{ 0x0d30, "SMHC3_CLK" },
	{ 0x05e0, "STORE_MBUS_GATE" },
	{ 0x059c, "MSI_LITE1_GATE" },
};

static int __init t736_emmc_clocks_readonly_init(void)
{
	u32 values[T736_CLOCK_SAMPLES][ARRAY_SIZE(t736_emmc_clock_regs)];
	void __iomem *base;
	unsigned int sample, i;
	bool stable = true;

	if (!of_machine_is_compatible("allwinner,t736") &&
	    !of_machine_is_compatible("allwinner,sun60i-a733")) {
		pr_err("t736-emmc-clocks-ro: unsupported DT compatible\n");
		return -ENODEV;
	}
	base = ioremap(T736_CCU_BASE, T736_CCU_SIZE);
	if (!base)
		return -ENOMEM;
	for (sample = 0; sample < T736_CLOCK_SAMPLES; sample++)
		for (i = 0; i < ARRAY_SIZE(t736_emmc_clock_regs); i++)
			values[sample][i] = readl(base + t736_emmc_clock_regs[i].offset);
	iounmap(base);

	pr_info("t736-emmc-clocks-ro: begin samples=%u registers=%zu; raw reads only\n",
		T736_CLOCK_SAMPLES, ARRAY_SIZE(t736_emmc_clock_regs));
	for (sample = 0; sample < T736_CLOCK_SAMPLES; sample++) {
		for (i = 0; i < ARRAY_SIZE(t736_emmc_clock_regs); i++) {
			const struct t736_emmc_clock_reg *reg = &t736_emmc_clock_regs[i];

			if (values[sample][i] != values[0][i])
				stable = false;
			pr_info("t736-emmc-clocks-ro: sample=%u %s addr=0x%08lx value=0x%08x\n",
				sample, reg->name, T736_CCU_BASE + reg->offset, values[sample][i]);
		}
	}
	pr_info("t736-emmc-clocks-ro: end stable_across_three_passes=%u; no MMIO writes performed\n",
		stable);
	return 0;
}

static void __exit t736_emmc_clocks_readonly_exit(void)
{
}

module_init(t736_emmc_clocks_readonly_init);
module_exit(t736_emmc_clocks_readonly_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 read-only eMMC parent PLL, pattern, clock and bus-gate snapshot");
