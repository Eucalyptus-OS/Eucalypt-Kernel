extern ap_entry
extern ap_stack_tops

global smp_trampoline

smp_trampoline:
    mov rax, [rdi + 24]
    lea rdx, [rel ap_stack_tops]
    mov rsp, [rdx + rax * 8]
    mov rdi, rax
    call ap_entry
    
    cli
    hlt
    jmp $ - 1
