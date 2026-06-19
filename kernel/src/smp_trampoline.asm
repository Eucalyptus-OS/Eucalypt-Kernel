extern ap_entry
extern ap_stack_tops

global smp_trampoline

smp_trampoline:
    mov rax, [rdi + 24]
    mov rdx, ap_stack_tops
    mov rsp, [rdx + rax * 8]
    mov rdi, rax
    mov rax, ap_entry
    call rax
    
    cli
    hlt
    jmp $ - 1
