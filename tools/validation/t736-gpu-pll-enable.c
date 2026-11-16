// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot diagnostic for the independent T736 PLL_GPU0.
 * Source sequence: Tina sun60iw2 ccu_pll_gate_helper_enable(), with strict
 * readback and lock failure rollback added. This is not a production CCU.
 * Only the measured disabled control word is accepted. No frequency, SDM,
 * shared PLL, GPU mux, GPU core register, reset or power-domain write occurs.
 * Success deliberately leaves PLL_GPU0 enabled after module removal, until
 * board reset; the GPU core remains on its existing PERI400 source.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define PLL_GPU_PHYS 0x020020e0
#define GPU_CLK_PHYS 0x02002b20
#define EXPECTED_PLL 0x40104500U
#define PLL_OUTPUT BIT(27)
#define PLL_LOCK BIT(28)
#define PLL_LOCK_DETECT BIT(29)
#define PLL_LDO BIT(30)
#define PLL_ENABLE BIT(31)
#define PLL_CONTROL (PLL_OUTPUT | PLL_LOCK_DETECT | PLL_LDO | PLL_ENABLE)

static bool applied;

static int write_checked(void __iomem *pll, u32 expected, const char *stage)
{
	u32 actual;

	writel(expected, pll);
	actual = readl(pll);
	pr_info("t736-gpu-pll-enable: %s expected=%08x actual=%08x\n",
		stage, expected, actual);
	if ((actual ^ expected) & ~PLL_LOCK) {
		pr_err("t736-gpu-pll-enable: %s readback mismatch\n", stage);
		return -EIO;
	}
	return 0;
}

static int __init t736_gpu_pll_enable_init(void)
{
	struct device_node *np;
	struct resource res;
	void __iomem *pll = NULL, *gpu = NULL;
	u32 original, value, core_before, bus_before, core_after, bus_after;
	unsigned int reads, locked = 0;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "allwinner,t736-gpu-ccu");
	if (!np)
		return -ENODEV;
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret)
		return ret;
	if (res.start != GPU_CLK_PHYS || resource_size(&res) != 8)
		return -ENODEV;
	if (!request_mem_region(PLL_GPU_PHYS, sizeof(u32), "t736-gpu-pll-enable"))
		return -EBUSY;
	pll = ioremap(PLL_GPU_PHYS, sizeof(u32));
	gpu = ioremap(res.start, resource_size(&res));
	if (!pll || !gpu) {
		ret = -ENOMEM;
		goto out;
	}
	original = readl(pll);
	core_before = readl(gpu);
	bus_before = readl(gpu + 4);
	pr_info("t736-gpu-pll-enable: initial PLL_GPU0=%08x GPU0_CLK=%08x GPU_BGR=%08x\n",
		original, core_before, bus_before);
	/* RO2 measured this exact word; unexpected state requires a new review. */
	if (original != EXPECTED_PLL ||
	    (core_before != 0 && core_before != 0x83000000U)) {
		pr_err("t736-gpu-pll-enable: refusing unexpected initial PLL/core state\n");
		ret = -EINVAL;
		goto out;
	}
	pr_info("t736-gpu-pll-enable: preserving N=70 M=2 and all frequency controls; integer nominal 840MHz at 24MHz reference\n");
	ret = write_checked(pll, original & ~PLL_OUTPUT, "output-off");
	if (ret)
		goto restore;
	ret = write_checked(pll, readl(pll) | PLL_LDO, "ldo-on");
	if (ret)
		goto restore;
	ret = write_checked(pll, readl(pll) | PLL_ENABLE, "enable-on");
	if (ret)
		goto restore;
	ret = write_checked(pll, readl(pll) | PLL_LOCK_DETECT, "lock-detect-on");
	if (ret)
		goto restore;
	for (reads = 0; reads < 100 && locked < 3; reads++) {
		value = readl(pll);
		locked = value & PLL_LOCK ? locked + 1 : 0;
		udelay(3);
	}
	pr_info("t736-gpu-pll-enable: lock samples=%u consecutive=%u control=%08x\n",
		reads, locked, readl(pll));
	if (locked != 3) {
		ret = -ETIMEDOUT;
		goto restore;
	}
	udelay(20);
	if (!(readl(pll) & PLL_LOCK)) {
		ret = -EIO;
		goto restore;
	}
	ret = write_checked(pll, readl(pll) | PLL_OUTPUT, "output-on");
	if (ret)
		goto restore;
	value = readl(pll);
	core_after = readl(gpu);
	bus_after = readl(gpu + 4);
	if ((value & (PLL_CONTROL | PLL_LOCK)) != (PLL_CONTROL | PLL_LOCK) ||
	    ((value ^ original) & ~(PLL_CONTROL | PLL_LOCK)) ||
	    core_after != core_before || bus_after != bus_before) {
		pr_err("t736-gpu-pll-enable: final state mismatch PLL=%08x core=%08x bus=%08x\n",
			value, core_after, bus_after);
		ret = -EIO;
		goto restore;
	}
	applied = true;
	pr_info("t736-gpu-pll-enable: SUCCESS PLL_GPU0=%08x core=%08x bus=%08x unchanged; PLL left enabled until reset\n",
		value, core_after, bus_after);
	ret = 0;
	goto out;

restore:
	pr_err("t736-gpu-pll-enable: failure=%d; restoring only PLL_GPU0\n", ret);
	/* Tina disable order, retaining the initially enabled LDO. Continue each
	 * rollback stage even if readback fails; restore the original word last.
	 */
	write_checked(pll, readl(pll) & ~PLL_OUTPUT, "restore-output-off");
	write_checked(pll, readl(pll) & ~PLL_LOCK_DETECT, "restore-lock-detect-off");
	write_checked(pll, readl(pll) & ~PLL_ENABLE, "restore-enable-off");
	if (write_checked(pll, original, "restore-original"))
		pr_err("t736-gpu-pll-enable: RESTORE FAILED; reset board before continuing\n");
out:
	if (gpu)
		iounmap(gpu);
	if (pll)
		iounmap(pll);
	release_mem_region(PLL_GPU_PHYS, sizeof(u32));
	return ret;
}

static void __exit t736_gpu_pll_enable_exit(void)
{
	if (applied)
		pr_info("t736-gpu-pll-enable: unloaded; independent PLL remains enabled until board reset\n");
}

module_init(t736_gpu_pll_enable_init);
module_exit(t736_gpu_pll_enable_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 measured-state independent PLL_GPU0 enable diagnostic");
