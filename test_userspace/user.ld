ENTRY(_start)
SECTIONS
{
    . = 0x400000;

    .text   : { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }   /* <-- string literals live here */
    . = ALIGN(4096);
    .data   : { *(.data .data.*) }
    . = ALIGN(4096);
    .bss    : { *(.bss .bss.* COMMON) }

    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}