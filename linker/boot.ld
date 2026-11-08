ENTRY(boot_reset)
MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K }
SECTIONS {
 .vectors : { KEEP(*(.vectors)) } > FLASH
 .text : { *(.text*) *(.rodata*) } > FLASH
 /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.note*) }
}
