/* SPDX-License-Identifier: GPL-2.0-only */
/* NCAT3 CPUCFG layout from TF-A 7d322909 sunxi_cpucfg_ncat3.h. */
.syntax unified
.arm
.section .text.entry,"ax",%progbits
.global _start
_start:
 cpsid if
 ldr r5,=0x02500000
 mov r0,#'H'
 bl tx
 mrc p15,0,r0,c1,c0,0
 mov r7,r0
 bl hex
 mov r0,r7
 tst r0,#5
 bne reject
 mrc p15,0,r0,c0,c0,5
 mov r7,r0
 bl hex
 mov r0,r7
 /* NCAT3 cores are affinity level 1, Aff0 must be zero. */
 tst r0,#0xff
 bne reject
 ubfx r0,r0,#8,#8
 cmp r0,#7
 bhi reject
 ldr r1,=0x08001000
 add r1,r1,r0,lsl #12
 ldr r2,=0x47000000
 str r2,[r1,#4]
 mov r3,#0
 str r3,[r1,#8]
 ldr r2,[r1]
 orr r2,r2,#1
 str r2,[r1]
 mov r4,r1
 ldr r0,[r4,#4]
 bl hex
 ldr r0,[r4]
 bl hex
 mov r2,#0
 mcr p15,0,r2,c7,c5,0
 dsb sy
 isb
 mrc p15,0,r2,c12,c0,2
 mov r7,r2
 mov r0,r2
 bl hex
 mov r0,#10
 bl tx
 mov r2,r7
 orr r2,r2,#3
 mcr p15,0,r2,c12,c0,2
 isb
1: wfi
 b 1b
reject:
 mov r0,#'!'
 bl tx
 b 1b
/* Stackless UART diagnostics, clobber r0-r3/r8-r9, retain r4-r7. */
tx:
 ldr r3,=1000000
2: ldr r2,[r5,#0x14]
 tst r2,#0x40
 bne 3f
 subs r3,r3,#1
 bne 2b
 bx lr
3: str r0,[r5]
 bx lr
hex:
 mov r8,lr
 mov r9,r0
 mov r6,#28
4: lsr r0,r9,r6
 and r0,r0,#15
 cmp r0,#10
 addlo r0,r0,#48
 addhs r0,r0,#87
 bl tx
 subs r6,r6,#4
 bpl 4b
 mov r0,#' '
 bl tx
 bx r8
.section .note.GNU-stack,"",%progbits
