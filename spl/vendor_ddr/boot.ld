/* Callable FEL image, entirely within the prior golden FES upload range. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY { sram (rwx) : ORIGIN = 0x47000, LENGTH = 0x38c00 }
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 .text : { KEEP(*(.text.entry)) *(.text .text.*) } > sram :text
 .rodata : ALIGN(8) { *(.rodata .rodata.*) } > sram :text
 .data : ALIGN(8) { *(.data .data.*) } > sram :data
 .bss (NOLOAD) : ALIGN(8) {
  __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(8); __bss_end = .;
 } > sram :data
 ASSERT(__bss_end <= 0x7d000, "library overlaps report/stack")
 .report 0x7d000 (NOLOAD) : { KEEP(*(.report)) } > sram :data
 .fault_report 0x7d400 (NOLOAD) : { KEEP(*(.fault_report)) } > sram :data
 .stack 0x7dc00 (NOLOAD) : {
  __stack_bottom = .; . += 0x2000; __stack_top = .;
 } > sram :data
 /DISCARD/ : { *(.comment .note* .ARM.exidx* .ARM.extab*) }
 ASSERT(_start == 0x47000, "entry mismatch")
 ASSERT(SIZEOF(.report) == 512, "report ABI changed")
 ASSERT(__stack_top == 0x7fc00, "SRAM range mismatch")
}
