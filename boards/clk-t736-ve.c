// SPDX-License-Identifier: GPL-2.0-only
/* T736 decoder-only CCU subset. Fixed PERI0_800M / 4, no PLL writes.
 * Manual 4.1.6.114: VE_DEC has a plain divider, NOT the GPU UPD handshake.
 * Shared master gates use the same CCF enable serialization as USB/display.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>

#define VE_RATE 200000000UL
#define VE_CLOCKS 8
#define VE_CORE 0
#define VE_PERI800 7
#define VE_MUX GENMASK(26, 24)
#define VE_DIV GENMASK(4, 0)
#define VE_FIXED ((2U << 24) | 3)

struct ve_ccu;
struct ve_clk {
	struct clk_hw hw;
	struct ve_ccu *ccu;
	void __iomem *reg;
	u32 mask;
	unsigned int id;
};
struct ve_ccu {
	void __iomem *ve, *ref, *peri;
	unsigned long dcxo;
	spinlock_t lock;
	struct reset_controller_dev reset;
	struct ve_clk clock[VE_CLOCKS];
	struct clk_hw_onecell_data *clocks;
};

static unsigned long ve_peri_rate(struct ve_ccu *c)
{
	u32 ref = readl(c->ref), peri = readl(c->peri);
	u32 rready = BIT(31) | BIT(30) | BIT(28) | BIT(27);
	u32 pready = BIT(31) | BIT(30) | BIT(28) | BIT(26);
	u64 rate;

	if ((ref & rready) != rready || (peri & pready) != pready)
		return 0;
	rate = div_u64((u64)c->dcxo * (((ref >> 8) & 0xff) + 1),
			(((ref >> 1) & 1) + 1) * (((ref >> 16) & 0x7f) + 1));
	if (rate != 24000000)
		return 0;
	return div_u64(rate * (((peri >> 8) & 0xff) + 1),
			(((peri >> 1) & 1) + 1) * (((peri >> 16) & 7) + 1));
}

static int ve_clk_enable(struct clk_hw *hw)
{
	struct ve_clk *g = container_of(hw, struct ve_clk, hw);
	unsigned long flags;
	u32 v, check;
	int ret = 0;

	if (g->id == VE_CORE && ve_peri_rate(g->ccu) != 800000000)
		return -ERANGE;
	spin_lock_irqsave(&g->ccu->lock, flags);
	v = readl(g->reg);
	if (g->id == VE_CORE) {
		if ((v & BIT(31)) && (v & (VE_MUX | VE_DIV)) != VE_FIXED) {
			ret = -EBUSY;
			goto out;
		}
		v = (v & ~(VE_MUX | VE_DIV)) | VE_FIXED;
	}
	/* Donor "KEY" constants are other real gate bits in the manual, not a
	 * write-unlock field. Preserve every unrelated gate instead. */
	v |= g->mask;
	writel(v, g->reg);
	check = readl(g->reg);
	if ((check & g->mask) != g->mask ||
	    (g->id == VE_CORE && (check & (VE_MUX | VE_DIV)) != VE_FIXED))
		ret = -EIO;
out:
	spin_unlock_irqrestore(&g->ccu->lock, flags);
	return ret;
}

static void ve_clk_disable(struct clk_hw *hw)
{
	struct ve_clk *g = container_of(hw, struct ve_clk, hw);
	unsigned long flags;

	/* Shared IOMMU1 reset is only deasserted during enable, never asserted;
	 * keep it and its clocks intact when dropping a local enable reference. */
	if (g->id == 5 || g->id == 6)
		return;
	spin_lock_irqsave(&g->ccu->lock, flags);
	writel(readl(g->reg) & ~g->mask, g->reg);
	readl(g->reg);
	spin_unlock_irqrestore(&g->ccu->lock, flags);
}

static int ve_clk_enabled(struct clk_hw *hw)
{
	struct ve_clk *g = container_of(hw, struct ve_clk, hw);
	return (readl(g->reg) & g->mask) == g->mask;
}

static unsigned long ve_clk_rate(struct clk_hw *hw, unsigned long parent)
{
	struct ve_clk *g = container_of(hw, struct ve_clk, hw);
	if (g->id == VE_PERI800)
		return ve_peri_rate(g->ccu);
	if (g->id == VE_CORE) {
		u32 v = readl(g->reg);
		return (v & VE_MUX) == (2U << 24) ? parent / ((v & VE_DIV) + 1) : 0;
	}
	return parent;
}

static long ve_round_rate(struct clk_hw *hw, unsigned long rate, unsigned long *parent)
{
	return rate == VE_RATE ? VE_RATE : -EINVAL;
}

static int ve_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent)
{
	struct ve_clk *g = container_of(hw, struct ve_clk, hw);
	unsigned long flags;
	u32 v;
	int ret = 0;
	if (rate != VE_RATE || ve_peri_rate(g->ccu) != 800000000)
		return -EINVAL;
	spin_lock_irqsave(&g->ccu->lock, flags);
	v = readl(g->reg);
	if ((v & BIT(31)) && (v & (VE_MUX | VE_DIV)) != VE_FIXED)
		ret = -EBUSY;
	else {
		writel((v & ~(VE_MUX | VE_DIV)) | VE_FIXED, g->reg);
		if ((readl(g->reg) & (VE_MUX | VE_DIV)) != VE_FIXED)
			ret = -EIO;
	}
	spin_unlock_irqrestore(&g->ccu->lock, flags);
	return ret;
}

static const struct clk_ops ve_core_ops = {
	.enable = ve_clk_enable, .disable = ve_clk_disable,
	.is_enabled = ve_clk_enabled, .recalc_rate = ve_clk_rate,
	.round_rate = ve_round_rate, .set_rate = ve_set_rate,
};
static const struct clk_ops ve_gate_ops = {
	.enable = ve_clk_enable, .disable = ve_clk_disable,
	.is_enabled = ve_clk_enabled, .recalc_rate = ve_clk_rate,
};
static const struct clk_ops ve_parent_ops = { .recalc_rate = ve_clk_rate };

static int ve_reset_update(struct reset_controller_dev *rc, unsigned long id, bool assert)
{
	struct ve_ccu *c = container_of(rc, struct ve_ccu, reset);
	unsigned long flags;
	u32 v;
	if (id)
		return -EINVAL;
	spin_lock_irqsave(&c->lock, flags);
	v = readl(c->ve + 4);
	writel(assert ? v & ~BIT(18) : v | BIT(18), c->ve + 4);
	v = readl(c->ve + 4);
	spin_unlock_irqrestore(&c->lock, flags);
	/* Readback verifies CCU reset state, not a promise of AXI drain. */
	return !!(v & BIT(18)) == !assert ? 0 : -EIO;
}
static int ve_reset_assert(struct reset_controller_dev *rc, unsigned long id)
{ return ve_reset_update(rc, id, true); }
static int ve_reset_deassert(struct reset_controller_dev *rc, unsigned long id)
{ return ve_reset_update(rc, id, false); }
static int ve_reset_status(struct reset_controller_dev *rc, unsigned long id)
{
	struct ve_ccu *c = container_of(rc, struct ve_ccu, reset);
	return id ? -EINVAL : !(readl(c->ve + 4) & BIT(18));
}
static const struct reset_control_ops ve_reset_ops = {
	.assert = ve_reset_assert, .deassert = ve_reset_deassert, .status = ve_reset_status,
};

static void __iomem *ve_shared(struct platform_device *pdev, const char *name)
{
	struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	void __iomem *base;
	if (!r || resource_size(r) != 4)
		return ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, r->start, 4);
	return base ? base : ERR_PTR(-ENOMEM);
}

static int ve_ccu_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "t736-ve-dec", "t736-ve-bus",
		"t736-ve-ahb", "t736-ve-mbus-master", "t736-ve-mbus",
		"t736-ve-iommu1-sys", "t736-ve-iommu1-mbus", "t736-ve-peri800" };
	static const char * const regs[] = { NULL, NULL, "ahb-master", "mbus-master",
		"mbus-gate", "iommu1-sys", "mbus-master", "pll-peri0" };
	static const u32 masks[] = { BIT(31), BIT(2), BIT(0), BIT(14), BIT(18),
		BIT(16) | GENMASK(2, 0), BIT(1), 0 };
	static const u8 order[] = { 7, 0, 1, 2, 3, 4, 5, 6 };
	struct device *dev = &pdev->dev;
	struct ve_ccu *c;
	struct clk *dcxo;
	int n, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c) return -ENOMEM;
	spin_lock_init(&c->lock);
	dcxo = devm_clk_get(dev, "dcxo");
	if (IS_ERR(dcxo)) return PTR_ERR(dcxo);
	c->dcxo = clk_get_rate(dcxo);
	c->ve = devm_platform_ioremap_resource_byname(pdev, "ve");
	c->ref = ve_shared(pdev, "pll-ref");
	c->peri = ve_shared(pdev, "pll-peri0");
	if (IS_ERR(c->ve)) return PTR_ERR(c->ve);
	if (IS_ERR(c->ref)) return PTR_ERR(c->ref);
	if (IS_ERR(c->peri)) return PTR_ERR(c->peri);
	if (ve_peri_rate(c) != 800000000)
		return dev_err_probe(dev, -ERANGE, "need locked PERI0_800M, ref=%08x peri=%08x\n",
			readl(c->ref), readl(c->peri));
	c->clocks = devm_kzalloc(dev, struct_size(c->clocks, hws, VE_CLOCKS), GFP_KERNEL);
	if (!c->clocks) return -ENOMEM;
	c->clocks->num = VE_CLOCKS;
	for (n = 0; n < VE_CLOCKS; n++) {
		int id = order[n];
		struct ve_clk *g = &c->clock[id];
		struct clk_parent_data parent = { .fw_name = "dcxo" };
		struct clk_init_data init = { .name = names[id], .parent_data = &parent,
			.num_parents = 1, .flags = CLK_GET_RATE_NOCACHE };
		g->ccu = c; g->id = id; g->mask = masks[id];
		g->reg = id < 2 ? c->ve + 4 * id : ve_shared(pdev, regs[id]);
		if (IS_ERR(g->reg)) return PTR_ERR(g->reg);
		init.ops = id == VE_CORE ? &ve_core_ops :
			(id == VE_PERI800 ? &ve_parent_ops : &ve_gate_ops);
		if (id == VE_CORE) { parent.fw_name = NULL; parent.hw = &c->clock[VE_PERI800].hw; }
		else if (id == 1 || id == 2 || id == 5) parent.fw_name = "ahb";
		else if (id != VE_PERI800) parent.fw_name = "mbus";
		g->hw.init = &init;
		ret = devm_clk_hw_register(dev, &g->hw);
		if (ret) return ret;
		c->clocks->hws[id] = &g->hw;
	}
	c->reset.owner = THIS_MODULE; c->reset.ops = &ve_reset_ops;
	c->reset.nr_resets = 1; c->reset.of_node = dev->of_node;
	ret = devm_reset_controller_register(dev, &c->reset);
	if (ret) return ret;
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, c->clocks);
}
static const struct of_device_id ve_ccu_match[] = {
	{ .compatible = "allwinner,t736-ve-ccu" }, { }
};
static struct platform_driver ve_ccu_driver = {
	.probe = ve_ccu_probe,
	.driver = { .name = "t736-ve-ccu", .of_match_table = ve_ccu_match,
		.suppress_bind_attrs = true },
};
builtin_platform_driver(ve_ccu_driver);
