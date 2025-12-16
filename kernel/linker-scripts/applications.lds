ENTRY(_start)
OUTPUT_FORMAT(binary)
SECTIONS {
    . = 0x10000;
    
    .header : {
        BYTE(0x42)
        BYTE(0x49)
        BYTE(0x4E)
        BYTE(0x00)
    }
    
    .text : ALIGN(16) {
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