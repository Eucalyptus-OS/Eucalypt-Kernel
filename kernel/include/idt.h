#pragma once

#include <stdint.h>

// Register state at interrupt entry, laid out exactly as stubs.asm builds it.
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    // CPU-pushed frame; ascending addresses are rip..ss because the stack
    // grows down while the CPU pushes ss first.
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

uint8_t idt_init();
// Handlers own their EOIs; exceptions without a handler halt via panic.
void idt_register_handler(uint8_t vector, void (*handler)(interrupt_frame_t *));
