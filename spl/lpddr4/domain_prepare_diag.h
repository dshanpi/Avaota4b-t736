/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_DOMAIN_PREPARE_DIAG_H
#define T736_DOMAIN_PREPARE_DIAG_H

#include "phy/host.h"

#define T736_DOMAIN_SYS UINT32_C(0x0a020010)
#define T736_DOMAIN_CHANNEL_TYPE UINT32_C(0x0a020014)
#define T736_DOMAIN_ISOLATION UINT32_C(0x07010250)
#define T736_DOMAIN_WAIT_US 10000u

enum t736_domain_result {
	T736_DOMAIN_WRITES_OBSERVED_ONLY = 1,
	T736_DOMAIN_INVALID = -1,
	T736_DOMAIN_IO = -2,
	T736_DOMAIN_PRECONDITION = -3,
	T736_DOMAIN_VERIFY = -4,
	T736_DOMAIN_TIMER = -5
};

struct t736_domain_values {
	uint32_t sys;
	uint32_t channel_type;
	uint32_t isolation;
};

struct t736_domain_report {
	struct t736_domain_values before;
	struct t736_domain_values after;
	uint32_t waited_us;
	uint32_t last_write_address;
	uint32_t last_write_value;
	uint8_t before_valid;
	uint8_t after_valid;
	uint8_t writes_attempted;
	uint8_t writes_completed;
};

/* A narrowly scoped SRAM experiment from the measured source-CCU cold state.
 * cold_sram_exclusive must be exactly 1: caller attests no vendor DDR init,
 * SRAM code/data/stack, and stopped interrupts/DMA/other DRAM clients.
 * Requires exact PLL=f9004a00, DRAMCLK=80000003, BGR=10001 and common
 * SYS_CFG=3200, CHANNEL_TYPE=0, PRCM07010250=0 before any write.
 *
 * Exactly three 32-bit RMW writes, each immediately read back:
 * SYS_CFG mask3f3f ->2728; CHANNEL_TYPE mask3 ->2; PRCM mask1 ->1.
 * Then wait 10000us and read back the three common registers again.
 * All bits outside each mask are preserved. Unexpected initial/change/readback
 * state aborts. Ordered MMIO callbacks must terminate; a bus hang is not bounded
 * by this function. Failure performs no retry, cleanup, or rollback.
 *
 * This deliberately stops before the first controller or PHY access. It does
 * not write common master ports, SMC, other PRCM addresses or PMIC, and does not
 * execute the later SYS_CFG=273c/273e/273f transitions. Compared with SDK host
 * observations it also omits the 0100/2600 bridge phase. Therefore its positive
 * return proves only the specified writes/readbacks/delay completed, NOT that
 * a controller is accessible or DRAM is initialized. PRCM bit0's electrical
 * polarity and the exact common gate/reset field names remain unconfirmed.
 *
 * Report is progressive on valid invocation; attempted and completed writes
 * differ when a callback fails after possibly affecting hardware. Caller must
 * use its controlled power-cycle path after failure, not rerun blindly.
 */
int t736_domain_prepare_experiment(const struct t736_phy_io *io,
				  unsigned int cold_sram_exclusive,
				  struct t736_domain_report *report);

#endif
