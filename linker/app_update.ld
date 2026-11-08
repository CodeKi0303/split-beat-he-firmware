ENTRY(Reset_Handler)
MEMORY {
 FLASH (rx) : ORIGIN = 0x08020000, LENGTH = 0x3FFE0
 DTCM (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
 AXISRAM (xrw) : ORIGIN = 0x24000000, LENGTH = 320K
}
_estack = ORIGIN(DTCM) + LENGTH(DTCM);
__settings_base__ = 0x080C0000;
__update_metadata_base__ = 0x0805FFE0;
SECTIONS {
 .isr_vector : { . = ALIGN(1024); KEEP(*(.isr_vector)) } > FLASH
 .fw_identity ORIGIN(FLASH) + 0x400 : { KEEP(*(.fw_identity)) } > FLASH
 .text : { *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); } > FLASH
 .settings_layout : { KEEP(*(.settings_layout)) } > FLASH
 .ARM.extab : { *(.ARM.extab*) } > FLASH
 .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > FLASH
 .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } > FLASH
 .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE_HIDDEN(__init_array_end = .); } > FLASH
 .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) PROVIDE_HIDDEN(__fini_array_end = .); } > FLASH
 _sidata = LOADADDR(.data);
 .data : { . = ALIGN(4); _sdata = .; *(.data*) . = ALIGN(4); _edata = .; } > DTCM AT> FLASH
 .bss (NOLOAD) : { . = ALIGN(4); _sbss = .; *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; } > DTCM
 .heap_stack (NOLOAD) : { . = ALIGN(8); end = .; _end = .; . += 4096; . += 8192; . = ALIGN(8); } > DTCM
 .dma_buffer (NOLOAD) : { . = ALIGN(32); __dma_start__ = .; KEEP(*(.dma_buffer*)) . = ALIGN(32); __dma_end__ = .; } > AXISRAM
 /DISCARD/ : { *(.note.GNU-stack) }
 .ARM.attributes 0 : { *(.ARM.attributes) }
}
ASSERT(SIZEOF(.isr_vector) <= 0x400, "Vector table overlaps fixed image identity")
ASSERT(SIZEOF(.fw_identity) == 32, "Missing or duplicate C86 application identity")
ASSERT(_ebss + 12288 <= _estack, "DTCM overflow")
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= __update_metadata_base__, "App overlaps commit metadata")
