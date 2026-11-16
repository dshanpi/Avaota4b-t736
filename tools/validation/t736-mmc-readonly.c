// SPDX-License-Identifier: GPL-2.0
/* One-shot reads of the active T736 SMHC3 version and storage bus gates.
 * Offsets come from Tina sun60iw2/v5p6x register definitions. These registers
 * have no read-clear semantics. This module performs no MMIO writes.
 */
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

static int __init t736_mmc_ro_init(void)
{
	static const struct { unsigned long address; const char *name; } regs[] = {
		{ 0x04023300, "SMHC3_SMCV" },
		/* Main CCU starts at 0x02002000, not the PIO base 0x02000000. */
		{ 0x020025e0, "STORE_MBUS_GATE" },
		{ 0x0200259c, "MSI_LITE1_GATE" },
	};
	unsigned int i;

	if (!of_machine_is_compatible("allwinner,t736") &&
	    !of_machine_is_compatible("allwinner,sun60i-a733"))
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		void __iomem *mapped = ioremap(regs[i].address, 4);
		u32 value;
		if (!mapped)
			return -ENOMEM;
		value = readl(mapped);
		iounmap(mapped);
		pr_info("t736-mmc-ro: %s addr=%08lx value=%08x\n",
			regs[i].name, regs[i].address, value);
	}
	pr_info("t736-mmc-ro: no MMIO writes performed\n");
	return 0;
}

static void __exit t736_mmc_ro_exit(void) { }
module_init(t736_mmc_ro_init);
module_exit(t736_mmc_ro_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 active SMHC3 version and bus-gate read-only snapshot");
