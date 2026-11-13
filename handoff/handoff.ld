/* SPDX-License-Identifier: GPL-2.0-only */
ENTRY(_start)
SECTIONS {
	. = 0x4a001000;
	.text : { *(.text.start) *(.text*) *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) }
}
