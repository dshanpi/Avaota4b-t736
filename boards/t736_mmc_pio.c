// SPDX-License-Identifier: GPL-2.0-only
/* Initial T736 SMHC3 port. PIO avoids the unvalidated DMA descriptor ABI. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
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
#define TIMING 0x5c
#define CARDRESET 0x78
#define FIFO 0x200
#define CMD_START BIT(31)
#define IRQ_ERRORS 0xbfc2

struct t736_host {
	struct mmc_host *mmc;
	void __iomem *regs, *clock, *gate, *pins;
	struct work_struct work;
	struct mmc_request *request;
	int ios_error;
	u32 buffer[1024];
};

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

static int t736_command(struct t736_host *host, struct mmc_command *cmd,
			struct mmc_data *data)
{
	u32 flags = CMD_START | cmd->opcode, value, status;
	unsigned int count = 0, words = 0;
	unsigned long deadline;
	bool write = data && (data->flags & MMC_DATA_WRITE);
	bool auto_stop = data && data->stop;
	int ret, i;

	if (data) {
		unsigned int bytes = data->blksz * data->blocks;

		if (!data->blocks || bytes > sizeof(host->buffer) || bytes % 4)
			return -EINVAL;
		words = bytes / 4;
		if (write && sg_copy_to_buffer(data->sg, data->sg_len,
					     host->buffer, bytes) != bytes)
			return -EINVAL;
		writel(readl(host->regs + CTRL) | BIT(1), host->regs + CTRL);
		ret = readl_poll_timeout(host->regs + CTRL, value,
					!(value & BIT(1)), 10, 100000);
		if (ret)
			return ret;
		writel(data->blksz, host->regs + BLOCKSIZE);
		writel(bytes, host->regs + BYTECOUNT);
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
		if (status & IRQ_ERRORS) {
			dev_dbg(mmc_dev(host->mmc), "CMD%u status %08x\n", cmd->opcode, status);
			return status & (BIT(8) | BIT(9)) ? -ETIMEDOUT : -EILSEQ;
		}
		if (count < words) {
			value = readl(host->regs + STATUS);
			if (write && !(value & BIT(3)))
				writel(host->buffer[count++], host->regs + FIFO);
			else if (!write && !(value & BIT(2)))
				host->buffer[count++] = readl(host->regs + FIFO);
		}
		if (count == words && (status & BIT(2)) &&
		    (!data || (status & (auto_stop ? BIT(14) : BIT(3)))))
			break;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		cond_resched();
	}
	if (cmd->flags & MMC_RSP_136)
		for (i = 0; i < 4; i++)
			cmd->resp[i] = readl(host->regs + RESPONSE + (3 - i) * 4);
	else
		cmd->resp[0] = readl(host->regs + RESPONSE);
	if (cmd->flags & MMC_RSP_BUSY) {
		ret = readl_poll_timeout(host->regs + STATUS, value,
					!(value & BIT(9)), 10, 2000000);
		if (ret)
			return ret;
	}
	if (data && !write && sg_copy_from_buffer(data->sg, data->sg_len,
						host->buffer, words * 4) != words * 4)
		return -EINVAL;
	writel(~0U, host->regs + IRQSTATUS);
	return 0;
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
	schedule_work(&host->work);
}

static const struct mmc_host_ops t736_ops = {
	.request = t736_request,
	.set_ios = t736_set_ios,
};

static int t736_probe(struct platform_device *pdev)
{
	static const u8 pins[] = { 0, 1, 5, 6, 8, 9, 10, 11, 13, 14, 15, 16 };
	struct mmc_host *mmc;
	struct t736_host *host;
	u32 value, shift;
	int i, ret;

	mmc = mmc_alloc_host(sizeof(*host), &pdev->dev);
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->mmc = mmc;
	INIT_WORK(&host->work, t736_worker);
	host->regs = devm_platform_ioremap_resource(pdev, 0);
	host->clock = devm_platform_ioremap_resource(pdev, 1);
	host->gate = devm_platform_ioremap_resource(pdev, 2);
	host->pins = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(host->regs) || IS_ERR(host->clock) || IS_ERR(host->gate) || IS_ERR(host->pins)) {
		ret = -ENODEV;
		goto free_host;
	}
	/* Power rails and pad drive remain configured by board firmware. */
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		shift = (pins[i] % 8) * 4;
		value = readl(host->pins + (pins[i] / 8) * 4);
		writel((value & ~(0xfU << shift)) | (4U << shift),
		       host->pins + (pins[i] / 8) * 4);
	}
	writel(BIT(31), host->clock);
	writel(readl(host->gate) & ~BIT(16), host->gate);
	udelay(10);
	writel(readl(host->gate) | BIT(16) | BIT(0), host->gate);
	writel(7, host->regs + CTRL);
	ret = readl_poll_timeout(host->regs + CTRL, value, !(value & 7), 10, 100000);
	if (ret)
		goto free_host;
	writel(BIT(31), host->regs + CTRL);
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
	mmc->max_blk_count = 8;
	mmc->max_req_size = sizeof(host->buffer);
	mmc->max_seg_size = sizeof(host->buffer);
	mmc->max_segs = 8;
	platform_set_drvdata(pdev, mmc);
	ret = mmc_add_host(mmc);
	if (!ret)
		return 0;
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
MODULE_DESCRIPTION("T736 SMHC3 initial PIO driver");
MODULE_LICENSE("GPL");
