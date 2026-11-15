// SPDX-License-Identifier: GPL-2.0-only
/* T736 manual 0.90, SMHC3 chapter; project t736_mmc.c PIO sequence. */
#include "emmc.h"
#include <stddef.h>

#define BASE UINT32_C(0x04023000)
#define MODULE_CLK UINT32_C(0x02002d30)
#define BIT(n) (UINT32_C(1) << (n))
#define CTRL 0x00u
#define CLKDIV 0x04u
#define TIMEOUT 0x08u
#define WIDTH 0x0cu
#define BLOCKSIZE 0x10u
#define BYTECOUNT 0x14u
#define COMMAND 0x18u
#define ARGUMENT 0x1cu
#define RESPONSE 0x20u
#define IRQMASK 0x30u
#define IRQSTATUS 0x38u
#define STATUS 0x3cu
#define TIMING 0x5cu
#define CARDRESET 0x78u
#define FIFO 0x200u
#define IRQ_ERRORS UINT32_C(0xbfc2)
/* R1 errors: OUT_OF_RANGE through ERROR, plus CID/CSD overwrite,
 * WP erase skip, erase reset, switch error and AKE sequence error. */
#define R1_ERRORS UINT32_C(0xfff9a088)
#define RESP BIT(6)
#define LONG_RESP BIT(7)
#define CRC BIT(8)
#define LIMIT_US 1000000u

static uint32_t rd(struct t736_emmc *c, uint32_t offset)
{
	return c->io.read32(c->io.ctx, BASE + offset);
}

static void wr(struct t736_emmc *c, uint32_t offset, uint32_t value)
{
	c->io.write32(c->io.ctx, BASE + offset, value);
}

static int clear_wait(struct t736_emmc *c, uint32_t offset, uint32_t mask)
{
	uint32_t n;
	for (n = 0; n < LIMIT_US; n++) {
		if (!(rd(c, offset) & mask))
			return 0;
		if (c->io.delay_us(c->io.ctx, 1))
			return T736_EMMC_TIMER;
	}
	return T736_EMMC_TIMEOUT;
}

static int update_clock(struct t736_emmc *c)
{
	int ret;
	wr(c, COMMAND, BIT(31) | BIT(21) | BIT(13));
	ret = clear_wait(c, COMMAND, BIT(31));
	c->last_irq = rd(c, IRQSTATUS);
	if (!ret && (c->last_irq & IRQ_ERRORS))
		ret = T736_EMMC_IO;
	wr(c, IRQSTATUS, UINT32_MAX);
	return ret;
}

/* Fixed oscillator, 400 kHz identification or 12 MHz SDR transfer. */
static int set_clock(struct t736_emmc *c, uint32_t divisor)
{
	int ret;
	wr(c, CLKDIV, BIT(31));
	ret = update_clock(c);
	if (ret)
		return ret;
	c->io.write32(c->io.ctx, MODULE_CLK, BIT(31) | (divisor - 1u));
	wr(c, TIMING, rd(c, TIMING) | BIT(31));
	wr(c, CLKDIV, BIT(31) | BIT(16));
	ret = update_clock(c);
	if (!ret)
		wr(c, CLKDIV, BIT(16));
	return ret;
}

static int command(struct t736_emmc *c, uint32_t index, uint32_t arg,
		   uint32_t response_flags, int check_r1, int busy, uint8_t *data)
{
	uint32_t flags = BIT(31) | index | response_flags;
	uint32_t n, got = 0;
	int ret, checked_r1 = 0;
	c->last_irq = 0;
	c->last_r1 = 0;
	if (data) {
		wr(c, CTRL, rd(c, CTRL) | BIT(1));
		ret = clear_wait(c, CTRL, BIT(1));
		if (ret)
			return ret;
		wr(c, BLOCKSIZE, 512);
		wr(c, BYTECOUNT, 512);
		flags |= BIT(9) | BIT(13);
	}
	if (!index)
		flags |= BIT(15);
	wr(c, IRQSTATUS, UINT32_MAX);
	wr(c, ARGUMENT, arg);
	wr(c, COMMAND, flags);
	for (n = 0; n < LIMIT_US; n++) {
		uint32_t irq = rd(c, IRQSTATUS);
		c->last_irq = irq;
		if (irq & IRQ_ERRORS)
			return irq & (BIT(8) | BIT(9)) ? T736_EMMC_TIMEOUT : T736_EMMC_IO;
		if (check_r1 && !checked_r1 && (irq & BIT(2))) {
			c->last_r1 = rd(c, RESPONSE);
			checked_r1 = 1;
			if (c->last_r1 & R1_ERRORS)
				return T736_EMMC_IO;
		}
		if (data && got < 512u && !(rd(c, STATUS) & BIT(2))) {
			uint32_t word = rd(c, FIFO);
			unsigned int b;
			for (b = 0; b < 4; b++)
				data[got++] = (uint8_t)(word >> (b * 8u));
		}
		if ((irq & BIT(2)) && (!data || (got == 512u && (irq & BIT(3)))))
			break;
		if (c->io.delay_us(c->io.ctx, 1))
			return T736_EMMC_TIMER;
	}
	if (n == LIMIT_US)
		return T736_EMMC_TIMEOUT;
	if ((response_flags & RESP) && !checked_r1)
		c->last_r1 = rd(c, RESPONSE);
	if (check_r1 && (c->last_r1 & R1_ERRORS))
		return T736_EMMC_IO;
	if (busy) {
		ret = clear_wait(c, STATUS, BIT(9));
		if (ret)
			return ret;
	}
	wr(c, IRQSTATUS, UINT32_MAX);
	return 0;
}

int t736_emmc_open(struct t736_emmc *c, const struct t736_emmc_io *io,
		   uint8_t workspace[512])
{
	unsigned int attempt;
	int ret;
	if (!c)
		return T736_EMMC_INVALID;
	c->initialized = 0;
	c->sectors = 0;
	if (!io || !workspace || !io->read32 || !io->write32 || !io->delay_us)
		return T736_EMMC_INVALID;
	c->io = *io;
	c->last_irq = 0;
	c->last_r1 = 0;
	/* CCU bus clock/reset are caller prerequisites. The module clock must
	 * also run before SMHC internal reset can complete on a cold start. */
	c->io.write32(c->io.ctx, MODULE_CLK, BIT(31) | 29u);
	wr(c, CTRL, 7);
	ret = clear_wait(c, CTRL, 7);
	if (ret)
		return ret;
	wr(c, CTRL, BIT(31));
	wr(c, IRQMASK, 0);
	wr(c, IRQSTATUS, UINT32_MAX);
	wr(c, TIMEOUT, UINT32_MAX);
	wr(c, WIDTH, 0);
	wr(c, CARDRESET, 0);
	if (c->io.delay_us(c->io.ctx, 10))
		return T736_EMMC_TIMER;
	wr(c, CARDRESET, 1);
	if (c->io.delay_us(c->io.ctx, 1000))
		return T736_EMMC_TIMER;
	ret = set_clock(c, 30);
	if (ret)
		return ret;
	ret = command(c, 0, 0, 0, 0, 0, NULL);
	if (ret)
		return ret;
	for (attempt = 0; attempt < 1000; attempt++) {
		/* Host supports sector addressing and the board's 3.3 V VCC range. */
		ret = command(c, 1, UINT32_C(0x40ff8000), RESP, 0, 0, NULL);
		if (ret)
			return ret;
		if (c->last_r1 & BIT(31))
			break;
		if (c->io.delay_us(c->io.ctx, 1000))
			return T736_EMMC_TIMER;
	}
	if (attempt == 1000)
		return T736_EMMC_TIMEOUT;
	if ((c->last_r1 & UINT32_C(0x60000000)) != BIT(30) ||
	    !(c->last_r1 & UINT32_C(0x00ff8000)))
		return T736_EMMC_UNSUPPORTED;
	ret = command(c, 2, 0, RESP | LONG_RESP | CRC, 0, 0, NULL);
	if (!ret)
		ret = command(c, 3, UINT32_C(0x00010000), RESP | CRC, 1, 0, NULL);
	if (!ret)
		ret = command(c, 7, UINT32_C(0x00010000), RESP | CRC, 1, 1, NULL);
	if (!ret)
		ret = command(c, 8, 0, RESP | CRC, 1, 0, workspace);
	if (ret)
		return ret;
	/* EXT_CSD revision >= 2 includes SEC_COUNT. Never change partition access. */
	if (workspace[192] < 2 || (workspace[179] & 7u))
		return T736_EMMC_UNSUPPORTED;
	c->sectors = (uint32_t)workspace[212] | ((uint32_t)workspace[213] << 8) |
		((uint32_t)workspace[214] << 16) | ((uint32_t)workspace[215] << 24);
	if (!c->sectors)
		return T736_EMMC_UNSUPPORTED;
	ret = set_clock(c, 1);
	if (!ret)
		c->initialized = 1;
	return ret;
}

int t736_emmc_read(struct t736_emmc *c, uint32_t sector, uint8_t dest[512])
{
	int ret;
	if (!c || !dest || !c->initialized || sector >= c->sectors)
		return T736_EMMC_INVALID;
	ret = command(c, 17, sector, RESP | CRC, 1, 0, dest);
	/* A failed transfer needs explicit reinitialization before another command. */
	if (ret)
		c->initialized = 0;
	return ret;
}
