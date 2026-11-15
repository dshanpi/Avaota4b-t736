// SPDX-License-Identifier: GPL-2.0-only
/* T736 User Manual v0.90, 18.11.10: S_TWI0 byte engine.
 * INT_FLAG is W1C. No automatic retries: PMIC writes must be explicit.
 */
#include "twi.h"

static int valid(const struct t736_twi *b)
{
	return b && b->read && b->write && b->ticks &&
		b->timeout_ticks && b->timeout_ticks < 0x80000000u && b->poll_limit;
}

static int wait_mask(struct t736_twi *b, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t begin = b->ticks(b->ctx), count;
	for (count = 0; count < b->poll_limit; count++) {
		if ((b->read(b->ctx, reg) & mask) == value)
			return TWI_OK;
		if ((uint32_t)(b->ticks(b->ctx) - begin) >= b->timeout_ticks)
			break;
	}
	return TWI_TIMEOUT;
}

static int reset(struct t736_twi *b)
{
	b->write(b->ctx, TWI_RESET, 1);
	return wait_mask(b, TWI_RESET, 1, 0);
}

static int state(struct t736_twi *b, unsigned expected)
{
	int rc = wait_mask(b, TWI_CTRL, TWI_IF, TWI_IF);
	b->last_status = b->read(b->ctx, TWI_STATUS) & 0xff;
	if (rc)
		return rc;
	if (b->last_status == expected)
		return TWI_OK;
	switch (b->last_status) {
	case 0x20: case 0x30: case 0x48: return TWI_NACK;
	case 0x38: return TWI_ARBITRATION;
	case 0: return TWI_BUS_ERROR;
	default: return TWI_UNEXPECTED;
	}
}

static int stop(struct t736_twi *b)
{
	int rc;
	b->write(b->ctx, TWI_CTRL, TWI_EN | TWI_STOP | TWI_IF);
	rc = wait_mask(b, TWI_CTRL, TWI_STOP, 0);
	if (rc)
		return rc;
	return wait_mask(b, TWI_STATUS, 0xff, 0xf8);
}

static int finish(struct t736_twi *b, int rc)
{
	/* After arbitration loss we no longer own the wires: never send STOP. */
	b->cleanup_error = rc == TWI_ARBITRATION ? reset(b) : stop(b);
	if (b->cleanup_error && rc != TWI_ARBITRATION)
		(void)reset(b);
	return rc ? rc : b->cleanup_error;
}

static int send(struct t736_twi *b, uint8_t value, unsigned expected)
{
	b->write(b->ctx, TWI_DATA, value);
	b->write(b->ctx, TWI_CTRL, TWI_EN | TWI_IF);
	return state(b, expected);
}

static int begin(struct t736_twi *b, uint8_t address, uint8_t reg)
{
	int rc;
	b->cleanup_error = 0;
	/* No attempt to clear a stuck bus by clocking the PMIC blindly. */
	if ((b->read(b->ctx, TWI_LINE) & 0x30) != 0x30 ||
	    (b->read(b->ctx, TWI_STATUS) & 0xff) != 0xf8)
		return TWI_BUSY;
	b->write(b->ctx, TWI_CTRL, TWI_EN | TWI_START | TWI_IF);
	rc = state(b, 0x08);
	if (!rc)
		rc = send(b, (uint8_t)(address << 1), 0x18);
	if (!rc)
		rc = send(b, reg, 0x28);
	return rc;
}

int t736_twi_init(struct t736_twi *b)
{
	int rc;
	if (!valid(b))
		return TWI_INVALID;
	b->cleanup_error = 0;
	b->last_status = 0xf8;
	/* Refuse to take over an active hardware DVFS/packet master. */
	if ((b->read(b->ctx, TWI_DRV_CTRL) & 0x80000001u) ||
	    (b->read(b->ctx, TWI_PMU_CTRL) & 0xff))
		return TWI_BUSY;
	rc = reset(b);
	if (rc)
		return rc;
	/* 24 MHz / (2^1 * (11 + 1) * 10) = 100 kHz, duty cycle 50%. */
	b->write(b->ctx, TWI_CLOCK, (11u << 3) | 1u);
	b->write(b->ctx, TWI_EFR, 0);
	b->write(b->ctx, TWI_CTRL, TWI_EN);
	return TWI_OK;
}

int t736_twi_read8(struct t736_twi *b, uint8_t address, uint8_t reg, uint8_t *value)
{
	int rc;
	uint8_t received = 0;
	if (!valid(b) || !value || address > 0x7f)
		return TWI_INVALID;
	rc = begin(b, address, reg);
	if (rc == TWI_BUSY)
		return rc;
	if (!rc) {
		b->write(b->ctx, TWI_CTRL, TWI_EN | TWI_START | TWI_IF);
		rc = state(b, 0x10);
	}
	if (!rc)
		rc = send(b, (uint8_t)((address << 1) | 1), 0x40);
	if (!rc) {
		/* One byte only: leave ACK clear and end with a NACK. */
		b->write(b->ctx, TWI_CTRL, TWI_EN | TWI_IF);
		rc = state(b, 0x58);
	}
	if (!rc)
		received = (uint8_t)b->read(b->ctx, TWI_DATA);
	rc = finish(b, rc);
	if (!rc)
		*value = received;
	return rc;
}

int t736_twi_write8(struct t736_twi *b, uint8_t address, uint8_t reg, uint8_t value)
{
	int rc;
	if (!valid(b) || address > 0x7f)
		return TWI_INVALID;
	rc = begin(b, address, reg);
	if (rc == TWI_BUSY)
		return rc;
	if (!rc)
		rc = send(b, value, 0x28);
	return finish(b, rc);
}
