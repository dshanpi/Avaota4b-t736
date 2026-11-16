// SPDX-License-Identifier: GPL-2.0-only
/*
 * Explicit, reversible A76 clock bring-up trial for the measured T736 state.
 * Default action=audit never writes a CPU register. action=enable selects
 * the donor's common 26MHz OPP: N32 / external2 = 416MHz at >=800mV.
 * action=upgrade requires the exact 416MHz state and selects N39 /1=1014MHz.
 * action=restore accepts either trial state and restores the measured ROM
 * state (DCXO, PLL disabled). The reviewed v1 source is retained separately.
 * CPU6/7 MUST be offline for every mutation and stay blocked by cpus_read_lock.
 * PMIC reads go through the existing owner's serialized, read-only API.
 * No voltage, A55, DSU, CPU_BACK, modulation or power/reset register is written.
 * Successful changes persist across rmmod; rollback is explicit action=restore.
 */
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <asm/cputype.h>
#include "t736-pmic.h"

#define CPU_BASE 0x08870000UL
#define CPU_SIZE 0x4000
#define XO_PHYS 0x07090160UL
#define B_CTRL 0x2000
#define B_CLK 0x201c
#define B_STAT 0x202c
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define LDO BIT(30)
#define ENABLE BIT(31)
#define ORIGINAL_CTRL 0x40801400U
#define ORIGINAL_CLK 0x00000000U
#define CTRL_416 0xf8802000U
#define CLK_416 0x03010000U
#define CTRL_1014 0xf8802700U
#define CLK_1014 0x03000000U
#define TAG "t736-a76-clock-trial-v2: "

static char *action = "audit";
module_param(action, charp, 0400);
MODULE_PARM_DESC(action, "audit (default), enable (416MHz), upgrade (416 to1014MHz), restore (either trial to DCXO)");

struct guard_reg { u16 offset; u32 value; };
static const u16 guard_offsets[] = {
	0x0000, 0x1000, 0x101c, 0x3000, 0x301c, 0x3034,
	0x2004, 0x2008, 0x2018,
};

static int read_supply(void);

static int write_checked(void __iomem *base, u16 offset, u32 value)
{
	u32 actual, ignore = offset == B_CTRL ? LOCK : 0;

	/* Do not echo the read-only lock bit into a control write. */
	if (offset == B_CTRL)
		value &= ~LOCK;
	writel(value, base + offset);
	actual = readl(base + offset);
	pr_info(TAG "write +%04x expected=%08x actual=%08x\n",
		offset, value, actual);
	return ((actual ^ value) & ~ignore) ? -EIO : 0;
}

static int update_control(void __iomem *base)
{
	u32 value = readl(base + B_CTRL);
	unsigned int i;

	if (value & UPDATE)
		return -EBUSY;
	writel((value & ~LOCK) | UPDATE, base + B_CTRL);
	for (i = 0; i < 1000; i++) {
		u32 actual = readl(base + B_CTRL);

		if (!(actual & UPDATE)) {
			if ((actual ^ value) & ~(LOCK | UPDATE))
				return -EIO;
			return 0;
		}
		udelay(10);
	}
	pr_err(TAG "update did not clear in 10ms\n");
	return -ETIMEDOUT;
}

static int wait_for_lock(void __iomem *base)
{
	unsigned int i, consecutive = 0;

	for (i = 0; i < 7000; i++) {
		u32 value = readl(base + B_CTRL);

		consecutive = ((value & LOCK) && !(value & UPDATE)) ?
			consecutive + 1 : 0;
		if (consecutive == 3)
			return 0;
		udelay(10);
	}
	pr_err(TAG "PLL failed to lock in 70ms\n");
	return -ETIMEDOUT;
}

static int verify_final(void __iomem *base, unsigned int mhz)
{
	u32 expected_ctrl = mhz == 1014 ? CTRL_1014 : mhz == 416 ? CTRL_416 : ORIGINAL_CTRL;
	u32 expected_clk = mhz == 1014 ? CLK_1014 : mhz == 416 ? CLK_416 : ORIGINAL_CLK;
	u32 ctrl = readl(base + B_CTRL), stat = readl(base + B_STAT);
	u32 clk = readl(base + B_CLK);

	pr_info(TAG "final CTRL=%08x STAT=%08x CLK=%08x\n", ctrl, stat, clk);
	if (ctrl != expected_ctrl || stat != expected_ctrl || clk != expected_clk)
		return -EIO;
	return 0;
}

static int restore_original(void __iomem *base)
{
	int ret, first_error = 0;

	/* Never disable a PLL while its consumer may still select it. */
	ret = write_checked(base, B_CLK, ORIGINAL_CLK);
	if (ret) {
		pr_crit(TAG "cannot confirm slow mux; leaving PLL enabled\n");
		return ret;
	}
	udelay(20);
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) & ~OUTPUT);
	if (ret) first_error = ret;
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) & ~LOCK_EN);
	if (ret) first_error = ret;
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) & ~ENABLE);
	if (ret) first_error = ret;
	/* Retain the originally enabled LDO and all original factor/reserved bits. */
	ret = write_checked(base, B_CTRL, ORIGINAL_CTRL);
	if (ret) first_error = ret;
	ret = update_control(base);
	if (ret) first_error = ret;
	udelay(20);
	ret = verify_final(base, 0);
	if (ret) first_error = ret;
	if (first_error)
		pr_crit(TAG "restore incomplete; keep CPU6/7 offline and reset before retry\n");
	else
		pr_info(TAG "RESTORED exact measured DCXO/PLL-disabled state\n");
	return first_error;
}

/* Establish a stopped output before changing factors; only called with both
 * big cores offline. A failed slow-mux readback prohibits any PLL shutdown.
 */
static int park_slow_and_disable(void __iomem *base)
{
	const u32 stopped_mask = OUTPUT | ENABLE | LOCK_EN | UPDATE | LOCK;
	unsigned int i;
	int ret;

	ret = write_checked(base, B_CLK, ORIGINAL_CLK);
	if (ret) return ret;
	udelay(20);
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) & ~OUTPUT);
	if (ret) return ret;
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) & ~LOCK_EN);
	if (ret) return ret;
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) & ~ENABLE);
	if (ret) return ret;
	ret = update_control(base);
	if (ret) return ret;
	/* Observe old LOCK deasserted in both control and effective status before
	 * accepting a new lock. Otherwise a stale416MHz lock could pass the later
	 * consecutive-sample check after a failed1014MHz reconfiguration.
	 */
	for (i = 0; i < 1000; i++) {
		if (!(readl(base + B_CTRL) & stopped_mask) &&
		    !(readl(base + B_STAT) & stopped_mask))
			return 0;
		udelay(10);
	}
	pr_err(TAG "PLL did not confirm disabled/unlocked in10ms\n");
	return -ETIMEDOUT;
}

/* Only the two reviewed OPPs are accepted by this internal routine. The
 * divider is changed while the CPU selects slow, never while consuming PLL.
 */
static int program_frequency(void __iomem *base, unsigned int mhz)
{
	u32 stopped_ctrl, target_ctrl, target_clk;
	int ret;

	if (mhz != 416 && mhz != 1014)
		return -EINVAL;
	if (readl(base + B_CLK) != ORIGINAL_CLK ||
	    (readl(base + B_CTRL) & (OUTPUT | ENABLE | LOCK_EN | UPDATE | LOCK)) ||
	    (readl(base + B_STAT) & (OUTPUT | ENABLE | LOCK_EN | UPDATE | LOCK)))
		return -EINVAL;
	target_ctrl = mhz == 1014 ? CTRL_1014 : CTRL_416;
	target_clk = mhz == 1014 ? CLK_1014 : CLK_416;
	stopped_ctrl = target_ctrl & ~(ENABLE | LOCK_EN | LOCK | OUTPUT);
	/* Keep mux at slow while establishing the divider and stopped PLL. */
	ret = write_checked(base, B_CLK, target_clk & ~GENMASK(26, 24));
	if (ret) return ret;
	ret = write_checked(base, B_CTRL, stopped_ctrl);
	if (ret) return ret;
	ret = update_control(base);
	if (ret) return ret;
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) | LDO);
	if (ret) return ret;
	udelay(20);
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) | ENABLE);
	if (ret) return ret;
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) | LOCK_EN);
	if (ret) return ret;
	ret = update_control(base);
	if (ret) return ret;
	ret = wait_for_lock(base);
	if (ret) return ret;
	ret = write_checked(base, B_CTRL, readl(base + B_CTRL) | OUTPUT);
	if (ret) return ret;
	/* The CPU-specific donor probe commits output gating with UPDATE.
	 * Do so while still on slow, then re-confirm lock and effective status.
	 */
	ret = update_control(base);
	if (ret) return ret;
	ret = wait_for_lock(base);
	if (ret) return ret;
	udelay(20);
	if (readl(base + B_CTRL) != target_ctrl ||
	    readl(base + B_STAT) != target_ctrl)
		return -EIO;
	/* Only a locked, verified output may become the CPU source. */
	ret = write_checked(base, B_CLK, target_clk);
	if (ret) return ret;
	udelay(20);
	return verify_final(base, mhz);
}

static void rollback_upgrade(void __iomem *base)
{
	int ret = read_supply();

	/* Do not restart even the lower PLL rate after a failed supply check. */
	if (!ret)
		ret = park_slow_and_disable(base);
	if (!ret)
		ret = program_frequency(base, 416);
	if (!ret)
		ret = read_supply();
	if (!ret) {
		pr_warn(TAG "ROLLBACK_416_SUCCESS; CPU6/7 remain offline pending review\n");
		return;
	}
	pr_err(TAG "416MHz rollback failed (%d); attempting measured slow state\n", ret);
	ret = restore_original(base);
	if (!ret)
		pr_warn(TAG "ROLLBACK_SLOW_SUCCESS; CPU6/7 remain offline\n");
	else
		pr_crit(TAG "ROLLBACK_INCOMPLETE; keep CPU6/7 offline and reset\n");
}

static int read_supply(void)
{
	struct t736_cpu_supply_snapshot snapshot;
	typeof(&t736_pmic_read_cpu_supplies) get_snapshot;
	u32 little_uv, big_uv;
	int ret;

	get_snapshot = symbol_get(t736_pmic_read_cpu_supplies);
	if (!get_snapshot) {
		pr_err(TAG "PMIC owner snapshot API unavailable; refusing writes\n");
		return -ENODEV;
	}
	ret = get_snapshot(&snapshot);
	symbol_put(t736_pmic_read_cpu_supplies);
	if (ret)
		return ret;
	pr_info(TAG "PMIC id=%02x enable=%02x dcdc3=%02x dcdc4=%02x phase=%02x feedback=%02x\n",
		snapshot.id, snapshot.enable, snapshot.dcdc3, snapshot.dcdc4,
		snapshot.phase, snapshot.feedback);
	if (snapshot.id != 3 || (snapshot.enable & 0x0c) != 0x0c ||
	    (snapshot.phase & 0xc0) ||
	    (snapshot.dcdc3 & 0x7f) > 0x46 || (snapshot.dcdc4 & 0x7f) > 0x46)
		return -ERANGE;
	little_uv = 500000U + (snapshot.dcdc3 & 0x7f) * 10000U;
	big_uv = 500000U + (snapshot.dcdc4 & 0x7f) * 10000U;
	pr_info(TAG "verified owner setpoints DCDC3=%uuV DCDC4=%uuV\n", little_uv, big_uv);
	if (little_uv < 800000 || little_uv > 1100000 ||
	    big_uv < 800000 || big_uv > 1100000)
		return -ERANGE;
	return 0;
}

static int __init t736_a76_clock_trial_init(void)
{
	struct guard_reg guards[ARRAY_SIZE(guard_offsets)];
	void __iomem *base = NULL, *xo = NULL;
	bool enable, upgrade, restore, audit, state_416, state_1014, changed = false;
	bool guard_failed = false;
	u32 control, status, mux, crystal;
	unsigned int i;
	int ret;

	audit = !strcmp(action, "audit");
	enable = !strcmp(action, "enable");
	upgrade = !strcmp(action, "upgrade");
	restore = !strcmp(action, "restore");
	if ((!audit && !enable && !upgrade && !restore) || !of_machine_is_compatible("allwinner,t736"))
		return -EINVAL;
	cpus_read_lock();
	if (num_possible_cpus() != 8 || !cpu_possible(6) || !cpu_possible(7)) {
		ret = -ENODEV;
		goto unlock;
	}
	if (!audit && (cpu_online(6) || cpu_online(7))) {
		pr_err(TAG "CPU6 and CPU7 must both be offline before any clock mutation\n");
		ret = -EBUSY;
		goto unlock;
	}
	if (!audit && MIDR_PARTNUM(read_cpuid_id()) != ARM_CPU_PART_CORTEX_A55) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = read_supply();
	if (ret && !restore)
		goto unlock;
	if (ret)
		pr_warn(TAG "supply read/check failed (%d); explicit reduction to slow remains available\n", ret);
	if (!request_mem_region(CPU_BASE + B_CTRL, 0x30, "t736-a76-clock-trial")) {
		ret = -EBUSY;
		goto unlock;
	}
	base = ioremap(CPU_BASE, CPU_SIZE);
	xo = ioremap(XO_PHYS, sizeof(u32));
	if (!base || !xo) {
		ret = -ENOMEM;
		goto out;
	}
	crystal = readl(xo);
	control = readl(base + B_CTRL);
	status = readl(base + B_STAT);
	mux = readl(base + B_CLK);
	pr_info(TAG "action=%s XO=%08x B_CTRL=%08x B_STAT=%08x B_CLK=%08x online6=%d online7=%d\n",
		action, crystal, control, status, mux, cpu_online(6), cpu_online(7));
	for (i = 0; i < ARRAY_SIZE(guard_offsets); i++) {
		guards[i].offset = guard_offsets[i];
		guards[i].value = readl(base + guard_offsets[i]);
	}
	/* Established 26MHz oscillator; no SDM/fraction/LFM or alternative slow source. */
	if ((crystal & (BIT(14) | BIT(15))) != BIT(15) ||
	    readl(base + 0x3034) != 0 || readl(base + 0x2004) != 0 ||
	    readl(base + 0x2008) != 0x30000000U ||
	    readl(base + 0x2018) != 0x40000001U) {
		ret = -EINVAL;
		goto out;
	}
	if (audit) {
		pr_info(TAG "AUDIT complete: no CPU register writes\n");
		ret = 0;
		goto out;
	}
	state_416 = control == CTRL_416 && status == CTRL_416 && mux == CLK_416;
	state_1014 = control == CTRL_1014 && status == CTRL_1014 && mux == CLK_1014;
	if ((enable && (control != ORIGINAL_CTRL || status != ORIGINAL_CTRL || mux != ORIGINAL_CLK)) ||
	    (upgrade && !state_416) || (restore && !state_416 && !state_1014)) {
		pr_err(TAG "unexpected initial state; no mutation attempted\n");
		ret = -EINVAL;
		goto out;
	}
	changed = true;
	if (upgrade) {
		ret = park_slow_and_disable(base);
		if (!ret)
			ret = program_frequency(base, 1014);
	} else {
		ret = enable ? program_frequency(base, 416) : restore_original(base);
	}
	if (!ret && (enable || upgrade))
		ret = read_supply();
	for (i = 0; i < ARRAY_SIZE(guards); i++) {
		if (readl(base + guards[i].offset) != guards[i].value) {
			pr_err(TAG "unmodified guard +%04x changed unexpectedly\n", guards[i].offset);
			ret = -EIO;
			guard_failed = true;
		}
	}
	if (ret && enable) {
		pr_err(TAG "enable failed (%d); restoring original B state\n", ret);
		restore_original(base);
	}
	if (ret && upgrade) {
		if (guard_failed) {
			pr_err(TAG "guard changed; refusing416MHz rebuild, attempting slow only\n");
			restore_original(base);
		} else {
			pr_err(TAG "upgrade failed (%d); attempting416MHz then slow rollback\n", ret);
			rollback_upgrade(base);
		}
	}
	if (guard_failed)
		pr_crit(TAG "unrelated clock/mode state changed; keep CPU6/7offline and reset even if slow restored\n");
	if (!ret && enable)
		pr_info(TAG "SUCCESS: A76 416MHz (26MHz*N32/2), DCDC4 untouched; online CPU6/7 only after review\n");
	if (!ret && upgrade)
		pr_info(TAG "SUCCESS: A76 1014MHz (26MHz*N39/1), DCDC4 untouched; online CPU6/7 only after review\n");
out:
	if (xo) iounmap(xo);
	if (base) iounmap(base);
	release_mem_region(CPU_BASE + B_CTRL, 0x30);
unlock:
	cpus_read_unlock();
	pr_info(TAG "finished rc=%d mutation_attempted=%d\n", ret, changed);
	return ret;
}

static void __exit t736_a76_clock_trial_exit(void)
{
	pr_info(TAG "unloaded; clock setting retained. Explicit restore requires CPU6/7 offline.\n");
}
module_init(t736_a76_clock_trial_init);
module_exit(t736_a76_clock_trial_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 A76 verified-supply offline-core 416/1014MHz bring-up trial v2");
