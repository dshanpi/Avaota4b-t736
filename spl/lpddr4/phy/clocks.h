/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_PHY_CLOCKS_H
#define T736_LP4_PHY_CLOCKS_H

#include "host.h"

enum t736_phy_ccu_stage {
	T736_CCU_NOT_STARTED,
	T736_CCU_PREFLIGHT_PASSED,
	T736_CCU_CONSUMERS_ISOLATED,
	T736_CCU_PLL_LOCKED,
	T736_CCU_CLOCK_SOURCES_CONFIGURED,
	T736_CCU_CFG_BUSES_ENABLED,
	T736_CCU_CLOCKS_ENABLED
};

struct t736_phy_ccu_report {
	enum t736_phy_ccu_stage completed_stage;
	int result;
};

/* DDR CCU preparation with an already-valid independent fabric clock.
 *
 * SRAM-only, no interrupts/DMA/other DRAM clients, no useful DRAM contents.
 * Requires NSI SYS24M or PERI0_600M, MBUS SYS24M or PERI0_400M, both fabric
 * gates on, NSI main/configuration resets released and configuration gate on,
 * no clock updates in flight and no NSI/MBUS dynamic scaling. PERI0 source
 * validity is a caller/platform precondition: this API does not initialize it.
 * If the DRAM
 * controller is already clocked and out of reset, both STATs must be INIT.
 * Running-system reclocking/suspend/resume are intentionally unsupported.
 *
 * Isolates DRAM0 and its configuration gate/reset. Starts DDRPLL at 1800 MHz
 * without executing vendor DDR code, then configures DRAM0=450 MHz. Entire
 * NSI_CLK, NSI_BGR and MBUS_CLK values remain unchanged, preserving the live
 * FEL communication path. Normal source boot may use SYS24M or a separately
 * source-initialized PERI0 PLL; the observed early-Boot0 FEL PERI0 setup is
 * not evidence that the final independent cold clock tree is complete.
 * All update bits, write readbacks and time intervals are checked. The report
 * gives the last completed stage, never a statement that memory is usable.
 *
 * Does NOT configure PRCM DMC features, SMC permissions/glitch workaround,
 * controller timing/addressing/ports, PHY analog calibration, DRAM reset/CKE,
 * PMU/PIE/training, PERI0 PLL or memory isolation. Those have separate contracts.
 * On any error, stop bring-up and reset; no rollback or automatic retry.
 */
int t736_phy_ccu_cold_start(const struct t736_phy_io *io, uint32_t timeout_us,
			    struct t736_phy_ccu_report *report);

#endif
