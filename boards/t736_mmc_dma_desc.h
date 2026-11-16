/* SPDX-License-Identifier: GPL-2.0-only */
/* T736 SMHC3 v5p6x descriptor ABI. Also compiled by the host boundary tests. */
#ifndef T736_MMC_DMA_DESC_H
#define T736_MMC_DMA_DESC_H

#define T736_DMA_BYTES (64U * 1024)
/* SMHC3 has a 12-bit byte length; zero skips a buffer. Keep chunks at 1 KiB. */
#define T736_DMA_CHUNK 1024U
#define T736_DMA_DESCS (T736_DMA_BYTES / T736_DMA_CHUNK)
#define T736_DESC_LAST BIT(2)
#define T736_DESC_FIRST BIT(3)
#define T736_DESC_OWN BIT(31)
#define T736_IRQ_ERRORS (0xbfc2 | BIT(18))
#define T736_DMA_ERRORS (BIT(4) | BIT(5))

struct t736_dma_desc {
	__le32 flags;
	__le32 length;
	__le32 buffer;
	__le32 next;
};

static inline int t736_dma_describe(struct t736_dma_desc *desc,
				  u64 desc_dma, u64 buffer_dma,
				  unsigned int bytes)
{
	unsigned int count, i, offset = 0;

	if (!bytes || bytes > T736_DMA_BYTES || (bytes & 3) ||
	    (desc_dma & 15) || (buffer_dma & 3) ||
	    desc_dma > 0xffffffffULL - sizeof(*desc) * T736_DMA_DESCS + 1 ||
	    buffer_dma > 0xffffffffULL - bytes + 1)
		return -EINVAL;
	count = DIV_ROUND_UP(bytes, T736_DMA_CHUNK);
	for (i = 0; i < count; i++) {
		u32 length = min(bytes - offset, T736_DMA_CHUNK);
		/* A733 table 11-4 is SMHC3-specific: CH/DIC/ER/CES from
		 * SMHC0-2 are reserved here. Address extension fields stay 0
		 * because the coherent mask keeps both allocations below 4 GiB.
		 */
		u32 flags = T736_DESC_OWN;

		if (!i)
			flags |= T736_DESC_FIRST;
		if (i == count - 1)
			flags |= T736_DESC_LAST;
		desc[i].length = cpu_to_le32(length);
		desc[i].buffer = cpu_to_le32((buffer_dma + offset) >> 2);
		desc[i].next = cpu_to_le32(i == count - 1 ? 0 :
					 (desc_dma + (i + 1) * sizeof(*desc)) >> 2);
		desc[i].flags = cpu_to_le32(flags);
		offset += length;
	}
	return count;
}

/* OWN clear is required for every descriptor, including intermediate ones. */
static inline int t736_dma_descriptors_done(struct t736_dma_desc *desc,
					   unsigned int count)
{
	unsigned int i;
	bool pending = false;

	for (i = 0; i < count; i++) {
		u32 flags = le32_to_cpu(READ_ONCE(desc[i].flags));

		if (flags & T736_DESC_OWN)
			pending = true;
	}
	return pending ? 0 : 1;
}

/* 0: pending, 1: complete, negative: error. Read statuses before calling. */
static inline int t736_dma_poll_result(u32 status, u32 dma_status,
		bool has_data, bool write, bool auto_stop,
		struct t736_dma_desc *desc, unsigned int count)
{
	u32 errors = status & T736_IRQ_ERRORS;

	/* SMHC3 bit13 is a start-bit error for reads, normal busy-clear for
	 * writes. This fixed legacy host never performs voltage switching,
	 * so bit10 is a starvation timeout, not a voltage-switch event.
	 */
	if (write)
		errors &= ~BIT(13);
	if (errors)
		return errors & (BIT(8) | BIT(9) | BIT(10) | BIT(18)) ?
			-ETIMEDOUT : -EILSEQ;
	if (has_data && (dma_status & T736_DMA_ERRORS))
		return -EIO;
	if (!(status & BIT(2)))
		return 0;
	if (!has_data)
		return 1;
	if (!(status & (auto_stop ? BIT(14) : BIT(3))) ||
	    !(dma_status & (write ? BIT(0) : BIT(1))))
		return 0;
	dma_rmb();
	return t736_dma_descriptors_done(desc, count);
}

#endif
