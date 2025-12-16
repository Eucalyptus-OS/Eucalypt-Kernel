global isr_stub_table

extern exception_handler
extern irq_handler

%macro isr_no_err_stub 1
isr_stub_%+%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro isr_err_stub 1
isr_stub_%+%1:
    push qword %1
    jmp isr_common_stub
%endmacro

isr_common_stub:
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
    
    mov rdi, [rsp + 15*8]
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
    add rsp, 16
    iretq
    iretq

%macro irq_stub 1
irq_stub_%+%1:
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
    
    mov rdi, %1 - 32
    call irq_handler
    
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
%endmacro

isr_no_err_stub 0  ; Division error
isr_no_err_stub 1  ; Debug
isr_no_err_stub 2  ; Non-maskable Interrupts
isr_no_err_stub 3  ; Breakpoint
isr_no_err_stub 4  ; Overflow
isr_no_err_stub 5  ; Bound Range Exceeded
isr_no_err_stub 6  ; Invalid Opcode
isr_no_err_stub 7  ; Device Not Available
isr_err_stub    8  ; Double Fault
isr_no_err_stub 9  ; Something I guess
isr_err_stub    10 ; Invalid TSS
isr_err_stub    11 ; Segment Not Present
isr_err_stub    12 ; Stack Segment Fault
isr_err_stub    13 ; G(eneral)E(lectric) Fault
isr_err_stub    14 ; Page Fault
isr_no_err_stub 15 ; Reserved
isr_no_err_stub 16 ; Floating Point Exception
isr_err_stub    17 ; Alignment
isr_no_err_stub 18 ; Machine
isr_no_err_stub 19 ; SIMD Floating
isr_no_err_stub 20 ; Virtualization
isr_no_err_stub 21 ; CPE
isr_no_err_stub 22 ; Reserved
isr_no_err_stub 23 ; Reserved
isr_no_err_stub 24 ; Reserved
isr_no_err_stub 25 ; Reserved
isr_no_err_stub 26 ; Reserved
isr_no_err_stub 27 ; Reserved
isr_no_err_stub 28 ; Hypervisor
isr_no_err_stub 29 ; VMM
isr_err_stub    30 ; SE
isr_no_err_stub 31 ; FPU

irq_stub 32 ; Timer
irq_stub 33 ; Keyboard
irq_stub 34
irq_stub 35
irq_stub 36
irq_stub 37
irq_stub 38
irq_stub 39
irq_stub 40
irq_stub 41
irq_stub 42
irq_stub 43
irq_stub 44
irq_stub 45
irq_stub 46
irq_stub 47

isr_stub_table:
%assign i 0 
%rep    32 
    dq isr_stub_%+i
%assign i i+1 
%endrep

%assign i 32
%rep    16
    dq irq_stub_%+i
%assign i i+1
%endrep