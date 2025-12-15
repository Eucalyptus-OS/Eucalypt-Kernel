ENTRY(_start)
OUTPUT_FORMAT(binary)

SECTIONS {
    . = 0x10000;
    .text : {
        *(.text)
    }
    .data : {
        *(.data)
    }
    .bss : {
        *(.bss)
    }
    .rodata : {
        *(.rodata)
    }
}