/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_SPL_EMMC_H
#define T736_SPL_EMMC_H
#include <stdint.h>

enum t736_emmc_error {
	T736_EMMC_INVALID = -1,
	T736_EMMC_TIMEOUT = -2,
	T736_EMMC_IO = -3,
	T736_EMMC_UNSUPPORTED = -4,
	T736_EMMC_TIMER = -5,
};

struct t736_emmc_io {
	void *ctx;
	uint32_t (*read32)(void *ctx, uint32_t address);
	void (*write32)(void *ctx, uint32_t address, uint32_t value);
	/* Return 0 on elapsed delay, nonzero if the timer is unavailable/stalled. */
	int (*delay_us)(void *ctx, uint32_t usec);
};

struct t736_emmc {
	struct t736_emmc_io io;
	uint32_t sectors;
	uint32_t last_irq;
	uint32_t last_r1;
	uint8_t initialized;
};

/*
 * SMHC3 protocol/controller only. Caller MUST first establish the actual board's
 * VCC/VCCQ supplies, PC pad-voltage selection, pinmux/drive and CCU bus reset.
 * No claim that these cold-start prerequisites have yet been implemented.
 * Resets host/card to 1-bit SDR, negotiates sector-addressed eMMC, and checks
 * EXT_CSD user-area selection/capacity. Does not write storage or EXT_CSD.
 * workspace is 512 caller-owned bytes in SRAM; stack usage is bounded.
 */
int t736_emmc_open(struct t736_emmc *card, const struct t736_emmc_io *io,
		   uint8_t workspace[512]);
/* Exact one-sector PIO read; no arbitrary command/write API exposed. */
int t736_emmc_read(struct t736_emmc *card, uint32_t sector, uint8_t dest[512]);
#endif
