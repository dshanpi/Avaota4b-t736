// SPDX-License-Identifier: GPL-2.0-only
/* Isolated A55 494 -> 1014MHz warm trial, with A76 already at 1014MHz.
 * All six A55 cores must be Linux-offline and PPU-off before mutation.
 * PLL_EN stays set throughout. Failure only selects verified slow; it never
 * rewrites the inherited, below-spec N19 as a new PLL target or toggles EN.
 * The caller must keep A55 offline after failure and arrange recovery.
 * PMIC reads use the existing owner; no voltage, DSU or GPU changes.
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
#define PPU_BASE 0x07050000UL
#define PPU_SIZE 0x8000
#define XO_PHYS 0x07090160UL
#define L_CTRL 0x1000
#define L_CLK 0x101c
#define L_STAT 0x102c
#define N_MASK GENMASK(15, 8)
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define LDO BIT(30)
#define ENABLE BIT(31)
#define CTRL_494 0xf8801300U
#define CTRL_1014 0xf8802700U
#define CLK_PLL 0x03000000U
#define TAG "t736-a55-clock-trial-v1: "

static char *action = "audit";
module_param(action, charp, 0400);
MODULE_PARM_DESC(action, "audit (default, no CPU writes), upgrade (494 to1014MHz; all A55 offline)");

static const u16 guard_offsets[] = {
	0x0000, 0x0008, 0x000c,
	0x1004, 0x1008, 0x1018,
	0x2000, 0x2004, 0x2008, 0x2018, 0x201c, 0x202c,
	0x3000, 0x3004, 0x3008, 0x3018, 0x301c, 0x302c, 0x3034,
};

static bool little_offline(void)
{
	unsigned int cpu;
	for (cpu = 0; cpu < 6; cpu++)
		if (!cpu_possible(cpu) || cpu_online(cpu))
			return false;
	return cpu_online(6) && cpu_online(7) && num_online_cpus() == 2;
}

static bool little_powered_off(void __iomem *ppu)
{
	unsigned int cpu;
	bool off = true;
	/* TF-A native PSCI uses Aff1=core and PPU_PWSR(core+1).
	 * CPU0..5 therefore map to 0x07052008..0x07057008. */
	for (cpu = 0; cpu < 6; cpu++) {
		u32 value = readl(ppu + (cpu + 1) * 0x1000 + 0x1008);
		pr_info(TAG "CPU%u PPU_PWSR=%08x\n", cpu, value);
		if (value & 0xf)
			off = false;
	}
	return off;
}

static int check_supply(const struct t736_cpu_supply_snapshot *s)
{
	pr_info(TAG "PMIC id=%02x enable=%02x dcdc3=%02x dcdc4=%02x phase=%02x feedback=%02x\n",
		s->id, s->enable, s->dcdc3, s->dcdc4, s->phase, s->feedback);
	return s->id == 3 && (s->enable & 0x0c) == 0x0c &&
		!(s->phase & 0xc0) && (s->dcdc3 & 0x7f) == 40 &&
		(s->dcdc4 & 0x7f) == 40 ? 0 : -ERANGE;
}

static int write_mux(void __iomem *base, u32 value)
{
	u32 actual;
	writel(value, base + L_CLK);
	actual = readl(base + L_CLK);
	pr_info(TAG "mux expected=%08x actual=%08x\n", value, actual);
	return actual == value ? 0 : -EIO;
}

static int stage_control(void __iomem *base, u32 desired, u32 committed_n,
			 bool may_latch_n)
{
	u32 before = readl(base + L_CTRL), actual;
	if (!(desired & ENABLE) || (desired & (LOCK | UPDATE)) ||
	    !(before & ENABLE) || (before & UPDATE) ||
	    (may_latch_n && ((before & LOCK_EN) || !(desired & LOCK_EN))))
		return -EINVAL;
	writel(desired, base + L_CTRL);
	actual = readl(base + L_CTRL);
	pr_info(TAG "stage desired=%08x committed=%08x actual=%08x may_latch_n=%d\n",
		desired, committed_n, actual, may_latch_n);
	if (((actual ^ desired) & ~(LOCK | N_MASK)) ||
	    ((actual & N_MASK) != committed_n &&
	     !(may_latch_n && (actual & N_MASK) == (desired & N_MASK))))
		return -EIO;
	return 0;
}

static int wait_ready(void __iomem *base, u32 desired)
{
	unsigned int i, consecutive = 0;
	u32 expected = desired | LOCK;
	for (i = 0; i < 7000; i++) {
		if (readl(base + L_CTRL) == expected &&
		    readl(base + L_STAT) == expected)
			consecutive++;
		else
			consecutive = 0;
		if (consecutive == 3) {
			udelay(20);
			return readl(base + L_CTRL) == expected &&
			       readl(base + L_STAT) == expected ? 0 : -EIO;
		}
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int commit_control(void __iomem *base, u32 desired)
{
	unsigned int i;
	u32 ctrl, stat;
	if ((desired & (ENABLE | LDO | LOCK_EN)) != (ENABLE | LDO | LOCK_EN) ||
	    (desired & (LOCK | UPDATE | OUTPUT)) || (desired & N_MASK) != 0x2700)
		return -EINVAL;
	ctrl = readl(base + L_CTRL);
	if (!(ctrl & ENABLE) || (ctrl & UPDATE))
		return -EBUSY;
	udelay(1);
	/* Keep cached N39: rereading old N to compose UPDATE loses the target. */
	writel(desired | UPDATE, base + L_CTRL);
	for (i = 0; i < 1000; i++) {
		ctrl = readl(base + L_CTRL);
		if (!(ctrl & UPDATE)) {
			stat = readl(base + L_STAT);
			pr_info(TAG "commit desired=%08x CTRL=%08x STAT=%08x\n", desired, ctrl, stat);
			return ((ctrl ^ desired) & ~LOCK) ||
			       ((stat ^ desired) & ~LOCK) ? -EIO : 0;
		}
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int upgrade_1014(void __iomem *base)
{
	u32 desired = CTRL_494 & ~LOCK;
	const u32 old_n = 0x1300;
	int ret;
	ret = write_mux(base, 0);
	if (ret) return ret;
	udelay(20);
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
	return write_mux(base, CLK_PLL);
}

static int safe_slow(void __iomem *base)
{
	u32 ctrl, stat;
	int ret = write_mux(base, 0);
	udelay(20);
	ctrl = readl(base + L_CTRL);
	stat = readl(base + L_STAT);
	/* This failure path writes only CLK. In particular it cannot clear EN
	 * or commit any N, including the inherited below-spec N19. */
	if (ret || readl(base + L_CLK) || readl(base + 0x3034) ||
	    !(ctrl & ENABLE) || !(stat & ENABLE)) {
		pr_crit(TAG "SAFE_SLOW_INCOMPLETE CTRL=%08x STAT=%08x CLK=%08x; keep0-5offline and reset\n",
			ctrl, stat, readl(base + L_CLK));
		return -EIO;
	}
	pr_warn(TAG "SAFE_SLOW: CTRL=%08x STAT=%08x CLK=00000000; PLL_EN retained; no CTRL recovery writes; keep0-5offline\n",
		ctrl, stat);
	return 0;
}

static int guards_check(void __iomem *base, void __iomem *xo,
			void __iomem *ppu, const u32 *guards, u32 crystal)
{
	unsigned int i;
	int ret = 0;
	for (i = 0; i < ARRAY_SIZE(guard_offsets); i++) {
		u32 actual = readl(base + guard_offsets[i]);
		if (actual != guards[i]) {
			pr_err(TAG "guard +%04x expected=%08x actual=%08x\n",
				guard_offsets[i], guards[i], actual);
			ret = -EIO;
		}
	}
	if (readl(xo) != crystal || !little_offline() || !little_powered_off(ppu))
		ret = -EIO;
	return ret;
}

static int __init trial_init(void)
{
	struct t736_cpu_supply_snapshot before, after;
	typeof(&t736_pmic_read_cpu_supplies) get_snapshot = NULL;
	void __iomem *base = NULL, *xo = NULL, *ppu = NULL;
	u32 guards[ARRAY_SIZE(guard_offsets)], crystal, ctrl, stat, mux;
	bool audit = !strcmp(action, "audit"), changed = false, reserved = false;
	unsigned int i;
	int ret, rollback_ret = 0;
	if ((!audit && strcmp(action, "upgrade")) ||
	    !of_machine_is_compatible("allwinner,t736"))
		return -EINVAL;
	cpus_read_lock();
	if (num_possible_cpus() != 8 || !cpu_possible(6) || !cpu_possible(7)) {
		ret = -ENODEV; goto out;
	}
	if (!audit && (!little_offline() ||
	    MIDR_PARTNUM(read_cpuid_id()) != ARM_CPU_PART_CORTEX_A76)) {
		pr_err(TAG "require CPU0-5 offline, CPU6/7 online, execution on A76\n");
		ret = -EBUSY; goto out;
	}
	get_snapshot = symbol_get(t736_pmic_read_cpu_supplies);
	if (!get_snapshot) { ret = -ENODEV; goto out; }
	ret = get_snapshot(&before);
	if (ret) goto out;
	ret = check_supply(&before);
	if (ret) goto out;
	if (!request_mem_region(CPU_BASE + L_CTRL, 0x30, "t736-a55-clock-trial-v1")) {
		ret = -EBUSY; goto out;
	}
	reserved = true;
	base = ioremap(CPU_BASE, CPU_SIZE);
	xo = ioremap(XO_PHYS, sizeof(u32));
	ppu = ioremap(PPU_BASE, PPU_SIZE);
	if (!base || !xo || !ppu) { ret = -ENOMEM; goto out; }
	crystal = readl(xo);
	ctrl = readl(base + L_CTRL); stat = readl(base + L_STAT); mux = readl(base + L_CLK);
	pr_info(TAG "action=%s XO=%08x A55=%08x/%08x/%08x A76=%08x/%08x/%08x online=%*pbl MIDR=%08x\n",
		action, crystal, ctrl, stat, mux, readl(base + 0x2000),
		readl(base + 0x202c), readl(base + 0x201c),
		cpumask_pr_args(cpu_online_mask), read_cpuid_id());
	for (i = 0; i < ARRAY_SIZE(guard_offsets); i++)
		guards[i] = readl(base + guard_offsets[i]);
	if (audit) {
		little_powered_off(ppu);
		pr_info(TAG "AUDIT: no CPU writes; PMIC owner read only\n");
		ret = 0; goto out;
	}
	if ((crystal & (BIT(14) | BIT(15))) != BIT(15) ||
	    ctrl != CTRL_494 || stat != CTRL_494 || mux != CLK_PLL ||
	    readl(base + 0x3034) || readl(base + 0x1004) ||
	    readl(base + 0x1008) != 0x30000000U ||
	    readl(base + 0x1018) != 0x40000001U ||
	    readl(base + 0x2000) != CTRL_1014 ||
	    readl(base + 0x202c) != CTRL_1014 || readl(base + 0x201c) != CLK_PLL) {
		ret = -EINVAL; goto out;
	}
	ret = guards_check(base, xo, ppu, guards, crystal);
	if (ret) goto out;
	udelay(20);
	ret = guards_check(base, xo, ppu, guards, crystal);
	if (ret) goto out;
	changed = true;
	ret = upgrade_1014(base);
	if (!ret && (readl(base + L_CTRL) != CTRL_1014 ||
	    readl(base + L_STAT) != CTRL_1014 || readl(base + L_CLK) != CLK_PLL))
		ret = -EIO;
	if (!ret) ret = get_snapshot(&after);
	if (!ret) ret = check_supply(&after);
	if (!ret && memcmp(&before, &after, sizeof(before))) ret = -EIO;
	if (guards_check(base, xo, ppu, guards, crystal)) ret = -EIO;
	if (ret) {
		pr_err(TAG "trial failed=%d; retaining EN and reducing A55 mux only\n", ret);
		rollback_ret = safe_slow(base);
		if (guards_check(base, xo, ppu, guards, crystal)) {
			pr_crit(TAG "guard changed; keep0-5offline and reset\n");
			rollback_ret = -EIO;
		}
		/* Report post-failure supply through its owner, never alter it. */
		if (get_snapshot(&after) || check_supply(&after) ||
		    memcmp(&before, &after, sizeof(before)))
			pr_crit(TAG "post-failure PMIC unchanged check failed\n");
	} else {
		pr_info(TAG "SUCCESS: A55=1014MHz CTRL=%08x STAT=%08x CLK=%08x; A76/DSU/PMIC unchanged; caller may online0-5\n",
			readl(base + L_CTRL), readl(base + L_STAT), readl(base + L_CLK));
	}
out:
	if (ppu) iounmap(ppu);
	if (xo) iounmap(xo);
	if (base) iounmap(base);
	if (reserved) release_mem_region(CPU_BASE + L_CTRL, 0x30);
	if (get_snapshot) symbol_put(t736_pmic_read_cpu_supplies);
	cpus_read_unlock();
	pr_info(TAG "finished rc=%d mutation_attempted=%d rollback_rc=%d\n", ret, changed, rollback_ret);
	return ret;
}

static void __exit trial_exit(void)
{
	pr_info(TAG "unloaded; setting retained; no MMIO writes\n");
}
module_init(trial_init);
module_exit(trial_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 A55 warm 494 to1014MHz offline-core trial; PLL_EN retained");
