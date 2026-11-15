// SPDX-License-Identifier: GPL-2.0-only
#include "platform.h"
#include "twi.h"

#define PIO 0x02000000u
#define CCU 0x02002000u
#define UART 0x02500000u
#define PRCM 0x07010000u
#define RPIO 0x07025000u
#define TWI0 0x07083000u
#define PMIC 0x36u

/* Fixed SRAM report at 0x4e000, 16 little-endian uint32_t words. */
struct report {
	uint32_t magic, version, status, counter_hz, timer_delta;
	uint32_t pmic_id, twi_status, cleanup_error;
	uint32_t rail_registers[6], sctlr, raw_cntfrq;
};
volatile struct report probe_report __attribute__((section(".report")));
extern char __stack_bottom[], __stack_top[];

static void uart_init(void)
{
	/* PB9/PB10, function 2; T736 main PIO uses 0x80 bank stride. */
	t736_clrset32(PIO + 0x104, 0xff0, 0x220);
	/* APB_UART = SYS_CLK24M / 1. UART0 clock/reset only. */
	t736_clrset32(CCU + 0x538, 0x0700001f, 0);
	t736_clrset32(CCU + 0xe00, 0, (1u << 16) | 1u);
	t736_write32(UART + 0x04, 0);
	t736_write32(UART + 0x0c, 0x80);
	t736_write32(UART + 0x00, 13); /* 115384 baud at 24 MHz, error +0.16%. */
	t736_write32(UART + 0x04, 0);
	t736_write32(UART + 0x0c, 3);
	t736_write32(UART + 0x08, 7);
}

static void putc_uart(char c)
{
	uint32_t n;
	for (n = 0; n < T736_POLL_LIMIT; n++)
		if (t736_read32(UART + 0x14) & 0x20) {
			t736_write32(UART, (uint8_t)c);
			return;
		}
}

static void puts_uart(const char *s)
{
	for (; *s; s++) {
		if (*s == '\n')
			putc_uart('\r');
		putc_uart(*s);
	}
}

static void hex(uint32_t value)
{
	int shift;
	for (shift = 28; shift >= 0; shift -= 4)
		putc_uart("0123456789abcdef"[(value >> shift) & 15]);
}

static void field(const char *name, uint32_t value)
{
	puts_uart(name);
	hex(value);
	puts_uart("\n");
}

static uint32_t bus_read(void *ctx, uint32_t offset)
{
	(void)ctx;
	return t736_read32(TWI0 + offset);
}

static void bus_write(void *ctx, uint32_t offset, uint32_t value)
{
	(void)ctx;
	t736_write32(TWI0 + offset, value);
}

static uint32_t bus_ticks(void *ctx)
{
	(void)ctx;
	return t736_counter_low();
}

int probe_main(void)
{
	static const uint8_t registers[] = {0x10, 0x19, 0x20, 0x27, 0x22, 0x3a};
	static struct t736_twi bus = {
		.ctx = 0, .read = bus_read, .write = bus_write, .ticks = bus_ticks,
		.timeout_ticks = 240000, .poll_limit = T736_POLL_LIMIT
	};
	uint32_t i, begin;
	uint8_t value = 0;
	int rc;
	volatile uint32_t *words = (volatile uint32_t *)&probe_report;
	for (i = 0; i < 16; i++)
		words[i] = 0;
	probe_report.magic = 0x54373336; /* T736 */
	probe_report.version = 2;
	probe_report.status = 1; /* entered */
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(i));
	probe_report.sctlr = i;
	uart_init();
	puts_uart("T736 SRAM source probe v2; NO DDR INITIALIZATION\n");
	field("stack.bottom=", (uintptr_t)__stack_bottom);
	field("stack.top=", (uintptr_t)__stack_top);
	probe_report.raw_cntfrq = t736_counter_frequency();
	field("counter.raw_cntfrq=", probe_report.raw_cntfrq);
	rc = t736_timer_init();
	probe_report.counter_hz = t736_timer_observation.effective_hz;
	field("timestamp.ctrl.before=", t736_timer_observation.ctrl_before);
	field("timestamp.freqid.before=", t736_timer_observation.freqid_before);
	field("timestamp.advanced.before=", t736_timer_observation.advanced_before);
	field("timestamp.ctrl.after=", t736_timer_observation.ctrl_after);
	field("timestamp.freqid.after=", t736_timer_observation.freqid_after);
	field("counter.effective_hw_hz=", probe_report.counter_hz);
	if (rc) {
		probe_report.status = 0x101;
		puts_uart("FAIL: physical counter unavailable or wrong frequency\n");
		return rc;
	}
	begin = t736_counter_low();
	rc = t736_delay_us(10);
	probe_report.timer_delta = t736_counter_low() - begin;
	if (rc) {
		probe_report.status = 0x102;
		return rc;
	}
	/* Read existing autonomous-master state before changing APBS1/pins. */
	t736_clrset32(PRCM + 0x19c, 0, (1u << 16) | 1u);
	if ((bus_read(0, TWI_DRV_CTRL) & 0x80000001u) ||
	    (bus_read(0, TWI_PMU_CTRL) & 0xff)) {
		probe_report.status = 0x103;
		puts_uart("FAIL: active TWI packet/DVFS engine\n");
		return TWI_BUSY;
	}
	/* APBS1 = SYS_CLK24M / 1; PL0/PL1 function 2, preserve other pins. */
	t736_clrset32(PRCM + 0x10, 0x0700001f, 4u << 24);
	t736_clrset32(RPIO, 0xff, 0x22);
	rc = t736_twi_init(&bus);
	if (!rc)
		rc = t736_twi_read8(&bus, PMIC, 0x0e, &value);
	probe_report.pmic_id = value;
	field("pmic.id=", value);
	if (!rc && value != 3)
		rc = -8;
	for (i = 0; !rc && i < sizeof(registers); i++) {
		rc = t736_twi_read8(&bus, PMIC, registers[i], &value);
		if (!rc) {
			probe_report.rail_registers[i] = value;
			puts_uart("pmic.reg="); hex(registers[i]);
			puts_uart(" value="); hex(value); puts_uart("\n");
		}
	}
	probe_report.twi_status = bus.last_status;
	probe_report.cleanup_error = (uint32_t)bus.cleanup_error;
	probe_report.status = rc ? 0x200u + (uint32_t)(-rc) : 2;
	field("probe.status=", probe_report.status);
	puts_uart("PMIC writes: 0; DDR tested: NO; returning to FEL caller\n");
	__asm__ volatile("dsb sy" ::: "memory");
	return rc;
}
