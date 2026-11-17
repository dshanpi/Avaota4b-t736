// SPDX-License-Identifier: GPL-2.0-only
/* Decoder PCK600 + only DEC0/DEC1 IOMMU bypass. Experimental always-on
 * provider for a private, physically contiguous, 32-bit WC DMA heap.
 * Never asserts IOMMU1 reset or alters its global translation/TTB state.
 */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reset.h>

struct ve_power {
	struct device *dev;
	struct generic_pm_domain genpd;
	struct clk_bulk_data clocks[8];
	void __iomem *ppu, *bypass[2];
};

static int ve_power_on(struct generic_pm_domain *domain)
{
	struct ve_power *p = container_of(domain, struct ve_power, genpd);
	u32 v;
	int i, ret;

	writel((readl(p->ppu) & ~0xf) | 8, p->ppu);
	ret = readl_poll_timeout(p->ppu + 8, v, (v & 0xf) == 8, 10, 10000);
	if (ret) {
		dev_err(p->dev, "VE_DEC PPU ON timeout: %08x\n", v);
		return ret;
	}
	/* MIC6 and MIC7 are physically in the decoder domain. */
	for (i = 0; i < ARRAY_SIZE(p->bypass); i++) {
		v = readl(p->bypass[i]);
		writel(v | BIT(0), p->bypass[i]);
		if (readl(p->bypass[i]) != (v | BIT(0)))
			return -EIO;
	}
	return 0;
}

static int ve_power_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "ppu", "ahb", "mbus-master",
		"mbus", "bus", "iommu1-mbus", "iommu1-sys", "ve" };
	struct device *dev = &pdev->dev;
	struct ve_power *p;
	struct reset_control *reset;
	int i, ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p) return -ENOMEM;
	p->dev = dev;
	p->ppu = devm_platform_ioremap_resource_byname(pdev, "ppu");
	p->bypass[0] = devm_platform_ioremap_resource_byname(pdev, "dec0-bypass");
	p->bypass[1] = devm_platform_ioremap_resource_byname(pdev, "dec1-bypass");
	if (IS_ERR(p->ppu)) return PTR_ERR(p->ppu);
	for (i = 0; i < ARRAY_SIZE(p->bypass); i++)
		if (IS_ERR(p->bypass[i])) return PTR_ERR(p->bypass[i]);
	for (i = 0; i < ARRAY_SIZE(p->clocks); i++) p->clocks[i].id = names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(p->clocks), p->clocks);
	if (ret) return dev_err_probe(dev, ret, "missing decoder clocks\n");
	reset = reset_control_get_exclusive(dev, "ve");
	if (IS_ERR(reset)) return PTR_ERR(reset);
	/* Pin all bus/core references before publishing this built-in domain.
	 * An error retains any requested PPU ON state; no consumer was exposed.
	 */
	ret = reset_control_assert(reset);
	if (ret) goto put_reset;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(p->clocks), p->clocks);
	if (ret) goto put_reset;
	if (clk_get_rate(p->clocks[7].clk) != 200000000) {
		ret = -ERANGE;
		goto disable_clocks;
	}
	writel(0x1f1f1f, p->ppu + 0x170);
	writel(0x1f1f, p->ppu + 0x174);
	writel(0x08080808, p->ppu + 0xc00);
	writel(0x808, p->ppu + 0xc04);
	writel(8, p->ppu + 0xc10);
	p->genpd.name = "t736-ve-dec";
	p->genpd.flags = GENPD_FLAG_ALWAYS_ON;
	p->genpd.power_on = ve_power_on;
	ret = ve_power_on(&p->genpd);
	if (ret) goto disable_clocks;
	ret = pm_genpd_init(&p->genpd, NULL, false);
	if (ret) goto disable_clocks;
	/* Leave decoder reset asserted. Its sole consumer owns deassert/start. */
	ret = reset_control_status(reset);
	if (ret != 1) {
		ret = ret < 0 ? ret : -EIO;
		goto remove_domain;
	}
	reset_control_put(reset);
	reset = NULL;
	ret = of_genpd_add_provider_simple(dev->of_node, &p->genpd);
	if (ret) goto remove_domain;
	dev_info(dev, "VE_DEC PPU=ON, 200MHz, DEC0/1 bypass verified; reset held\n");
	return 0;
remove_domain:
	pm_genpd_remove(&p->genpd);
disable_clocks:
	clk_bulk_disable_unprepare(ARRAY_SIZE(p->clocks), p->clocks);
put_reset:
	reset_control_put(reset);
	return dev_err_probe(dev, ret, "decoder power bootstrap failed\n");
}

static const struct of_device_id ve_power_match[] = {
	{ .compatible = "allwinner,t736-ve-power" }, { }
};
static struct platform_driver ve_power_driver = {
	.probe = ve_power_probe,
	.driver = { .name = "t736-ve-power", .of_match_table = ve_power_match,
		.suppress_bind_attrs = true },
};
builtin_platform_driver(ve_power_driver);
