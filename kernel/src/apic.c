#include <stdint.h>
#include <mm/page.h>
#include <mm/hhdm.h>
#include <multitasking/sched.h>
#include <portio.h>
#include <acpi.h>
#include <logging/print.h>
#include <apic.h>

#define APIC_ENABLE         0x800
#define APIC_SVR            0xF0
#define APIC_LVT_TIMER      0x320
#define APIC_TIMER_DIV      0x3E0
#define APIC_TIMER_INIT     0x380
#define APIC_TIMER_CUR      0x390

#define IOAPICREDTBL(n)     (0x10 + 2 * (n))
#define IOAPIC_MASKED       (1ULL << 16)
#define IOAPIC_TRIGGER_LEVEL (1ULL << 15)
#define IOAPIC_POLARITY_LOW  (1ULL << 13)

// Initial LAPIC timer count; dictates the tick rate each time it is reloaded
uint64_t apic_count = 0x100000;

// Global monotonic tick counter bumped by every timer interrupt
volatile uint64_t system_ticks = 0;

// Return the 64-bit value held in an MSR (read via rdmsr)
static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// Store a 64-bit value to an MSR (write via wrmsr)
static inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low  = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Read a 32-bit LAPIC register; byte offsets are scaled down to dword indices
uint32_t apic_read(uint32_t reg) {
    volatile uint32_t *lapic = (volatile uint32_t *)phys_to_virt(lapic_addr);
    return lapic[reg / 4];
}

// Write a 32-bit LAPIC register at the given byte offset
void apic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t *lapic = (volatile uint32_t *)phys_to_virt(lapic_addr);
    lapic[reg / 4] = value;
}

// IOAPIC access goes through an index register at +0x0 and data window at +0x10
uint32_t ioapic_read(uint32_t reg) {
    volatile uint32_t *ioregsel = (volatile uint32_t *)phys_to_virt(ioapic_addr);
    volatile uint32_t *iowin    = (volatile uint32_t *)((uint8_t *)phys_to_virt(ioapic_addr) + 0x10);

    *ioregsel = reg;
    return *iowin;
}

// Poke a 64-bit value across the IOAPIC's two 32-bit data-window slots
void ioapic_write(uint32_t reg, uint64_t value) {
    volatile uint32_t *ioregsel = (volatile uint32_t *)phys_to_virt(ioapic_addr);
    volatile uint32_t *iowin    = (volatile uint32_t *)((uint8_t *)phys_to_virt(ioapic_addr) + 0x10);

    *ioregsel = reg;
    *iowin    = (uint32_t)(value & 0xFFFFFFFF);

    *ioregsel = reg + 1;
    *iowin    = (uint32_t)(value >> 32);
}

// Signal end-of-interrupt so the LAPIC will deliver further interrupts
void apic_eoi() {
    // 0xB0 is the EOI register; any value signals completion
    apic_write(0xB0, 0x0);
}

// Route an IRQ line to a vector on the target local APIC via a redirection entry
void ioapic_set_entry(uint8_t irq, uint8_t vector) {
    // LAPIC ID register; the id lives in bits 31:24
    uint32_t lapic_id = apic_read(0x20) >> 24;

    // Entry layout: vector | trigger mode | polarity | destination LAPIC id in bits 63:56
    uint64_t entry = vector;
    entry |= IOAPIC_TRIGGER_LEVEL;
    entry |= IOAPIC_POLARITY_LOW;
    entry |= ((uint64_t)lapic_id << 56);

    ioapic_write(IOAPICREDTBL(irq), entry);
}

// Set the mask bit (16) of a redirection entry to stop it delivering interrupts
void ioapic_mask(uint8_t irq) {
    uint32_t low = ioapic_read(IOAPICREDTBL(irq));
    low |= (uint32_t)IOAPIC_MASKED;
    ioapic_write(IOAPICREDTBL(irq), low);
}

// Clear the mask bit of a redirection entry to re-enable interrupt delivery
void ioapic_unmask(uint8_t irq) {
    uint32_t low = ioapic_read(IOAPICREDTBL(irq));
    low &= ~(uint32_t)IOAPIC_MASKED;
    ioapic_write(IOAPICREDTBL(irq), low);
}

// Retire the legacy PIC, enable the LAPIC, and wire the timer through the IOAPIC
uint8_t apic_init() {
    // Mask all IRQs on the master (0x21) and slave (0xA1) PICs
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    // Both APICs are MMIO devices and must be mapped uncacheable before use
    paging_map_page(kernel_pml4, phys_to_virt(lapic_addr), lapic_addr,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_DISABLE_CACHE);
    paging_map_page(kernel_pml4, phys_to_virt(ioapic_addr), ioapic_addr,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_DISABLE_CACHE);

    // IA32_APIC_BASE (0x1B): bit 11 turns the local APIC on
    uint64_t apic_base = read_msr(0x1B);
    apic_base |= APIC_ENABLE;
    write_msr(0x1B, apic_base);

    // Spurious vector 0xFF plus bit 8 (enable) in the SVR
    apic_write(APIC_SVR, apic_read(APIC_SVR) | 0x100 | 0xFF);

    // Timer setup: divider 0x3 divides the bus clock by 16, LVT uses vector 0x20 in periodic mode (bit 17), then load the count
    apic_write(APIC_TIMER_DIV, 0x3);
    apic_write(APIC_LVT_TIMER, 0x20 | 0x20000);
    apic_write(APIC_TIMER_INIT, apic_count);

    uint32_t ioapic_ver = ioapic_read(0x01);
    // Bits 16-23 of the version register give the highest redirection entry index
    uint8_t  max_redir  = (ioapic_ver >> 16) & 0xFF;
    uint8_t  irq_lines  = max_redir + 1;

    // Start with every redirection entry masked so no IRQ fires until unmasked
    for (int i = 0; i < irq_lines; i++) {
        ioapic_write(IOAPICREDTBL(i), IOAPIC_MASKED);
    }
    
    // Route legacy IRQ 0 (the timer) to vector 0x20 and unmask it
    ioapic_set_entry(0, 0x20);
    ioapic_unmask(0);
    print("APIC initialized\n");
    return 0;
}

// Periodic timer ISR: bump the tick counter, service timeouts, then reschedule
void timer_handler() {
    system_ticks++;
    sched_check_timeouts();
    apic_eoi();
    schedule();
}