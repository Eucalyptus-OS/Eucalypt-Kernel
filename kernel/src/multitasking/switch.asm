[BITS 64]

extern current_tcb
extern tss_set_kernel_stack

global switch_task
global fork_child_restore
global exec_switch_resume

struc tcb
    .tid:         resq 1
    .ksp:         resq 1
    .kstack:      resq 1
    .tsp:         resq 1
    .addr_space:  resq 1
    .next:        resq 1
    .proc_next:   resq 1
    .parent:      resq 1
    .fpu_area:    resq 1
    .state:       resb 1
    .fs_base:     resq 1
endstruc

switch_task:
    pushfq
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rax, [rel current_tcb]
    test rax, rax
    jz .first_switch

    mov rsi, [rax + tcb.fpu_area]
    fxsave [rsi]

    mov [rax + tcb.ksp], rsp

.first_switch:
    mov [rel current_tcb], rdi

    push rdi
    mov rdi, [rdi + tcb.kstack]
    sub rsp, 8
    call tss_set_kernel_stack
    add rsp, 8
    pop rdi

    mov rsp, [rdi + tcb.ksp]

    mov rsi, [rdi + tcb.fpu_area]
    fxrstor [rsi]

    mov rax, [rdi + tcb.addr_space]
    mov rcx, cr3

    cmp rax, rcx
    je .same_cr3

    mov cr3, rax

.same_cr3:
    mov rcx, 0xC0000100
    mov rax, [rdi + tcb.fs_base]
    mov edx, [rdi + tcb.fs_base + 4]
    wrmsr

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    popfq
    ret

fork_child_restore:
    mov rcx, 0xC0000100
    push rax
    push rdx
    mov rdx, [rel current_tcb]
    mov rax, [rdx + tcb.fs_base]
    mov edx, [rdx + tcb.fs_base + 4]
    wrmsr
    pop rdx
    pop rax

    iretq

exec_switch_resume:
    mov rsp, rdi
    mov rax, [rel current_tcb]
    mov rcx, 0xC0000100
    mov rdx, rax
    mov rax, [rdx + tcb.fs_base]
    mov edx, [rdx + tcb.fs_base + 4]
    wrmsr

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    popfq
    ret