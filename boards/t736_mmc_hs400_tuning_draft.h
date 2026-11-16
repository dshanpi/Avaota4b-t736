/* SPDX-License-Identifier: GPL-2.0-only */
/* HS400 read-only reference and response checks; no first-read golden data. */
#ifndef T736_MMC_HS400_TUNING_DRAFT_H
#define T736_MMC_HS400_TUNING_DRAFT_H

/* Same26 read-only bytes as Linux mmc_compare_ext_csds(), in the same order. */
static const unsigned int t736_hs400_ro_offsets[] = {
	160, 181, 192, 194, 196, 217, 221, 223, 224, 229, 230, 231, 232,
	212, 213, 214, 215, 200, 201, 202, 203, 236, 237, 238, 239, 253
};

struct t736_hs400_reference {
	u8 bytes[26];
	u8 hs_timing;
};

static inline bool t736_hs400_extcsd_matches(const struct t736_hs400_reference *ref,
					  const u8 *ext)
{
	unsigned int i;

	/* EXT_CSD HS_TIMING185 and BUS_WIDTH183 prove actual non-ES HS400. */
	if (ext[185] != ref->hs_timing || ext[183] != 6)
		return false;
	for (i = 0; i < 26; i++)
		if (ext[t736_hs400_ro_offsets[i]] != ref->bytes[i])
			return false;
	return true;
}

static inline bool t736_hs400_status_ok(u32 status)
{
	/* R1_STATUS omits SWITCH_ERROR bit7: check it separately. */
	return !R1_STATUS(status) && !(status & R1_SWITCH_ERROR) &&
	       (status & R1_READY_FOR_DATA) && R1_CURRENT_STATE(status) == R1_STATE_TRAN;
}

static inline bool t736_hs400_command_error_recoverable(int error, u32 irq)
{
	u32 response_errors = BIT(1) | BIT(6) | BIT(8);

	/* No data transfer accompanies CMD13, so data/FIFO errors are fatal. */
	if ((irq & T736_IRQ_ERRORS) & ~response_errors)
		return false;
	return (error == -EILSEQ && (irq & response_errors)) || error == -ETIMEDOUT;
}

#endif
