// SPDX-License-Identifier: GPL-2.0-only
/* Fixed-board display CCU subset, from T736 CCU register documentation and
 * Tina GPL ccu-sun60iw2.c. Does not retune DDR, PERI or the reference PLL.
 * The separately owned USB AHB register is shared only through CCF callbacks,
 * which the common clock core serializes with its global enable lock.
 */
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

#define DISPLAY_CLOCKS 12
#define DISPLAY_RESETS 8
struct t736_display_ccu;
struct display_gate {
	struct clk_hw hw;
	struct t736_display_ccu *ccu;
	void __iomem *reg;
	u32 mask, key;
	unsigned long fixed_rate;
};
struct t736_display_ccu {
	void __iomem *de, *vo, *pll, *ref, *ahb, *mbus, *ppu;
	spinlock_t lock;
	struct reset_controller_dev reset;
	struct display_gate gates[DISPLAY_CLOCKS];
	struct clk_hw_onecell_data *clocks;
};

static int display_gate_enable(struct clk_hw *hw)
{
	struct display_gate *g = container_of(hw, struct display_gate, hw);
	unsigned long flags;
	spin_lock_irqsave(&g->ccu->lock, flags);
	writel(readl(g->reg) | g->key | g->mask, g->reg);
	spin_unlock_irqrestore(&g->ccu->lock, flags);
	return 0;
}
static void display_gate_disable(struct clk_hw *hw)
{
	struct display_gate *g = container_of(hw, struct display_gate, hw);
	unsigned long flags;
	spin_lock_irqsave(&g->ccu->lock, flags);
	writel((readl(g->reg) & ~g->mask) | g->key, g->reg);
	spin_unlock_irqrestore(&g->ccu->lock, flags);
}
static int display_gate_enabled(struct clk_hw *hw)
{
	struct display_gate *g = container_of(hw, struct display_gate, hw);
	return !!(readl(g->reg) & g->mask);
}
static unsigned long display_gate_rate(struct clk_hw *hw, unsigned long parent)
{
	struct display_gate *g = container_of(hw, struct display_gate, hw);
	return g->fixed_rate ? g->fixed_rate : parent;
}
static const struct clk_ops display_gate_ops = {
	.enable = display_gate_enable, .disable = display_gate_disable,
	.is_enabled = display_gate_enabled, .recalc_rate = display_gate_rate,
};

static int display_reset_update(struct reset_controller_dev *rc,
			       unsigned long id, bool assert)
{
	struct t736_display_ccu *c = container_of(rc, struct t736_display_ccu, reset);
	static const u16 offsets[] = { 0x04, 0x74, 0xc8, 0xe8, 0, 0x88, 0x88, 0x88 };
	static const u8 bits[] = { 16, 16, 16, 16, 16, 17, 16, 18 };
	void __iomem *reg;
	unsigned long flags;
	u32 v;
	if (id >= DISPLAY_RESETS)
		return -EINVAL;
	reg = (id < 2 ? c->de : c->vo) + offsets[id];
	spin_lock_irqsave(&c->lock, flags);
	v = readl(reg);
	writel(assert ? v & ~BIT(bits[id]) : v | BIT(bits[id]), reg);
	spin_unlock_irqrestore(&c->lock, flags);
	return 0;
}
static int display_reset_assert(struct reset_controller_dev *rc, unsigned long id)
{ return display_reset_update(rc, id, true); }
static int display_reset_deassert(struct reset_controller_dev *rc, unsigned long id)
{ return display_reset_update(rc, id, false); }
static const struct reset_control_ops display_reset_ops = {
	.assert = display_reset_assert, .deassert = display_reset_deassert,
};

static int display_pll_prepare(struct device *dev, struct t736_display_ccu *c)
{
	u32 ref = readl(c->ref), v;
	u64 rate = 26000000ULL * (((ref >> 8) & 255) + 1);
	int ret;
	rate /= ((ref >> 1) & 1) + 1;
	rate /= ((ref >> 16) & 127) + 1;
	if ((ref & (BIT(31) | BIT(28) | BIT(27))) !=
	    (BIT(31) | BIT(28) | BIT(27)) || rate != 24000000)
		return dev_err_probe(dev, -EINVAL,
			"PLL reference is not locked 24 MHz: %08x (%llu Hz)\n", ref, rate);
	/* Dedicated DE PLL: 24 MHz * 75 / 3 = 600 MHz on output 1.
	 * Explicitly turn off its spread-spectrum modulation before retuning.
	 */
	writel(readl(c->de) & ~BIT(31), c->de);
	writel(readl(c->pll + 0xc) & ~BIT(27), c->pll + 0xc);
	v = readl(c->pll);
	v &= ~(BIT(31) | BIT(29) | BIT(27) | BIT(26) |
		GENMASK(18, 16) | GENMASK(15, 8) | BIT(1));
	v |= BIT(30) | (74 << 8) | (2 << 16);
	writel(v, c->pll);
	udelay(10);
	writel(v | BIT(31) | BIT(29), c->pll);
	ret = readl_poll_timeout(c->pll, v, v & BIT(28), 10, 100000);
	if (ret)
		return dev_err_probe(dev, ret, "DE PLL lock timed out\n");
	writel(v | BIT(26), c->pll);
	/* DE source=PLL_DE_3X, divider=1, kept gated until the client enables it. */
	v = readl(c->de) & ~(GENMASK(26, 24) | GENMASK(4, 0) | BIT(31));
	writel(v, c->de);
	/* HDMI CEC uses LOSC; HDMI SFR uses the fixed SYS_CLK24M source. */
	writel(readl(c->vo + 0x7c) & ~GENMASK(26, 24), c->vo + 0x7c);
	writel(readl(c->vo + 0x8c) & ~GENMASK(26, 24), c->vo + 0x8c);
	dev_info(dev, "DE PLL locked at 600 MHz; HDMI PHY reference is board DCXO 26 MHz\n");
	return 0;
}

static int t736_display_ccu_probe(struct platform_device *pdev)
{
	static const char * const names[] = {
		"t736-de", "t736-de-bus", "t736-de-ahb", "t736-de-mbus",
		"t736-vo1-top", "t736-vo1-ahb", "t736-tv0", "t736-hdmi-cec",
		"t736-hdmi-bus", "t736-hdmi-sfr", "t736-hdcp", "t736-display-ppu"
	};
	/* DT input clocks: sys24m, dcxo, losc, ahb, mbus, peri300m. */
	static const u8 parents[] = { 0, 3, 3, 4, 3, 3, 3, 2, 1, 0, 5, 1 };
	static const u8 bits[] = { 31, 0, 5, 11, 0, 4, 0, 31, 0, 31, 31, 0 };
	struct device *dev = &pdev->dev;
	struct t736_display_ccu *c;
	struct resource *r;
	void __iomem *regs[DISPLAY_CLOCKS];
	int i, ret;
	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c) return -ENOMEM;
	spin_lock_init(&c->lock);
	c->de = devm_platform_ioremap_resource_byname(pdev, "de");
	c->vo = devm_platform_ioremap_resource_byname(pdev, "vo1");
	c->pll = devm_platform_ioremap_resource_byname(pdev, "pll-de");
	c->ref = devm_platform_ioremap_resource_byname(pdev, "pll-ref");
	c->mbus = devm_platform_ioremap_resource_byname(pdev, "mbus-master");
	c->ppu = devm_platform_ioremap_resource_byname(pdev, "ppu");
	if (IS_ERR(c->de)) return PTR_ERR(c->de);
	if (IS_ERR(c->vo)) return PTR_ERR(c->vo);
	if (IS_ERR(c->pll)) return PTR_ERR(c->pll);
	if (IS_ERR(c->ref)) return PTR_ERR(c->ref);
	if (IS_ERR(c->mbus)) return PTR_ERR(c->mbus);
	if (IS_ERR(c->ppu)) return PTR_ERR(c->ppu);
	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ahb-master");
	if (!r || resource_size(r) != 4) return -EINVAL;
	c->ahb = devm_ioremap(dev, r->start, resource_size(r));
	if (!c->ahb) return -ENOMEM;
	ret = display_pll_prepare(dev, c);
	if (ret) return ret;
	regs[0]=c->de; regs[1]=c->de+4; regs[2]=c->ahb; regs[3]=c->mbus;
	regs[4]=c->vo+0xc8; regs[5]=c->ahb; regs[6]=c->vo;
	regs[7]=c->vo+0x7c; regs[8]=c->vo+0x88; regs[9]=c->vo+0x8c;
	regs[10]=c->vo+0x90; regs[11]=c->ppu;
	c->clocks=devm_kzalloc(dev,struct_size(c->clocks,hws,DISPLAY_CLOCKS),GFP_KERNEL);
	if (!c->clocks) return -ENOMEM;
	c->clocks->num=DISPLAY_CLOCKS;
	for (i=0;i<DISPLAY_CLOCKS;i++) {
		struct display_gate *g=&c->gates[i];
		struct clk_parent_data parent={ .index=parents[i] };
		struct clk_init_data init={ .name=names[i], .ops=&display_gate_ops,
			.parent_data=&parent, .num_parents=1, .flags=CLK_GET_RATE_NOCACHE };
		g->ccu=c; g->reg=regs[i]; g->mask=BIT(bits[i]);
		g->key=(i==2 || i==5) ? 0x010000ff : (i==3 ? 0x41055800 : 0);
		g->fixed_rate=i==0 ? 600000000 : 0;
		g->hw.init=&init;
		ret=devm_clk_hw_register(dev,&g->hw);
		if (ret) return ret;
		c->clocks->hws[i]=&g->hw;
	}
	c->reset.owner=THIS_MODULE; c->reset.ops=&display_reset_ops;
	c->reset.nr_resets=DISPLAY_RESETS; c->reset.of_node=dev->of_node;
	ret=devm_reset_controller_register(dev,&c->reset);
	if (ret) return ret;
	return devm_of_clk_add_hw_provider(dev,of_clk_hw_onecell_get,c->clocks);
}
static const struct of_device_id display_ccu_match[] = {
	{ .compatible="allwinner,t736-display-ccu" }, { }
};
static struct platform_driver display_ccu_driver = {
	.probe=t736_display_ccu_probe,
	.driver={ .name="t736-display-ccu", .of_match_table=display_ccu_match },
};
builtin_platform_driver(display_ccu_driver);
