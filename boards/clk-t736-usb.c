// SPDX-License-Identifier: GPL-2.0-only
/* Initial USB1 clock/reset subset; does not claim ownership of the whole CCU. */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

#define N_CLOCKS 6
#define N_RESETS 3

struct t736_usb_ccu;
struct t736_gate {
	struct clk_hw hw;
	struct t736_usb_ccu *ccu;
	void __iomem *reg;
	u32 mask, key, mux_mask;
};

struct t736_usb_ccu {
	void __iomem *usb, *msi, *master;
	spinlock_t lock;
	struct reset_controller_dev reset;
	struct t736_gate gates[N_CLOCKS];
	struct clk_hw_onecell_data *clocks;
};

static int t736_gate_enable(struct clk_hw *hw)
{
	struct t736_gate *gate = container_of(hw, struct t736_gate, hw);
	unsigned long flags;
	u32 value;

	spin_lock_irqsave(&gate->ccu->lock, flags);
	value = readl(gate->reg);
	/* USB reference mux value 0 selects the existing 24 MHz system clock. */
	writel((value & ~gate->mux_mask) | gate->key | gate->mask, gate->reg);
	spin_unlock_irqrestore(&gate->ccu->lock, flags);
	return 0;
}

static void t736_gate_disable(struct clk_hw *hw)
{
	struct t736_gate *gate = container_of(hw, struct t736_gate, hw);
	unsigned long flags;

	spin_lock_irqsave(&gate->ccu->lock, flags);
	writel((readl(gate->reg) | gate->key) & ~gate->mask, gate->reg);
	spin_unlock_irqrestore(&gate->ccu->lock, flags);
}

static int t736_gate_enabled(struct clk_hw *hw)
{
	struct t736_gate *gate = container_of(hw, struct t736_gate, hw);

	return !!(readl(gate->reg) & gate->mask);
}

static const struct clk_ops t736_gate_ops = {
	.enable = t736_gate_enable,
	.disable = t736_gate_disable,
	.is_enabled = t736_gate_enabled,
};

static int t736_reset_update(struct reset_controller_dev *rc, unsigned long id,
			    bool assert)
{
	struct t736_usb_ccu *ccu = container_of(rc, struct t736_usb_ccu, reset);
	static const u32 offsets[] = { 0x08, 0x0c, 0x0c };
	static const u32 masks[] = { BIT(30), BIT(20), BIT(16) };
	unsigned long flags;
	u32 value;

	if (id >= N_RESETS)
		return -EINVAL;
	spin_lock_irqsave(&ccu->lock, flags);
	value = readl(ccu->usb + offsets[id]);
	writel(assert ? value & ~masks[id] : value | masks[id],
	       ccu->usb + offsets[id]);
	spin_unlock_irqrestore(&ccu->lock, flags);
	return 0;
}

static int t736_reset_assert(struct reset_controller_dev *rc, unsigned long id)
{
	return t736_reset_update(rc, id, true);
}

static int t736_reset_deassert(struct reset_controller_dev *rc, unsigned long id)
{
	return t736_reset_update(rc, id, false);
}

static const struct reset_control_ops t736_reset_ops = {
	.assert = t736_reset_assert,
	.deassert = t736_reset_deassert,
};

static int t736_usb_ccu_probe(struct platform_device *pdev)
{
	static const char * const names[] = {
		"t736-usb-ref", "t736-usb1-ehci", "t736-usb1-ohci",
		"t736-usb1-ohci-clock", "t736-usb-msi", "t736-usb-master"
	};
	static const u32 masks[] = { BIT(31), BIT(4), BIT(0), BIT(31), BIT(0), BIT(9) };
	struct clk_parent_data parent = { .index = 0 };
	struct device *dev = &pdev->dev;
	struct t736_usb_ccu *ccu;
	void __iomem *regs[N_CLOCKS];
	int i, ret;

	ccu = devm_kzalloc(dev, sizeof(*ccu), GFP_KERNEL);
	if (!ccu)
		return -ENOMEM;
	spin_lock_init(&ccu->lock);
	ccu->usb = devm_platform_ioremap_resource(pdev, 0);
	ccu->msi = devm_platform_ioremap_resource(pdev, 1);
	ccu->master = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(ccu->usb))
		return PTR_ERR(ccu->usb);
	if (IS_ERR(ccu->msi))
		return PTR_ERR(ccu->msi);
	if (IS_ERR(ccu->master))
		return PTR_ERR(ccu->master);
	regs[0] = ccu->usb + 0x40;
	regs[1] = ccu->usb + 0x0c;
	regs[2] = ccu->usb + 0x0c;
	regs[3] = ccu->usb + 0x08;
	regs[4] = ccu->msi;
	regs[5] = ccu->master;
	ccu->clocks = devm_kzalloc(dev, struct_size(ccu->clocks, hws, N_CLOCKS), GFP_KERNEL);
	if (!ccu->clocks)
		return -ENOMEM;
	ccu->clocks->num = N_CLOCKS;
	for (i = 0; i < N_CLOCKS; i++) {
		struct t736_gate *gate = &ccu->gates[i];
		struct clk_init_data init = {
			.name = names[i], .ops = &t736_gate_ops,
			.parent_data = &parent, .num_parents = 1,
		};

		gate->ccu = ccu;
		gate->reg = regs[i];
		gate->mask = masks[i];
		gate->key = i == 5 ? 0x010000ff : 0;
		gate->mux_mask = i == 0 ? GENMASK(26, 24) : 0;
		gate->hw.init = &init;
		ret = devm_clk_hw_register(dev, &gate->hw);
		if (ret)
			return ret;
		ccu->clocks->hws[i] = &gate->hw;
	}
	ccu->reset.owner = THIS_MODULE;
	ccu->reset.ops = &t736_reset_ops;
	ccu->reset.nr_resets = N_RESETS;
	ccu->reset.of_node = dev->of_node;
	ret = devm_reset_controller_register(dev, &ccu->reset);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, ccu->clocks);
}

static const struct of_device_id t736_usb_ccu_match[] = {
	{ .compatible = "allwinner,t736-usb-ccu" }, { }
};
static struct platform_driver t736_usb_ccu_driver = {
	.probe = t736_usb_ccu_probe,
	.driver = { .name = "t736-usb-ccu", .of_match_table = t736_usb_ccu_match },
};
builtin_platform_driver(t736_usb_ccu_driver);
