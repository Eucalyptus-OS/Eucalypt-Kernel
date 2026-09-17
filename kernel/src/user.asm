[BITS 64]

global jump_to_user

USER_CS equ 0x20 | 3
USER_SS equ 0x18 | 3

jump_to_user:
    mov ax, USER_SS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push USER_SS
    push rsi
    push 0x202
    push USER_CS
    push rdi

    iretq