/* SPDX-License-Identifier: GPL-2.0-only */
.syntax unified
.arm
.section .text.entry,"ax",%progbits
.global _start
.type _start,%function
_start:
	b enter
.balign 32
vectors:
	b fault
	b fault
	b fault
	b fault
	b fault
	b fault
	b fault
	b fault
enter:
	/* A callable FEL probe, not an eGON image or normal-boot SPL.
	 * Require privileged SVC and MMU/D-cache off. Do not touch caller stack.
	 */
	mrs r0, cpsr
	and r1, r0, #0x1f
	cmp r1, #0x13
	bne reject
	mrc p15, 0, r1, c1, c0, 0
	tst r1, #5
	bne reject
	tst r1, #0x2000 /* VBAR must govern exceptions, not high vectors. */
	bne reject
	cpsid if
	ldr r12, =saved_context
	stmia r12, {r4-r11, sp, lr}
	str r0, [r12, #40]
	mrc p15, 0, r1, c12, c0, 0
	str r1, [r12, #44]
	ldr sp, =__stack_top
	ldr r1, =vectors
	mcr p15, 0, r1, c12, c0, 0
	isb
	ldr r1, =__bss_start
	ldr r2, =__bss_end
	mov r3, #0
1:
	cmp r1, r2
	strlo r3, [r1], #4
	blo 1b
	bl probe_main
	ldr r12, =saved_context
	ldr r1, [r12, #44]
	mcr p15, 0, r1, c12, c0, 0
	dsb sy
	isb
	ldr r1, [r12, #40]
	ldmia r12, {r4-r11, sp, lr}
	msr cpsr_c, r1
	bx lr
reject:
	mov r0, #0x100
	bx lr
fault:
	/* No stack use in abort/undefined modes. Host must reset on fault. */
	ldr r0, =probe_report
	ldr r1, =0xdead0001
	str r1, [r0, #8]
	dsb sy
2:
	wfi
	b 2b
.size _start, .-_start
.section .data.context,"aw",%progbits
.balign 8
saved_context:
	.space 48
.section .note.GNU-stack,"",%progbits
