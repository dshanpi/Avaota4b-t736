/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY { sram (rwx) : ORIGIN = 0x48c00, LENGTH = 0x7400 }
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 .text : { KEEP(*(.text.entry)) *(.text .text.*) } > sram :text
 .rodata : ALIGN(4) { *(.rodata .rodata.*) } > sram :text
 .data : ALIGN(8) { *(.data .data.*) } > sram :data
 .bss (NOLOAD) : ALIGN(8) { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(8); __bss_end = .; } > sram :data
 ASSERT(__bss_end <= 0x4e000, "overlaps report")
 .report 0x4e000 (NOLOAD) : { KEEP(*(.report)) } > sram :data
 .stack 0x4f000 (NOLOAD) : { __stack_bottom = .; . += 0x1000; __stack_top = .; } > sram :data
 /DISCARD/ : { *(.comment .note* .ARM.exidx* .ARM.extab*) }
 ASSERT(_start == 0x48c00, "wrong entry")
 ASSERT(SIZEOF(.report) == 2352, "wrong report")
 ASSERT(ADDR(.report) + SIZEOF(.report) <= 0x4f000, "report overlaps stack")
 ASSERT(__stack_top == 0x50000, "wrong stack")
}
