// SPDX-License-Identifier: GPL-2.0-only
/* T736 vendor BL31 ABI adapter. No storage or DDR initialization. */
typedef unsigned int u32;

static void puts_uart(const char *text)
{
	volatile u32 *uart = (volatile u32 *)0x02500000;

	while (*text) {
		while (!(uart[5] & (1U << 5)))
			;
		uart[0] = *text++;
	}
}

static void copy_bytes(unsigned long dest, unsigned long source, u32 size)
{
	volatile unsigned char *d = (volatile unsigned char *)dest;
	const unsigned char *s = (const unsigned char *)source;

	while (size--)
		*d++ = *s++;
}

static u32 smc(u32 id, u32 arg1, u32 arg2, u32 arg3)
{
	register u32 r0 __asm__("r0") = id;
	register u32 r1 __asm__("r1") = arg1;
	register u32 r2 __asm__("r2") = arg2;
	register u32 r3 __asm__("r3") = arg3;

	__asm__ volatile("dsb sy\n\tsmc #0\n\tisb" : "+r"(r0), "+r"(r1),
			 "+r"(r2), "+r"(r3) : : "r12", "lr", "cc", "memory");
	return r0;
}

void handoff_main(void)
{
	u32 control, zero = 0;

	puts_uart("\r\nT736 independent AArch32 handoff v0.1\r\n");
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(control));
	/* Refuse an unsafe copy if a RAM test caller leaves MMU or D-cache on. */
	if (control & 5) {
		puts_uart("handoff: MMU/D-cache must be disabled before entry\r\n");
		return;
	}
	copy_bytes(0x52000000, 0x4a010000, UBOOT_BYTES);
	copy_bytes(0x41000000, 0x4a080000, DTB_BYTES);
	__asm__ volatile("dsb sy\n\tmcr p15, 0, %0, c7, c5, 0\n\tisb"
			 : : "r"(zero) : "memory");
	puts_uart("handoff: starting vendor SCP with board firmware DTB\r\n");
	smc(0x8000ff10, 0x41000000, 0, 0);
	puts_uart("handoff: entering upstream U-Boot at 0x52000000\r\n");
	smc(0x8000ff04, 0x52000000, 0x41000000, 1);
	puts_uart("handoff: BL31 returned unexpectedly\r\n");
}
