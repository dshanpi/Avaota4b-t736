/* SPDX-License-Identifier: GPL-2.0-only */
#include "mode_register.h"
#include "controller.h"

#define MR_REQUEST UINT32_C(0x80000000)
#define MR_READ UINT32_C(1)
#define MR_BUSY UINT32_C(1)
#define MR_SELECT_MASK UINT32_C(0x31)
#define CTL_NORMAL UINT32_C(1)
#define CTL_MODE_MASK UINT32_C(7)

int t736_lp4_mrr_read_id(const struct t736_lp4_mrr_io *io,
		       unsigned int channel, unsigned int rank,
		       unsigned int mr, uint32_t timeout_us,
		       struct t736_lp4_mrr_result *result)
{
	uint32_t base, value, control, busy, select, raw, elapsed = 0;

	if (!io || !io->read32 || !io->write32 || !io->delay_us || !result ||
	    channel > 1u || rank > 1u || (mr != 5u && mr != 6u && mr != 8u) ||
	    timeout_us == 0u || timeout_us > T736_LP4_MRR_MAX_WAIT_US)
		return T736_LP4_MRR_INVALID;
	base = T736_CTL0_BASE + (uint32_t)channel * T736_CHANNEL_STRIDE;
	if (io->read32(io->ctx, base + T736_CTL_MSTR, &value))
		return T736_LP4_MRR_IO;
	if ((value & T736_MSTR_TYPE_MASK) != T736_MSTR_LPDDR4 ||
	    !(value & (UINT32_C(1) << (24u + rank))))
		return T736_LP4_MRR_UNSUPPORTED;
	if (io->read32(io->ctx, base + T736_CTL_STAT, &value))
		return T736_LP4_MRR_IO;
	if ((value & CTL_MODE_MASK) != CTL_NORMAL)
		return T736_LP4_MRR_BUSY;
	if (io->read32(io->ctx, base + T736_LP4_MRR_STAT, &busy) ||
	    io->read32(io->ctx, base + T736_LP4_MRR_CTRL0, &control))
		return T736_LP4_MRR_IO;
	if ((busy & MR_BUSY) || (control & MR_REQUEST))
		return T736_LP4_MRR_BUSY;

	select = (UINT32_C(1) << (4u + rank)) | MR_READ;
	if (io->write32(io->ctx, base + T736_LP4_MRR_CTRL0, select) ||
	    io->write32(io->ctx, base + T736_LP4_MRR_CTRL1, (uint32_t)mr << 8) ||
	    io->write32(io->ctx, base + T736_LP4_MRR_CTRL0, select | MR_REQUEST))
		return T736_LP4_MRR_IO;

	for (;;) {
		if (io->read32(io->ctx, base + T736_LP4_MRR_CTRL0, &control) ||
		    io->read32(io->ctx, base + T736_LP4_MRR_STAT, &busy))
			return T736_LP4_MRR_IO;
		if ((control & MR_SELECT_MASK) != select)
			return T736_LP4_MRR_VERIFY;
		if (!(control & MR_REQUEST) && !(busy & MR_BUSY))
			break;
		if (elapsed == timeout_us)
			return T736_LP4_MRR_TIMEOUT;
		if (io->delay_us(io->ctx, 1u))
			return T736_LP4_MRR_IO;
		++elapsed;
	}
	if (io->read32(io->ctx, base + T736_CTL_STAT, &value))
		return T736_LP4_MRR_IO;
	if ((value & CTL_MODE_MASK) != CTL_NORMAL)
		return T736_LP4_MRR_VERIFY;
	if (io->read32(io->ctx, channel == 0u ? T736_LP4_MRR_RESULT_CH0 :
					T736_LP4_MRR_RESULT_CH1, &raw))
		return T736_LP4_MRR_IO;
	result->raw = raw;
	result->value = (uint8_t)(channel == 0u ? (raw >> 8) & 0xffu : raw & 0xffu);
	return T736_LP4_MRR_OK;
}
