// SPDX-License-Identifier: GPL-2.0-only
/* Read-only observation under the legacy boot chain, not DRAM initialization. */
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

static int observe(const char *name, phys_addr_t address)
{
	void __iomem *reg = ioremap(address, sizeof(u32));

	if (!reg)
		return -ENOMEM;
	pr_info("t736-lp4-observe: %s @%pa = %08x\n",
		name, &address, readl(reg));
	iounmap(reg);
	return 0;
}

static int __init lp4_observe_init(void)
{
	static const struct {
		const char *name;
		phys_addr_t address;
	} regs[] = {
		{ "PLL_DDR", 0x02002020 },
		{ "DRAM0_CLK", 0x02002c00 },
		{ "DRAMC_SYS_CFG", 0x0a020010 },
		{ "DRAMC_CH_EN", 0x0a020014 },
		{ "CH0_MSTR", 0x0a110000 },
		{ "CH1_MSTR", 0x0a510000 },
		{ "CH0_STAT", 0x0a110014 },
		{ "CH1_STAT", 0x0a510014 },
		{ "CH0_SWSTAT", 0x0a110c84 },
		{ "CH1_SWSTAT", 0x0a510c84 },
		{ "CH0_ADDR_CS", 0x0a130004 },
		{ "CH0_ADDR_BANK", 0x0a13000c },
		{ "CH0_ADDR_BG", 0x0a130010 },
		{ "CH0_ADDR_COL_HI", 0x0a130014 },
		{ "CH0_ADDR_COL_LO", 0x0a130018 },
		{ "CH0_ADDR_ROW_HI", 0x0a13001c },
		{ "CH0_ADDR_ROW_0", 0x0a130020 },
		{ "CH0_ADDR_ROW_1", 0x0a130024 },
		{ "CH0_ADDR_ROW_2", 0x0a130028 },
		{ "CH0_ADDR_ROW_3", 0x0a13002c },
		{ "CH1_ADDR_CS", 0x0a530004 },
		{ "CH1_ADDR_BANK", 0x0a53000c },
		{ "CH1_ADDR_BG", 0x0a530010 },
		{ "CH1_ADDR_COL_HI", 0x0a530014 },
		{ "CH1_ADDR_COL_LO", 0x0a530018 },
		{ "CH1_ADDR_ROW_HI", 0x0a53001c },
		{ "CH1_ADDR_ROW_0", 0x0a530020 },
		{ "CH1_ADDR_ROW_1", 0x0a530024 },
		{ "CH1_ADDR_ROW_2", 0x0a530028 },
		{ "CH1_ADDR_ROW_3", 0x0a53002c },
	};
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("allwinner,t736"))
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		ret = observe(regs[i].name, regs[i].address);
		if (ret)
			return ret;
	}
	return 0;
}

static void __exit lp4_observe_exit(void) {}
module_init(lp4_observe_init);
module_exit(lp4_observe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only T736 LPDDR4 controller identification");
