// SPDX-License-Identifier: GPL-2.0-only
/* Board-scoped AXP8191 DCDC5 supply. The exclusive R-TWI0 byte engine
 * reuses the source SPL's tested transactions; it does not expose arbitrary
 * PMIC writes or change DDR rails. Source: AXP8191 datasheet and Tina GPL
 * axp2101-regulator.c (DCDC5: 0x16[6:0], enable 0x10[4]).
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/t736-pmic.h>
#include "t736-pmic-twi.inc"

struct t736_gpu_supply {
	void __iomem *base;
	struct t736_twi bus;
	struct mutex lock;
};

/* One physical PMIC bus. Keep the owner alive throughout snapshots, and
 * withdraw it before devres releases its mappings on device removal.
 * Lock order is cpu_supply_owner_lock then the existing transaction mutex.
 */
static DEFINE_MUTEX(cpu_supply_owner_lock);
static struct t736_gpu_supply *cpu_supply_owner;

static void cpu_supply_unpublish(void *data)
{
	mutex_lock(&cpu_supply_owner_lock);
	if (cpu_supply_owner == data)
		cpu_supply_owner = NULL;
	mutex_unlock(&cpu_supply_owner_lock);
}

static u32 pmic_read_reg(void *ctx, u32 off)
{
	struct t736_gpu_supply *p = ctx;
	u32 v = readl(p->base + off);
	udelay(1);
	return v;
}
static void pmic_write_reg(void *ctx, u32 off, u32 value)
{
	struct t736_gpu_supply *p = ctx;
	writel(value, p->base + off);
}
static u32 pmic_ticks(void *ctx)
{
	return (u32)ktime_to_us(ktime_get());
}
static int pmic_read(struct t736_gpu_supply *p, u8 reg, u8 *value)
{
	return t736_twi_read8(&p->bus, 0x36, reg, value) ? -EIO : 0;
}

int t736_pmic_read_cpu_supplies(struct t736_cpu_supply_snapshot *snapshot)
{
	struct t736_cpu_supply_snapshot values;
	struct t736_gpu_supply *p;
	int ret = -ENODEV;

	if (!snapshot)
		return -EINVAL;
	mutex_lock(&cpu_supply_owner_lock);
	p = cpu_supply_owner;
	if (!p)
		goto out_owner;
	mutex_lock(&p->lock);
	ret = pmic_read(p, 0x0e, &values.id);
	if (!ret && values.id != 3)
		ret = -ENODEV;
	if (!ret)
		ret = pmic_read(p, 0x10, &values.enable);
	if (!ret)
		ret = pmic_read(p, 0x14, &values.dcdc3);
	if (!ret)
		ret = pmic_read(p, 0x15, &values.dcdc4);
	if (!ret)
		ret = pmic_read(p, 0x1b, &values.phase);
	if (!ret)
		ret = pmic_read(p, 0x1d, &values.feedback);
	mutex_unlock(&p->lock);
	if (!ret)
		*snapshot = values;
out_owner:
	mutex_unlock(&cpu_supply_owner_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(t736_pmic_read_cpu_supplies);

static int pmic_write_checked(struct t736_gpu_supply *p, u8 reg, u8 value)
{
	u8 check;
	if (reg != 0x16 && reg != 0x10)
		return -EPERM;
	if (t736_twi_write8(&p->bus, 0x36, reg, value) ||
	    pmic_read(p, reg, &check))
		return -EIO;
	return value == check ? 0 : -EIO;
}

static int gpu_supply_get_sel(struct regulator_dev *rdev)
{
	struct t736_gpu_supply *p = rdev_get_drvdata(rdev);
	u8 value;
	int ret;
	mutex_lock(&p->lock);
	ret = pmic_read(p, 0x16, &value);
	mutex_unlock(&p->lock);
	if (ret)
		return ret;
	return (value & 0x7f) <= 0x57 ? value & 0x7f : -ERANGE;
}
static int gpu_supply_enabled(struct regulator_dev *rdev)
{
	struct t736_gpu_supply *p = rdev_get_drvdata(rdev);
	u8 value;
	int ret;
	mutex_lock(&p->lock);
	ret = pmic_read(p, 0x10, &value);
	mutex_unlock(&p->lock);
	return ret ? ret : !!(value & BIT(4));
}
static int gpu_supply_prepare(struct t736_gpu_supply *p)
{
	u8 voltage, enabled, id;
	int ret;
	mutex_lock(&p->lock);
	ret = pmic_read(p, 0x0e, &id);
	if (!ret && id != 3)
		ret = -ENODEV;
	if (!ret)
		ret = pmic_read(p, 0x16, &voltage);
	if (!ret)
		ret = pmic_read(p, 0x10, &enabled);
	if (ret)
		goto out;
	/* Never lower a live rail, nor silently accept an unexpected voltage.
	 * 400 MHz donor OPP needs 800mV; keep a live 800..960mV supply intact.
	 */
	if ((enabled & BIT(4)) && (voltage & 0x7f) > 46) {
		ret = -ERANGE;
		goto out;
	}
	if (!(enabled & BIT(4)) || (voltage & 0x7f) < 30) {
		ret = pmic_write_checked(p, 0x16, (voltage & 0x80) | 30);
		if (ret)
			goto out;
		usleep_range(1000, 1500);
	}
	if (!(enabled & BIT(4)))
		ret = pmic_write_checked(p, 0x10, enabled | BIT(4));
	if (!ret)
		usleep_range(1000, 1500);
out:
	mutex_unlock(&p->lock);
	return ret;
}
static int gpu_supply_enable(struct regulator_dev *rdev)
{
	return gpu_supply_prepare(rdev_get_drvdata(rdev));
}
static const struct linear_range gpu_voltage_ranges[] = {
	REGULATOR_LINEAR_RANGE(500000, 0, 0x46, 10000),
	REGULATOR_LINEAR_RANGE(1220000, 0x47, 0x57, 20000),
};
static const struct regulator_ops gpu_supply_ops = {
	.list_voltage = regulator_list_voltage_linear_range,
	.get_voltage_sel = gpu_supply_get_sel,
	.is_enabled = gpu_supply_enabled,
	.enable = gpu_supply_enable,
};
static const struct regulator_desc gpu_supply_desc = {
	.name = "t736-axp8191-dcdc5",
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &gpu_supply_ops,
	.n_voltages = 0x58,
	.linear_ranges = gpu_voltage_ranges,
	.n_linear_ranges = ARRAY_SIZE(gpu_voltage_ranges),
	.enable_time = 1000,
};
static int gpu_supply_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct t736_gpu_supply *p;
	struct regulator_config config = { .dev = dev, .of_node = dev->of_node };
	struct regulator_dev *rdev;
	const char * const checks[] = { "apbs1", "twi-gate", "pins" };
	const u32 masks[] = { 0x0700001f, 0x10001, 0xff };
	const u32 expected[] = { 0x04000000, 0x10001, 0x22 };
	int i, ret;
	u8 id;
	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	mutex_init(&p->lock);
	p->base = devm_platform_ioremap_resource_byname(pdev, "twi");
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	/* These shared CCU/pinctrl resources are inspected, never modified.
	 * The source SPL established the 24MHz bus and PL0/1 function 2.
	 */
	for (i = 0; i < ARRAY_SIZE(checks); i++) {
		struct resource *r = platform_get_resource_byname(pdev, IORESOURCE_MEM, checks[i]);
		void __iomem *map;
		u32 value;
		if (!r || resource_size(r) != 4)
			return -EINVAL;
		map = devm_ioremap(dev, r->start, 4);
		if (!map)
			return -ENOMEM;
		value = readl(map);
		if ((value & masks[i]) != expected[i])
			return dev_err_probe(dev, -EINVAL, "unexpected %s: %08x\n", checks[i], value);
	}
	p->bus = (struct t736_twi){ .ctx = p, .read = pmic_read_reg,
		.write = pmic_write_reg, .ticks = pmic_ticks,
		.timeout_ticks = 2000, .poll_limit = 10000 };
	ret = t736_twi_init(&p->bus);
	if (ret)
		return dev_err_probe(dev, -EIO, "R-TWI0 init failed: %d\n", ret);
	ret = pmic_read(p, 0x0e, &id);
	if (ret || id != 3)
		return dev_err_probe(dev, ret ? ret : -ENODEV, "AXP8191 ID mismatch\n");
	ret = gpu_supply_prepare(p);
	if (ret)
		return dev_err_probe(dev, ret, "DCDC5 preparation failed\n");
	config.driver_data = p;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node, &gpu_supply_desc);
	if (!config.init_data)
		return -EINVAL;
	rdev = devm_regulator_register(dev, &gpu_supply_desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev), "DCDC5 registration failed\n");
	/* Registration may skip enable when already on. Validate and raise a
	 * low live supply explicitly; all other PMIC registers remain untouched.
	 */
	ret = gpu_supply_enable(rdev);
	if (ret)
		return dev_err_probe(dev, ret, "DCDC5 voltage/enable validation failed\n");
	ret = gpu_supply_get_sel(rdev);
	if (ret < 0)
		return ret;
	dev_info(dev, "AXP8191 DCDC5 verified at %d uV, fixed GPU supply policy\n",
		 500000 + ret * 10000);
	ret = devm_add_action_or_reset(dev, cpu_supply_unpublish, p);
	if (ret)
		return ret;
	mutex_lock(&cpu_supply_owner_lock);
	if (cpu_supply_owner)
		ret = -EBUSY;
	else
		cpu_supply_owner = p;
	mutex_unlock(&cpu_supply_owner_lock);
	if (ret)
		return ret;
	return 0;
}
static const struct of_device_id gpu_supply_match[] = {
	{ .compatible = "allwinner,t736-gpu-supply" }, { }
};
static struct platform_driver gpu_supply_driver = {
	.probe = gpu_supply_probe,
	.driver = { .name = "t736-gpu-supply", .of_match_table = gpu_supply_match,
		.suppress_bind_attrs = true },
};
builtin_platform_driver(gpu_supply_driver);
