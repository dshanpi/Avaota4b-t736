// SPDX-License-Identifier: GPL-2.0-only
/* Diagnostic only: no DDR writes, no claim of independent initialization. */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

static int __init observe_timing_init(void)
{
	static const u16 timing_offsets[] = {
		0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c,
		0x024, 0x030, 0x034, 0x038, 0x05c, 0x060, 0x064,
		0x500, 0x504, 0x508, 0x50c,
		0x580, 0x584, 0x588, 0x600, 0x604, 0x608,
	};
	static const u16 ctl_offsets[] = { 0x000, 0x014, 0x510, 0x514, 0xc80, 0xc84 };
	unsigned int ch, i, bank;

	if (!of_machine_is_compatible("allwinner,t736"))
		return -ENODEV;
	for (ch = 0; ch < 2; ch++) {
		for (bank = 0; bank < 2; bank++) {
			phys_addr_t base = (bank ? 0x0a110000 : 0x0a100000) + ch * 0x400000;
			const u16 *offsets = bank ? ctl_offsets : timing_offsets;
			unsigned int count = bank ? ARRAY_SIZE(ctl_offsets) : ARRAY_SIZE(timing_offsets);
			for (i = 0; i < count; i++) {
				phys_addr_t addr = base + offsets[i];
				void __iomem *reg = ioremap(addr, sizeof(u32));
				if (!reg)
					return -ENOMEM;
				pr_info("t736-lp4-timing: CH%u_%s_%03x @%pa = %08x\n",
					ch, bank ? "CTL" : "TMG", offsets[i], &addr, readl(reg));
				iounmap(reg);
			}
		}
	}
	return 0;
}

static void __exit observe_timing_exit(void) {}
module_init(observe_timing_init);
module_exit(observe_timing_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only T736 LPDDR4 timing/DFI observation");
