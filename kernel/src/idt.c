#include <idt.h>
#include <portio.h>
#include <stdint.h>
#include <mm/hhdm.h>
#include <logging/print.h>
#include <stdint.h>

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

__attribute__((aligned(0x10)))
static idt_entry_t idt[256];
static idtr_t idtr;

extern void *isr_stub_table[];

// C-level dispatch table: index = vector, NULL = unhandled.
static void (*handlers[256])(interrupt_frame_t *);

void idt_register_handler(uint8_t vector, void (*handler)(interrupt_frame_t *)) {
    handlers[vector] = handler;
}

static const char *exception_name(uint64_t vector) {
    static const char *names[] = {
        "Divide error",
        "Debug",
        "NMI",
        "Breakpoint",
        "Overflow",
        "Bound range",
        "Invalid opcode",
        "Device unavailable",
        "Double fault",
        "Coprocessor segment overrun",
        "Invalid TSS",
        "Segment not present",
        "Stack fault",
        "General protection",
        "Page fault",
        "Reserved",
        "x87 floating-point",
        "Alignment check",
        "Machine check",
        "SIMD floating-point",
        "Virtualization",
        "Control protection",
    };

    if (vector < sizeof(names) / sizeof(names[0]) && names[vector])
        return names[vector];
    return "Unknown";
}

void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags) {
    idt_entry_t *d = &idt[vector];
    d->isr_low   = (uint64_t)isr & 0xFFFF;
    d->kernel_cs = 0x08;
    d->ist       = 0;
    d->attributes = flags;
    d->isr_mid   = ((uint64_t)isr >> 16) & 0xFFFF;
    d->isr_high  = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    d->reserved  = 0;
}

uint8_t idt_init() {
    idtr.base  = (uintptr_t)&idt[0];
    idtr.limit = sizeof(idt_entry_t) * 256 - 1;

    for (uint16_t v = 0; v < 256; v++)
        idt_set_descriptor((uint8_t)v, isr_stub_table[v], 0x8E);

    // Mask the legacy PICs; all external IRQs come through the IOAPIC instead.
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    __asm__ volatile ("lidt %0" :: "m"(idtr));
    print("IDT initialized");
    return 0;
}

// Common entry from stubs.asm. Handlers receive the frame and do their own EOI.
void isr_handler(interrupt_frame_t *f) {
    if (handlers[f->vector]) {
        handlers[f->vector](f);
        return;
    }

    // Unhandled CPU exception: dump state and halt rather than faulting again.
    if (f->vector < 32) {
        __asm__ volatile ("cli");
        uint64_t cr2 = 0, cr3 = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));

        print("PANIC: unhandled exception: %s", exception_name(f->vector));
        print("  vector=%lu error=%lu rip=0x%lx cs=0x%lx rflags=0x%lx",
              f->vector, f->error_code, f->rip, f->cs, f->rflags);
        print("  rsp=0x%lx ss=0x%lx cr2=0x%lx cr3=0x%lx", f->rsp, f->ss, cr2, cr3);

        for (;;) __asm__ volatile ("cli\nhlt");
    }

    // Unregistered external vector: ignore (no handler ran, so no EOI).
}
