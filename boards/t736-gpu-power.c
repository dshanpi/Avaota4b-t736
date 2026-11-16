// SPDX-License-Identifier: GPL-2.0-only
/* Fixed-board GPU_TOP + GPU_CORE always-on domain. PLL_GPU0 must lock and
 * drive its output before CORE is requested, even while GPU uses PERI400.
 * Publish only after both PPU ACKs, actual voltage, reset and rate readback.
 */
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

struct t736_gpu_power {
	struct device *dev;
	struct generic_pm_domain genpd;
	struct generic_pm_domain *domains[1];
	struct genpd_onecell_data data;
	void __iomem *top, *core;
	struct regulator *supply;
	struct clk_bulk_data clocks[4];
	struct clk_bulk_data bootstrap_clocks[2];
};

static void gpu_ppu_delays(void __iomem *base)
{
	writel(0x1f1f1f, base + 0x170);
	writel(0x1f1f, base + 0x174);
	writel(0x08080808, base + 0xc00);
	writel(0x808, base + 0xc04);
	writel(8, base + 0xc10);
}

static int gpu_ppu_wait_on(struct t736_gpu_power *p, void __iomem *base,
			   const char *name)
{
	u32 v;
	int ret;

	ret = readl_poll_timeout(base + 8, v, (v & 0xf) == 8, 10, 10000);
	if (ret)
		dev_err(p->dev, "%s power-on timeout: status=%08x\n", name, v);
	return ret;
}

static int gpu_power_request_on(struct t736_gpu_power *p)
{
	int ret;

	ret = regulator_get_voltage(p->supply);
	if (ret < 0)
		return ret;
	if (ret != 900000)
		return dev_err_probe(p->dev, -ERANGE,
				     "DCDC5 must read back 900 mV: %d uV\n", ret);
	/* PLL recalc returns zero unless enable/LDO/lock/output are all set. */
	if (!clk_get_rate(p->clocks[0].clk))
		return dev_err_probe(p->dev, -EIO, "PLL_GPU0 is not ready\n");
	writel((readl(p->top) & ~0xf) | 8, p->top);
	readl(p->top);
	ret = gpu_ppu_wait_on(p, p->top, "GPU_TOP");
	if (ret)
		return ret;
	/* Match Tina's CORE policy/request without generic CORE delay writes.
	 * With PLL_GPU0 ready, real-board status reaches 8. Early DDK MMIO
	 * hangs the CPU if this precondition has not been satisfied.
	 */
	writel(0, p->core + 0x20);
	if (readl(p->core + 0x20))
		return -EIO;
	writel(8, p->core);
	if (readl(p->core) != 8)
		return -EIO;
	return 0;
}

static int gpu_power_on(struct generic_pm_domain *domain)
{
	struct t736_gpu_power *p = container_of(domain, struct t736_gpu_power, genpd);
	int ret = gpu_power_request_on(p);

	return ret ? ret : gpu_ppu_wait_on(p, p->core, "GPU_CORE");
}

static void gpu_bootstrap_abort(struct t736_gpu_power *p,
				struct reset_control *reset)
{
	int ret;

	if (reset) {
		ret = reset_control_assert(reset);
		if (ret)
			dev_err(p->dev, "GPU reset rollback failed: %d\n", ret);
	}
	/* No consumer exists before publication. Cancel CORE only; retain TOP
	 * and leave the other PCK domains unchanged.
	 */
	writel(0, p->core);
	readl(p->core);
}

static int t736_gpu_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t736_gpu_power *p;
	struct reset_control *reset;
	bool bootstrap_enabled = false;
	int ret, reset_status;
	unsigned long rate;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;
	p->top = devm_platform_ioremap_resource_byname(pdev, "top");
	p->core = devm_platform_ioremap_resource_byname(pdev, "core");
	if (IS_ERR(p->top)) return PTR_ERR(p->top);
	if (IS_ERR(p->core)) return PTR_ERR(p->core);
	p->supply = devm_regulator_get(dev, "gpu");
	if (IS_ERR(p->supply))
		return dev_err_probe(dev, PTR_ERR(p->supply), "missing real DCDC5\n");
	/* All six clocks stay enabled for this built-in provider's lifetime.
	 * PLL_GPU0 is an independent prerequisite, not the core mux parent.
	 */
	p->clocks[0].id = "pll-gpu0";
	p->clocks[1].id = "ppu";
	p->clocks[2].id = "ahb";
	p->clocks[3].id = "mbus";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(p->clocks), p->clocks);
	if (ret)
		return dev_err_probe(dev, ret, "missing GPU power clocks\n");
	p->bootstrap_clocks[0].id = "gpu-bus";
	p->bootstrap_clocks[1].id = "gpu";
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(p->bootstrap_clocks), p->bootstrap_clocks);
	if (ret)
		return dev_err_probe(dev, ret, "missing GPU bus/core clocks\n");
	reset = reset_control_get_exclusive(dev, "reset_bus");
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "missing GPU reset\n");
	ret = regulator_enable(p->supply);
	if (ret)
		goto put_reset;
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(p->clocks), p->clocks);
	if (ret)
		goto disable_supply;
	gpu_ppu_delays(p->top);
	p->genpd.name = "t736-gpu-top-core";
	p->genpd.power_on = gpu_power_on;
	p->genpd.flags = GENPD_FLAG_ALWAYS_ON;
	ret = gpu_power_request_on(p);
	if (ret)
		goto abort;
	ret = reset_control_deassert(reset);
	if (ret)
		goto abort;
	reset_status = reset_control_status(reset);
	if (reset_status) {
		ret = reset_status < 0 ? reset_status : -EIO;
		dev_err(dev, "GPU reset deassert readback failed: %d\n", reset_status);
		goto abort;
	}
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(p->bootstrap_clocks), p->bootstrap_clocks);
	if (ret)
		goto abort;
	bootstrap_enabled = true;
	rate = clk_get_rate(p->bootstrap_clocks[1].clk);
	if (rate != 400000000) {
		ret = -ERANGE;
		dev_err(dev, "GPU bootstrap clock is not 400 MHz: %lu\n", rate);
		goto abort;
	}
	/* Cold-start ACK is verified after reset and the GPU clock UPD;
	 * the observed hardware sequence does not require an earlier ACK.
	 */
	ret = gpu_ppu_wait_on(p, p->core, "GPU_CORE");
	if (ret)
		goto abort;
	ret = pm_genpd_init(&p->genpd, NULL, false);
	if (ret)
		goto abort;
	p->domains[0] = &p->genpd;
	p->data.domains = p->domains;
	p->data.num_domains = ARRAY_SIZE(p->domains);
	/* DDK takes this reset exclusively after publication. */
	reset_control_put(reset);
	reset = NULL;
	ret = of_genpd_add_provider_onecell(dev->of_node, &p->data);
	if (ret) {
		pm_genpd_remove(&p->genpd);
		reset = reset_control_get_exclusive(dev, "reset_bus");
		if (IS_ERR(reset)) {
			dev_err(dev, "cannot reacquire reset for rollback: %ld\n", PTR_ERR(reset));
			reset = NULL;
		}
		goto abort;
	}
	dev_info(dev, "GPU_TOP/CORE ACK=8; PLL_GPU0 ready, GPU=%lu Hz, DCDC5=900000 uV, reset verified\n",
		 rate);
	return 0;
abort:
	gpu_bootstrap_abort(p, reset);
	if (bootstrap_enabled)
		clk_bulk_disable_unprepare(ARRAY_SIZE(p->bootstrap_clocks), p->bootstrap_clocks);
	clk_bulk_disable_unprepare(ARRAY_SIZE(p->clocks), p->clocks);
disable_supply:
	regulator_disable(p->supply);
put_reset:
	reset_control_put(reset);
	return dev_err_probe(dev, ret, "GPU bootstrap failed; domain not published\n");
}

static const struct of_device_id gpu_power_match[] = {
	{ .compatible = "allwinner,t736-gpu-power" }, { }
};
static struct platform_driver gpu_power_driver = {
	.probe = t736_gpu_power_probe,
	.driver = { .name = "t736-gpu-power", .of_match_table = gpu_power_match,
		    .suppress_bind_attrs = true },
};
builtin_platform_driver(gpu_power_driver);
