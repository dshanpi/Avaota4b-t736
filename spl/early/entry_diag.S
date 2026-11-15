/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional SRAM entry with an independent 128-byte exception record.
 * Link .fault_report at 0x4ed00, after the largest current report (0x4e930)
 * and before stack bottom (0x4f000). Never replace a frozen entry.S build.
 * No exception-stack use; UART is the only peripheral written by faults.
 */
.syntax unified
.arm
.section .text.entry,"ax",%progbits
.global _start
.type _start,%function
_start:
 b enter
.balign 32
.global diag_vectors
diag_vectors:
 b diag_vector_reset
 b diag_vector_undef
 b diag_vector_svc
 b diag_vector_prefetch
 b diag_vector_data
 b diag_vector_reserved
 b diag_vector_irq
 b diag_vector_fiq
enter:
 mrs r0, cpsr
 and r1, r0, #0x1f
 cmp r1, #0x13
 bne reject
 mrc p15, 0, r1, c1, c0, 0
 tst r1, #5
 bne reject
 tst r1, #0x2000
 bne reject
 cpsid if
 ldr r12, =saved_context
 stmia r12, {r4-r11, sp, lr}
 str r0, [r12, #40]
 mrc p15, 0, r1, c12, c0, 0
 str r1, [r12, #44]
 ldr sp, =__stack_top
 /* Clear before installing our vectors so any record is from this run. */
 ldr r1, =diag_fault_record
 add r2, r1, #128
 mov r3, #0
1:
 str r3, [r1], #4
 cmp r1, r2
 blo 1b
 ldr r1, =diag_vectors
 mcr p15, 0, r1, c12, c0, 0
 isb
 ldr r1, =__bss_start
 ldr r2, =__bss_end
2:
 cmp r1, r2
 strlo r3, [r1], #4
 blo 2b
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
.size _start, .-_start

/* Branches preserve architectural exception LR and SPSR. */
.macro DIAG_VECTOR name, number
.global diag_vector_\name
diag_vector_\name:
 mov r0, #\number
 b diag_fault
.endm
DIAG_VECTOR reset, 0
DIAG_VECTOR undef, 1
DIAG_VECTOR svc, 2
DIAG_VECTOR prefetch, 3
DIAG_VECTOR data, 4
DIAG_VECTOR reserved, 5
DIAG_VECTOR irq, 6
DIAG_VECTOR fiq, 7

.type diag_fault,%function
diag_fault:
 mrs r12, cpsr
 cpsid if
 ldr r1, =diag_fault_record
 ldr r2, [r1]
 ldr r3, =0x54374644
 cmp r2, r3
 bne 3f
 ldr r2, [r1, #8]
 cmp r2, #0
 bne diag_nested
3:
 str r3, [r1]       /* 0: magic */
 mov r2, #1
 str r2, [r1, #4]   /* 4: version */
 str r2, [r1, #8]   /* 8: 1=capture underway, 2=captured */
 str r0, [r1, #12]  /* 12: vector index */
 str r12, [r1, #16] /* 16: exception CPSR, before our CPSID */
 mrs r2, spsr
 str r2, [r1, #20]  /* 20: raw SPSR */
 str lr, [r1, #24]  /* 24: raw exception LR, no guessed PC */
 /* Validity: bit0 SPSR, bit1 data-fault pair, bit2 instruction-fault pair,
  * bit3 SCTLR, bit4 VBAR. Other fault registers may contain stale values. */
 mov r2, #0
 cmp r0, #0
 beq 4f
 cmp r0, #5
 orrne r2, r2, #1
4:
 str r2, [r1, #64]
 mrc p15, 0, r2, c5, c0, 0
 str r2, [r1, #28]  /* DFSR */
 mrc p15, 0, r2, c6, c0, 0
 str r2, [r1, #32]  /* DFAR */
 ldr r3, [r1, #64]
 cmp r0, #4
 orreq r3, r3, #2
 str r3, [r1, #64]
 mrc p15, 0, r2, c5, c0, 1
 str r2, [r1, #36]  /* IFSR */
 mrc p15, 0, r2, c6, c0, 2
 str r2, [r1, #40]  /* IFAR */
 ldr r3, [r1, #64]
 cmp r0, #3
 orreq r3, r3, #4
 str r3, [r1, #64]
 mrc p15, 0, r2, c1, c0, 0
 str r2, [r1, #52]  /* SCTLR */
 ldr r3, [r1, #64]
 orr r3, r3, #8
 str r3, [r1, #64]
 mrc p15, 0, r2, c12, c0, 0
 str r2, [r1, #56]  /* VBAR */
 orr r3, r3, #16
 str r3, [r1, #64]
 mov r2, #2
 str r2, [r1, #8]
 dsb sy             /* Preserve evidence even when UART is unavailable. */
 mov r4, r1
 adr r5, diag_labels
 add r6, r4, #12
 mov r7, #8
5:
 ldrb r0, [r5], #1
 bl diag_tx
 cmp r0, #0
 bne diag_uart_failed
 mov r0, #'='
 bl diag_tx
 cmp r0, #0
 bne diag_uart_failed
 ldr r8, [r6], #4
 mov r9, #28
6:
 lsr r0, r8, r9
 and r0, r0, #15
 cmp r0, #9
 addls r0, r0, #'0'
 addhi r0, r0, #('a'-10)
 bl diag_tx
 cmp r0, #0
 bne diag_uart_failed
 subs r9, r9, #4
 bpl 6b
 mov r0, #13
 bl diag_tx
 cmp r0, #0
 bne diag_uart_failed
 mov r0, #10
 bl diag_tx
 cmp r0, #0
 bne diag_uart_failed
 subs r7, r7, #1
 bne 5b
 /* Bound and drain the final byte before WFI. */
 ldr r1, =0x02500000
 ldr r2, =1000000
7:
 ldr r3, [r1, #0x14]
 dsb sy
 tst r3, #0x40
 bne 8f
 subs r2, r2, #1
 bne 7b
 mov r0, #3
 b diag_uart_failed
8:
 mov r0, #1
 str r0, [r4, #48]  /* uart_completed */
 b diag_stop
diag_uart_failed:
 str r0, [r4, #44]  /* 1=DLAB, 2=THRE timeout, 3=TEMT timeout */
 b diag_stop
diag_nested:
 /* Do not overwrite the first fault or recurse through more CP15/UART. */
 ldr r2, [r1, #60]
 add r2, r2, #1
 str r2, [r1, #60]
 str lr, [r1, #68]
 str r12, [r1, #72]
 str r0, [r1, #76]
diag_stop:
 dsb sy
9:
 wfi
 b 9b
.size diag_fault, .-diag_fault

/* Stackless leaf. Clobbers r0-r3 only, preserving the formatting loop. */
.type diag_tx,%function
diag_tx:
 ldr r1, =0x02500000
 ldr r3, [r1, #0x0c]
 dsb sy
 tst r3, #0x80
 movne r0, #1
 bxne lr
 ldr r2, =1000000
10:
 ldr r3, [r1, #0x14]
 dsb sy
 tst r3, #0x20
 bne 11f
 subs r2, r2, #1
 bne 10b
 mov r0, #2
 bx lr
11:
 str r0, [r1]
 dsb sy
 mov r0, #0
 bx lr
.size diag_tx, .-diag_tx
diag_labels:
 .ascii "FCSLDAIP"
.balign 4
.ltorg
.section .data.context,"aw",%progbits
.balign 8
saved_context:
 .space 48
.section .fault_report,"aw",%nobits
.balign 4
.global diag_fault_record
diag_fault_record:
 .space 128
.section .note.GNU-stack,"",%progbits
