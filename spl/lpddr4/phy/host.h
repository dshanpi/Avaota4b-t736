/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_LP4_PHY_HOST_H
#define T736_LP4_PHY_HOST_H

#include <stdint.h>

#define T736_PHY_MUX UINT32_C(0x0aaa0000)
#define T736_PHY_MICRO_RESET UINT32_C(0x0aaa0132)
#define T736_PHY_DQ_SELECTOR0 UINT32_C(0x0a920100)
#define T736_PHY_DBYTE_STRIDE UINT32_C(0x2000)
#define T736_PHY_RESET_STALL UINT16_C(0x0009)
#define T736_PLL_DDR UINT32_C(0x02002020)
#define T736_DRAM0_CLK UINT32_C(0x02002c00)
#define T736_DRAM0_BGR UINT32_C(0x02002c0c)
#define T736_NSI_CLK UINT32_C(0x02002580)
#define T736_NSI_BGR UINT32_C(0x02002584)
#define T736_MBUS_CLK UINT32_C(0x02002588)

enum t736_phy_error {
	T736_PHY_OK = 0,
	T736_PHY_INVALID = -1,
	T736_PHY_IO = -2,
	T736_PHY_BUSY = -3,
	T736_PHY_UNSUPPORTED = -4,
	T736_PHY_TIMEOUT = -5,
	T736_PHY_VERIFY = -6,
	T736_PHY_NOT_OWNER = -7,
	T736_PHY_TIMER = -8
};

struct t736_phy_io {
	void *context;
	/* Absolute physical byte addresses; PHY must use ordered halfword MMIO.
	 * Callbacks return zero on success and must terminate on their own.
	 * Read/write errors abort the current operation without a retry.
	 */
	int (*read16)(void *context, uint32_t address, uint16_t *value);
	int (*write16)(void *context, uint32_t address, uint16_t value);
	int (*read32)(void *context, uint32_t address, uint32_t *value);
	int (*write32)(void *context, uint32_t address, uint32_t value);
	/* Must confirm actual elapsed time, return nonzero on timer failure. */
	int (*delay_us)(void *context, uint32_t delay);
};

struct t736_phy_host_state {
	uint16_t mux;
	uint16_t micro_reset;
	uint8_t mux_valid;
	uint8_t micro_reset_valid;
};

/* Non-clearing reads only. Read MuxSel first. If not exactly host-owned (0),
 * return NOT_OWNER immediately, without reading MicroReset or any PHY CSR.
 * No mailbox, ACK, IMEM, DMEM or internal PHY CSR is read by this function.
 */
int t736_phy_observe_host(const struct t736_phy_io *io,
			 struct t736_phy_host_state *state);

/* Pure decode of documented N/P0/M1 fields with an explicit 24 MHz input.
 * Requires PLL enable, LDO, lock-detect, lock and output gate asserted.
 * Returns exact MHz, rejecting fractional MHz and out-of-range N encodings.
 * The observed opaque bit24 has no claimed frequency semantics.
 */
int t736_phy_pll_frequency(uint32_t pll, uint32_t *mhz);

/* Cold-start PLL only; does not configure MBUS, SMC, controller or PHY.
 * Caller must run entirely in SRAM with interrupts/DMA/DRAM clients stopped.
 * Refuses unless DRAM0 clock and bus gates are off. Fabric clocks are retained:
 * NSI must select SYS24M or PERI0_600M, MBUS SYS24M or PERI0_400M; neither may
 * have a pending update/DFS. No NSI/MBUS gate/reset/source is changed here.
 * PERI0 sources must already be valid; this does not initialize PERI0 PLL.
 * Programs 1800 MHz at 24 MHz reference,
 * including the corroborated T736 opaque PLL bit24; see provenance.json.
 * LDO precedes divider/PLL enable. Bounded lock qualification requires three
 * consecutive lock observations, then 20us AFTER lock before output enable.
 * Failure leaves the output gate disabled up
 * to the final output-enable transaction; an IO failure requires caller to
 * abort bring-up and reset, not assume rollback or use DRAM.
 */
int t736_phy_start_pll_1800(const struct t736_phy_io *io,
			  uint32_t timeout_us);

/* Cold-start only, never usable from a running Linux/U-Boot DRAM context.
 * Both CTLs must be in init mode with verified LP4 geometry, bus enabled and
 * 1800 MHz PLL stable. Assert PMU reset+stall before giving APB to the host.
 * Leaves reset and stall asserted. Does not load/start executable firmware.
 */
int t736_phy_cold_claim(const struct t736_phy_io *io);

/* Caller supplies four complete 0..7 permutations IN PHY CSR SELECTOR ORDER.
 * This API does not infer schematic net direction, DQS swap, or board wiring.
 * All permutations are validated before any MMIO. Requires cold init mode,
 * stable 1800 MHz clock and host ownership with reset+stall asserted.
 * Writes exactly 32 halfword selectors, not the DMI/9th-lane/CA selectors.
 */
int t736_phy_set_dq_selectors(const struct t736_phy_io *io,
			    const uint8_t selectors[4][8]);

/* Pstate 0 and controller 1:4 clock only. Requires the same cold prepared
 * state as set_dq_selectors. Performs both SWCTL/SWSTAT transactions and a
 * bounded CCU update handshake. Does not perform post-training DFI init.
 * On any failure, caller must abort; partial transactions are not rolled back.
 */
int t736_phy_select_pstate0(const struct t736_phy_io *io,
			  uint32_t timeout_us);

#endif
