/* SPDX-License-Identifier: GPL-2.0-only */
/* Offline link/emulation stub only. Not a hardware payload. */
.syntax unified
.arm
.section .text.probe_main,"ax",%progbits
.global probe_main
probe_main:
 mov r0, #0
 bx lr
.section .report,"aw",%nobits
.global probe_report
probe_report:
 .space 2352
.section .note.GNU-stack,"",%progbits
