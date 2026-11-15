// SPDX-License-Identifier: GPL-2.0-only
/*
 * AXP8191 Serial datasheet v1.0, sections 6.10.2 and 7.1.
 * The interface/rail register mapping was cross-checked with GPL SyterKit
 * drivers/pmu/axp8191.c. Exact voltage requests are required (no rounding).
 */
#include "axp8191.h"

struct rail_registers {
	uint8_t voltage, enable, enable_bit, mask;
};

static int rail_registers(enum t736_axp_rail rail, struct rail_registers *r)
{
	switch (rail) {
	case T736_AXP_DCDC8:
		*r = (struct rail_registers){0x19, 0x10, 7, 0x7f};
		break;
	case T736_AXP_ALDO4:
		*r = (struct rail_registers){0x27, 0x20, 3, 0x1f};
		break;
	case T736_AXP_ELDO1:
		*r = (struct rail_registers){0x3a, 0x22, 6, 0x3f};
		break;
	default:
		return T736_AXP_INVALID;
	}
	return T736_AXP_OK;
}

int t736_axp_encode_mv(enum t736_axp_rail rail, unsigned int mv, uint8_t *code)
{
	unsigned int base, step, start;
	if (!code)
		return T736_AXP_INVALID;
	switch (rail) {
	case T736_AXP_DCDC8:
		if (mv >= 500 && mv <= 1200) {
			base = 500; step = 10; start = 0;
		} else if (mv >= 1220 && mv <= 1840) {
			base = 1220; step = 20; start = 71;
		} else if (mv >= 1900 && mv <= 3400) {
			base = 1900; step = 100; start = 103;
		} else {
			return T736_AXP_RANGE;
		}
		break;
	case T736_AXP_ALDO4:
		if (mv < 500 || mv > 3400)
			return T736_AXP_RANGE;
		base = 500; step = 100; start = 0;
		break;
	case T736_AXP_ELDO1:
		if (mv < 500 || mv > 1500)
			return T736_AXP_RANGE;
		base = 500; step = 25; start = 0;
		break;
	default:
		return T736_AXP_INVALID;
	}
	if ((mv - base) % step)
		return T736_AXP_RANGE;
	*code = (uint8_t)(start + (mv - base) / step);
	return T736_AXP_OK;
}

int t736_axp_decode_mv(enum t736_axp_rail rail, uint8_t code, unsigned int *mv)
{
	if (!mv)
		return T736_AXP_INVALID;
	switch (rail) {
	case T736_AXP_DCDC8:
		if (code <= 70)
			*mv = 500u + code * 10u;
		else if (code <= 102)
			*mv = 1220u + (code - 71u) * 20u;
		else if (code <= 118)
			*mv = 1900u + (code - 103u) * 100u;
		else
			return T736_AXP_RANGE;
		break;
	case T736_AXP_ALDO4:
		if (code > 29)
			return T736_AXP_RANGE;
		*mv = 500u + code * 100u;
		break;
	case T736_AXP_ELDO1:
		if (code > 40)
			return T736_AXP_RANGE;
		*mv = 500u + code * 25u;
		break;
	default:
		return T736_AXP_INVALID;
	}
	return T736_AXP_OK;
}

int t736_axp_set_rail(const struct t736_axp_bus *bus, enum t736_axp_rail rail,
		    unsigned int mv)
{
	struct rail_registers r;
	uint8_t id, code, old_voltage, old_enable, value, verify;
	unsigned int old_mv;
	int ret;

	if (!bus || !bus->read8 || !bus->write8)
		return T736_AXP_INVALID;
	ret = rail_registers(rail, &r);
	if (ret)
		return ret;
	ret = t736_axp_encode_mv(rail, mv, &code);
	if (ret)
		return ret;
	if (bus->read8(bus->ctx, 0x0e, &id))
		return T736_AXP_IO;
	if (id != 0x03)
		return T736_AXP_ID;
	if (bus->read8(bus->ctx, r.voltage, &old_voltage) ||
	    bus->read8(bus->ctx, r.enable, &old_enable))
		return T736_AXP_IO;
	ret = t736_axp_decode_mv(rail, old_voltage & r.mask, &old_mv);
	if (ret)
		return ret;
	/* Datasheet 7.1 forbids a live DCDC8 change across the 1.54 V boundary. */
	if (rail == T736_AXP_DCDC8 && (old_enable & (1u << r.enable_bit)) &&
	    ((old_mv > 1540u) != (mv > 1540u)))
		return T736_AXP_LIVE_RANGE_CHANGE;
	value = (uint8_t)((old_voltage & (uint8_t)~r.mask) | code);
	if (value != old_voltage && bus->write8(bus->ctx, r.voltage, value))
		return T736_AXP_IO;
	if (bus->read8(bus->ctx, r.voltage, &verify))
		return T736_AXP_IO;
	if (verify != value)
		return T736_AXP_VERIFY;
	value = (uint8_t)(old_enable | (1u << r.enable_bit));
	if (value != old_enable && bus->write8(bus->ctx, r.enable, value))
		return T736_AXP_IO;
	if (bus->read8(bus->ctx, r.enable, &verify))
		return T736_AXP_IO;
	return verify == value ? T736_AXP_OK : T736_AXP_VERIFY;
}
