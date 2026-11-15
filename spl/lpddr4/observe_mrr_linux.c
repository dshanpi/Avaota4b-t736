// SPDX-License-Identifier: GPL-2.0-only
/* Executes the source MRR component against initialized DRAM. Not training. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include "mode_register.h"

struct regions {
	void __iomem *ctl[2];
	void __iomem *com;
};

static void __iomem *read_pointer(struct regions *r, uint32_t addr)
{
	unsigned int ch;
	for (ch = 0; ch < 2; ch++) {
		uint32_t base = 0x0a110000u + ch * 0x400000u;
		if (addr == base || addr == base + 0x14 || addr == base + 0x80 ||
		    addr == base + 0x84 || addr == base + 0x90)
			return r->ctl[ch] + (addr - base);
	}
	if (addr == T736_LP4_MRR_RESULT_CH0 || addr == T736_LP4_MRR_RESULT_CH1)
		return r->com + (addr - 0x0a020000u);
	return NULL;
}

static int get32(void *ctx, uint32_t addr, uint32_t *value)
{
	void __iomem *p = read_pointer(ctx, addr);
	if (!p)
		return -EINVAL;
	*value = readl(p);
	return 0;
}

static int put32(void *ctx, uint32_t addr, uint32_t value)
{
	struct regions *r = ctx;
	unsigned int ch;
	for (ch = 0; ch < 2; ch++) {
		uint32_t base = 0x0a110000u + ch * 0x400000u;
		if (addr == base + 0x80 &&
		    (value == 0x11 || value == 0x21 ||
		     value == 0x80000011u || value == 0x80000021u)) {
			writel(value, r->ctl[ch] + 0x80);
			return 0;
		}
		if (addr == base + 0x84 &&
		    (value == (5u << 8) || value == (6u << 8) || value == (8u << 8))) {
			writel(value, r->ctl[ch] + 0x84);
			return 0;
		}
	}
	return -EPERM;
}

static int wait_us(void *ctx, uint32_t usec)
{
	(void)ctx;
	if (usec != 1)
		return -EINVAL;
	udelay(usec);
	return 0;
}

static int __init mrr_observe_init(void)
{
	static const unsigned int regs[2][3] = {{5, 6, 8}, {8, 6, 5}};
	struct regions r = {0};
	struct t736_lp4_mrr_io io = {&r, get32, put32, wait_us};
	unsigned int pass, ch, rank, i;
	int ret = 0;
	if (!of_machine_is_compatible("allwinner,t736"))
		return -ENODEV;
	r.ctl[0] = ioremap(0x0a110000, 0x100);
	r.ctl[1] = ioremap(0x0a510000, 0x100);
	r.com = ioremap(0x0a020000, 0x44);
	if (!r.ctl[0] || !r.ctl[1] || !r.com) {
		ret = -ENOMEM;
		goto out;
	}
	/* Caller checks no DDR DVFS or other software-MR diagnostic is active.
	 * Function itself verifies LP4/type/rank/normal/idle before each request.
	 * A failure stops the entire diagnostic; never retry/reset a controller. */
	for (pass = 0; pass < 2; pass++)
		for (ch = 0; ch < 2; ch++)
			for (rank = 0; rank < 2; rank++)
				for (i = 0; i < 3; i++) {
					struct t736_lp4_mrr_result result = {0};
					unsigned int mr = regs[pass][i];
					int rc = t736_lp4_mrr_read_id(&io, ch, rank, mr, 1000, &result);
					pr_info("t736-lp4-mrr: pass=%u ch=%u rank=%u mr=%u rc=%d raw=%08x value=%02x\n",
						pass, ch, rank, mr, rc, result.raw, result.value);
					if (rc) {
						ret = -EIO;
						goto out;
					}
				}
out:
	if (r.ctl[0]) iounmap(r.ctl[0]);
	if (r.ctl[1]) iounmap(r.ctl[1]);
	if (r.com) iounmap(r.com);
	return ret;
}

static void __exit mrr_observe_exit(void) {}
module_init(mrr_observe_init);
module_exit(mrr_observe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("T736 source MRR identity diagnostic; no MRW or PHY reconfiguration");
