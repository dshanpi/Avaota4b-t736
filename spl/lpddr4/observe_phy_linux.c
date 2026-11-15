// SPDX-License-Identifier: GPL-2.0-only
/* Read only PHY host control status. Never takes ownership from the PMU. */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

static int __init observe_phy_init(void)
{
	void __iomem *control;
	u16 owner, reset;

	if (!of_machine_is_compatible("allwinner,t736"))
		return -ENODEV;
	control = ioremap(0x0aaa0000, 0x134);
	if (!control)
		return -ENOMEM;
	owner = readw(control);
	pr_info("t736-lp4-phy: MuxSel @0x0aaa0000 = %04x\n", owner);
	if (owner != 0) {
		pr_info("t736-lp4-phy: host does not own APB; stop without any ownership write\n");
	} else {
		reset = readw(control + 0x132);
		pr_info("t736-lp4-phy: MicroReset @0x0aaa0132 = %04x\n", reset);
	}
	iounmap(control);
	return 0;
}

static void __exit observe_phy_exit(void) {}
module_init(observe_phy_init);
module_exit(observe_phy_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only T736 LPDDR4 PHY host ownership check");
