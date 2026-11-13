/* SPDX-License-Identifier: GPL-2.0-only */
.syntax unified
.arch armv8-a
.arm
.section .text.start, "ax"
.global _start
_start:
	cpsid if
	ldr sp, =0x51e00000
	bl handoff_main
1:
	wfi
	b 1b
