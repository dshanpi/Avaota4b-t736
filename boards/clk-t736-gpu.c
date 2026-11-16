// SPDX-License-Identifier: GPL-2.0-only
/* Fixed 400 MHz T736 GPU CCU subset, based on Tina ccu-sun60iw2.c.
 * PLL_REF and PLL_PERI0 are inspected, never retuned. Shared AHB/MBUS
 * registers are modified only in CCF enable callbacks, under the global
 * CCF enable lock also used by the existing USB/display clock drivers.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reset-controller.h>
#include <linux/spinlock.h>

#define GPU_RATE 400000000UL
#define GPU_CLOCKS 6
#define GPU_CORE 0
#define GPU_BUS 1
#define GPU_PERI400 2
#define GPU_AHB 3
#define GPU_MBUS 4
#define GPU_PLL 5
#define GPU_PLL_OUTPUT BIT(27)
#define GPU_PLL_LOCK BIT(28)
#define GPU_PLL_LOCK_DETECT BIT(29)
#define GPU_PLL_LDO BIT(30)
#define GPU_PLL_ENABLE BIT(31)
#define GPU_PLL_CONTROL (GPU_PLL_OUTPUT | GPU_PLL_LOCK_DETECT | \
			 GPU_PLL_LDO | GPU_PLL_ENABLE)
#define GPU_PLL_READY (GPU_PLL_CONTROL | GPU_PLL_LOCK)
#define GPU_UPDATE_REQUEST BIT(27)
#define GPU_UPDATE_TIMEOUT_US 1000
#define GPU_MUX_MASK GENMASK(26, 24)
#define GPU_DIV_MASK GENMASK(3, 0)
#define GPU_MUX_PERI400 (3U << 24)

struct t736_gpu_ccu;
struct gpu_clock {
	struct clk_hw hw;
	struct t736_gpu_ccu *ccu;
	void __iomem *reg;
	u32 mask, key;
	unsigned int id;
};

struct t736_gpu_ccu {
	struct device *dev;
	void __iomem *gpu, *ref, *peri, *ahb, *mbus, *pll;
	unsigned long dcxo_rate;
	spinlock_t lock;
	struct reset_controller_dev reset;
	struct gpu_clock clock[GPU_CLOCKS];
	struct clk_hw_onecell_data *clocks;
};

/* Manual 4.1.4.5 and 4.1.6.120: mux, cycle mask AND gating are committed
 * through UPD. CCF enable/disable callbacks cannot sleep, so bound the atomic
 * req/ack wait. Readback must match the requested fields after ACK clears.
 */
static int gpu_core_update(struct gpu_clock *g, u32 requested)
{
	u32 v, mask = GPU_MUX_MASK | GPU_DIV_MASK | BIT(31);
	int ret;

	writel(requested | GPU_UPDATE_REQUEST, g->reg);
	ret = readl_poll_timeout_atomic(g->reg, v,
			!(v & GPU_UPDATE_REQUEST), 1, GPU_UPDATE_TIMEOUT_US);
	if (ret)
		return ret;
	return (v & mask) == (requested & mask) ? 0 : -EIO;
}

/* PLL_PERI0_400M = PLL_REF * (N + 1) / (M_2X + 1) / 3.
 * PLL_REF uses the board's 26 MHz DCXO, not a presumed 24 MHz crystal.
 */
static unsigned long gpu_ref_rate(struct t736_gpu_ccu *c)
{
	u32 ref = readl(c->ref);
	u32 ready = BIT(31) | BIT(28) | BIT(27);
	u64 rate;

	if ((ref & ready) != ready)
		return 0;
	rate = (u64)c->dcxo_rate * (((ref >> 8) & 0xff) + 1);
	return div_u64(rate, (((ref >> 1) & 1) + 1) *
		       (((ref >> 16) & 0x7f) + 1));
}

static unsigned long gpu_peri400_rate(struct t736_gpu_ccu *c)
{
	u32 peri = readl(c->peri);
	u32 ready = BIT(31) | BIT(28) | BIT(27);
	u64 rate = gpu_ref_rate(c);

	if ((peri & ready) != ready)
		return 0;
	if (rate != 24000000)
		return 0;
	rate *= ((peri >> 8) & 0xff) + 1;
	return div_u64(rate, (((peri >> 20) & 7) + 1) * 3);
}

static int gpu_pll_write(struct t736_gpu_ccu *c, u32 requested)
{
	writel(requested, c->pll);
	return ((readl(c->pll) ^ requested) & ~GPU_PLL_LOCK) ? -EIO : 0;
}

static int gpu_pll_wait_lock(struct t736_gpu_ccu *c)
{
	unsigned int samples, locked = 0;

	for (samples = 0; samples < 100 && locked < 3; samples++) {
		locked = readl(c->pll) & GPU_PLL_LOCK ? locked + 1 : 0;
		udelay(3);
	}
	if (locked != 3)
		return -ETIMEDOUT;
	udelay(20);
	return readl(c->pll) & GPU_PLL_LOCK ? 0 : -EIO;
}

/* PLL_GPU0 is a power/isolation prerequisite even while GPU0_CLK selects
 * PERI400. Match the verified Tina sequence, preserving N/M and SDM fields.
 * The power provider keeps a real CCF enable reference for its lifetime.
 */
static int gpu_pll_enable(struct clk_hw *hw)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);
	struct t736_gpu_ccu *c = g->ccu;
	unsigned long flags;
	u32 original, value;
	int ret;

	if (gpu_ref_rate(c) != 24000000)
		return -EINVAL;
	spin_lock_irqsave(&c->lock, flags);
	original = readl(c->pll);
	if ((original & GPU_PLL_READY) == GPU_PLL_READY) {
		ret = gpu_pll_wait_lock(c);
		goto out;
	}
	ret = gpu_pll_write(c, original & ~GPU_PLL_OUTPUT);
	if (ret)
		goto restore;
	ret = gpu_pll_write(c, readl(c->pll) | GPU_PLL_LDO);
	if (ret)
		goto restore;
	ret = gpu_pll_write(c, readl(c->pll) | GPU_PLL_ENABLE);
	if (ret)
		goto restore;
	ret = gpu_pll_write(c, readl(c->pll) | GPU_PLL_LOCK_DETECT);
	if (ret)
		goto restore;
	ret = gpu_pll_wait_lock(c);
	if (ret)
		goto restore;
	ret = gpu_pll_write(c, readl(c->pll) | GPU_PLL_OUTPUT);
	if (ret)
		goto restore;
	value = readl(c->pll);
	if ((value & GPU_PLL_READY) != GPU_PLL_READY ||
	    ((value ^ original) & ~GPU_PLL_READY)) {
		ret = -EIO;
		goto restore;
	}
	goto out;
restore:
	/* Continue rollback even if a write fails; never touch a shared PLL. */
	gpu_pll_write(c, readl(c->pll) & ~GPU_PLL_OUTPUT);
	gpu_pll_write(c, readl(c->pll) & ~GPU_PLL_LOCK_DETECT);
	gpu_pll_write(c, readl(c->pll) & ~GPU_PLL_ENABLE);
	if (gpu_pll_write(c, original))
		dev_err(c->dev, "PLL_GPU0 rollback readback failed\n");
out:
	spin_unlock_irqrestore(&c->lock, flags);
	if (ret)
		dev_err(c->dev, "PLL_GPU0 enable/lock failed: %d\n", ret);
	return ret;
}

static void gpu_pll_disable(struct clk_hw *hw)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);
	struct t736_gpu_ccu *c = g->ccu;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&c->lock, flags);
	ret = gpu_pll_write(c, readl(c->pll) & ~GPU_PLL_OUTPUT);
	ret |= gpu_pll_write(c, readl(c->pll) & ~GPU_PLL_LOCK_DETECT);
	ret |= gpu_pll_write(c, readl(c->pll) & ~GPU_PLL_ENABLE);
	spin_unlock_irqrestore(&c->lock, flags);
	if (ret)
		dev_err(c->dev, "PLL_GPU0 disable readback failed\n");
}

static int gpu_pll_enabled(struct clk_hw *hw)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);

	return (readl(g->reg) & GPU_PLL_READY) == GPU_PLL_READY;
}

static int gpu_clock_enable(struct clk_hw *hw)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);
	unsigned long flags;
	u32 v;
	int ret = 0;

	if (g->id == GPU_CORE && gpu_peri400_rate(g->ccu) != GPU_RATE)
		return -EINVAL;
	spin_lock_irqsave(&g->ccu->lock, flags);
	v = readl(g->reg);
	if (g->id == GPU_CORE) {
		/* Commit the fixed source and output gate in one update request. */
		v &= ~(GPU_MUX_MASK | GPU_DIV_MASK | GPU_UPDATE_REQUEST);
		ret = gpu_core_update(g, v | GPU_MUX_PERI400 | BIT(31));
		if (ret)
			writel((readl(g->reg) & ~BIT(31)) | GPU_UPDATE_REQUEST, g->reg);
	} else {
		writel(v | g->key | g->mask, g->reg);
		readl(g->reg);
	}
	spin_unlock_irqrestore(&g->ccu->lock, flags);
	if (ret)
		dev_err(g->ccu->dev, "GPU clock enable update/ACK failed: %d; gate-off requested\n", ret);
	return ret;
}

static void gpu_clock_disable(struct clk_hw *hw)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);
	unsigned long flags;
	u32 v;
	int ret = 0;

	spin_lock_irqsave(&g->ccu->lock, flags);
	v = (readl(g->reg) | g->key) & ~g->mask;
	if (g->id == GPU_CORE)
		ret = gpu_core_update(g, v);
	else
		writel(v, g->reg);
	spin_unlock_irqrestore(&g->ccu->lock, flags);
	if (ret)
		dev_err(g->ccu->dev, "GPU clock disable update/ACK failed: %d\n", ret);
}

static int gpu_clock_enabled(struct clk_hw *hw)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);

	return !!(readl(g->reg) & g->mask);
}

static unsigned long gpu_clock_rate(struct clk_hw *hw, unsigned long parent)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);
	u32 v;

	if (g->id == GPU_PERI400)
		return gpu_peri400_rate(g->ccu);
	if (g->id == GPU_PLL) {
		v = readl(g->reg);
		if ((v & GPU_PLL_READY) != GPU_PLL_READY)
			return 0;
		return div_u64((u64)gpu_ref_rate(g->ccu) *
			       (((v >> 8) & 0xff) + 1),
			       ((((v >> 20) & 7) + 1) *
				(((v >> 1) & 1) + 1)));
	}
	if (g->id == GPU_CORE) {
		v = readl(g->reg);
		if ((v & GPU_MUX_MASK) != GPU_MUX_PERI400 ||
		    (v & (GPU_DIV_MASK | GPU_UPDATE_REQUEST)))
			return 0;
	}
	return parent;
}

static long gpu_clock_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent)
{
	return rate == GPU_RATE ? GPU_RATE : -EINVAL;
}

static int gpu_clock_set_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long parent)
{
	struct gpu_clock *g = container_of(hw, struct gpu_clock, hw);
	unsigned long flags;
	u32 v;
	int ret;

	if (rate != GPU_RATE || gpu_peri400_rate(g->ccu) != GPU_RATE)
		return -EINVAL;
	spin_lock_irqsave(&g->ccu->lock, flags);
	v = readl(g->reg);
	if ((v & BIT(31)) &&
	    ((v & GPU_MUX_MASK) != GPU_MUX_PERI400 || (v & GPU_DIV_MASK))) {
		spin_unlock_irqrestore(&g->ccu->lock, flags);
		return -EBUSY;
	}
	v &= ~(GPU_MUX_MASK | GPU_DIV_MASK | GPU_UPDATE_REQUEST);
	ret = gpu_core_update(g, v | GPU_MUX_PERI400);
	spin_unlock_irqrestore(&g->ccu->lock, flags);
	return ret;
}

static const struct clk_ops gpu_core_ops = {
	.enable = gpu_clock_enable, .disable = gpu_clock_disable,
	.is_enabled = gpu_clock_enabled, .recalc_rate = gpu_clock_rate,
	.round_rate = gpu_clock_round_rate, .set_rate = gpu_clock_set_rate,
};
static const struct clk_ops gpu_gate_ops = {
	.enable = gpu_clock_enable, .disable = gpu_clock_disable,
	.is_enabled = gpu_clock_enabled, .recalc_rate = gpu_clock_rate,
};
static const struct clk_ops gpu_peri_ops = {
	.recalc_rate = gpu_clock_rate,
};
static const struct clk_ops gpu_pll_ops = {
	.enable = gpu_pll_enable, .disable = gpu_pll_disable,
	.is_enabled = gpu_pll_enabled, .recalc_rate = gpu_clock_rate,
};

static int gpu_reset_update(struct reset_controller_dev *rc,
			    unsigned long id, bool assert)
{
	struct t736_gpu_ccu *c = container_of(rc, struct t736_gpu_ccu, reset);
	unsigned long flags;
	u32 v;

	if (id)
		return -EINVAL;
	spin_lock_irqsave(&c->lock, flags);
	v = readl(c->gpu + 4);
	writel(assert ? v & ~BIT(16) : v | BIT(16), c->gpu + 4);
	v = readl(c->gpu + 4);
	spin_unlock_irqrestore(&c->lock, flags);
	return !!(v & BIT(16)) == !assert ? 0 : -EIO;
}
static int gpu_reset_assert(struct reset_controller_dev *rc, unsigned long id)
{ return gpu_reset_update(rc, id, true); }
static int gpu_reset_deassert(struct reset_controller_dev *rc, unsigned long id)
{ return gpu_reset_update(rc, id, false); }
static int gpu_reset_status(struct reset_controller_dev *rc, unsigned long id)
{
	struct t736_gpu_ccu *c = container_of(rc, struct t736_gpu_ccu, reset);
	unsigned long flags;
	u32 v;

	if (id)
		return -EINVAL;
	spin_lock_irqsave(&c->lock, flags);
	v = readl(c->gpu + 4);
	spin_unlock_irqrestore(&c->lock, flags);
	/* Tina CCU: hardware 1 means deasserted; reset API 0 means deasserted. */
	return !(v & BIT(16));
}
static const struct reset_control_ops gpu_reset_ops = {
	.assert = gpu_reset_assert, .deassert = gpu_reset_deassert,
	.status = gpu_reset_status,
};

/* Existing USB/display providers own these shared resource reservations. */
static void __iomem *gpu_map_shared(struct platform_device *pdev,
				   const char *name)
{
	struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	void __iomem *base;

	if (!r || resource_size(r) != 4)
		return ERR_PTR(-EINVAL);
	base = devm_ioremap(&pdev->dev, r->start, resource_size(r));
	return base ? base : ERR_PTR(-ENOMEM);
}

static int t736_gpu_ccu_probe(struct platform_device *pdev)
{
	static const char * const names[GPU_CLOCKS] = {
		"t736-gpu", "t736-gpu-bus", "t736-gpu-peri400",
		"t736-gpu-ahb", "t736-gpu-mbus", "t736-pll-gpu0"
	};
	static const u8 bits[GPU_CLOCKS] = { 31, 0, 0, 7, 16, 31 };
	struct device *dev = &pdev->dev;
	struct t736_gpu_ccu *c;
	struct clk *dcxo;
	void __iomem *regs[GPU_CLOCKS];
	/* Register the measured parent before the core clock. */
	static const u8 order[GPU_CLOCKS] = { 2, 0, 1, 3, 4, 5 };
	int i, n, ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	spin_lock_init(&c->lock);
	dcxo = devm_clk_get(dev, "dcxo");
	if (IS_ERR(dcxo))
		return dev_err_probe(dev, PTR_ERR(dcxo), "missing DCXO\n");
	c->dcxo_rate = clk_get_rate(dcxo);
	c->gpu = devm_platform_ioremap_resource_byname(pdev, "gpu");
	c->ref = gpu_map_shared(pdev, "pll-ref");
	c->peri = gpu_map_shared(pdev, "pll-peri0");
	c->ahb = gpu_map_shared(pdev, "ahb-master");
	c->mbus = gpu_map_shared(pdev, "mbus-master");
	c->pll = devm_platform_ioremap_resource_byname(pdev, "pll-gpu0");
	if (IS_ERR(c->gpu)) return PTR_ERR(c->gpu);
	if (IS_ERR(c->ref)) return PTR_ERR(c->ref);
	if (IS_ERR(c->peri)) return PTR_ERR(c->peri);
	if (IS_ERR(c->ahb)) return PTR_ERR(c->ahb);
	if (IS_ERR(c->mbus)) return PTR_ERR(c->mbus);
	if (IS_ERR(c->pll)) return PTR_ERR(c->pll);
	if (gpu_peri400_rate(c) != GPU_RATE)
		return dev_err_probe(dev, -EINVAL,
			"need locked PLL_REF=24 MHz, PERI0/2x/3=400 MHz: dcxo=%lu ref=%08x peri=%08x\n",
			c->dcxo_rate, readl(c->ref), readl(c->peri));
	c->clocks = devm_kzalloc(dev, struct_size(c->clocks, hws, GPU_CLOCKS), GFP_KERNEL);
	if (!c->clocks)
		return -ENOMEM;
	c->clocks->num = GPU_CLOCKS;
	regs[0] = c->gpu; regs[1] = c->gpu + 4; regs[2] = NULL;
	regs[3] = c->ahb; regs[4] = c->mbus; regs[5] = c->pll;
	for (n = 0; n < GPU_CLOCKS; n++) {
		struct clk_parent_data parent = { .fw_name = "dcxo" };
		struct clk_init_data init = {
			.parent_data = &parent, .num_parents = 1,
			.flags = CLK_GET_RATE_NOCACHE,
		};
		struct gpu_clock *g;

		i = order[n];
		g = &c->clock[i];
		init.name = names[i];
		init.ops = i == GPU_CORE ? &gpu_core_ops :
			(i == GPU_PERI400 ? &gpu_peri_ops :
			 (i == GPU_PLL ? &gpu_pll_ops : &gpu_gate_ops));
		if (i == GPU_CORE) {
			parent.fw_name = NULL;
			parent.hw = &c->clock[GPU_PERI400].hw;
		} else if (i == GPU_BUS || i == GPU_AHB) {
			parent.fw_name = "ahb";
		} else if (i == GPU_MBUS) {
			parent.fw_name = "mbus";
		}
		g->ccu = c; g->id = i; g->reg = regs[i]; g->mask = BIT(bits[i]);
		g->key = i == GPU_AHB ? 0x010000ff : (i == GPU_MBUS ? 0x41055800 : 0);
		g->hw.init = &init;
		ret = devm_clk_hw_register(dev, &g->hw);
		if (ret)
			return ret;
		c->clocks->hws[i] = &g->hw;
	}
	c->reset.owner = THIS_MODULE; c->reset.ops = &gpu_reset_ops;
	c->reset.nr_resets = 1; c->reset.of_node = dev->of_node;
	ret = devm_reset_controller_register(dev, &c->reset);
	if (ret)
		return ret;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, c->clocks);
	if (!ret)
		dev_info(dev, "GPU fixed PERI400 clock with independent PLL_GPU0 power prerequisite\n");
	return ret;
}

static const struct of_device_id gpu_ccu_match[] = {
	{ .compatible = "allwinner,t736-gpu-ccu" }, { }
};
static struct platform_driver gpu_ccu_driver = {
	.probe = t736_gpu_ccu_probe,
	.driver = { .name = "t736-gpu-ccu", .of_match_table = gpu_ccu_match,
		    .suppress_bind_attrs = true },
};
builtin_platform_driver(gpu_ccu_driver);
