/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_CONTROLLER_H
#define T736_LP4_CONTROLLER_H

#include <stdint.h>
#include <stddef.h>

/* sun60iw2 uses a different controller layout/type encoding from H616. */
#define T736_CTL0_BASE UINT32_C(0x0a110000)
#define T736_MAP0_BASE UINT32_C(0x0a130000)
#define T736_CHANNEL_STRIDE UINT32_C(0x00400000)
#define T736_CTL_MSTR 0x000u
#define T736_CTL_STAT 0x014u
#define T736_CTL_SWCTL 0xc80u
#define T736_CTL_SWSTAT 0xc84u
#define T736_MSTR_TYPE_MASK 0x0fu
#define T736_MSTR_LPDDR4 0x02u

enum t736_lp4_error {
	T736_LP4_OK = 0,
	T736_LP4_INVALID = -1,
	T736_LP4_UNSUPPORTED = -2,
	T736_LP4_BUSY = -3,
	T736_LP4_TIMEOUT = -4,
	T736_LP4_VERIFY = -5,
};

struct t736_lp4_geometry {
	uint8_t channels;
	uint8_t ranks_per_channel;
	uint8_t channel_width_bits;
	uint8_t row_bits;
	uint8_t column_bits;
	uint8_t bank_bits;
};

struct t736_lp4_map_word {
	uint32_t offset;
	uint32_t value;
};

#define T736_LP4_MAP_WORDS 10u
struct t736_lp4_layout {
	uint32_t mstr;
	uint64_t capacity_bytes;
	struct t736_lp4_map_word map[T736_LP4_MAP_WORDS];
};

struct t736_lp4_io {
	void *ctx;
	uint32_t (*read32)(void *ctx, uint32_t address);
	void (*write32)(void *ctx, uint32_t address, uint32_t value);
	void (*delay_us)(void *ctx, uint32_t delay);
};

/* Observed profile only: two 16-bit channels, two ranks, 8 GiB total. */
int t736_lp4_build_layout(const struct t736_lp4_geometry *geometry,
			 struct t736_lp4_layout *layout);
int t736_lp4_is_lpddr4(uint32_t mstr);

/*
 * Programs ONLY MSTR/address mapping while both controllers report init mode.
 * Caller must execute in SRAM with DRAM clients stopped and the documented
 * reset/clock prerequisites satisfied. It does not initialize or train DRAM.
 */
int t736_lp4_apply_layout(const struct t736_lp4_io *io,
			 const struct t736_lp4_geometry *geometry);

/* Quasi-dynamic SWCTL transaction; no arbitrary register write interface. */
int t736_lp4_set_update(const struct t736_lp4_io *io, unsigned int channel,
		       int commit, uint32_t timeout_us);

#endif
