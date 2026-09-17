[BITS 64]

extern exception_handler
extern timer_handler
extern ahci_handler
extern keyboard_handler
extern syscall_handler
extern current_tcb

global ahci_stub
global isr_stub_table
global apic_stub
global keyboard_stub
global int128_handler
global syscall_entry_stub

; User-mode code/data segment selectors (DPL 3).
USER_CS equ 0x20 | 3
USER_SS equ 0x18 | 3

; Table of stub addresses for exceptions 0-31, consumed by idt_init.
isr_stub_table:
%assign i 0
%rep 32
    dq isr_stub_%+i
%assign i i+1
%endrep

; Stub for exceptions without an error code: save GPRs, call C handler, iretq.
%macro ISR_NOERR 1
isr_stub_%+%1:
    push 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdx, rsp
    mov rdi, %1
    xor rsi, rsi
    call exception_handler
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
    add rsp, 8
    iretq
%endmacro

; Stub for exceptions with an error code pushed by the CPU: pass it to the handler.
%macro ISR_ERR 1
isr_stub_%+%1:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdx, rsp
    mov rdi, %1
    ; arg2: fetch the error code stored above all the saved registers.
    mov rsi, [rsp + 120]
    call exception_handler
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
    add rsp, 8
    iretq
%endmacro

; Generate stubs for the 32 CPU exceptions (8, 10-14, 17, 30 push error codes).
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; IRQ 0x20 (APIC timer): save regs, call timer_handler, iretq.
apic_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call timer_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; IRQ 0x21 (AHCI storage): dispatch to ahci_handler.
ahci_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call ahci_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; IRQ 0x22 (PS/2 keyboard): dispatch to keyboard_handler.
keyboard_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    call keyboard_handler
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; int 0x80 from user mode: dispatch to syscall_handler.
int128_handler:
    push 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov r8, [rsp + 40]      ; arg4 = user r10
    mov r9, [rsp + 56]      ; arg5 = user r8
    mov rax, rsp
    push rax
    call syscall_handler
    add rsp, 8
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
    add rsp, 8
    iretq

; SYSCALL entry: move to the kernel stack and build a user_context_t frame.
syscall_entry_stub:
    cli
    ; Keep user RSP in r11, then load this thread's kernel stack (tcb+16).
    mov r11, rsp
    mov rsp, [rel current_tcb]
    mov rsp, [rsp + 16]
    ; Push a user iretq frame (SS, RSP, RFLAGS with IF set, CS, RIP=rcx)...
    push USER_SS
    push r11
    pushfq
    or dword [rsp], 0x200
    push USER_CS
    push rcx
    ; ...plus fake error/vector slots so the layout matches the exception stubs.
    push 0
    push rax
    push rbx
    push 0 
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push 0 
    push r12
    push r13
    push r14
    push r15
    ; Run the C handler with interrupts enabled.
    sti
    ; Extract the syscall number and its arguments from the saved user GPRs.
    mov rdi, [rsp + 112]    ; num
    mov rsi, [rsp + 72]     ; arg1 = user rdi
    mov rdx, [rsp + 80]     ; arg2 = user rsi
    mov rcx, [rsp + 88]     ; arg3 = user rdx
    mov r8,  [rsp + 40]     ; arg4 = user r10
    mov r9,  [rsp + 56]     ; arg5 = user r8
    mov rax, rsp
    push rax
    call syscall_handler
    add rsp, 8
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
    add rsp, 8
    iretq
