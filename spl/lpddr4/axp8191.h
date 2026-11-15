/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_AXP8191_H
#define T736_LP4_AXP8191_H

#include <stdint.h>

#define T736_AXP8191_I2C_ADDRESS 0x36u
enum t736_axp_rail { T736_AXP_DCDC8, T736_AXP_ALDO4, T736_AXP_ELDO1 };
enum t736_axp_error {
	T736_AXP_OK = 0,
	T736_AXP_INVALID = -1,
	T736_AXP_RANGE = -2,
	T736_AXP_IO = -3,
	T736_AXP_ID = -4,
	T736_AXP_VERIFY = -5,
	T736_AXP_LIVE_RANGE_CHANGE = -6,
};

/* Callbacks perform one register access at fixed I2C address 0x36. */
struct t736_axp_bus {
	void *ctx;
	int (*read8)(void *ctx, uint8_t reg, uint8_t *value);
	int (*write8)(void *ctx, uint8_t reg, uint8_t value);
};

int t736_axp_encode_mv(enum t736_axp_rail rail, unsigned int mv, uint8_t *code);
int t736_axp_decode_mv(enum t736_axp_rail rail, uint8_t code, unsigned int *mv);
/* Explicit rail operation, never a whole-board power preset. */
int t736_axp_set_rail(const struct t736_axp_bus *bus, enum t736_axp_rail rail,
		    unsigned int mv);

#endif
