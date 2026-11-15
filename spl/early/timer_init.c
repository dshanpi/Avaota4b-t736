// SPDX-License-Identifier: GPL-2.0-only
#include "timer_init.h"

int t736_timestamp_prepare(const struct t736_timer_io *io,
			   struct t736_timer_state *s, uint32_t limit)
{
	uint32_t i, begin;
	if (!io || !s || !io->read32 || !io->write32 || !io->counter || !io->cntfrq ||
	    !limit || limit > 1000000u)
		return -1;
	s->effective_hz = 0;
	s->advanced_before = 0;
	s->ctrl_before = s->ctrl_after = io->read32(io->context, T736_TIMESTAMP_CTRL);
	s->freqid_before = s->freqid_after = io->read32(io->context, T736_TIMESTAMP_FREQID);
	s->raw_cntfrq = io->cntfrq(io->context);
	s->counter_before = io->counter(io->context);
	/* This is an observation, not a timing loop; CNTFRQ=0 is not a dead timer. */
	for (i = 0; i < 128u; i++)
		if (io->counter(io->context) != s->counter_before) {
			s->advanced_before = 1;
			break;
		}
	if ((s->raw_cntfrq && s->raw_cntfrq != T736_TIMESTAMP_HZ) ||
	    (s->freqid_before && s->freqid_before != T736_TIMESTAMP_HZ))
		return -2;
	if (!s->freqid_before) {
		io->write32(io->context, T736_TIMESTAMP_FREQID, T736_TIMESTAMP_HZ);
		s->freqid_after = io->read32(io->context, T736_TIMESTAMP_FREQID);
		if (s->freqid_after != T736_TIMESTAMP_HZ)
			return -3;
	}
	if (!(s->ctrl_before & 1u)) {
		io->write32(io->context, T736_TIMESTAMP_CTRL, s->ctrl_before | 1u);
		s->ctrl_after = io->read32(io->context, T736_TIMESTAMP_CTRL);
		if (!(s->ctrl_after & 1u))
			return -3;
	}
	begin = io->counter(io->context);
	for (i = 0; i < limit; i++)
		if (io->counter(io->context) != begin) {
			/* Documented fixed hardware rate; not an oscillator measurement. */
			s->effective_hz = T736_TIMESTAMP_HZ;
			return 0;
		}
	return -4;
}
