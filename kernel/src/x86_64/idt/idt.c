#include <x86_64/idt/idt.h>

#include <x86_64/interrupts/pic.h>
#include <x86_64/interrupts/timer.h>
#include <x86_64/interrupts/keyboard.h>
#include <x86_64/serial.h>

#include <stdint.h>
#include <stdbool.h>
#include <flanterm/fb.h>

#define MAX_IDT_DESCS 255

extern struct flanterm_context *ft_ctx;

extern void isr128_handler();
extern void* isr_stub_table[];
static bool vectors[MAX_IDT_DESCS];

typedef struct {
	uint16_t    isr_low;      // The lower 16 bits of the ISR's address
	uint16_t    kernel_cs;    // The GDT segment selector that the CPU will load into CS before calling the ISR
	uint8_t	    ist;          // The IST in the TSS that the CPU will load into RSP; set to zero for now
	uint8_t     attributes;   // Type and attributes; see the IDT page
	uint16_t    isr_mid;      // The higher 16 bits of the lower 32 bits of the ISR's address
	uint32_t    isr_high;     // The higher 32 bits of the ISR's address
	uint32_t    reserved;     // Set to zero
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

static idtr_t idtr;

__attribute__((aligned(0x10))) 
static idt_entry_t idt[256];

void exception_handler(uint8_t exception_number) {
    switch (exception_number) {
    case 0:
        flanterm_write(ft_ctx, "Divide By Zero Exception\n");
        serial_print("Divide By Zero Exception\n");
        break;
    case 2:
        flanterm_write(ft_ctx, "Non Maskable Interrupt Exception\n");
        serial_print("Non Maskable Interrupt Exception\n");
        break;
    case 6:
        flanterm_write(ft_ctx, "Invalid Opcode Exception\n");
        serial_print("Invalid Opcode Exception\n");
        break;
    case 8:
        flanterm_write(ft_ctx, "Double Fault Exception\n");
        serial_print("Double Fault Exception\n");
        break;
    case 13:
        flanterm_write(ft_ctx, "General Protection Fault Exception\n");
        serial_print("General Protection Fault Exception\n");
        break;
    case 14:
        flanterm_write(ft_ctx, "Page Fault Exception\n");
        serial_print("Page Fault Exception\n");
        break;
    case 18:
        flanterm_write(ft_ctx, "Machine Check Exception\n");
        serial_print("Machine Check Exception\n");
        break;
    
    default:
        flanterm_write(ft_ctx, "Unhandled Exception\n");
        serial_print("Unhandled Exception\n");
        serial_print_num(exception_number);
        break;
    }
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void irq_handler(uint8_t irq) {
    switch (irq) {
    case 0:
        on_irq0();
        serial_print(".");
        pic_send_eoi(0);
        break;
    case 1:
        keyboard_handler();
        pic_send_eoi(1);
        break;
    default:
        break;
    }
}

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    idt_entry_t* descriptor = &idt[vector];

    descriptor->isr_low        = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs      = 8;
    descriptor->ist            = 0;
    descriptor->attributes     = flags;
    descriptor->isr_mid        = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high       = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved       = 0;
}

void idt_init() {
    idtr.base = (uintptr_t)&idt[0];
    idtr.limit = (uint16_t)sizeof(idt_entry_t) * MAX_IDT_DESCS - 1;
    for (uint8_t vector = 0; vector < 32; vector++) {
        idt_set_descriptor(vector, isr_stub_table[vector], 0x8E);
        vectors[vector] = true;
    }

    for (uint8_t vector = 0; vector < 16; vector++) {
        idt_set_descriptor(32 + vector, isr_stub_table[32 + vector], 0x8E);
        vectors[32 + vector] = true;
    }

    idt_set_descriptor(128, isr128_handler, 0x8E);
    IRQ_clear_mask(0);
    IRQ_clear_mask(1);

    __asm__ volatile ("lidt %0" : : "m"(idtr));
    __asm__ volatile ("sti");
}