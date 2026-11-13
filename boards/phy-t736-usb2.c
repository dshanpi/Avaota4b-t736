// SPDX-License-Identifier: GPL-2.0-only
/* Initial host-only USB1 PHY port, using the board's existing 24 MHz source. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

struct t736_usb_phy {
	void __iomem *pmu;
	struct clk_bulk_data clocks[4];
	struct reset_control *reset;
	struct regulator *vbus;
	u32 tuning;
};

static int t736_phy_init(struct phy *phy)
{
	struct t736_usb_phy *data = phy_get_drvdata(phy);
	u32 value, tune = data->tuning;
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(data->clocks), data->clocks);
	if (ret)
		return ret;
	ret = reset_control_assert(data->reset);
	if (ret)
		goto clocks_off;
	udelay(10);
	/* The port PMU is mapped separately from the EHCI and OHCI registers. */
	writel(readl(data->pmu + 0x10) & ~BIT(3), data->pmu + 0x10);
	ret = reset_control_deassert(data->reset);
	if (ret)
		goto clocks_off;
	udelay(10);
	value = readl(data->pmu + 0x18) & ~0xf3f;
	value |= (tune & 0xf) << 8 | (tune & 0x30) | ((tune >> 6) & 0xf);
	writel(value, data->pmu + 0x18);
	/* USB1 AHB INCR16/INCR4/alignment and host bypass; preserve other bits. */
	writel(readl(data->pmu) | BIT(15) | BIT(11) | BIT(9) | BIT(8) | BIT(0), data->pmu);
	return 0;
clocks_off:
	clk_bulk_disable_unprepare(ARRAY_SIZE(data->clocks), data->clocks);
	return ret;
}

static int t736_phy_exit(struct phy *phy)
{
	struct t736_usb_phy *data = phy_get_drvdata(phy);

	writel(readl(data->pmu + 0x10) | BIT(3), data->pmu + 0x10);
	reset_control_assert(data->reset);
	clk_bulk_disable_unprepare(ARRAY_SIZE(data->clocks), data->clocks);
	return 0;
}

static int t736_phy_on(struct phy *phy)
{
	return regulator_enable(((struct t736_usb_phy *)phy_get_drvdata(phy))->vbus);
}

static int t736_phy_off(struct phy *phy)
{
	return regulator_disable(((struct t736_usb_phy *)phy_get_drvdata(phy))->vbus);
}

static const struct phy_ops t736_phy_ops = {
	.init = t736_phy_init, .exit = t736_phy_exit,
	.power_on = t736_phy_on, .power_off = t736_phy_off, .owner = THIS_MODULE,
};

static int t736_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t736_usb_phy *data;
	struct phy_provider *provider;
	struct phy *phy;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->pmu = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(data->pmu))
		return PTR_ERR(data->pmu);
	data->clocks[0].id = "ref";
	data->clocks[1].id = "bus";
	data->clocks[2].id = "master";
	data->clocks[3].id = "msi";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(data->clocks), data->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "USB PHY clocks unavailable\n");
	data->reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(data->reset))
		return dev_err_probe(dev, PTR_ERR(data->reset), "USB PHY reset unavailable\n");
	data->vbus = devm_regulator_get(dev, "vbus");
	if (IS_ERR(data->vbus))
		return dev_err_probe(dev, PTR_ERR(data->vbus), "USB VBUS supply unavailable\n");
	ret = of_property_read_u32(dev->of_node, "allwinner,phy-tuning", &data->tuning);
	if (ret || data->tuning > 0x3ff)
		return -EINVAL;
	phy = devm_phy_create(dev, NULL, &t736_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, data);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id t736_phy_match[] = {
	{ .compatible = "allwinner,t736-usb1-phy" }, { }
};
static struct platform_driver t736_phy_driver = {
	.probe = t736_phy_probe,
	.driver = { .name = "t736-usb1-phy", .of_match_table = t736_phy_match },
};
builtin_platform_driver(t736_phy_driver);
