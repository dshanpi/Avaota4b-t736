/* SPDX-License-Identifier: GPL-2.0-only */
/* CMD21-only recovery classification; normal data error handling is unchanged. */
#ifndef T736_MMC_TUNING_ERROR_DRAFT_H
#define T736_MMC_TUNING_ERROR_DRAFT_H

/* A733 SMHC3 11.3.9.28: IDST[5] is Card Error Summary, the OR of
 * RINT EBE/RTO/RCRC/SBE/DRTO/DCRC/RE. It is not a DMA engine failure.
 */
#define T736_TUNING_CARD_ERRORS (BIT(15) | BIT(13) | BIT(9) | BIT(8) | \
				 BIT(7) | BIT(6) | BIT(1))
#define T736_TUNING_IDST_ALLOWED (BIT(8) | BIT(5) | BIT(1))

static inline bool t736_tuning_idst_invalid(u32 idst)
{
	return idst & ~T736_TUNING_IDST_ALLOWED;
}

static inline bool t736_tuning_error_recoverable(int error, u32 irq, u32 idst)
{
	u32 card_errors = irq & T736_TUNING_CARD_ERRORS;

	/* Descriptor unavailable, unexpected TX, reserved/fatal-bus bits and
	 * every unknown IDST bit fail closed. No new interrupt bits are enabled.
	 */
	if (t736_tuning_idst_invalid(idst))
		return false;
	/* FIFO/illegal-command/starvation/write-busy errors are not bad taps. */
	if ((irq & T736_IRQ_ERRORS) & ~T736_TUNING_CARD_ERRORS)
		return false;
	/* A summary without its matching raw card error is not explained. */
	if ((idst & BIT(5)) && !card_errors)
		return false;
	if (error == -EILSEQ)
		return card_errors != 0;
	if (error == -ETIMEDOUT)
		return true; /* Includes a software timeout with no latched RINT. */
	/* mmc_send_tuning returns EIO for a complete but mismatched pattern.
	 * Require both controller completions and RX completion before treating
	 * that as a bad tap. The command path already required all OWN clear.
	 */
	if (error == -EIO)
		return !card_errors && !(idst & BIT(5)) &&
		       (irq & (BIT(2) | BIT(3))) == (BIT(2) | BIT(3)) &&
		       (idst & BIT(1));
	return false;
}

#endif
