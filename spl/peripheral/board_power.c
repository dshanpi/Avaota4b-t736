// SPDX-License-Identifier: GPL-2.0-only
/* T736 schematic power tree, AXP8191 datasheet and Tina GPL regulator mapping.
 * This is a candidate component; integration/hardware validation is separate.
 */
#include "board_power.h"

static int write_verify(const struct t736_axp_bus *bus, uint8_t reg,
			uint8_t old, uint8_t value, uint8_t *result)
{
	if (old != value && bus->write8(bus->ctx, reg, value))
		return T736_AXP_IO;
	if (bus->read8(bus->ctx, reg, result))
		return T736_AXP_IO;
	return *result == value ? T736_AXP_OK : T736_AXP_VERIFY;
}

int t736_board_power_enable(const struct t736_axp_bus *bus,
			   struct t736_board_power_report *r)
{
	uint8_t id;
	int rc;
	if (!bus || !bus->read8 || !bus->write8 || !r)
		return T736_AXP_INVALID;
	if (bus->read8(bus->ctx, 0x0e, &id))
		return T736_AXP_IO;
	if (id != 3)
		return T736_AXP_ID;
	/* Finish all preconditions before enabling either supply. */
	if (bus->read8(bus->ctx, 0x10, &r->dcdc_enable) ||
	    bus->read8(bus->ctx, 0x12, &r->dcdc1_voltage) ||
	    bus->read8(bus->ctx, 0x11, &r->switch_before) ||
	    bus->read8(bus->ctx, 0x22, &r->ldo_before) ||
	    bus->read8(bus->ctx, 0x35, &r->dldo2_before))
		return T736_AXP_IO;
	/* DCDC1 is 1000 mV + code * 100 mV, unlike the LDO encoding. */
	if (!(r->dcdc_enable & 1) || (r->dcdc1_voltage & 0x1f) != 23)
		return T736_AXP_RANGE;
	/* Refuse a live DLDO2 voltage change; 500 + 13 * 100 = 1800 mV. */
	if ((r->ldo_before & 2) && (r->dldo2_before & 0x1f) != 13)
		return T736_AXP_LIVE_RANGE_CHANGE;
	if ((r->dldo2_before & 0x1f) > 29)
		return T736_AXP_RANGE;
	rc = write_verify(bus, 0x35, r->dldo2_before,
		(r->dldo2_before & 0xe0) | 13, &r->dldo2_after);
	if (rc)
		return rc;
	/* Preserve ELDO1 (LPDDR4x VDDQ) and every other LDO enable bit. */
	rc = write_verify(bus, 0x22, r->ldo_before, r->ldo_before | 2,
			  &r->ldo_after);
	if (rc)
		return rc;
	return write_verify(bus, 0x11, r->switch_before, r->switch_before | 8,
			    &r->switch_after);
}
