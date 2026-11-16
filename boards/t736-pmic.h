/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_T736_PMIC_H
#define _LINUX_T736_PMIC_H

#include <linux/types.h>

/**
 * struct t736_cpu_supply_snapshot - raw AXP8191 CPU supply registers
 * @id: chip ID, register 0x0e (expected 3)
 * @enable: DCDC enable bits, register 0x10 (DCDC3 bit 2, DCDC4 bit 3)
 * @dcdc3: A55/DSU voltage selector and DVM bit, register 0x14
 * @dcdc4: A76 voltage selector and DVM bit, register 0x15
 * @phase: poly-phase configuration, register 0x1b
 * @feedback: remote-feedback configuration, register 0x1d
 *
 * DCDC3/4 selectors are bits 6:0; bit 7 is DVM, not a voltage bit.
 * Selectors 0..0x46 encode 500000 + selector * 10000 microvolts;
 * selectors 0x47..0x57 encode 1220000 + (selector - 0x47) * 20000.
 * Larger selectors are reserved. Phase bits 6/7 join DCDC2/3 and DCDC4/5.
 */
struct t736_cpu_supply_snapshot {
	u8 id;
	u8 enable;
	u8 dcdc3;
	u8 dcdc4;
	u8 phase;
	u8 feedback;
};

/**
 * t736_pmic_read_cpu_supplies - read CPU rails through the existing bus owner
 * @snapshot: receives all six raw registers on success; unchanged on failure
 *
 * Sleepable context only. Serializes with the GPU regulator using its existing
 * bus mutex. Does not initialize the bus, write PMIC data, or change supplies.
 * Register reads necessarily issue TWI control writes and PMIC address bytes.
 * The readings are sequential under one lock, not simultaneous ADC samples.
 * Callers must validate enable, phase, and selector values before using them.
 *
 * Return: 0 on success, -EINVAL for NULL, -ENODEV before successful owner probe
 * or for a chip ID mismatch, or a negative transaction error. The API is
 * GPL-only; it is absent from kernels predating this board-specific interface.
 */
int t736_pmic_read_cpu_supplies(struct t736_cpu_supply_snapshot *snapshot);

#endif
