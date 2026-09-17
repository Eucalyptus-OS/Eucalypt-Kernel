[BITS 64]

global reload

; After GDT setup, reload CS and the data segments from the new descriptors.
reload:
    ; Far return: pop CS=0x08 and resume at .reload_cs.
    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    ; Point all data segments at the kernel data selector (0x10).
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret