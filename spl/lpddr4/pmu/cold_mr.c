/* SPDX-License-Identifier: GPL-2.0-only */
#include "cold_mr.h"
#include "../controller.h"

#define CTRL0 0x080u
#define CTRL1 0x084u
#define MRSTAT 0x090u
#define DATA0 0x094u
#define DATA1 0x098u
#define PWRCTL 0x180u
#define DFIMISC 0x510u
#define DFISTAT 0x514u
#define REQUEST UINT32_C(0x80000000)
#define CLEAR_DONE UINT32_C(0x01000000)
#define SW_INIT UINT32_C(8)
#define BUSY UINT32_C(1)
#define DONE UINT32_C(0x10000)
#define SELECT_MASK UINT32_C(0x39)
#define SYS_CFG UINT32_C(0x0a020010)
#define CLOCKS UINT32_C(0x0aa80100)
#define MEM_RESET UINT32_C(0x0a9400c0)
#define LP5_MODE UINT32_C(0x0a94014a)
/* Includes all low-power entry/hold and DFI clock-disable controls. */
#define LOW_POWER_MASK UINT32_C(0x00078a11)

static void init_report(struct t736_cold_mr_report *r)
{
	/* Explicit stores avoid freestanding compiler-generated memset calls. */
	r->stage = T736_COLD_MR_CHECK;
	r->last_address = r->last_value = r->last_ctrl0 = r->last_mrstat = 0;
	r->mrr_data[0] = r->mrr_data[1] = r->common_latch = 0;
	r->waited_us = r->writes_attempted = r->writes_completed = 0;
	r->last_width = r->last_is_write = r->channels_armed = 0;
	r->done_was_cleared = r->fresh_done_observed = r->data_valid = 0;
}

static int valid(const struct t736_phy_io *io,
		 const struct t736_cold_mr_context *c,
		 const struct t736_cold_mr_report *r)
{
	return io && io->read16 && io->read32 && io->write32 && c && r &&
		c->sram_exclusive_cold_context == 1 &&
		c->same_generation_mapping_reviewed == 1 &&
		(c->expected_micro_reset == 1 || c->expected_micro_reset == 9);
}

static int read_word(const struct t736_phy_io *io,
		     struct t736_cold_mr_report *r, uint32_t a, uint32_t *v)
{
	r->last_address = a; r->last_width = 32; r->last_is_write = 0;
	if (io->read32(io->context, a, v)) return T736_COLD_MR_IO;
	r->last_value = *v;
	return 0;
}

static int expect_word(const struct t736_phy_io *io,
	struct t736_cold_mr_report *r, uint32_t a, uint32_t mask, uint32_t v)
{
	uint32_t got;
	int rc = read_word(io, r, a, &got);
	if (rc) return rc;
	return (got & mask) == v ? 0 : T736_COLD_MR_PRECONDITION;
}

static int expect_half(const struct t736_phy_io *io,
	struct t736_cold_mr_report *r, uint32_t a, uint16_t v)
{
	uint16_t got;
	r->last_address = a; r->last_width = 16; r->last_is_write = 0;
	if (io->read16(io->context, a, &got)) return T736_COLD_MR_IO;
	r->last_value = got;
	return got == v ? 0 : T736_COLD_MR_PRECONDITION;
}

static int write_word(const struct t736_phy_io *io,
	struct t736_cold_mr_report *r, uint32_t a, uint32_t v)
{
	r->last_address = a; r->last_value = v;
	r->last_width = 32; r->last_is_write = 1;
	++r->writes_attempted;
	if (io->write32(io->context, a, v)) return T736_COLD_MR_IO;
	++r->writes_completed;
	return 0;
}

static int check_phy(const struct t736_phy_io *io,
	struct t736_cold_mr_report *r, int command, uint16_t expected_micro)
{
	int rc;
	if ((rc = expect_word(io, r, SYS_CFG, UINT32_MAX, 0x273f))) return rc;
	/* Never access internal PHY before host ownership and frozen PMU stall. */
	if ((rc = expect_half(io, r, T736_PHY_MUX, 0))) return rc;
	if ((rc = expect_half(io, r, T736_PHY_MICRO_RESET, expected_micro))) return rc;
	if ((rc = expect_half(io, r, CLOCKS, 3))) return rc;
	if ((rc = expect_half(io, r, MEM_RESET, command ? 3 : 2))) return rc;
	return expect_half(io, r, LP5_MODE, 0);
}

static int check_controllers(const struct t736_phy_io *io,
	struct t736_cold_mr_report *r, int command, uint32_t ctrl[2])
{
	unsigned int ch;
	uint32_t b, s;
	int rc;
	for (ch = 0; ch < 2; ++ch) {
		b = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		if ((rc = expect_word(io, r, b, UINT32_MAX, 0x03081002))) return rc;
		if ((rc = expect_word(io, r, b + T736_CTL_STAT, 7, 0))) return rc;
		if ((rc = expect_word(io, r, b + PWRCTL,
			command ? LOW_POWER_MASK : UINT32_MAX, command ? 0 : 0x800))) return rc;
		if ((rc = expect_word(io, r, b + DFIMISC,
			command ? 0x21 : UINT32_MAX, command ? 1 : 0x10014))) return rc;
		if ((rc = expect_word(io, r, b + DFISTAT, 1, command ? 1 : 0))) return rc;
		if ((rc = expect_word(io, r, b + T736_CTL_SWCTL, 1, 1))) return rc;
		if ((rc = expect_word(io, r, b + T736_CTL_SWSTAT, 1, 1))) return rc;
		/* Automatic MR4 polling must not create an unrelated receive event. */
		if (command && (rc = expect_word(io, r, b + 0x100, 1, 0))) return rc;
		if ((rc = read_word(io, r, b + CTRL0, &ctrl[ch]))) return rc;
		if (ctrl[ch] & (REQUEST | CLEAR_DONE)) return T736_COLD_MR_PRECONDITION;
		if (command && !(ctrl[ch] & SW_INIT)) return T736_COLD_MR_PRECONDITION;
		if ((rc = read_word(io, r, b + MRSTAT, &s))) return rc;
		if (s & BUSY) return T736_COLD_MR_PRECONDITION;
	}
	return 0;
}

int t736_cold_mr_arm(const struct t736_phy_io *io,
		    const struct t736_cold_mr_context *context,
		    struct t736_cold_mr_report *r)
{
	uint32_t ctrl[2], b, got;
	unsigned int ch;
	int rc;
	if (!valid(io, context, r)) return T736_COLD_MR_INVALID;
	init_report(r);
	if ((rc = check_phy(io, r, 0, context->expected_micro_reset))) return rc;
	if ((rc = check_controllers(io, r, 0, ctrl))) return rc;
	r->stage = T736_COLD_MR_ARM;
	for (ch = 0; ch < 2; ++ch) {
		b = T736_CTL0_BASE + ch * T736_CHANNEL_STRIDE;
		if ((rc = write_word(io, r, b + CTRL0, ctrl[ch] | SW_INIT))) return rc;
		if ((rc = read_word(io, r, b + CTRL0, &got))) return rc;
		if (got != (ctrl[ch] | SW_INIT)) return T736_COLD_MR_VERIFY;
		r->channels_armed |= (uint8_t)(1u << ch);
	}
	r->stage = T736_COLD_MR_FINISHED;
	return T736_COLD_MR_ARMED_ONLY;
}

/* Each phase gets the caller's bounded poll budget. No delay after success. */
static int wait_status(const struct t736_phy_io *io,
	struct t736_cold_mr_report *r, uint32_t base, uint32_t select,
	uint32_t timeout, int clear, int read)
{
	uint32_t elapsed = 0, c, s;
	int rc;
	for (;;) {
		if ((rc = read_word(io, r, base + CTRL0, &c))) return rc;
		r->last_ctrl0 = c;
		if ((c & SELECT_MASK) != select || (!clear && (c & CLEAR_DONE)))
			return T736_COLD_MR_VERIFY;
		if ((rc = read_word(io, r, base + MRSTAT, &s))) return rc;
		r->last_mrstat = s;
		if (!(c & REQUEST) && !(s & BUSY) &&
		    (clear ? !(s & DONE) : (!read || (s & DONE)))) return 0;
		if (elapsed == timeout) return T736_COLD_MR_TIMEOUT;
		if (io->delay_us(io->context, 1)) return T736_COLD_MR_TIMER;
		++elapsed; ++r->waited_us;
	}
}

int t736_cold_mr_command(const struct t736_phy_io *io,
	const struct t736_cold_mr_context *context,
	const struct t736_cold_mr_request *q, struct t736_cold_mr_report *r)
{
	uint32_t ctrl[2], b, select, got;
	int rc;
	if (!valid(io, context, r) || !io->delay_us || !q ||
	    context->command_path_and_timing_ready != 1 || q->channel > 1 ||
	    q->rank > 1 || q->mr > 63 || q->data > 255 || q->read > 1 ||
	    (q->read && q->data) || !q->timeout_us ||
	    q->timeout_us > T736_COLD_MR_MAX_WAIT_US) return T736_COLD_MR_INVALID;
	init_report(r);
	if ((rc = check_phy(io, r, 1, context->expected_micro_reset))) return rc;
	if ((rc = check_controllers(io, r, 1, ctrl))) return rc;
	r->channels_armed = 3;
	/* Demonstrate a working elapsed-time callback before the first write. */
	if (io->delay_us(io->context, 1)) return T736_COLD_MR_TIMER;
	++r->waited_us;
	b = T736_CTL0_BASE + q->channel * T736_CHANNEL_STRIDE;
	select = (UINT32_C(1) << (4u + q->rank)) | SW_INIT | q->read;
	if (q->read) {
		r->stage = T736_COLD_MR_CLEAR_DONE;
		if ((rc = write_word(io, r, b + CTRL0, select | CLEAR_DONE))) return rc;
		if ((rc = wait_status(io, r, b, select, q->timeout_us, 1, 1))) return rc;
		r->done_was_cleared = 1;
	}
	r->stage = T736_COLD_MR_SELECT;
	/* Explicitly clear bit24; no assumption that it self-clears. */
	if ((rc = write_word(io, r, b + CTRL0, select))) return rc;
	if ((rc = read_word(io, r, b + CTRL0, &got))) return rc;
	if (got != select) return T736_COLD_MR_VERIFY;
	if ((rc = write_word(io, r, b + CTRL1, (q->mr << 8) | q->data))) return rc;
	if ((rc = read_word(io, r, b + CTRL1, &got))) return rc;
	if (got != ((q->mr << 8) | q->data)) return T736_COLD_MR_VERIFY;
	/* Guard against unsolicited DONE between clearing and triggering. */
	if ((rc = read_word(io, r, b + MRSTAT, &got))) return rc;
	if ((got & BUSY) || (q->read && (got & DONE))) return T736_COLD_MR_VERIFY;
	r->stage = T736_COLD_MR_TRIGGER;
	if ((rc = write_word(io, r, b + CTRL0, select | REQUEST))) return rc;
	r->stage = T736_COLD_MR_WAIT;
	if ((rc = wait_status(io, r, b, select, q->timeout_us, 0, (int)q->read))) return rc;
	if ((rc = expect_word(io, r, b + T736_CTL_STAT, 7, 0))) return rc;
	if (q->read) {
		r->fresh_done_observed = 1;
		r->stage = T736_COLD_MR_DATA;
		if ((rc = read_word(io, r, b + DATA0, &r->mrr_data[0]))) return rc;
		if ((rc = read_word(io, r, b + DATA1, &r->mrr_data[1]))) return rc;
		if ((rc = read_word(io, r, q->channel ? 0x0a020040 : 0x0a02002c,
			&r->common_latch))) return rc;
		r->data_valid = 1;
	}
	r->stage = T736_COLD_MR_FINISHED;
	return q->read ? T736_COLD_MR_RECEIVED_CANDIDATE : T736_COLD_MR_ISSUED_ONLY;
}
