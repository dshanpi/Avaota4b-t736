// SPDX-License-Identifier: GPL-2.0-only
/* T736 RGMII glue candidate; not enabled in the USB1 v4 kernel. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include "stmmac.h"
#include "stmmac_platform.h"

struct t736_dwmac {
	void __iomem *cfg;
	struct clk *phy_clock, *mbus_clock;
	struct regulator *phy_supply;
	struct reset_control *ahb_reset;
	u32 config;
	u32 tx_taps, rx_taps;
};

static int t736_dwmac_init(struct platform_device *pdev, void *private)
{
	struct t736_dwmac *data = private;
	u32 value;
	int ret;

	ret = regulator_enable(data->phy_supply);
	if (ret)
		return ret;
	ret = clk_prepare_enable(data->mbus_clock);
	if (ret) {
		regulator_disable(data->phy_supply);
		return ret;
	}
	ret = clk_prepare_enable(data->phy_clock);
	if (ret) {
		clk_disable_unprepare(data->mbus_clock);
		regulator_disable(data->phy_supply);
		return ret;
	}
	if (clk_get_rate(data->phy_clock) != 25000000) {
		clk_disable_unprepare(data->phy_clock);
		clk_disable_unprepare(data->mbus_clock);
		regulator_disable(data->phy_supply);
		return dev_err_probe(&pdev->dev, -EINVAL, "PHY requires 25 MHz\n");
	}
	/* The extension register belongs to the AHB reset domain. Release it
	 * before programming RGMII; the core later pulses only the AXI reset.
	 * Balance this shared reference in exit, including probe failures.
	 */
	ret = reset_control_deassert(data->ahb_reset);
	if (ret) {
		clk_disable_unprepare(data->phy_clock);
		clk_disable_unprepare(data->mbus_clock);
		regulator_disable(data->phy_supply);
		return ret;
	}
	/* RGMII with external transmit clock. Delay values are hardware taps,
	 * not picoseconds; preserve all unrelated bits in the extension block.
	 */
	value = readl(data->cfg);
	/* PHY_SELECT (bit 15) resets to internal PHY. This board uses an
	 * external RTL8211F; leaving it set disconnects the external MDIO.
	 */
	value &= ~(GENMASK(17, 16) | BIT(15) | BIT(13) | GENMASK(12, 5) | GENMASK(2, 0));
	value |= BIT(2) | 1;
	value |= FIELD_PREP(GENMASK(17, 16), data->tx_taps >> 3);
	value |= FIELD_PREP(GENMASK(12, 10), data->tx_taps & 7);
	value |= FIELD_PREP(GENMASK(9, 5), data->rx_taps);
	writel(value, data->cfg);
	data->config = value;
	dev_info(&pdev->dev, "RGMII MAC delays TX=%u RX=%u, PHY clock 25 MHz\n",
		 data->tx_taps, data->rx_taps);
	return 0;
}

static void t736_dwmac_exit(struct platform_device *pdev, void *private)
{
	struct t736_dwmac *data = private;

	reset_control_assert(data->ahb_reset);
	clk_disable_unprepare(data->phy_clock);
	clk_disable_unprepare(data->mbus_clock);
	regulator_disable(data->phy_supply);
}

static int t736_dwmac_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct stmmac_resources resources;
	struct plat_stmmacenet_data *plat;
	struct t736_dwmac *data;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	ret = stmmac_get_platform_resources(pdev, &resources);
	if (ret)
		return ret;
	plat = devm_stmmac_probe_config_dt(pdev, resources.mac);
	if (IS_ERR(plat))
		return PTR_ERR(plat);
	data->ahb_reset = plat->stmmac_ahb_rst;
	/* Board RGMII uses an external clock and MAC tap calibration. RXID
	 * additionally requests the RTL8211F's 2 ns receive delay; validated
	 * with PHY loopback and bidirectional traffic on this board.
	 */
	if (plat->phy_interface != PHY_INTERFACE_MODE_RGMII &&
	    plat->phy_interface != PHY_INTERFACE_MODE_RGMII_RXID)
		return dev_err_probe(dev, -EINVAL, "Only RGMII and RGMII-RXID are implemented\n");
	data->cfg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(data->cfg))
		return PTR_ERR(data->cfg);
	data->phy_clock = devm_clk_get(dev, "phy");
	if (IS_ERR(data->phy_clock))
		return dev_err_probe(dev, PTR_ERR(data->phy_clock), "PHY clock unavailable\n");
	data->mbus_clock = devm_clk_get(dev, "mbus");
	if (IS_ERR(data->mbus_clock))
		return dev_err_probe(dev, PTR_ERR(data->mbus_clock), "MBUS clock unavailable\n");
	data->phy_supply = devm_regulator_get(dev, "phy");
	if (IS_ERR(data->phy_supply))
		return dev_err_probe(dev, PTR_ERR(data->phy_supply), "PHY supply unavailable\n");
	ret = of_property_read_u32(dev->of_node, "allwinner,tx-delay-taps", &data->tx_taps);
	if (ret || data->tx_taps > 31)
		return -EINVAL;
	ret = of_property_read_u32(dev->of_node, "allwinner,rx-delay-taps", &data->rx_taps);
	if (ret || data->rx_taps > 31)
		return -EINVAL;
	resources.tx_irq[0] = platform_get_irq_byname(pdev, "tx0_irq");
	if (resources.tx_irq[0] < 0)
		return resources.tx_irq[0];
	resources.rx_irq[0] = platform_get_irq_byname(pdev, "rx0_irq");
	if (resources.rx_irq[0] < 0)
		return resources.rx_irq[0];
	if (plat->tx_queues_to_use != 1 || plat->rx_queues_to_use != 1)
		return dev_err_probe(dev, -EINVAL, "Only queue 0 is wired on this path\n");
	plat->bsp_priv = data;
	plat->init = t736_dwmac_init;
	plat->exit = t736_dwmac_exit;
	plat->host_dma_width = 32;
	plat->core_type = DWMAC_CORE_GMAC4;
	plat->pmt = 1;
	plat->flags |= STMMAC_FLAG_SPH_DISABLE | STMMAC_FLAG_MULTI_MSI_EN;
	/* Board AHB is presently initialized to 200 MHz by vendor firmware.
	 * The selected CSR divisor is 102, keeping MDC below 2.5 MHz.
	 */
	plat->clk_csr = 4;
	ret = devm_stmmac_pltfr_probe(pdev, plat, &resources);
	if (ret)
		return ret;
	if (readl(data->cfg) != data->config)
		return dev_err_probe(dev, -EIO, "RGMII configuration lost during MAC reset\n");
	dev_info(dev, "RGMII configuration retained after MAC reset: %#x\n", data->config);
	return 0;
}

static const struct of_device_id t736_dwmac_match[] = {
	{ .compatible = "allwinner,t736-dwmac" }, { }
};
MODULE_DEVICE_TABLE(of, t736_dwmac_match);

static struct platform_driver t736_dwmac_driver = {
	.probe = t736_dwmac_probe,
	.driver = {
		.name = "t736-dwmac", .of_match_table = t736_dwmac_match,
		.pm = &stmmac_pltfr_pm_ops,
	},
};
module_platform_driver(t736_dwmac_driver);
MODULE_DESCRIPTION("T736 board initial RGMII integration");
MODULE_LICENSE("GPL");
