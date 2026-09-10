[BITS 64]

extern current_tcb
extern tss_set_kernel_stack
extern proc_fork_c

global switch_task
global fork_child_restore
global exec_switch_resume
global fork_call

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

; int fork_call(void) - snapshots the caller's context and asks proc_fork_c
; to clone this process. The parent resumes here normally with the child PID
; in rax; the child is first scheduled by switch_task restoring a copy of this
; same frame with rax = 0, so it resumes just after the `call fork_call`.
global fork_call
fork_call:
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

    ; rsp = r15 slot (frame[0]); rax slot is [rsp+112], rflags [rsp+120],
    ; resume_rip (the call's return address) is [rsp+128].
    mov rdi, rsp
    mov rsi, [rsp + 128]
    call proc_fork_c

    ; discard the 15 saved GPRs + rflags (128 bytes); ret pops resume_rip.
    add rsp, 128
    ret