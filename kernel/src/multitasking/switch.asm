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
    .state:       resb 1
    .wake_tick:   resq 1
    .timed:       resb 1
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

    mov rax, [rdi + tcb.addr_space]
    mov rcx, cr3

    cmp rax, rcx
    je .same_cr3

    mov cr3, rax

.same_cr3:
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
    iretq

exec_switch_resume:
    mov rsp, rdi

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