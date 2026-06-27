#pragma once
#include <stdint.h>

#define APIC_TIMER_VECTOR    0x20
#define SYSCALL_VECTOR       0x80
#define PS2_KEYBOARD_VECTOR  0x21
#define PS2_MOUSE_VECTOR     0x2C
#define TLB_SHOOTDOWN_VECTOR 0x30

typedef struct {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t  ist;
    uint8_t  attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

typedef struct {
    idt_entry_t idt[256];
    interrupt_frame_t interrupt_frame;
} idt_per_cpu_t;

void idt_init();
void idt_init_per_cpu();
void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags);