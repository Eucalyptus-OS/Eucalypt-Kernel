#include <stdint.h>
#include <stdbool.h>
#include <idt/idt.h>

typedef struct __attribute__((packed)) {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

typedef struct __attribute__((packed)) {
    uint16_t isr_low;
    uint16_t kernel_cs;
    uint8_t  ist;
    uint8_t  attributes;
    uint16_t isr_mid;
    uint32_t isr_high;
    uint32_t reserved;
} idt_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} idtr_t;

__attribute__((aligned(0x10)))
static idt_entry_t idt[256];
static idtr_t idtr;

#define IDT_MAX_DESCRIPTORS 256
static bool vectors[IDT_MAX_DESCRIPTORS];

extern void *isr_stub_table[];

void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags) {
    idt_entry_t *d  = &idt[vector];
    uint64_t addr   = (uint64_t)isr;
    d->isr_low      = addr & 0xFFFF;
    d->kernel_cs    = 0x08;
    d->ist          = 0;
    d->attributes   = flags;
    d->isr_mid      = (addr >> 16) & 0xFFFF;
    d->isr_high     = (addr >> 32) & 0xFFFFFFFF;
    d->reserved     = 0;
}

void idt_install_handler(uint8_t vector, void *handler) {
    if (vectors[vector]) return;
    idt_set_descriptor(vector, handler, 0x8E);
    vectors[vector] = true;
}

[[gnu::noreturn]]
void exception_handler(interrupt_frame_t *frame) {
    (void)frame;
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void idt_init() {
    idtr.base  = (uint64_t)(uintptr_t)&idt[0];
    idtr.limit = sizeof(idt_entry_t) * IDT_MAX_DESCRIPTORS - 1;

    for (uint8_t vector = 0; vector < 32; vector++) {
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);
        vectors[vector] = true;
    }

    __asm__ volatile ("lidt %0" :: "m"(idtr));
    __asm__ volatile ("sti");
}