/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_EARLY_TWI_H
#define T736_EARLY_TWI_H
#include <stdint.h>

enum {
	TWI_DATA = 0x08, TWI_CTRL = 0x0c, TWI_STATUS = 0x10,
	TWI_CLOCK = 0x14, TWI_RESET = 0x18, TWI_EFR = 0x1c,
	TWI_LINE = 0x20, TWI_DRV_CTRL = 0x200, TWI_PMU_CTRL = 0x314,
	TWI_ACK = 4, TWI_IF = 8, TWI_STOP = 16, TWI_START = 32, TWI_EN = 64
};
enum t736_twi_error {
	TWI_OK = 0, TWI_TIMEOUT = -1, TWI_NACK = -2,
	TWI_ARBITRATION = -3, TWI_BUS_ERROR = -4, TWI_UNEXPECTED = -5,
	TWI_BUSY = -6, TWI_INVALID = -7
};
struct t736_twi {
	void *ctx;
	uint32_t (*read)(void *ctx, uint32_t offset);
	void (*write)(void *ctx, uint32_t offset, uint32_t value);
	uint32_t (*ticks)(void *ctx);
	uint32_t timeout_ticks, poll_limit;
	uint32_t last_status;
	int cleanup_error;
};
int t736_twi_init(struct t736_twi *bus);
int t736_twi_read8(struct t736_twi *bus, uint8_t address, uint8_t reg, uint8_t *value);
int t736_twi_write8(struct t736_twi *bus, uint8_t address, uint8_t reg, uint8_t value);
#endif
