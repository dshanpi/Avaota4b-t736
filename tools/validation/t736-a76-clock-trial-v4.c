// SPDX-License-Identifier: GPL-2.0-only
/* T736 A76 trial v4: manual-update semantics from manual section5.1.4.2.
 * N/LFM read back the committed value until UPDATE; UPDATE is invalid with
 * PLL_EN off. Preserve the desired N in software until committing it with
 * ENABLE on, then verify CTRL and CTRL_STAT. Gates/divider are immediate.
 * Hardware also commits staged N during PLL startup/lock-detector reopening:
 * only the EN and LOCK_EN stages may read either old N or requested N. All
 * other fields remain strict, and explicit UPDATE still validates the target.
 * All CPU writes require both A76 cores offline. PMIC access uses its owner.
 * Failure/restore selects slow and disables PLL, retaining committed N.
 * Never enable an out-of-range N20 VCO merely to restore a register value.
 * Default audit has no CPU writes; unload retains the current setting.
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
#define N_MASK GENMASK(15, 8)
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define LDO BIT(30)
#define ENABLE BIT(31)
#define STOPPED_BASE 0x40800000U
#define CTRL_416 0xf8802000U
#define CLK_416 0x03010000U
#define CTRL_1014 0xf8802700U
#define CLK_1014 0x03000000U
#define TAG "t736-a76-clock-trial-v4: "

static char *action = "audit";
module_param(action, charp, 0400);
MODULE_PARM_DESC(action, "audit, enable (416MHz), upgrade (416 to1014MHz), restore (safe slow; retain N)");

struct guard_reg { u16 offset; u32 value; };
static const u16 guard_offsets[] = {
	0x0000, 0x1000, 0x101c, 0x3000, 0x301c, 0x3034,
	0x2004, 0x2008, 0x2018,
};

static bool known_n(u32 value)
{
	u32 n = (value & N_MASK) >> 8;
	return n == 20 || n == 32 || n == 39;
}

static bool stopped_state(u32 ctrl, u32 stat, u32 clk)
{
	return !clk && ctrl == stat && known_n(ctrl) &&
		(ctrl & ~N_MASK) == STOPPED_BASE;
}

static int write_mux(void __iomem *base, u32 value)
{
	u32 actual;
	writel(value, base + B_CLK);
	actual = readl(base + B_CLK);
	pr_info(TAG "mux expected=%08x actual=%08x\n", value, actual);
	return actual == value ? 0 : -EIO;
}

/* Before PLL startup, N remains old until commit. During enable or lock
 * detector reopening it may become the staged target. All other writable bits
 * must read back now; no N outside the known old/target pair is accepted.
 */
static int stage_control(void __iomem *base, u32 desired, u32 committed_n,
			 bool may_latch_n)
{
	u32 actual;
	if (desired & (LOCK | UPDATE))
		return -EINVAL;
	writel(desired, base + B_CTRL);
	actual = readl(base + B_CTRL);
	pr_info(TAG "stage desired=%08x committed=%08x actual=%08x may_latch_n=%d\n",
		desired, committed_n, actual, may_latch_n);
	if (((actual ^ desired) & ~(LOCK | N_MASK)) ||
	    ((actual & N_MASK) != committed_n &&
	     !(may_latch_n && (desired & ENABLE) &&
	       (actual & N_MASK) == (desired & N_MASK))))
		return -EIO;
	return 0;
}

/* Directly commit cached desired N; never derive this write from readl(),
 * which still reports old N. At least1us separates possible updates.
 */
static int commit_control(void __iomem *base, u32 desired)
{
	unsigned int i;
	u32 ctrl, stat;
	if ((desired & (ENABLE | LDO | LOCK_EN)) != (ENABLE | LDO | LOCK_EN) ||
	    (desired & (LOCK | UPDATE)) ||
	    ((desired & N_MASK) != 0x2000 && (desired & N_MASK) != 0x2700))
		return -EINVAL;
	if (readl(base + B_CTRL) & UPDATE)
		return -EBUSY;
	udelay(1);
	writel(desired | UPDATE, base + B_CTRL);
	for (i = 0; i < 1000; i++) {
		ctrl = readl(base + B_CTRL);
		if (!(ctrl & UPDATE)) {
			stat = readl(base + B_STAT);
			pr_info(TAG "commit desired=%08x CTRL=%08x STAT=%08x\n", desired, ctrl, stat);
			return ((ctrl ^ desired) & ~LOCK) ||
			       ((stat ^ desired) & ~LOCK) ? -EIO : 0;
		}
		udelay(10);
	}
	pr_err(TAG "UPDATE did not clear within10ms\n");
	return -ETIMEDOUT;
}

static int wait_ready(void __iomem *base, u32 desired)
{
	unsigned int i, consecutive = 0;
	u32 expected = desired | LOCK;
	for (i = 0; i < 7000; i++) {
		if (readl(base + B_CTRL) == expected &&
		    readl(base + B_STAT) == expected)
			consecutive++;
		else
			consecutive = 0;
		if (consecutive == 3) {
			udelay(20);
			return readl(base + B_CTRL) == expected &&
			       readl(base + B_STAT) == expected ? 0 : -EIO;
		}
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int safe_slow(void __iomem *base)
{
	u32 value, ctrl, stat, mux, n;
	unsigned int i;
	int ret;
	ret = write_mux(base, 0);
	if (ret) {
		pr_crit(TAG "slow mux not confirmed; do not disable PLL\n");
		return ret;
	}
	udelay(20);
	/* Do not commit N while disabled. These three immediate gate operations
	 * preserve whichever N actually committed before the failure.
	 */
	value = readl(base + B_CTRL) & ~(LOCK | UPDATE | OUTPUT);
	writel(value, base + B_CTRL);
	value = readl(base + B_CTRL) & ~(LOCK | UPDATE | LOCK_EN);
	writel(value, base + B_CTRL);
	value = readl(base + B_CTRL) & ~(LOCK | UPDATE | ENABLE);
	writel(value, base + B_CTRL);
	for (i = 0; i < 1000; i++) {
		ctrl = readl(base + B_CTRL);
		stat = readl(base + B_STAT);
		mux = readl(base + B_CLK);
		if (stopped_state(ctrl, stat, mux) && !readl(base + 0x3034)) {
			n = (ctrl & N_MASK) >> 8;
			pr_warn(TAG "SAFE_SLOW: CTRL=%08x STAT=%08x CLK=%08x retained_N=%u; PLL disabled, no N20 re-enable\n",
				ctrl, stat, mux, n);
			return 0;
		}
		udelay(10);
	}
	pr_crit(TAG "SAFE_SLOW_INCOMPLETE CTRL=%08x STAT=%08x CLK=%08x; keep6/7offline and reset\n",
		readl(base + B_CTRL), readl(base + B_STAT), readl(base + B_CLK));
	return -EIO;
}

static int enable_416(void __iomem *base)
{
	u32 old_n = readl(base + B_CTRL) & N_MASK;
	u32 desired = STOPPED_BASE | old_n;
	int ret;
	ret = write_mux(base, 0x00010000U);
	if (ret) return ret;
	/* Manual5.1.4.2 enable: LDO, OUTPUT, factors, EN, LOCK_EN, UPDATE. */
	ret = stage_control(base, desired | LDO, old_n, false);
	if (ret) return ret;
	desired |= OUTPUT;
	ret = stage_control(base, desired, old_n, false);
	if (ret) return ret;
	desired = (desired & ~N_MASK) | 0x2000;
	ret = stage_control(base, desired, old_n, false);
	if (ret) return ret;
	desired |= ENABLE;
	ret = stage_control(base, desired, old_n, true);
	if (ret) return ret;
	desired |= LOCK_EN;
	ret = stage_control(base, desired, old_n, true);
	if (ret) return ret;
	ret = commit_control(base, desired);
	if (ret) return ret;
	ret = wait_ready(base, desired);
	if (ret) return ret;
	return write_mux(base, CLK_416);
}

static int upgrade_1014(void __iomem *base)
{
	u32 desired = CTRL_416 & ~LOCK;
	const u32 old_n = 0x2000;
	int ret;
	ret = write_mux(base, 0);
	if (ret) return ret;
	udelay(20);
	/* Manual non-LFM change: keep PLL_EN on, close output/lock detector,
	 * stage factors, re-enable detector and commit before opening output.
	 * The external /1 is selected only while the CPU uses slow.
	 */
	desired &= ~OUTPUT;
	ret = stage_control(base, desired, old_n, false);
	if (ret) return ret;
	desired &= ~LOCK_EN;
	ret = stage_control(base, desired, old_n, false);
	if (ret) return ret;
	desired = (desired & ~N_MASK) | 0x2700;
	ret = stage_control(base, desired, old_n, false);
	if (ret) return ret;
	desired |= LOCK_EN;
	ret = stage_control(base, desired, old_n, true);
	if (ret) return ret;
	ret = commit_control(base, desired);
	if (ret) return ret;
	ret = wait_ready(base, desired);
	if (ret) return ret;
	desired |= OUTPUT;
	ret = stage_control(base, desired, 0x2700, false);
	if (ret) return ret;
	ret = wait_ready(base, desired);
	if (ret) return ret;
	return write_mux(base, CLK_1014);
}

static int read_supply(void)
{
	struct t736_cpu_supply_snapshot s;
	typeof(&t736_pmic_read_cpu_supplies) get_snapshot;
	u32 little_uv, big_uv;
	int ret;
	get_snapshot = symbol_get(t736_pmic_read_cpu_supplies);
	if (!get_snapshot)
		return -ENODEV;
	ret = get_snapshot(&s);
	symbol_put(t736_pmic_read_cpu_supplies);
	if (ret) return ret;
	pr_info(TAG "PMIC id=%02x enable=%02x dcdc3=%02x dcdc4=%02x phase=%02x feedback=%02x\n",
		s.id, s.enable, s.dcdc3, s.dcdc4, s.phase, s.feedback);
	if (s.id != 3 || (s.enable & 0x0c) != 0x0c || (s.phase & 0xc0) ||
	    (s.dcdc3 & 0x7f) > 0x46 || (s.dcdc4 & 0x7f) > 0x46)
		return -ERANGE;
	little_uv = 500000U + (s.dcdc3 & 0x7f) * 10000U;
	big_uv = 500000U + (s.dcdc4 & 0x7f) * 10000U;
	pr_info(TAG "owner setpoints DCDC3=%uuV DCDC4=%uuV\n", little_uv, big_uv);
	return little_uv >= 800000 && little_uv <= 1100000 &&
	       big_uv >= 800000 && big_uv <= 1100000 ? 0 : -ERANGE;
}

static int __init trial_init(void)
{
	struct guard_reg guards[ARRAY_SIZE(guard_offsets)];
	void __iomem *base = NULL, *xo = NULL;
	bool audit = !strcmp(action, "audit"), enable = !strcmp(action, "enable");
	bool upgrade = !strcmp(action, "upgrade"), restore = !strcmp(action, "restore");
	bool state_416, state_1014, stopped, guard_failed = false, changed = false;
	u32 ctrl, stat, mux, crystal, target_ctrl, target_clk;
	unsigned int i;
	int ret;
	if ((!audit && !enable && !upgrade && !restore) || !of_machine_is_compatible("allwinner,t736"))
		return -EINVAL;
	cpus_read_lock();
	if (num_possible_cpus() != 8 || !cpu_possible(6) || !cpu_possible(7)) {
		ret = -ENODEV; goto unlock;
	}
	if (!audit && (cpu_online(6) || cpu_online(7))) {
		pr_err(TAG "CPU6 and CPU7 must both be offline\n");
		ret = -EBUSY; goto unlock;
	}
	if (!audit && MIDR_PARTNUM(read_cpuid_id()) != ARM_CPU_PART_CORTEX_A55) {
		ret = -ENODEV; goto unlock;
	}
	ret = read_supply();
	if (ret && !restore) goto unlock;
	if (ret) pr_warn(TAG "supply check failed; explicit reduction to slow is allowed\n");
	if (!request_mem_region(CPU_BASE + B_CTRL, 0x30, "t736-a76-clock-trial-v4")) {
		ret = -EBUSY; goto unlock;
	}
	base = ioremap(CPU_BASE, CPU_SIZE);
	xo = ioremap(XO_PHYS, sizeof(u32));
	if (!base || !xo) { ret = -ENOMEM; goto out; }
	crystal = readl(xo);
	ctrl = readl(base + B_CTRL); stat = readl(base + B_STAT); mux = readl(base + B_CLK);
	pr_info(TAG "action=%s XO=%08x CTRL=%08x STAT=%08x CLK=%08x online6=%d online7=%d\n",
		action, crystal, ctrl, stat, mux, cpu_online(6), cpu_online(7));
	for (i = 0; i < ARRAY_SIZE(guard_offsets); i++) {
		guards[i].offset = guard_offsets[i];
		guards[i].value = readl(base + guard_offsets[i]);
	}
	if ((crystal & (BIT(14) | BIT(15))) != BIT(15) || readl(base + 0x3034) ||
	    readl(base + 0x2004) || readl(base + 0x2008) != 0x30000000U ||
	    readl(base + 0x2018) != 0x40000001U) { ret = -EINVAL; goto out; }
	if (audit) { pr_info(TAG "AUDIT: no CPU writes\n"); ret = 0; goto out; }
	state_416 = ctrl == CTRL_416 && stat == CTRL_416 && mux == CLK_416;
	state_1014 = ctrl == CTRL_1014 && stat == CTRL_1014 && mux == CLK_1014;
	stopped = stopped_state(ctrl, stat, mux);
	if ((enable && !stopped) || (upgrade && !state_416) ||
	    (restore && !state_416 && !state_1014 && !stopped)) {
		ret = -EINVAL; goto out;
	}
	changed = true;
	ret = enable ? enable_416(base) : upgrade ? upgrade_1014(base) : safe_slow(base);
	if (!ret && !restore) {
		target_ctrl = enable ? CTRL_416 : CTRL_1014;
		target_clk = enable ? CLK_416 : CLK_1014;
		if (readl(base + B_CTRL) != target_ctrl || readl(base + B_STAT) != target_ctrl ||
		    readl(base + B_CLK) != target_clk) ret = -EIO;
		if (!ret) ret = read_supply();
	}
	for (i = 0; i < ARRAY_SIZE(guards); i++) {
		if (readl(base + guards[i].offset) != guards[i].value) {
			pr_err(TAG "guard +%04x changed\n", guards[i].offset);
			ret = -EIO; guard_failed = true;
		}
	}
	if (ret && !restore) {
		pr_err(TAG "trial failed=%d; reducing to slow with committed N retained\n", ret);
		safe_slow(base);
		for (i = 0; i < ARRAY_SIZE(guards); i++) {
			if (readl(base + guards[i].offset) != guards[i].value) {
				pr_err(TAG "guard +%04x changed during rollback\n", guards[i].offset);
				guard_failed = true;
			}
		}
	}
	if (guard_failed)
		pr_crit(TAG "guard changed; keep CPU6/7offline and reset even if slow restored\n");
	if (!ret && !restore)
		pr_info(TAG "SUCCESS: A76=%uMHz CTRL=%08x STAT=%08x CLK=%08x; PMIC untouched\n",
			enable ? 416 : 1014, readl(base + B_CTRL), readl(base + B_STAT), readl(base + B_CLK));
out:
	if (xo) iounmap(xo);
	if (base) iounmap(base);
	release_mem_region(CPU_BASE + B_CTRL, 0x30);
unlock:
	cpus_read_unlock();
	pr_info(TAG "finished rc=%d mutation_attempted=%d\n", ret, changed);
	return ret;
}

static void __exit trial_exit(void)
{
	pr_info(TAG "unloaded; setting retained; explicit restore requires CPU6/7offline\n");
}
module_init(trial_init);
module_exit(trial_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 A76 v4 startup and manual-update 416/1014MHz trial");
