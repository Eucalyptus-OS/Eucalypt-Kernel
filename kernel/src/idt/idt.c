
#include <stdint.h>
#include <stdbool.h>
#include <portio.h>
#include <interrupts/apic.h>
#include <logging/serial.h>
#include <panic.h>
#include <idt/idt.h>

#define APIC_TIMER_VECTOR 0x20

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

__attribute__((aligned(0x10)))
static idt_entry_t idt[256];
static idtr_t idtr;

extern void *isr_stub_table[];

void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags) {
    idt_entry_t *d = &idt[vector];
    d->isr_low     = (uint64_t)isr & 0xFFFF;
    d->kernel_cs   = 0x08;
    d->ist         = 0;
    d->attributes  = flags;
    d->isr_mid     = ((uint64_t)isr >> 16) & 0xFFFF;
    d->isr_high    = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    d->reserved    = 0;
}

void idt_init(void) {
    idtr.base  = (uintptr_t)&idt[0];
    idtr.limit = sizeof(idt_entry_t) * 256 - 1;

    for (uint16_t v = 0; v < 256; v++)
        idt_set_descriptor((uint8_t)v, isr_stub_table[v], 0x8E);

    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    __asm__ volatile ("lidt %0" :: "m"(idtr));
    __asm__ volatile ("sti");
}

__attribute__((noreturn))
static void exception_handler(interrupt_frame_t *f) {
    uint64_t cr2 = 0, cr3 = 0;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    panic("Exception %u, error %u, RIP=%#018llx, CR2=%#018llx, CR3=%#018llx",
          (unsigned)f->vector,
          (unsigned)f->error_code,
          (unsigned long long)f->rip,
          (unsigned long long)cr2,
          (unsigned long long)cr3);
    for (;;) {
        __asm__ volatile ("cli\nhlt");
    }
}

static void apic_timer_handler(interrupt_frame_t *f) {
    (void)f;
    serial_write_fmt(".");
    apic_eoi();
}

void isr_handler(interrupt_frame_t *f) {
    switch (f->vector) {
        case APIC_TIMER_VECTOR:  apic_timer_handler(f);  break;
        default:                 exception_handler(f);   break;
    }
}