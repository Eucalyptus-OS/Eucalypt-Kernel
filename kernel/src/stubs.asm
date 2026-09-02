[bits 64]

extern isr_handler

global isr_stub_table

; Stack layout produced by every stub (ascending addresses), matching
; interrupt_frame_t in idt.h:
;   r15 r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx rbx rax
;   vector error_code rip cs rflags rsp ss

%macro PUSHALL 0
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
%endmacro

%macro POPALL 0
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
%endmacro

; %1 = vector, %2 = 1 if the CPU pushes an error code for this vector.
; The dummy 0 keeps the frame layout uniform so C only sees one shape.
%macro ISR_STUB 2
isr_stub_%+%1:
%if !%2
    push 0
%endif
    push %1                 ; vector number, below error code
    PUSHALL
    mov rdi, rsp            ; SysV arg1: pointer to interrupt_frame_t
    call isr_handler
    POPALL
    add rsp, 16             ; discard vector + (real or dummy) error code;
    iretq                   ; iretq pops the CPU frame itself
%endmacro

; Pointer table consumed by idt_init().
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep

; Exceptions with hardware-pushed error codes (same map as tiny-kern):
%assign v 0
%rep 32
%if (v == 8) || (v >= 10 && v <= 14) || (v == 17) || (v == 30)
    ISR_STUB v, 1
%else
    ISR_STUB v, 0
%endif
%assign v v+1
%endrep

; External/spurious vectors 32-255 never carry a CPU error code.
%assign v 32
%rep 224
    ISR_STUB v, 0
%assign v v+1
%endrep
