/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_A55CLOCK_H
#define T736_A55CLOCK_H
#include "cpuclock.h"

#define T736_A55_CTRL UINT32_C(0x1000)
#define T736_A55_CLK UINT32_C(0x101c)
#define T736_A55_STAT UINT32_C(0x102c)
#define T736_A55_ORIGINAL UINT32_C(0xf8801300)
#define T736_A55_TARGET UINT32_C(0xf8802700)
#define T736_A55_PLL_CLK UINT32_C(0x03000000)
#define T736_A55_PPU(cpu) (UINT32_C(0x07052008) + (cpu) * UINT32_C(0x1000))
#define T736_A55_GUARDS 19

struct t736_a55clock_report {
	uint32_t stage, crystal, before_ctrl, before_stat, before_clk;
	uint32_t final_ctrl, final_stat, final_clk, ppu_before[8];
	uint32_t guards_before[T736_A55_GUARDS];
	struct t736_cpuclock_supply supply_before, supply_after;
	uint32_t mutation_attempted, slow_verified, guard_failure;
	int error, rollback_error;
};

/* Execute only on CPU0 A55 with interrupts masked and all other PPUs OFF.
 * N19 -> N39 warm change, PLL ENABLE/LDO retained. Errors after mutation
 * only select verified slow; never restore N19 or disable/restart the PLL.
 * Caller must halt on every error after mutation (no secondary release).
 */
int t736_a55clock_prepare(const struct t736_cpuclock_io *io,
			 struct t736_a55clock_report *report);
#endif
