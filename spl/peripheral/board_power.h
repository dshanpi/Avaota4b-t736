/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_BOARD_POWER_H
#define T736_BOARD_POWER_H
#include "axp8191.h"

struct t736_board_power_report {
	uint8_t dcdc_enable, dcdc1_voltage;
	uint8_t switch_before, switch_after;
	uint8_t ldo_before, ldo_after;
	uint8_t dldo2_before, dldo2_after;
};

/* Fixed board wiring: DC1SW1 feeds PH/PHY/3V3; DLDO2 feeds HDMI 1V8.
 * DCDC1 must already be enabled at 3.3 V. Never retune a shared DCDC rail.
 */
int t736_board_power_enable(const struct t736_axp_bus *bus,
			   struct t736_board_power_report *report);
#endif
