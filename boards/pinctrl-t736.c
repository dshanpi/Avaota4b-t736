// SPDX-License-Identifier: GPL-2.0-only
/* Next-stage main PIO candidate. Not enabled in the USB1 v4 kernel. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/platform_device.h>
#include "pinctrl-sunxi.h"

/* Bank A is absent; B..K counts are verified against the board SoC reference. */
static const u8 t736_pins[SUNXI_PINCTRL_MAX_BANKS] = {
	0, 11, 17, 24, 16, 7, 15, 20, 17, 28, 26
};
static const u8 t736_irq_muxes[SUNXI_PINCTRL_MAX_BANKS] = {
	0, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14
};
static const unsigned int t736_irq_map[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
/* Main PIO differs from R-PIO: PB starts at 0x100, PC at 0x180. */
static const struct sunxi_pinctrl_reg_layout t736_pio_layout = {
	.bank_offset = 0x80,
	.bank_stride = 0x80,
	.drive_offset = 0x20,
	.drive_width = 4,
	.pull_offset = 0x30,
	.irq_offset = 0xc0,
	.irq_stride = 0x80,
};
static struct sunxi_pinctrl_desc t736_desc = {
	.irq_banks = ARRAY_SIZE(t736_irq_map),
	.irq_bank_map = t736_irq_map,
	.reg_layout = &t736_pio_layout,
	/* Initial pad-voltage configuration remains owned by vendor firmware. */
};

static int t736_pio_probe(struct platform_device *pdev)
{
	return sunxi_pinctrl_dt_table_init(pdev, t736_pins, t736_irq_muxes,
					 &t736_desc, 0);
}

static const struct of_device_id t736_pio_match[] = {
	{ .compatible = "allwinner,t736-pinctrl" }, { }
};
static struct platform_driver t736_pio_driver = {
	.probe = t736_pio_probe,
	.driver = { .name = "t736-pinctrl", .of_match_table = t736_pio_match },
};
builtin_platform_driver(t736_pio_driver);
