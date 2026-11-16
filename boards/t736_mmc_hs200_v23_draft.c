// SPDX-License-Identifier: GPL-2.0-only
/* T736 SMHC3 experimental HS200 software tuning on the frozen v21 path.
 * Disabled by default. First opt-in stage100MHz; later stage200MHz.
 * CMD21 verifies the actual JEDEC pattern. No HS400 or sharedPLL/rail writes.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define CTRL 0x00
#define CLKDIV 0x04
#define TIMEOUT 0x08
#define WIDTH 0x0c
#define BLOCKSIZE 0x10
#define BYTECOUNT 0x14
#define COMMAND 0x18
#define ARGUMENT 0x1c
#define RESPONSE 0x20
#define IRQMASK 0x30
#define IRQSTATUS 0x38
#define STATUS 0x3c
#define FIFOTHRESHOLD 0x40
#define CARD_BYTECOUNT 0x48
#define HOST_BYTECOUNT 0x4c
#define CRC_CONTROL 0x54
#define TIMING 0x5c
#define CARD_THRESHOLD 0x100
#define SAMPLE_FIFO 0x104
#define CARD_THRESHOLD_MASK (GENMASK(28, 16) | BIT(2) | BIT(0))
#define EMMC_DDR 0x10c
#define DRIVE_DELAY 0x140
#define SAMPLE_DELAY 0x144
#define STROBE_DELAY 0x148
#define CARDRESET 0x78
#define DMAC 0x80
#define DLBA 0x84
#define IDST 0x88
#define IDIE 0x8c
#define SMCV 0x300
#define CTRL_DMA BIT(5)
#define CTRL_CPU BIT(31)
#define DMA_RESET BIT(0)
#define DMA_ENABLE BIT(7)
#define DMA_STATUS_CLEAR 0x133
#include "t736_mmc_dma_desc.h"
#include "t736_mmc_clock_hs200_draft.h"
#include "t736_mmc_tuning_draft.h"
#include "t736_mmc_tuning_error_draft.h"
#define CMD_START BIT(31)
#define MAX_REQUEST_BYTES T736_DMA_BYTES

static unsigned int hs_max_hz = 25000000;
module_param(hs_max_hz, uint, 0444);
MODULE_PARM_DESC(hs_max_hz, "Fixed-board HS SDR card cap: 25000000 or 50000000");
static bool enable_hs200;
module_param(enable_hs200, bool, 0444);
MODULE_PARM_DESC(enable_hs200, "Experimental CMD21-verified HS200; disabled by default");
static unsigned int hs200_max_hz = 100000000;
module_param(hs200_max_hz, uint, 0444);
MODULE_PARM_DESC(hs200_max_hz, "Experimental HS200 cap: 100000000 or 200000000");

static const u16 timing_offsets[] = {
	TIMING, EMMC_DDR, CRC_CONTROL, DRIVE_DELAY, SAMPLE_DELAY, STROBE_DELAY
};
static const u32 timing_masks[] = {
	BIT(31) | BIT(0), BIT(31), 0xf, BIT(16) | BIT(17), BIT(7) | 0x3f, BIT(7) | 0x3f
};
#define MODULE_CLOCK_MASK (BIT(31) | GENMASK(26, 24) | GENMASK(12, 8) | GENMASK(4, 0))

struct t736_host {
	struct mmc_host *mmc;
	void __iomem *regs, *clock, *gate, *pll_ref, *pll_peri0;
	u32 dcxo_hz, legacy_timing[ARRAY_SIZE(timing_offsets)];
	u32 last_clock, last_timing;
	bool tuning_running, tuning_valid, tuning_error_reported;
	u32 tuned_hz;
	u8 tuned_tap;
	u32 legacy_threshold;
	struct work_struct work;
	struct workqueue_struct *workqueue;
	struct mmc_request *request;
	int ios_error;
	u32 *buffer;
	dma_addr_t buffer_dma, desc_dma;
	struct t736_dma_desc *desc;
	unsigned int desc_count;
	bool dma_dead;
	u64 dma_requests, dma_bytes;
	u32 last_irq, last_idst;
};

/* The firmware initializes the shared DMA bus. Observe only these two
 * gates; never import Tina's broad "key" OR into another master's word.
 */
static int t736_check_dma_bus(struct platform_device *pdev)
{
	static const char * const names[] = { "STORE_MBUS", "MSI_LITE1" };
	int i;

	for (i = 0; i < 2; i++) {
		struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, i + 3);
		void __iomem *reg;
		u32 value, mask = i ? BIT(0) : BIT(30);

		if (!res)
			return -EINVAL;
		reg = devm_ioremap(&pdev->dev, res->start, resource_size(res));
		if (!reg)
			return -ENOMEM;
		value = readl(reg);
		dev_info(&pdev->dev, "%s addr=%pa value=%08x (read-only)\n",
			 names[i], &res->start, value);
		if (!(value & mask))
			return dev_err_probe(&pdev->dev, -ENODEV,
					     "DMA bus %s gate is off; refusing DMA\n", names[i]);
	}
	return 0;
}

static int t736_update_clock(struct t736_host *host)
{
	u32 value;
	int ret;

	writel(CMD_START | BIT(21) | BIT(13), host->regs + COMMAND);
	ret = readl_poll_timeout(host->regs + COMMAND, value,
				!(value & CMD_START), 10, 1000000);
	writel(~0U, host->regs + IRQSTATUS);
	return ret;
}

static int t736_read_clock_parents(struct platform_device *pdev,
				  struct t736_host *host)
{
	struct clk *dcxo;
	struct resource *res;

	dcxo = devm_clk_get(&pdev->dev, "dcxo");
	if (IS_ERR(dcxo))
		return PTR_ERR(dcxo);
	host->dcxo_hz = clk_get_rate(dcxo);
	if (host->dcxo_hz != 26000000)
		return dev_err_probe(&pdev->dev, -EINVAL, "Expected fixed 26 MHz board DCXO\n");
	res = platform_get_resource(pdev, IORESOURCE_MEM, 5);
	if (!res || res->start != 0x02002000 || resource_size(res) < 4)
		return -EINVAL;
	host->pll_ref = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	res = platform_get_resource(pdev, IORESOURCE_MEM, 6);
	/* CTRL, LOCK (not sampled), PAT0 and PAT1; this mapping is read-only. */
	if (!res || res->start != 0x020020a0 || resource_size(res) < 16)
		return -EINVAL;
	host->pll_peri0 = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	return host->pll_ref && host->pll_peri0 ? 0 : -ENOMEM;
}

static int t736_validate_clock_parents(struct t736_host *host, bool need_peri,
				       u32 *sys24, u32 *peri800)
{
	u32 reference, peripheral, pattern0, pattern1;

	reference = readl(host->pll_ref);
	peripheral = readl(host->pll_peri0);
	pattern0 = readl(host->pll_peri0 + 8);
	pattern1 = readl(host->pll_peri0 + 12);
	*sys24 = t736_ref24_hz(reference, host->dcxo_hz);
	*peri800 = t736_peri0_800_hz(reference, peripheral,
				    pattern0, pattern1, host->dcxo_hz);
	/* SYS_CLK24M is documented as fixed 24M. On this fixed 26M board,
	 * require a ready 24M REF even for low-speed SYS_CLK24M enumeration.
	 * It is not a physical frequency measurement or a claim about a
	 * software-visible SYS_CLK24M mux: no such selector is documented.
	 */
	if (!*sys24 || (need_peri && !*peri800)) {
		dev_err(mmc_dev(host->mmc),
			"PLL parent not verified: REF=%08x PERI0=%08x PAT0=%08x PAT1=%08x\n",
			reference, peripheral, pattern0, pattern1);
		return -EIO;
	}
	return 0;
}

static void t736_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct t736_host *host = mmc_priv(mmc);
	struct t736_clock_plan plan = { 0 };
	u32 value, target, sys24, peri800, requested;
	u32 timing_values[ARRAY_SIZE(timing_offsets)];
	bool highspeed = ios->timing == MMC_TIMING_MMC_HS;
	bool hs200 = ios->timing == MMC_TIMING_MMC_HS200;
	unsigned int i;
	int ret;

	mmc->actual_clock = 0;
	if (host->dma_dead) {
		host->ios_error = -EIO;
		return;
	}
	/* Stop the card clock and commit before changing any module clock or
	 * phase field. The v19 DMA worker has already reset its completed I/O.
	 */
	writel(BIT(31), host->regs + CLKDIV);
	ret = t736_update_clock(host);
	if (ret || !ios->clock)
		goto done;
	if ((!highspeed && !hs200 && ios->timing != MMC_TIMING_LEGACY) ||
	    (hs200 && (!enable_hs200 || ios->signal_voltage != MMC_SIGNAL_VOLTAGE_180 ||
		       ios->bus_width != MMC_BUS_WIDTH_8))) {
		ret = -EINVAL;
		goto done;
	}
	target = min(ios->clock, hs200 ? hs200_max_hz :
		     highspeed ? hs_max_hz : 12000000U);
	ret = t736_validate_clock_parents(host, target > 12000000, &sys24, &peri800);
	if (ret)
		goto done;
	/* Enumeration keeps mux0 even when the verified 800M parent is ready. */
	ret = t736_plan_sdr_clock(target, sys24, target > 12000000 ? peri800 : 0, &plan);
	if (ret)
		goto done;
	/* Factor M and N are linear 1..32 on SMHC3, not powers of two. */
	value = readl(host->clock);
	writel(value & ~BIT(31), host->clock);
	if (readl(host->clock) & BIT(31)) {
		ret = -EIO;
		goto done;
	}
	udelay(1);
	requested = (value & ~MODULE_CLOCK_MASK) | (plan.reg & ~BIT(31));
	writel(requested, host->clock);
	if ((readl(host->clock) & MODULE_CLOCK_MASK) != (requested & MODULE_CLOCK_MASK)) {
		ret = -EIO;
		goto done;
	}
	value = readl(host->regs + CTRL);
	writel(value & ~BIT(10), host->regs + CTRL); /* SDR only, never DDR52. */
	for (i = 0; i < ARRAY_SIZE(timing_offsets); i++)
		timing_values[i] = host->legacy_timing[i];
	/* Preserve the accepted legacy path for enumeration and failed-HS recovery. */
	timing_values[0] = (timing_values[0] & ~BIT(0)) | BIT(31);
	if (highspeed || hs200) {
		timing_values[0] = 0; /* Tina v5p6x 1x, no HS400 new sampling. */
		timing_values[1] = 0; /* HS400 enable clear; preserve register bit0. */
		timing_values[2] = 3; /* CRC detect parameter for non-HS400. */
		timing_values[3] = BIT(16); /* CMD180 degrees, DATA90 degrees. */
		/* Board sm1=0xff20ffff: <=25MHz uses fallback0; <=50MHz uses32. */
		timing_values[4] = BIT(7) | (plan.card_hz > 25000000 ? 32 : 0);
		timing_values[5] = BIT(7); /* No data-strobe delay in SDR mode. */
	}
	if (hs200) {
		if (host->tuned_hz != plan.card_hz) {
			if (host->tuning_valid)
				mmc_retune_needed(mmc);
			host->tuning_valid = false;
		}
		timing_values[4] = BIT(7) | (host->tuning_valid ?
			host->tuned_tap : t736_hs200_seed(plan.card_hz));
	} else {
		host->tuning_valid = false;
		value = readl(host->regs + CARD_THRESHOLD);
		value = (value & ~CARD_THRESHOLD_MASK) |
			(host->legacy_threshold & CARD_THRESHOLD_MASK);
		writel(value, host->regs + CARD_THRESHOLD);
		if ((readl(host->regs + CARD_THRESHOLD) & CARD_THRESHOLD_MASK) !=
		    (value & CARD_THRESHOLD_MASK)) {
			ret = -EIO;
			goto done;
		}
	}
	for (i = 0; i < ARRAY_SIZE(timing_offsets); i++) {
		value = readl(host->regs + timing_offsets[i]);
		value = (value & ~timing_masks[i]) | (timing_values[i] & timing_masks[i]);
		writel(value, host->regs + timing_offsets[i]);
		if ((readl(host->regs + timing_offsets[i]) & timing_masks[i]) !=
		    (value & timing_masks[i])) {
			ret = -EIO;
			goto done;
		}
	}
	writel(ios->bus_width == MMC_BUS_WIDTH_8 ? 2 :
	       ios->bus_width == MMC_BUS_WIDTH_4 ? 1 : 0, host->regs + WIDTH);
	writel(requested | BIT(31), host->clock);
	if ((readl(host->clock) & MODULE_CLOCK_MASK) !=
	    ((requested | BIT(31)) & MODULE_CLOCK_MASK)) {
		ret = -EIO;
		goto done;
	}
	udelay(1);
	/* Internal CLKDIV=0: SDR output is module/2. Mask DATA0 only for update. */
	writel(BIT(31) | BIT(16), host->regs + CLKDIV);
	ret = t736_update_clock(host);
	if (ret) {
		writel(BIT(31), host->regs + CLKDIV);
		t736_update_clock(host);
		goto done;
	}
	writel(BIT(16), host->regs + CLKDIV);
	mmc->actual_clock = plan.card_hz;
	if (host->last_clock != plan.card_hz || host->last_timing != ios->timing) {
		dev_info(mmc_dev(mmc),
			"SDR timing=%u card=%u parent=%u mux=%u M=%u N=%u module=%08x sample=%u signal=%u (no rail writes)\n",
			ios->timing, plan.card_hz, plan.parent_hz, plan.source, plan.m, plan.n,
			readl(host->clock), readl(host->regs + SAMPLE_DELAY) & 0x3f,
			ios->signal_voltage);
		host->last_clock = plan.card_hz;
		host->last_timing = ios->timing;
	}
done:
	host->ios_error = ret;
	if (ret)
		dev_err(mmc_dev(mmc), "set_ios failed %d; card clock disabled\n", ret);
}

static int t736_signal_voltage(struct mmc_host *mmc, struct mmc_ios *ios)
{
	/* ALDO3/PC VCCQ is physically fixed 1.8V. This callback describes that
	 * rail to core; it does not issue CMD11, write PMIC or switch pin voltage.
	 */
	return ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180 ? 0 : -EINVAL;
}

/* Never recycle DMA memory after a reset timeout. A dead engine pins both
 * allocations until reboot rather than risking a delayed bus write to freed RAM.
 */
static int t736_dma_stop(struct t736_host *host)
{
	u32 value;
	int ret;

	writel(0, host->regs + IDIE);
	writel(0, host->regs + DMAC);
	value = readl(host->regs + CTRL) & ~CTRL_DMA;
	writel(value | BIT(2), host->regs + CTRL);
	ret = readl_poll_timeout(host->regs + CTRL, value,
				!(value & BIT(2)), 10, 100000);
	if (ret)
		goto dead;
	writel(DMA_RESET, host->regs + DMAC);
	ret = readl_poll_timeout(host->regs + DMAC, value,
				!(value & DMA_RESET), 10, 100000);
	if (ret)
		goto dead;
	value = readl(host->regs + CTRL);
	writel(value | BIT(1), host->regs + CTRL);
	ret = readl_poll_timeout(host->regs + CTRL, value,
				!(value & BIT(1)), 10, 100000);
	if (ret)
		goto dead;
	writel(DMA_STATUS_CLEAR, host->regs + IDST);
	return 0;
dead:
	host->dma_dead = true;
	host->ios_error = -EIO;
	dev_crit(mmc_dev(host->mmc), "IDMA reset timeout; DMA allocations quarantined until reboot\n");
	return ret;
}

static int t736_dma_start(struct t736_host *host, struct mmc_data *data,
			  unsigned int bytes)
{
	int ret;
	u32 value;

	ret = t736_dma_stop(host);
	if (ret)
		return ret;
	if (host->mmc->ios.timing == MMC_TIMING_MMC_HS200) {
		/* SMHC3 FIFO is at least1024B: RX15=60B plus one128/512B
		 * block fits; TX240=960B can hold a full write block.
		 * Matching THLD to BLKSZ enables block-gap flow control, as
		 * in Tina v4p5x. Preserve BCIG and SFC timing; do not bypass.
		 */
		if (data->blksz != 128 && data->blksz != 512)
			return -EINVAL;
		value = readl(host->regs + CARD_THRESHOLD) & ~CARD_THRESHOLD_MASK;
		value |= data->blksz << 16;
		value |= data->flags & MMC_DATA_WRITE ? BIT(2) : BIT(0);
		writel(value, host->regs + CARD_THRESHOLD);
		if ((readl(host->regs + CARD_THRESHOLD) & CARD_THRESHOLD_MASK) !=
		    (value & CARD_THRESHOLD_MASK)) {
			host->ios_error = -EIO;
			return -EIO;
		}
	}
	ret = t736_dma_describe(host->desc, host->desc_dma, host->buffer_dma, bytes);
	if (ret < 0)
		return ret;
	host->desc_count = ret;
	if ((data->flags & MMC_DATA_WRITE) &&
	    sg_copy_to_buffer(data->sg, data->sg_len, host->buffer, bytes) != bytes)
		return -EINVAL;
	/* Coherent allocation still needs ordering before ownership reaches HW. */
	dma_wmb();
	writel(host->desc_dma >> 2, host->regs + DLBA);
	writel(data->blksz, host->regs + BLOCKSIZE);
	writel(bytes, host->regs + BYTECOUNT);
	/* SMHC3: MBUS outstanding encoding 3, RX15, TX240 (A733 11.3.9.17). */
	writel(0x300f00f0, host->regs + FIFOTHRESHOLD);
	value = readl(host->regs + CTRL);
	value &= ~(CTRL_CPU | BIT(4));
	writel(value | CTRL_DMA, host->regs + CTRL);
	/* Enable controller event sources as the donor does. Global CTRL.INT_EN
	 * remains clear, so completion is still owned by this polling worker.
	 * SMHC3 IDST event latching with IDIE=0 is not established by the manual.
	 */
	writel((data->flags & MMC_DATA_WRITE ? BIT(0) : BIT(1)) |
	       T736_DMA_ERRORS, host->regs + IDIE);
	/* SMHC3 DMAC bit1 is reserved; only bit7 enables this engine. */
	writel(DMA_ENABLE, host->regs + DMAC);
	return 0;
}

/* Observe before stop/reset destroys the failed transaction state. Do not
 * dump transferred payload: these diagnostics are also used for filesystem I/O.
 */
static void t736_dma_snapshot(struct t736_host *host, struct mmc_command *cmd)
{
	unsigned int i, owned = 0;
	u32 fifo = readl(host->regs + STATUS), owned_lo = 0, owned_hi = 0;
	u32 rint = readl(host->regs + IRQSTATUS);

	dma_rmb();
	for (i = 0; i < host->desc_count; i++)
		if (le32_to_cpu(READ_ONCE(host->desc[i].flags)) & T736_DESC_OWN) {
			owned++;
			if (i < 32)
				owned_lo |= BIT(i);
			else
				owned_hi |= BIT(i - 32);
		}
	dev_err(mmc_dev(host->mmc),
		"DMA snapshot CMD%u CTRL=%08x DMAC=%08x DLBA=%08x IDIE=%08x IDST=%08x RINT=%08x STATUS=%08x FIFO_words=%u FTH=%08x owned=%u/%u\n",
		cmd->opcode, readl(host->regs + CTRL), readl(host->regs + DMAC),
		readl(host->regs + DLBA), readl(host->regs + IDIE),
		readl(host->regs + IDST), rint, fifo,
		(fifo >> 17) & 0x7ff, readl(host->regs + FIFOTHRESHOLD),
		owned, host->desc_count);
	/* A733 requires transfer-complete before reading stable TBC0/TBC1. */
	if (rint & BIT(3))
		dev_err(mmc_dev(host->mmc),
			"DMA bytecounts card=%u host=%u OWN_bitmap=%08x:%08x\n",
			readl(host->regs + CARD_BYTECOUNT), readl(host->regs + HOST_BYTECOUNT),
			owned_hi, owned_lo);
	else
		dev_err(mmc_dev(host->mmc),
			"DMA bytecounts not-valid-before-data-complete OWN_bitmap=%08x:%08x\n",
			owned_hi, owned_lo);
	for (i = 0; i < host->desc_count; i++)
		if (!i || i == host->desc_count - 1)
			dev_err(mmc_dev(host->mmc), "DMA desc%u %08x %08x %08x %08x\n", i,
			le32_to_cpu(READ_ONCE(host->desc[i].flags)),
			le32_to_cpu(READ_ONCE(host->desc[i].length)),
			le32_to_cpu(READ_ONCE(host->desc[i].buffer)),
			le32_to_cpu(READ_ONCE(host->desc[i].next)));
}

static int t736_command(struct t736_host *host, struct mmc_command *cmd,
			struct mmc_data *data)
{
	u32 flags = CMD_START | cmd->opcode, value, status = 0, dma_status = 0;
	unsigned int bytes = 0;
	unsigned long deadline;
	bool write = data && (data->flags & MMC_DATA_WRITE);
	bool auto_stop = data && data->stop;
	int ret = 0, stop_ret, done, i;

	if (host->dma_dead)
		return -EIO;
	if (data) {
		if (!data->blksz || !data->blocks ||
		    data->blocks > MAX_REQUEST_BYTES / data->blksz)
			return -EINVAL;
		bytes = data->blksz * data->blocks;
		if (bytes % 4)
			return -EINVAL;
		if (host->tuning_running && cmd->opcode == MMC_SEND_TUNING_BLOCK_HS200) {
			if (write || data->blocks != 1 || bytes != 128)
				return -EINVAL;
			/* Poison every trial: an old matching DMA buffer must never
			 * count as a newly received JEDEC tuning pattern.
			 */
			memset(host->buffer, 0xa5, bytes);
		}
		ret = t736_dma_start(host, data, bytes);
		if (ret)
			return ret;
		flags |= BIT(9) | BIT(13);
		if (auto_stop)
			flags |= BIT(12);
		if (write)
			flags |= BIT(10);
	}
	if (!cmd->opcode)
		flags |= BIT(15);
	if (cmd->flags & MMC_RSP_PRESENT)
		flags |= BIT(6);
	if (cmd->flags & MMC_RSP_136)
		flags |= BIT(7);
	if (cmd->flags & MMC_RSP_CRC)
		flags |= BIT(8);
	if (cmd->opcode == 12)
		flags |= BIT(14);
	writel(~0U, host->regs + IRQSTATUS);
	writel(cmd->arg, host->regs + ARGUMENT);
	writel(flags, host->regs + COMMAND);
	deadline = jiffies + msecs_to_jiffies(host->tuning_running ? 200 : 2000);
	for (;;) {
		status = readl(host->regs + IRQSTATUS);
		dma_status = data ? readl(host->regs + IDST) : 0;
		/* Reserved/error bits cannot hide behind otherwise complete DMA.
		 * Restrict this guard to the read-only CMD21 tuning experiment.
		 */
		if (data && host->tuning_running &&
		    cmd->opcode == MMC_SEND_TUNING_BLOCK_HS200 &&
		    t736_tuning_idst_invalid(dma_status)) {
			ret = -EIO;
			break;
		}
		done = t736_dma_poll_result(status, dma_status, !!data, write,
					    auto_stop, host->desc, host->desc_count);
		if (done) {
			ret = done < 0 ? done : 0;
			break;
		}
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
		usleep_range(50, 100);
	}
	host->last_irq = status;
	host->last_idst = dma_status;
	if (!ret) {
		if (cmd->flags & MMC_RSP_136)
			for (i = 0; i < 4; i++)
				cmd->resp[i] = readl(host->regs + RESPONSE + (3 - i) * 4);
		else
			cmd->resp[0] = readl(host->regs + RESPONSE);
		if (cmd->flags & MMC_RSP_BUSY)
			ret = readl_poll_timeout(host->regs + STATUS, value,
						!(value & BIT(9)), 10, 2000000);
	}
	if (data) {
		if (ret && (!host->tuning_running || !host->tuning_error_reported ||
			    (dma_status & T736_DMA_ERRORS))) {
			t736_dma_snapshot(host, cmd);
			if (host->tuning_running)
				host->tuning_error_reported = true;
		}
		/* Stop/reset is mandatory before copying results, returning or reuse. */
		stop_ret = t736_dma_stop(host);
		if (!ret)
			ret = stop_ret;
		if (!ret && !write) {
			dma_rmb();
			if (sg_copy_from_buffer(data->sg, data->sg_len, host->buffer, bytes) != bytes)
				ret = -EINVAL;
		}
		if (!ret) {
			host->dma_requests++;
			host->dma_bytes += bytes;
			if (host->dma_requests == 1)
				dev_info(mmc_dev(host->mmc),
					 "first IDMA request completed: CMD%u bytes=%u IRQ=%08x IDST=%08x\n",
					 cmd->opcode, bytes, status, dma_status);
		}
	}
	if (ret && !host->tuning_running)
		dev_err_ratelimited(mmc_dev(host->mmc),
			"CMD%u err=%d IRQ=%08x IDST=%08x bytes=%u DMA_ok=%llu/%llu\n",
			cmd->opcode, ret, status, dma_status, bytes,
			host->dma_requests, host->dma_bytes);
	writel(~0U, host->regs + IRQSTATUS);
	return ret;
}

static void t736_worker(struct work_struct *work)
{
	struct t736_host *host = container_of(work, struct t736_host, work);
	struct mmc_request *mrq = host->request;
	int ret = host->ios_error;
	/* Core may issue non-data switch/status commands before initial tuning.
	 * Data I/O at an unverified HS200 point is limited to our CMD21 trials.
	 */
	if (!ret && mrq->data && host->mmc->ios.timing == MMC_TIMING_MMC_HS200 &&
	    !host->tuning_valid && !(host->tuning_running &&
		mrq->cmd->opcode == MMC_SEND_TUNING_BLOCK_HS200))
		ret = -EAGAIN;

	if (!ret && mrq->sbc) {
		ret = t736_command(host, mrq->sbc, NULL);
		mrq->sbc->error = ret;
	}
	if (!ret)
		ret = t736_command(host, mrq->cmd, mrq->data);
	mrq->cmd->error = ret;
	if (mrq->data) {
		mrq->data->error = ret;
		mrq->data->bytes_xfered = ret ? 0 : mrq->data->blksz * mrq->data->blocks;
	}
	if (mrq->stop && ret)
		mrq->stop->error = t736_command(host, mrq->stop, NULL);
	mmc_request_done(host->mmc, mrq);
}

static void t736_request(struct mmc_host *mmc, struct mmc_request *request)
{
	struct t736_host *host = mmc_priv(mmc);

	host->request = request;
	queue_work(host->workqueue, &host->work);
}

static int t736_tuning_set_tap(struct t736_host *host, unsigned int tap)
{
	u32 value;
	int ret;

	if (tap >= T736_TUNING_TAPS || host->dma_dead)
		return -EINVAL;
	/* execute_tuning runs under the core's host claim. Each prior CMD21
	 * has synchronously completed and the worker stopped/reset its DMA.
	 */
	writel(BIT(31), host->regs + CLKDIV);
	ret = t736_update_clock(host);
	if (ret)
		return ret;
	value = readl(host->regs + SAMPLE_DELAY);
	value = (value & ~(BIT(7) | 0x3f)) | BIT(7) | tap;
	writel(value, host->regs + SAMPLE_DELAY);
	if ((readl(host->regs + SAMPLE_DELAY) & (BIT(7) | 0x3f)) != (BIT(7) | tap))
		return -EIO;
	writel(BIT(31) | BIT(16), host->regs + CLKDIV);
	ret = t736_update_clock(host);
	if (ret) {
		writel(BIT(31), host->regs + CLKDIV);
		t736_update_clock(host);
		return ret;
	}
	writel(BIT(16), host->regs + CLKDIV);
	return 0;
}

static int t736_execute_tuning(struct mmc_host *mmc, u32 opcode)
{
	struct t736_host *host = mmc_priv(mmc);
	struct t736_tuning_window window;
	u64 passing = 0, abort_failed = 0;
	u32 failed_irq, failed_idst;
	unsigned long deadline = jiffies + msecs_to_jiffies(30000);
	unsigned int tap, trial;
	int ret = -EIO, abort_ret = -ECANCELED;
	int stop_ret = -ECANCELED, clock_ret = -ECANCELED;

	if (!enable_hs200 || opcode != MMC_SEND_TUNING_BLOCK_HS200 ||
	    mmc->ios.timing != MMC_TIMING_MMC_HS200 ||
	    mmc->ios.bus_width != MMC_BUS_WIDTH_8 ||
	    mmc->ios.signal_voltage != MMC_SIGNAL_VOLTAGE_180 ||
	    !mmc->actual_clock || mmc->actual_clock > hs200_max_hz ||
	    host->dma_dead || host->ios_error)
		return -EINVAL;
	host->tuning_valid = false;
	host->tuning_running = true;
	host->tuning_error_reported = false;
	for (tap = 0; tap < T736_TUNING_TAPS; tap++) {
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto fail;
		}
		ret = t736_tuning_set_tap(host, tap);
		if (ret)
			goto fail;
		for (trial = 0; trial < T736_TUNING_TRIALS; trial++) {
			if (time_after(jiffies, deadline)) {
				ret = -ETIMEDOUT;
				goto fail;
			}
			/* Linux compares all128 received bytes against the JEDEC
			 * eight-bit pattern, in addition to command/data status.
			 */
			ret = mmc_send_tuning(mmc, opcode, NULL);
			if (ret)
				break;
		}
		if (!ret) {
			passing |= 1ULL << tap;
			continue;
		}
		if (host->dma_dead || host->ios_error ||
		    !t736_tuning_error_recoverable(ret, host->last_irq,
						 host->last_idst))
			goto fail;
		/* A bad command-sampling tap can corrupt the CMD12 response too.
		 * Record this separately; it cannot make a tap pass. Subsequent
		 * trials remain read-only and must complete with a fresh pattern.
		 */
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto fail;
		}
		if (mmc_send_abort_tuning(mmc, opcode))
			abort_failed |= 1ULL << tap;
		if (host->dma_dead || host->ios_error ||
		    ((host->last_irq & T736_IRQ_ERRORS) &
		     ~T736_TUNING_CARD_ERRORS))
			goto fail;
	}
	ret = t736_tuning_select(passing, &window);
	if (ret)
		goto fail;
	ret = t736_tuning_set_tap(host, window.tap);
	if (ret)
		goto fail;
	for (trial = 0; trial < 16; trial++) {
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			goto fail;
		}
		ret = mmc_send_tuning(mmc, opcode, NULL);
		if (ret)
			goto fail;
	}
	host->tuned_hz = mmc->actual_clock;
	host->tuned_tap = window.tap;
	host->tuning_valid = true;
	host->tuning_running = false;
	dev_info(mmc_dev(mmc),
		"HS200 tuning card=%u pass=%016llx abort_err=%016llx window=%u+%u tap=%u verified=16x128 THLD=%08x SFC=%08x\n",
		mmc->actual_clock, passing, abort_failed, window.first, window.count, window.tap,
		readl(host->regs + CARD_THRESHOLD), readl(host->regs + SAMPLE_FIFO));
	return 0;
fail:
	failed_irq = host->last_irq;
	failed_idst = host->last_idst;
	if (!host->dma_dead) {
		abort_ret = mmc_send_abort_tuning(mmc, opcode);
		stop_ret = t736_dma_stop(host);
		if (!host->dma_dead) {
			writel(BIT(31), host->regs + CLKDIV);
			clock_ret = t736_update_clock(host);
		}
	}
	host->tuning_running = false;
	host->tuning_valid = false;
	host->ios_error = ret ? ret : -EIO;
	dev_err(mmc_dev(mmc),
		"HS200 tuning failed=%d pass=%016llx abort_err=%016llx IRQ=%08x IDST=%08x cleanup_abort=%d dma_stop=%d clock_stop=%d dma_dead=%u; no valid data point, use accepted boot fallback\n",
		host->ios_error, passing, abort_failed, failed_irq, failed_idst,
		abort_ret, stop_ret, clock_ret, host->dma_dead);
	return host->ios_error;
}

static const struct mmc_host_ops t736_ops = {
	.request = t736_request,
	.set_ios = t736_set_ios,
	.start_signal_voltage_switch = t736_signal_voltage,
	.execute_tuning = t736_execute_tuning,
};

static int t736_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct t736_host *host;
	struct pinctrl *pinctrl;
	u32 value, sys24, peri800;
	int ret, i;

	if (hs_max_hz != 25000000 && hs_max_hz != 50000000)
		return -EINVAL;
	if (hs200_max_hz != 100000000 && hs200_max_hz != 200000000)
		return -EINVAL;
	/* Claim the PC pins before touching the storage controller. */
	pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
	if (IS_ERR(pinctrl))
		return dev_err_probe(&pdev->dev, PTR_ERR(pinctrl),
				     "eMMC pinctrl unavailable\n");

	mmc = mmc_alloc_host(sizeof(*host), &pdev->dev);
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->mmc = mmc;
	INIT_WORK(&host->work, t736_worker);
	/* Keep descriptor and data DMA addresses below 4 GiB even on 8 GiB RAM. */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto free_host;
	host->buffer = dma_alloc_coherent(&pdev->dev, MAX_REQUEST_BYTES,
					 &host->buffer_dma, GFP_KERNEL);
	if (!host->buffer) {
		ret = -ENOMEM;
		goto free_host;
	}
	host->desc = dma_alloc_coherent(&pdev->dev,
				       sizeof(*host->desc) * T736_DMA_DESCS,
				       &host->desc_dma, GFP_KERNEL);
	if (!host->desc) {
		ret = -ENOMEM;
		goto free_buffer;
	}
	/* Ordered workqueues are unbound and serialize requests, including one
	 * submitted synchronously by mmc_request_done() before this work returns.
	 * The reclaim rescuer keeps root-filesystem I/O available under pressure.
	 */
	host->workqueue = alloc_ordered_workqueue("t736-mmc-dma", WQ_MEM_RECLAIM);
	if (!host->workqueue) {
		ret = -ENOMEM;
		goto free_desc;
	}
	host->regs = devm_platform_ioremap_resource(pdev, 0);
	host->clock = devm_platform_ioremap_resource(pdev, 1);
	host->gate = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(host->regs) || IS_ERR(host->clock) || IS_ERR(host->gate)) {
		ret = -ENODEV;
		goto destroy_workqueue;
	}
	ret = t736_check_dma_bus(pdev);
	if (ret)
		goto destroy_workqueue;
	ret = t736_read_clock_parents(pdev, host);
	if (ret)
		goto destroy_workqueue;
	ret = t736_validate_clock_parents(host, true, &sys24, &peri800);
	if (ret)
		goto destroy_workqueue;
	/* Power rails, pad drive and pulls remain configured by board firmware. */
	writel(BIT(31), host->clock);
	writel(readl(host->gate) & ~BIT(16), host->gate);
	udelay(10);
	writel(readl(host->gate) | BIT(16) | BIT(0), host->gate);
	writel(7, host->regs + CTRL);
	ret = readl_poll_timeout(host->regs + CTRL, value, !(value & 7), 10, 100000);
	if (ret)
		goto destroy_workqueue;
	for (i = 0; i < ARRAY_SIZE(timing_offsets); i++)
		host->legacy_timing[i] = readl(host->regs + timing_offsets[i]);
	host->legacy_threshold = readl(host->regs + CARD_THRESHOLD);
	/* v5p6x uses word-addressed DLBA and descriptor pointers. */
	value = readl(host->regs + SMCV) & 0xffffff;
	if (value < 0x50600 || value >= 0x50700) {
		dev_err(&pdev->dev, "SMHC version %06x is not audited v5p6x\n", value);
		ret = -ENODEV;
		goto destroy_workqueue;
	}
	writel(0, host->regs + CTRL);
	ret = t736_dma_stop(host);
	if (ret)
		goto destroy_workqueue;
	dev_info(&pdev->dev, "SMHC %06x: IDMA descriptors=%pad data=%pad (shift2, mask32)\n",
		 value, &host->desc_dma, &host->buffer_dma);
	writel(0, host->regs + IRQMASK);
	writel(~0U, host->regs + IRQSTATUS);
	writel(~0U, host->regs + TIMEOUT);
	writel(0, host->regs + CARDRESET);
	udelay(10);
	writel(1, host->regs + CARDRESET);
	usleep_range(1000, 2000);
	mmc->ops = &t736_ops;
	mmc->f_min = 400000;
	mmc->f_max = enable_hs200 ? hs200_max_hz : hs_max_hz;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_NONREMOVABLE | MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA | MMC_CAP_MMC_HIGHSPEED;
	mmc->caps2 = MMC_CAP2_NO_SD | MMC_CAP2_NO_SDIO | MMC_CAP2_AVOID_3_3V;
	if (enable_hs200)
		mmc->caps2 |= MMC_CAP2_HS200_1_8V_SDR;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = MAX_REQUEST_BYTES / 512;
	mmc->max_req_size = MAX_REQUEST_BYTES;
	mmc->max_seg_size = MAX_REQUEST_BYTES;
	mmc->max_segs = MAX_REQUEST_BYTES / 512;
	platform_set_drvdata(pdev, mmc);
	ret = mmc_add_host(mmc);
	if (!ret) {
		dev_info(&pdev->dev,
			 "HS200 v23 draft: v21 IDMA, HS cap=%u, HS200=%u cap=%u, fixed1.8V; no DDR/HS400\n",
			 hs_max_hz, enable_hs200, hs200_max_hz);
		return 0;
	}
destroy_workqueue:
	destroy_workqueue(host->workqueue);
free_desc:
	if (!host->dma_dead)
		dma_free_coherent(&pdev->dev, sizeof(*host->desc) * T736_DMA_DESCS,
				  host->desc, host->desc_dma);
free_buffer:
	if (!host->dma_dead)
		dma_free_coherent(&pdev->dev, MAX_REQUEST_BYTES, host->buffer, host->buffer_dma);
free_host:
	mmc_free_host(mmc);
	return ret;
}

static void t736_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct t736_host *host = mmc_priv(mmc);

	mmc_remove_host(mmc);
	cancel_work_sync(&host->work);
	destroy_workqueue(host->workqueue);
	if (!host->dma_dead && !t736_dma_stop(host)) {
		dma_free_coherent(&pdev->dev, sizeof(*host->desc) * T736_DMA_DESCS,
				  host->desc, host->desc_dma);
		dma_free_coherent(&pdev->dev, MAX_REQUEST_BYTES, host->buffer, host->buffer_dma);
	}
	mmc_free_host(mmc);
}

static const struct of_device_id t736_match[] = {
	{ .compatible = "allwinner,t736-smhc3-pio" },
	{ }
};
MODULE_DEVICE_TABLE(of, t736_match);

static struct platform_driver t736_driver = {
	.probe = t736_probe,
	.remove = t736_remove,
	.driver = { .name = "t736-smhc3-pio", .of_match_table = t736_match },
};
module_platform_driver(t736_driver);
MODULE_DESCRIPTION("T736 SMHC3 HS SDR draft: 25/50 MHz verified dividers, fixed1.8V, v19 IDMA");
MODULE_LICENSE("GPL");
