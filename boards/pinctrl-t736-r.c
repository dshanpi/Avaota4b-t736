// SPDX-License-Identifier: GPL-2.0-only
/* T736 always-on GPIO banks. Alternate-function tables remain board-specific. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/platform_device.h>
#include "pinctrl-sunxi.h"

static const u8 t736_r_pins[SUNXI_PINCTRL_MAX_BANKS] = { 14, 6 };
static const u8 t736_r_muxes[SUNXI_PINCTRL_MAX_BANKS] = { 14, 14 };
static const unsigned int t736_r_irq_map[] = { 0, 1 };
static struct sunxi_pinctrl_desc t736_r_desc = {
	.pin_base = PL_BASE,
	.irq_banks = ARRAY_SIZE(t736_r_irq_map),
	.irq_bank_map = t736_r_irq_map,
	/* Pad supply voltage remains configured by the board PMIC firmware. */
};

static int t736_r_probe(struct platform_device *pdev)
{
	return sunxi_pinctrl_dt_table_init(pdev, t736_r_pins, t736_r_muxes,
					 &t736_r_desc, SUNXI_PINCTRL_NEW_REG_LAYOUT);
}

static const struct of_device_id t736_r_match[] = {
	{ .compatible = "allwinner,t736-r-pinctrl" }, { }
};
static struct platform_driver t736_r_driver = {
	.probe = t736_r_probe,
	.driver = { .name = "t736-r-pinctrl", .of_match_table = t736_r_match },
};
builtin_platform_driver(t736_r_driver);
