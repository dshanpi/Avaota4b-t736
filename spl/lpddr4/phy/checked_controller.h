/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_CHECKED_CONTROLLER_H
#define T736_CHECKED_CONTROLLER_H

#include "host.h"
#include "../controller.h"
#include "../timing.h"

enum t736_ctl_checked_result {
	T736_CTL_CHECK_OK = 0,
	T736_CTL_CHECK_INVALID = -1,
	T736_CTL_CHECK_IO = -2,
	T736_CTL_CHECK_BUSY = -3,
	T736_CTL_CHECK_VERIFY = -4,
	T736_CTL_CHECK_TIMEOUT = -5,
	T736_CTL_CHECK_TIMER = -6,
	T736_CTL_CHECK_UNSUPPORTED = -7
};

struct t736_ctl_observation {
	uint32_t channel, attempted_address, valid_mask;
	uint32_t mstr, stat, mrstat;
};

struct t736_ctl_write_report {
	uint32_t attempted_address, attempted_value;
	uint32_t writes_attempted, write_callbacks_completed, verified_writes;
	uint32_t channels_prechecked;
};

/* A single-channel observation, including the first controlled CSR-access
 * experiment. It does not require a previously configured MSTR or a successful
 * prior channel read. Caller establishes SRAM-only execution and appropriate
 * clock/domain context; ports need not match an assumed vendor preset.
 *
 * Reads exactly MSTR, STAT, MRSTAT in that order, never another channel. The
 * volatile report may be placed in the harness's SRAM report area. Its last
 * attempted address is stored before each MMIO callback. A callback error stops
 * immediately. An individual hardware bus hang CANNOT be bounded by this API;
 * use a progress marker and the board's controlled power-cycle recovery.
 * Success means only these three reads completed, not controller readiness.
 */
int t736_ctl_observe_checked(const struct t736_phy_io *io, unsigned int channel,
			    volatile struct t736_ctl_observation *report);

/* Source SRAM configuration components; no domain/clock/ports/PHY writes or
 * DFI-init handshake. The timing subset includes static DFI pipeline fields
 * at timing-block offsets 0x580/0x584/0x588; it never writes CTL DFIMISC.
 * observed_channels is a caller assertion that both channels were successfully
 * observed in the CURRENT cold session (exact value 3). sram_exclusive must be
 * 1, with interrupts/DMA/other DRAM clients stopped. These assertions do not
 * replace platform prerequisites or prove an MMIO access cannot hang.
 *
 * The observed CCU/common phase is rechecked before either controller: PLL_DDR
 * f9004a00, DRAM0_CLK 80000003, DRAM0_BGR 10001, SYS_CFG 2728, CHANNEL_TYPE 2,
 * PRCM07010250 1. Both STATs must be INIT before any write. Layout accepts an arbitrary cold
 * reset MSTR and programs only the existing checked 8-GiB geometry/address map.
 * Timing additionally requires both MSTRs to match that layout, and uses the
 * existing 1800-MHz pure timing calculator. All callbacks report failure; every
 * write is immediately read back. The progressive report distinguishes a write
 * attempted from callback completion and verified readback. Failure means stop
 * initialization; no retries, cleanup writes, or speculative later accesses.
 * No training, normal-mode entry, or usable-memory claim is made.
 */
int t736_ctl_layout_checked(const struct t736_phy_io *io,
			   unsigned int observed_channels, unsigned int sram_exclusive,
			   const struct t736_lp4_geometry *geometry,
			   volatile struct t736_ctl_write_report *report);
int t736_ctl_timing_checked(const struct t736_phy_io *io,
			   unsigned int observed_channels, unsigned int sram_exclusive,
			   uint32_t ck_mhz,
			   volatile struct t736_ctl_write_report *report);

#endif
