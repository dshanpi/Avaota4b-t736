/* SPDX-License-Identifier: GPL-2.0-only */
.section .text.entry,"ax"
.global _start
_start:
 msr daifset,#15
 ldr x0,=0x02500000
 mov w1,#'A'
 str w1,[x0]
 adr x0,vectors
 msr vbar_el3,x0
 isb
 ldr x0,=0x47020000
 mov sp,x0
 bl stage_main
 dsb sy
 ic iallu
 dsb sy
 isb
 mov x0,xzr
 mov x1,xzr
 mov x2,xzr
 mov x3,xzr
 ldr x4,=0x48000000
 br x4
.balign 2048
vectors:
.rept 16
 b fault
 .balign 128
.endr
fault:
 msr daifset,#15
 mrs x6,esr_el3
 mrs x7,elr_el3
 mov x8,#2
 ldr x2,=0x02500000
 mov w0,#'!'
 str w0,[x2]
1: mov x4,#60
2: lsr x0,x6,x4
 and x0,x0,#15
 add w0,w0,#48
 cmp w0,#58
 b.lo 3f
 add w0,w0,#39
3: ldr w1,[x2,#0x14]
 tbz w1,#6,3b
 str w0,[x2]
 subs x4,x4,#4
 b.ge 2b
 mov x6,x7
 subs x8,x8,#1
 b.ne 1b
4: wfi
 b 4b
.section .note.GNU-stack,"",%progbits
