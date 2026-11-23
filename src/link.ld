MEMORY { CODE (rx) : ORIGIN = 0x080E7600, LENGTH = 0x18000 }
SECTIONS {
  .text 0x080E7600 : {
    KEEP(*(.text.entry))
    *(.text) *(.text.*)
    *(.rodata) *(.rodata.*)
  } > CODE
  /DISCARD/ : { *(.ARM.exidx*) *(.comment) *(.debug*) *(.bss*) *(.data*) }
}
