extern isr_handler

struc iframe
    .r15:        resq 1
    .r14:        resq 1
    .r13:        resq 1
    .r12:        resq 1
    .r11:        resq 1
    .r10:        resq 1
    .r9:         resq 1
    .r8:         resq 1
    .rbp:        resq 1
    .rdi:        resq 1
    .rsi:        resq 1
    .rdx:        resq 1
    .rcx:        resq 1
    .rbx:        resq 1
    .rax:        resq 1
    .vector:     resq 1
    .error_code: resq 1
    .rip:        resq 1
    .cs:         resq 1
    .rflags:     resq 1
    .rsp:        resq 1
    .ss:         resq 1
endstruc

%macro isr_stub 1
isr_stub_%+%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro isr_stub_err 1
isr_stub_%+%1:
    push qword %1
    jmp isr_common
%endmacro

isr_common:
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
    mov rdi, rsp
    call isr_handler
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
    add rsp, 16
    iretq

isr_stub     0
isr_stub     1
isr_stub     2
isr_stub     3
isr_stub     4
isr_stub     5
isr_stub     6
isr_stub     7
isr_stub_err 8
isr_stub     9
isr_stub_err 10
isr_stub_err 11
isr_stub_err 12
isr_stub_err 13
isr_stub_err 14
isr_stub     15
isr_stub     16
isr_stub_err 17
isr_stub     18
isr_stub     19
isr_stub     20
isr_stub_err 21
isr_stub     22
isr_stub     23
isr_stub     24
isr_stub     25
isr_stub     26
isr_stub     27
isr_stub     28
isr_stub_err 29
isr_stub_err 30
isr_stub     31

%assign i 32
%rep 224
    isr_stub i
%assign i i+1
%endrep

global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep