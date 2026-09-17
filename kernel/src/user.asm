[BITS 64]

global jump_to_user

; User-mode segment selectors (RPL 3): CS = GDT index 4, SS = index 3.
USER_CS equ 0x20 | 3
USER_SS equ 0x18 | 3

; Enter user mode: entry (RDI) and stack (RSI) are pushed as an iretq frame.
jump_to_user:
    ; Clear the user data segments one register at a time.
    mov ax, USER_SS
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; iretq frame: SS, RSP, RFLAGS (0x202 = IF set, IOPL 0), CS, RIP.
    push USER_SS
    push rsi
    push 0x202
    push USER_CS
    push rdi

    iretq