/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef T736_CPUCLOCK_H
#define T736_CPUCLOCK_H
#include <stdint.h>

#define T736_CPUCLOCK_BASE UINT32_C(0x08870000)
#define T736_CPUCLOCK_XO UINT32_C(0x07090160)
/* TF-A NCAT3 PPU_PWSR(core + 1), R_CPUCFG_BASE=0x07050000. */
#define T736_CPUCLOCK_PPU6 UINT32_C(0x07058008)
#define T736_CPUCLOCK_PPU7 UINT32_C(0x07059008)
#define T736_CPUCLOCK_CTRL UINT32_C(0x2000)
#define T736_CPUCLOCK_CLK UINT32_C(0x201c)
#define T736_CPUCLOCK_STAT UINT32_C(0x202c)
#define T736_CPUCLOCK_ORIGINAL UINT32_C(0x40801400)
#define T736_CPUCLOCK_TARGET UINT32_C(0xf8802700)
#define T736_CPUCLOCK_TARGET_CLK UINT32_C(0x03000000)
#define T736_CPUCLOCK_GUARDS 19

struct t736_cpuclock_io {
	void *ctx;
	uint32_t (*read32)(void *, uint32_t address);
	void (*write32)(void *, uint32_t address, uint32_t value);
	int (*delay_us)(void *, uint32_t usec);
	/* Uses the caller's already initialized PMIC owner; no write callback. */
	int (*pmic_read8)(void *, uint8_t reg, uint8_t *value);
	uint32_t midr, mpidr;
};

struct t736_cpuclock_supply {
	uint8_t id, enable, dcdc3, dcdc4, phase, feedback;
};

struct t736_cpuclock_report {
	uint32_t stage, midr, mpidr, crystal, ppu6, ppu7;
	uint32_t before_ctrl, before_stat, before_clk;
	uint32_t final_ctrl, final_stat, final_clk;
	uint32_t guards_before[T736_CPUCLOCK_GUARDS];
	struct t736_cpuclock_supply supply_before, supply_after;
	uint32_t mutation_attempted, rollback_attempted, slow_verified;
	uint32_t guard_failure;
	int error, rollback_error;
};

enum t736_cpuclock_error {
	T736_CPUCLOCK_OK = 0,
	T736_CPUCLOCK_INVALID = -1,
	T736_CPUCLOCK_PRECONDITION = -2,
	T736_CPUCLOCK_PMIC = -3,
	T736_CPUCLOCK_SUPPLY = -4,
	T736_CPUCLOCK_VERIFY = -5,
	T736_CPUCLOCK_TIMEOUT = -6,
	T736_CPUCLOCK_DELAY = -7,
	T736_CPUCLOCK_GUARD = -8,
};

/* Candidate fixed-board initialization: 26MHz * N39 /1 = 1014MHz, A76 only.
 * Caller is the A55 bootstrap core before releasing either A76 secondary.
 * Reject unexpected initial state without writing. Only B CTRL/CLK may be
 * written. Every wait is bounded; errors after mutation attempt restore the
 * slow/disabled state, retaining committed N20/N39 (N cannot update while
 * disabled). The caller must halt if restoration or a
 * guard fails, rather than releasing secondaries under an unknown clock.
 * Setpoints are read from PMIC; this is not an ADC/frequency measurement.
 */
int t736_cpuclock_prepare(const struct t736_cpuclock_io *io,
			  struct t736_cpuclock_report *report);
/* Production glue is compiled only with T736_CPUCLOCK; 0=target, 1=safe
 * slow/PLL-off state retained, negative=unsafe state and caller must halt. */
int t736_cpuclock_boot(int (*pmic_read8)(void *, uint8_t, uint8_t *), void *ctx);
#endif
