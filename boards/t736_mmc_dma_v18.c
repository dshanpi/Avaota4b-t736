// SPDX-License-Identifier: GPL-2.0-only
/* T736 SMHC3 DMA v18 candidate. Keep v16 clock, voltage and timing.
 * Coherent low-4-GiB bounce/descriptors, v5p6x word-addressed IDMA.
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mmc/host.h>
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
#define TIMING 0x5c
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
#define CMD_START BIT(31)
#define MAX_REQUEST_BYTES T736_DMA_BYTES

struct t736_host {
	struct mmc_host *mmc;
	void __iomem *regs, *clock, *gate;
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

static void t736_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct t736_host *host = mmc_priv(mmc);
	u32 divider, value;

	if (host->dma_dead) {
		host->ios_error = -EIO;
		return;
	}
	writel(BIT(31), host->regs + CLKDIV);
	host->ios_error = t736_update_clock(host);
	if (host->ios_error || !ios->clock)
		return;
	divider = DIV_ROUND_UP(24000000U, 2 * ios->clock);
	if (!divider || divider > 32) {
		host->ios_error = -EINVAL;
		return;
	}
	writel(BIT(31) | (divider - 1), host->clock);
	value = readl(host->regs + TIMING);
	writel(value | BIT(31), host->regs + TIMING);
	writel(ios->bus_width == MMC_BUS_WIDTH_8 ? 2 :
	       ios->bus_width == MMC_BUS_WIDTH_4 ? 1 : 0, host->regs + WIDTH);
	writel(BIT(31) | BIT(16), host->regs + CLKDIV);
	host->ios_error = t736_update_clock(host);
	writel(BIT(16), host->regs + CLKDIV);
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
	/* Polling worker owns all status. Neither IRQ output is enabled. */
	writel(0, host->regs + IDIE);
	/* SMHC3 DMAC bit1 is reserved; only bit7 enables this engine. */
	writel(DMA_ENABLE, host->regs + DMAC);
	return 0;
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
	deadline = jiffies + msecs_to_jiffies(2000);
	for (;;) {
		status = readl(host->regs + IRQSTATUS);
		dma_status = data ? readl(host->regs + IDST) : 0;
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
	if (ret)
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

static const struct mmc_host_ops t736_ops = {
	.request = t736_request,
	.set_ios = t736_set_ios,
};

static int t736_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct t736_host *host;
	struct pinctrl *pinctrl;
	u32 value;
	int ret;

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
	/* Power rails, pad drive and pulls remain configured by board firmware. */
	writel(BIT(31), host->clock);
	writel(readl(host->gate) & ~BIT(16), host->gate);
	udelay(10);
	writel(readl(host->gate) | BIT(16) | BIT(0), host->gate);
	writel(7, host->regs + CTRL);
	ret = readl_poll_timeout(host->regs + CTRL, value, !(value & 7), 10, 100000);
	if (ret)
		goto destroy_workqueue;
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
	mmc->f_max = 12000000;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->caps = MMC_CAP_NONREMOVABLE | MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA;
	mmc->caps2 = MMC_CAP2_NO_SD | MMC_CAP2_NO_SDIO;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = MAX_REQUEST_BYTES / 512;
	mmc->max_req_size = MAX_REQUEST_BYTES;
	mmc->max_seg_size = MAX_REQUEST_BYTES;
	mmc->max_segs = MAX_REQUEST_BYTES / 512;
	platform_set_drvdata(pdev, mmc);
	ret = mmc_add_host(mmc);
	if (!ret) {
		dev_info(&pdev->dev,
			 "DMA v18: 64 KiB coherent bounce, 12 MHz legacy, 8-bit, polling worker\n");
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
MODULE_DESCRIPTION("T736 SMHC3 DMA v18: coherent 64 KiB, word-addressed IDMA, 12 MHz");
MODULE_LICENSE("GPL");
