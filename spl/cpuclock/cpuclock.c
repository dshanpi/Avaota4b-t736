// SPDX-License-Identifier: GPL-2.0-only
/* Fixed board A76 sequence derived from the separately reviewed offline-core
 * trial. A733 CPU PLL manual: N raw (no +1), VCO 650..2260MHz; Tina's 26MHz
 * A76 1014MHz OPP is 800mV for every listed VF bin. No donor probe is called.
 */
#include "cpuclock.h"

#define BIT(n) (UINT32_C(1) << (n))
#define UPDATE BIT(26)
#define OUTPUT BIT(27)
#define LOCK BIT(28)
#define LOCK_EN BIT(29)
#define LDO BIT(30)
#define ENABLE BIT(31)
#define N_MASK UINT32_C(0xff00)

static const uint16_t guard_offsets[T736_CPUCLOCK_GUARDS] = {
	0x0000, 0x0008, 0x000c,
	0x1000, 0x1004, 0x1008, 0x1018, 0x101c, 0x102c,
	0x3000, 0x3004, 0x3008, 0x3018, 0x301c, 0x302c,
	0x3034, 0x2004, 0x2008, 0x2018,
};

static uint32_t read_reg(const struct t736_cpuclock_io *io, uint32_t off)
{
	return io->read32(io->ctx, T736_CPUCLOCK_BASE + off);
}

static int delay(const struct t736_cpuclock_io *io, uint32_t us)
{
	return io->delay_us(io->ctx, us) ? T736_CPUCLOCK_DELAY : 0;
}

static int write_checked(const struct t736_cpuclock_io *io, uint32_t off,
			 uint32_t value)
{
	uint32_t before = 0, actual, ignore = 0;
	if (off != T736_CPUCLOCK_CTRL && off != T736_CPUCLOCK_CLK)
		return T736_CPUCLOCK_INVALID;
	if (off == T736_CPUCLOCK_CTRL) {
		value &= ~LOCK;
		before = read_reg(io, off);
		/* Gates are immediate. N is buffered, but hardware can latch it
		 * when ENABLE or LOCK_EN rises. Accept only the known old N or,
		 * at those two stages, the requested N. UPDATE stays strict. */
		ignore = LOCK | N_MASK;
	}
	io->write32(io->ctx, T736_CPUCLOCK_BASE + off, value);
	actual = read_reg(io, off);
	if ((actual ^ value) & ~ignore)
		return T736_CPUCLOCK_VERIFY;
	if (off == T736_CPUCLOCK_CTRL && (actual & N_MASK) != (before & N_MASK) &&
	    !((value & ENABLE) && ((~before & value) & (ENABLE | LOCK_EN)) &&
	      (actual & N_MASK) == (value & N_MASK)))
		return T736_CPUCLOCK_VERIFY;
	return 0;
}

static int update(const struct t736_cpuclock_io *io, uint32_t desired)
{
	uint32_t active = ENABLE | OUTPUT | LOCK_EN | LDO;
	if ((read_reg(io, T736_CPUCLOCK_CTRL) & UPDATE) ||
	    (desired & active) != active)
		return T736_CPUCLOCK_PRECONDITION;
	/* UPDATE depends on running PLL clocks; never issue it while disabled.
	 * Carry the software N through commit, rather than re-reading old N.
	 * The manual requires at least 1us between UPDATE operations. */
	if (delay(io, 1))
		return T736_CPUCLOCK_DELAY;
	io->write32(io->ctx, T736_CPUCLOCK_BASE + T736_CPUCLOCK_CTRL,
		    (desired & ~LOCK) | UPDATE);
	for (unsigned int i = 0; i < 1000; i++) {
		uint32_t value = read_reg(io, T736_CPUCLOCK_CTRL);
		if (!(value & UPDATE))
			return ((value ^ desired) & ~(LOCK | UPDATE)) ?
				T736_CPUCLOCK_VERIFY : 0;
		if (delay(io, 10))
			return T736_CPUCLOCK_DELAY;
	}
	return T736_CPUCLOCK_TIMEOUT;
}

static int stopped_valid(uint32_t ctrl, uint32_t stat)
{
	uint32_t n = ctrl & N_MASK;
	return ctrl == stat &&
		(ctrl & ~N_MASK) == (T736_CPUCLOCK_ORIGINAL & ~N_MASK) &&
		(n == (T736_CPUCLOCK_ORIGINAL & N_MASK) ||
		 n == (T736_CPUCLOCK_TARGET & N_MASK));
}

static int wait_lock(const struct t736_cpuclock_io *io)
{
	unsigned int consecutive = 0;
	for (unsigned int i = 0; i < 7000; i++) {
		uint32_t value = read_reg(io, T736_CPUCLOCK_CTRL);
		uint32_t status = read_reg(io, T736_CPUCLOCK_STAT);
		consecutive = ((value & LOCK) && (status & LOCK) &&
			       !(value & UPDATE)) ? consecutive + 1 : 0;
		if (consecutive == 3)
			return 0;
		if (delay(io, 10))
			return T736_CPUCLOCK_DELAY;
	}
	return T736_CPUCLOCK_TIMEOUT;
}

static int read_supply(const struct t736_cpuclock_io *io,
		       struct t736_cpuclock_supply *s)
{
	static const uint8_t regs[] = {0x0e, 0x10, 0x14, 0x15, 0x1b, 0x1d};
	uint8_t *raw = (uint8_t *)s;
	_Static_assert(sizeof(*s) == sizeof(regs), "PMIC byte snapshot layout");
	for (unsigned int i = 0; i < sizeof(regs); i++)
		if (io->pmic_read8(io->ctx, regs[i], &raw[i]))
			return T736_CPUCLOCK_PMIC;
	/* Selectors 30..60 are 800..1100mV; bit7 is preserved DVM state. */
	if (s->id != 3 || (s->enable & 0x0c) != 0x0c || (s->phase & 0xc0) ||
	    (s->dcdc3 & 0x7f) < 30 || (s->dcdc3 & 0x7f) > 60 ||
	    (s->dcdc4 & 0x7f) < 30 || (s->dcdc4 & 0x7f) > 60)
		return T736_CPUCLOCK_SUPPLY;
	return 0;
}

static int same_supply(const struct t736_cpuclock_supply *a,
		       const struct t736_cpuclock_supply *b)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	for (unsigned int i = 0; i < sizeof(*a); i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static void capture_final(const struct t736_cpuclock_io *io,
			  struct t736_cpuclock_report *r)
{
	r->final_ctrl = read_reg(io, T736_CPUCLOCK_CTRL);
	r->final_stat = read_reg(io, T736_CPUCLOCK_STAT);
	r->final_clk = read_reg(io, T736_CPUCLOCK_CLK);
	r->slow_verified = r->final_clk == 0 && read_reg(io, 0x3034) == 0;
}

static int guard_check(const struct t736_cpuclock_io *io,
		       struct t736_cpuclock_report *r)
{
	for (unsigned int i = 0; i < T736_CPUCLOCK_GUARDS; i++)
		if (read_reg(io, guard_offsets[i]) != r->guards_before[i])
			r->guard_failure = 1;
	if (io->read32(io->ctx, T736_CPUCLOCK_XO) != r->crystal ||
	    (io->read32(io->ctx, T736_CPUCLOCK_PPU6) & 0xf) != 0 ||
	    (io->read32(io->ctx, T736_CPUCLOCK_PPU7) & 0xf) != 0)
		r->guard_failure = 1;
	return r->guard_failure ? T736_CPUCLOCK_GUARD : 0;
}

static int restore_slow(const struct t736_cpuclock_io *io,
			struct t736_cpuclock_report *r)
{
	int first = 0, rc;
	uint32_t desired;
	r->rollback_attempted = 1;
	if ((io->read32(io->ctx, T736_CPUCLOCK_PPU6) & 0xf) ||
	    (io->read32(io->ctx, T736_CPUCLOCK_PPU7) & 0xf))
		return T736_CPUCLOCK_PRECONDITION;
	/* No PLL disable is allowed until slow mux is read back. */
	rc = write_checked(io, T736_CPUCLOCK_CLK, 0);
	if (rc)
		return rc;
	rc = delay(io, 20);
	if (rc) first = rc;
	desired = read_reg(io, T736_CPUCLOCK_CTRL) & ~(LOCK | UPDATE | OUTPUT);
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) first = rc;
	desired &= ~LOCK_EN;
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) first = rc;
	desired &= ~ENABLE;
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) first = rc;
	/* Once stopped, N cannot be committed. Retain actual N20 or N39; never
	 * enable an out-of-range N20 VCO just to restore a cosmetic register. */
	for (unsigned int i = 0; i < 1000; i++) {
		capture_final(io, r);
		if (r->slow_verified && stopped_valid(r->final_ctrl, r->final_stat))
			return first;
		if (delay(io, 10))
			return T736_CPUCLOCK_DELAY;
	}
	return T736_CPUCLOCK_VERIFY;
}

static int program_1014(const struct t736_cpuclock_io *io,
			struct t736_cpuclock_report *r)
{
	int rc;
	uint32_t desired = T736_CPUCLOCK_ORIGINAL | LDO;
	r->stage = 4;
	r->mutation_attempted = 1;
	rc = write_checked(io, T736_CPUCLOCK_CLK, 0);
	if (rc) return rc;
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) return rc;
	rc = delay(io, 20);
	if (rc) return rc;
	/* Manual enable order: LDO, OUTPUT, factors, ENABLE, LOCK_ENABLE,
	 * UPDATE, three lock observations, then 20us. CPU stays on slow. */
	desired |= OUTPUT;
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) return rc;
	desired = (desired & ~N_MASK) | (T736_CPUCLOCK_TARGET & N_MASK);
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) return rc;
	desired |= ENABLE;
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) return rc;
	desired |= LOCK_EN;
	rc = write_checked(io, T736_CPUCLOCK_CTRL, desired);
	if (rc) return rc;
	rc = update(io, desired);
	if (rc) return rc;
	r->stage = 5;
	rc = wait_lock(io);
	if (rc) return rc;
	rc = delay(io, 20);
	if (rc) return rc;
	if (read_reg(io, T736_CPUCLOCK_CTRL) != T736_CPUCLOCK_TARGET ||
	    read_reg(io, T736_CPUCLOCK_STAT) != T736_CPUCLOCK_TARGET)
		return T736_CPUCLOCK_VERIFY;
	r->stage = 6;
	rc = write_checked(io, T736_CPUCLOCK_CLK, T736_CPUCLOCK_TARGET_CLK);
	if (rc) return rc;
	return delay(io, 20);
}

int t736_cpuclock_prepare(const struct t736_cpuclock_io *io,
			  struct t736_cpuclock_report *r)
{
	int rc;
	if (!io || !r || !io->read32 || !io->write32 || !io->delay_us ||
	    !io->pmic_read8)
		return T736_CPUCLOCK_INVALID;
	*r = (struct t736_cpuclock_report){0};
	r->stage = 1;
	r->midr = io->midr;
	r->mpidr = io->mpidr;
	r->crystal = io->read32(io->ctx, T736_CPUCLOCK_XO);
	r->ppu6 = io->read32(io->ctx, T736_CPUCLOCK_PPU6);
	r->ppu7 = io->read32(io->ctx, T736_CPUCLOCK_PPU7);
	r->before_ctrl = read_reg(io, T736_CPUCLOCK_CTRL);
	r->before_stat = read_reg(io, T736_CPUCLOCK_STAT);
	r->before_clk = read_reg(io, T736_CPUCLOCK_CLK);
	for (unsigned int i = 0; i < T736_CPUCLOCK_GUARDS; i++)
		r->guards_before[i] = read_reg(io, guard_offsets[i]);
	capture_final(io, r);
	/* NCAT3 uses Aff1 for core index; bootstrap must be a little core. */
	if (((io->midr >> 4) & 0xfff) != 0xd05 || (io->mpidr & 0xff) ||
	    ((io->mpidr >> 8) & 0xff) >= 6 || (r->ppu6 & 0xf) || (r->ppu7 & 0xf) ||
	    (r->crystal & (BIT(14) | BIT(15))) != BIT(15) ||
	    r->before_ctrl != T736_CPUCLOCK_ORIGINAL ||
	    r->before_stat != T736_CPUCLOCK_ORIGINAL || r->before_clk != 0 ||
	    read_reg(io, 0x3034) != 0 || read_reg(io, 0x2004) != 0 ||
	    read_reg(io, 0x2008) != UINT32_C(0x30000000) ||
	    read_reg(io, 0x2018) != UINT32_C(0x40000001)) {
		r->error = T736_CPUCLOCK_PRECONDITION;
		return r->error;
	}
	r->stage = 2;
	rc = read_supply(io, &r->supply_before);
	if (rc) {
		r->error = rc;
		return rc;
	}
	r->stage = 3;
	rc = guard_check(io, r);
	if (!rc)
		rc = program_1014(io, r);
	if (!rc) {
		r->stage = 7;
		capture_final(io, r);
		if (r->final_ctrl != T736_CPUCLOCK_TARGET ||
		    r->final_stat != T736_CPUCLOCK_TARGET ||
		    r->final_clk != T736_CPUCLOCK_TARGET_CLK)
			rc = T736_CPUCLOCK_VERIFY;
	}
	if (!rc)
		rc = read_supply(io, &r->supply_after);
	if (!rc && !same_supply(&r->supply_before, &r->supply_after))
		rc = T736_CPUCLOCK_SUPPLY;
	if (guard_check(io, r))
		rc = T736_CPUCLOCK_GUARD;
	if (rc && r->mutation_attempted)
		r->rollback_error = restore_slow(io, r);
	capture_final(io, r);
	if (guard_check(io, r))
		rc = T736_CPUCLOCK_GUARD;
	r->error = rc;
	if (!rc)
		r->stage = 8;
	return rc;
}

#ifdef T736_CPUCLOCK
#include "platform.h"
extern int printf(const char *, ...);
static uint32_t boot_read(void *ctx, uint32_t address)
{
	(void)ctx;
	return t736_read32(address);
}
static void boot_write(void *ctx, uint32_t address, uint32_t value)
{
	(void)ctx;
	t736_write32(address, value);
}
static int boot_delay(void *ctx, uint32_t us)
{
	(void)ctx;
	return t736_delay_us(us);
}
int t736_cpuclock_boot(int (*pmic_read8)(void *, uint8_t, uint8_t *), void *ctx)
{
	struct t736_cpuclock_report r;
	struct t736_cpuclock_io io = {ctx, boot_read, boot_write, boot_delay,
		pmic_read8, 0, 0};
	int rc;
	__asm__ volatile("mrc p15,0,%0,c0,c0,0" : "=r"(io.midr));
	__asm__ volatile("mrc p15,0,%0,c0,c0,5" : "=r"(io.mpidr));
	rc = t736_cpuclock_prepare(&io, &r);
	printf("CPU5 %d/%u core=%x/%x xo=%x ppu=%x/%x\n",
	       rc, r.stage, r.midr, r.mpidr, r.crystal, r.ppu6, r.ppu7);
	printf("B %x/%x/%x>%x/%x/%x m=%u r=%u/%d s=%u g=%u\n",
	       r.before_ctrl, r.before_stat, r.before_clk, r.final_ctrl, r.final_stat,
	       r.final_clk, r.mutation_attempted, r.rollback_attempted,
	       r.rollback_error, r.slow_verified, r.guard_failure);
	printf("PM %x/%x/%x/%x/%x/%x>%x/%x/%x/%x/%x/%x\n",
	       r.supply_before.id, r.supply_before.enable, r.supply_before.dcdc3,
	       r.supply_before.dcdc4, r.supply_before.phase, r.supply_before.feedback,
	       r.supply_after.id, r.supply_after.enable, r.supply_after.dcdc3,
	       r.supply_after.dcdc4, r.supply_after.phase, r.supply_after.feedback);
	if (!rc) {
		printf("CPU5 A76=1014MHz(N39/1), guards OK\n");
		return 0;
	}
	if (r.slow_verified && stopped_valid(r.final_ctrl, r.final_stat) &&
	    !r.rollback_error && !r.guard_failure) {
		printf("CPU5 FAIL slow/PLL-off; N retained\n");
		return 1;
	}
	printf("CPU5 UNSAFE halt/reset\n");
	return -1;
}
#endif
