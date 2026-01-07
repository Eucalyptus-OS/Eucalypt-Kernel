global isr128_handler
extern syscall_handler

isr128_handler:
    sti
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx

    mov rdi, rax
    mov rsi, rbx
    mov r8, rdx
    mov rdx, rcx
    mov rcx, r8

    call syscall_handler

    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11

    iretq