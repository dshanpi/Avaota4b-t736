// SPDX-License-Identifier: GPL-2.0
/*
 * One-shot, read-only T736/A733 CPU clock evidence collector.
 * This is an isolated diagnostic module, not a CPU clock driver.
 * It neither registers clocks nor calls vendor CCF recalc/probe routines.
 * All sampled offsets are ordinary R/RW registers in CPU_PLL_CFG; no
 * read-clear, write, clock-enable, reset, regulator or frequency operation.
 * Interpret raw values against the accompanying register audit, especially
 * CPU CTRL_STAT N and DSU mux documentation disagreements.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

#define T736_CPU_PLL_BASE 0x08870000UL
#define T736_CPU_PLL_SIZE 0x4000

struct cpu_clock_reg {
	u16 offset;
	const char *name;
};

static const struct cpu_clock_reg cpu_clock_regs[] = {
	{ 0x0000, "CPU_BACK_CTRL" },
	{ 0x1000, "A55_CTRL" },
	{ 0x1004, "A55_PAT0" },
	{ 0x1008, "A55_PAT1" },
	{ 0x1018, "A55_LFM" },
	{ 0x101c, "A55_CLK" },
	{ 0x102c, "A55_CTRL_STAT" },
	{ 0x2000, "A76_CTRL" },
	{ 0x2004, "A76_PAT0" },
	{ 0x2008, "A76_PAT1" },
	{ 0x2018, "A76_LFM" },
	{ 0x201c, "A76_CLK" },
	{ 0x202c, "A76_CTRL_STAT" },
	{ 0x3000, "DSU_CTRL" },
	{ 0x3004, "DSU_PAT0" },
	{ 0x3008, "DSU_PAT1" },
	{ 0x3018, "DSU_LFM" },
	{ 0x301c, "DSU_CLK" },
	{ 0x302c, "DSU_CTRL_STAT" },
	{ 0x3034, "CPU_SLOW_CLK" },
};

static int __init t736_cpu_clock_readonly_init(void)
{
	void __iomem *base;
	unsigned int i;

	if (!of_machine_is_compatible("allwinner,t736") &&
	    !of_machine_is_compatible("allwinner,sun60i-a733")) {
		pr_err("t736-cpu-clock-ro: unsupported DT compatible\n");
		return -ENODEV;
	}
	base = ioremap(T736_CPU_PLL_BASE, T736_CPU_PLL_SIZE);
	if (!base)
		return -ENOMEM;
	pr_info("t736-cpu-clock-ro: begin read-only snapshot base=0x%08lx\n",
		T736_CPU_PLL_BASE);
	for (i = 0; i < ARRAY_SIZE(cpu_clock_regs); i++) {
		const struct cpu_clock_reg *reg = &cpu_clock_regs[i];
		u32 value = readl(base + reg->offset);

		pr_info("t736-cpu-clock-ro: %s addr=0x%08lx value=0x%08x\n",
			reg->name, T736_CPU_PLL_BASE + reg->offset, value);
	}
	iounmap(base);
	pr_info("t736-cpu-clock-ro: end; no MMIO writes performed\n");
	return 0;
}

static void __exit t736_cpu_clock_readonly_exit(void)
{
}

module_init(t736_cpu_clock_readonly_init);
module_exit(t736_cpu_clock_readonly_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 read-only CPU PLL/mux register snapshot");
