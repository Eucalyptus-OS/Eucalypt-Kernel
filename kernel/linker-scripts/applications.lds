ENTRY(main)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS {
    . = 0x10000;
    
    .text : {
        *(.text*)
    }
    
    .rodata : ALIGN(16) {
        *(.rodata*)
    }
    
    .data : ALIGN(16) {
        *(.data*)
    }
    
    .bss : ALIGN(16) {
        *(.bss*)
        *(COMMON)
    }
}