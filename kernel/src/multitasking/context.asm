global thread_exit_switch
global thread_trampoline
extern schedule
extern current_thread
extern handle_ret 

thread_exit_switch:
    cli
    xor rdi, rdi
    call schedule
    mov rsp, rax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

thread_trampoline:
    sti
    call rbx
    cli
    mov rdi, rax
    call handle_ret
    ud2
.hang:
    cli
    hlt
    jmp .hang