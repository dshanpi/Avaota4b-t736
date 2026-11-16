// SPDX-License-Identifier: GPL-2.0
/*
 * One-shot T736/A733 eMMC parent and SDR-timing evidence collector v4.
 * Targets are ordinary CCU/SMHC3 R/RW configuration or read-only version/
 * clock-status registers. No interrupt, lock-clear, command or FIFO target.
 * No MMIO writes, CCF, clocks, resets, regulators, DMA, MMC or /dev/mem calls.
 * Three back-to-back passes are non-atomic observations, not a frequency
 * measurement. CTRL/THLD can change during normal I/O; preserve raw data.
 * Old v1/v2/v3 collectors and their compiled artifacts remain unchanged.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

#define T736_CLOCK_SAMPLES 3

static const struct {
	unsigned long address;
	unsigned long size;
} t736_blocks[] = {
	{ 0x02002000, 0x2000 }, /* CCU, including PERI0 distribution gates. */
	{ 0x04023000, 0x0400 }, /* Active SMHC3 configuration, no FIFO. */
};

static const struct {
	u8 block;
	u16 offset;
	const char *name;
} t736_regs[] = {
	{ 0, 0x0000, "PLL_REF_CTRL" },
	{ 0, 0x00a0, "PLL_PERI0_CTRL" },
	{ 0, 0x00a8, "PLL_PERI0_PAT0" },
	{ 0, 0x00ac, "PLL_PERI0_PAT1" },
	{ 0, 0x00c0, "PLL_PERI1_CTRL" },
	{ 0, 0x00c8, "PLL_PERI1_PAT0" },
	{ 0, 0x00cc, "PLL_PERI1_PAT1" },
	{ 0, 0x0d30, "SMHC3_CLK" },
	{ 0, 0x0d3c, "SMHC3_BGR" },
	{ 0, 0x05e0, "STORE_MBUS_GATE" },
	{ 0, 0x059c, "MSI_LITE1_GATE" },
	{ 0, 0x1908, "PERI0PLL_GATE_EN" },
	{ 0, 0x1988, "PERI0PLL_GATE_STAT" },
	{ 1, 0x0000, "SMHC3_CTRL" },
	{ 1, 0x0004, "SMHC3_CLKDIV" },
	{ 1, 0x000c, "SMHC3_WIDTH" },
	{ 1, 0x0054, "SMHC3_CSDC" },
	{ 1, 0x005c, "SMHC3_NTSR" },
	{ 1, 0x0100, "SMHC3_THLD" },
	{ 1, 0x0104, "SMHC3_SFC" },
	{ 1, 0x010c, "SMHC3_EDSD" },
	{ 1, 0x0140, "SMHC3_DRV_DL" },
	{ 1, 0x0144, "SMHC3_SAMP_DL" },
	{ 1, 0x0148, "SMHC3_DS_DL" },
	{ 1, 0x0300, "SMHC3_SMCV" },
};

static int __init t736_emmc_clocks_readonly_v4_init(void)
{
	u32 values[T736_CLOCK_SAMPLES][ARRAY_SIZE(t736_regs)];
	void __iomem *mapped[ARRAY_SIZE(t736_blocks)] = { NULL };
	unsigned int sample, i;
	bool stable = true;

	if (!of_machine_is_compatible("allwinner,t736") &&
	    !of_machine_is_compatible("allwinner,sun60i-a733"))
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(t736_blocks); i++) {
		mapped[i] = ioremap(t736_blocks[i].address, t736_blocks[i].size);
		if (!mapped[i]) {
			while (i)
				iounmap(mapped[--i]);
			return -ENOMEM;
		}
	}
	for (sample = 0; sample < T736_CLOCK_SAMPLES; sample++)
		for (i = 0; i < ARRAY_SIZE(t736_regs); i++)
			values[sample][i] = readl(mapped[t736_regs[i].block] + t736_regs[i].offset);
	for (i = 0; i < ARRAY_SIZE(t736_blocks); i++)
		iounmap(mapped[i]);

	pr_info("t736-emmc-clocks-ro-v4: begin samples=%u registers=%zu; raw reads only\n",
		T736_CLOCK_SAMPLES, ARRAY_SIZE(t736_regs));
	for (sample = 0; sample < T736_CLOCK_SAMPLES; sample++) {
		for (i = 0; i < ARRAY_SIZE(t736_regs); i++) {
			if (values[sample][i] != values[0][i])
				stable = false;
			pr_info("t736-emmc-clocks-ro-v4: sample=%u %s addr=0x%08lx value=0x%08x\n",
				sample, t736_regs[i].name,
				t736_blocks[t736_regs[i].block].address + t736_regs[i].offset,
				values[sample][i]);
		}
	}
	pr_info("t736-emmc-clocks-ro-v4: end stable_across_three_passes=%u; no MMIO writes performed\n",
		stable);
	return 0;
}

static void __exit t736_emmc_clocks_readonly_v4_exit(void) { }
module_init(t736_emmc_clocks_readonly_v4_init);
module_exit(t736_emmc_clocks_readonly_v4_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 read-only eMMC parent distribution and SDR timing snapshot v4");
