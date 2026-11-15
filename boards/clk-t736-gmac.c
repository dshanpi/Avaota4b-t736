// SPDX-License-Identifier: GPL-2.0-only
/* GMAC0 clock/reset subset. Parent PLL and bus rates are firmware-owned. */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

#define N_CLOCKS 3
#define N_RESETS 2
#define MBUS_KEY 0x40302

struct t736_gmac_ccu;
struct t736_gmac_gate {
	struct clk_hw hw;
	struct t736_gmac_ccu *ccu;
	void __iomem *reg;
	u32 mask, key;
	bool phy;
};

struct t736_gmac_ccu {
	void __iomem *gmac, *mbus;
	spinlock_t lock;
	struct reset_controller_dev reset;
	struct t736_gmac_gate gates[N_CLOCKS];
	struct clk_hw_onecell_data *clocks;
};

static int t736_gmac_gate_enable(struct clk_hw *hw)
{
	struct t736_gmac_gate *gate = container_of(hw, struct t736_gmac_gate, hw);
	unsigned long flags;
	u32 value;

	spin_lock_irqsave(&gate->ccu->lock, flags);
	value = readl(gate->reg);
	/* 150 MHz / (5 + 1) = 25 MHz. Do not change the shared PLL. */
	if (gate->phy)
		value = (value & ~GENMASK(4, 0)) | 5;
	writel(value | gate->key | gate->mask, gate->reg);
	spin_unlock_irqrestore(&gate->ccu->lock, flags);
	return 0;
}

static void t736_gmac_gate_disable(struct clk_hw *hw)
{
	struct t736_gmac_gate *gate = container_of(hw, struct t736_gmac_gate, hw);
	unsigned long flags;

	spin_lock_irqsave(&gate->ccu->lock, flags);
	writel((readl(gate->reg) | gate->key) & ~gate->mask, gate->reg);
	spin_unlock_irqrestore(&gate->ccu->lock, flags);
}

static int t736_gmac_gate_enabled(struct clk_hw *hw)
{
	struct t736_gmac_gate *gate = container_of(hw, struct t736_gmac_gate, hw);

	return !!(readl(gate->reg) & gate->mask);
}

static unsigned long t736_gmac_gate_rate(struct clk_hw *hw, unsigned long parent)
{
	struct t736_gmac_gate *gate = container_of(hw, struct t736_gmac_gate, hw);

	return gate->phy ? parent / ((readl(gate->reg) & GENMASK(4, 0)) + 1) : parent;
}

static const struct clk_ops t736_gmac_gate_ops = {
	.enable = t736_gmac_gate_enable,
	.disable = t736_gmac_gate_disable,
	.is_enabled = t736_gmac_gate_enabled,
	.recalc_rate = t736_gmac_gate_rate,
};

static int t736_gmac_reset_update(struct reset_controller_dev *rc,
				  unsigned long id, bool assert)
{
	struct t736_gmac_ccu *ccu = container_of(rc, struct t736_gmac_ccu, reset);
	unsigned long flags;
	u32 value, mask;

	if (id >= N_RESETS)
		return -EINVAL;
	mask = BIT(16 + id);
	spin_lock_irqsave(&ccu->lock, flags);
	value = readl(ccu->gmac + 0xc);
	writel(assert ? value & ~mask : value | mask, ccu->gmac + 0xc);
	spin_unlock_irqrestore(&ccu->lock, flags);
	return 0;
}

static int t736_gmac_reset_assert(struct reset_controller_dev *rc, unsigned long id)
{
	return t736_gmac_reset_update(rc, id, true);
}

static int t736_gmac_reset_deassert(struct reset_controller_dev *rc, unsigned long id)
{
	return t736_gmac_reset_update(rc, id, false);
}

static const struct reset_control_ops t736_gmac_reset_ops = {
	.assert = t736_gmac_reset_assert,
	.deassert = t736_gmac_reset_deassert,
};

static int t736_gmac_ccu_probe(struct platform_device *pdev)
{
	static const char * const names[] = {
		"t736-gmac0-bus", "t736-gmac0-phy", "t736-gmac0-mbus"
	};
	static const u32 masks[] = { BIT(0), BIT(31), BIT(11) };
	struct device *dev = &pdev->dev;
	struct t736_gmac_ccu *ccu;
	void __iomem *regs[N_CLOCKS];
	int i, ret;

	ccu = devm_kzalloc(dev, sizeof(*ccu), GFP_KERNEL);
	if (!ccu)
		return -ENOMEM;
	spin_lock_init(&ccu->lock);
	ccu->gmac = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ccu->gmac))
		return PTR_ERR(ccu->gmac);
	ccu->mbus = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ccu->mbus))
		return PTR_ERR(ccu->mbus);
	regs[0] = ccu->gmac + 0xc;
	regs[1] = ccu->gmac;
	regs[2] = ccu->mbus;
	ccu->clocks = devm_kzalloc(dev, struct_size(ccu->clocks, hws, N_CLOCKS), GFP_KERNEL);
	if (!ccu->clocks)
		return -ENOMEM;
	ccu->clocks->num = N_CLOCKS;
	for (i = 0; i < N_CLOCKS; i++) {
		struct t736_gmac_gate *gate = &ccu->gates[i];
		struct clk_parent_data parent = { .index = i };
		struct clk_init_data init = {
			.name = names[i], .ops = &t736_gmac_gate_ops,
			.parent_data = &parent, .num_parents = 1,
			.flags = CLK_GET_RATE_NOCACHE,
		};

		gate->ccu = ccu;
		gate->reg = regs[i];
		gate->mask = masks[i];
		gate->key = i == 2 ? MBUS_KEY : 0;
		gate->phy = i == 1;
		gate->hw.init = &init;
		ret = devm_clk_hw_register(dev, &gate->hw);
		if (ret)
			return ret;
		ccu->clocks->hws[i] = &gate->hw;
	}
	ccu->reset.owner = THIS_MODULE;
	ccu->reset.ops = &t736_gmac_reset_ops;
	ccu->reset.nr_resets = N_RESETS;
	ccu->reset.of_node = dev->of_node;
	ret = devm_reset_controller_register(dev, &ccu->reset);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, ccu->clocks);
}

static const struct of_device_id t736_gmac_ccu_match[] = {
	{ .compatible = "allwinner,t736-gmac-ccu" }, { }
};
static struct platform_driver t736_gmac_ccu_driver = {
	.probe = t736_gmac_ccu_probe,
	.driver = { .name = "t736-gmac-ccu", .of_match_table = t736_gmac_ccu_match },
};
builtin_platform_driver(t736_gmac_ccu_driver);
