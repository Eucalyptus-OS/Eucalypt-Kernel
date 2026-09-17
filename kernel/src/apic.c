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

uint64_t apic_count = 0x100000;

volatile uint64_t system_ticks = 0;

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    asm volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void write_msr(uint32_t msr, uint64_t value) {
    uint32_t low  = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    asm volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

uint32_t apic_read(uint32_t reg) {
    volatile uint32_t *lapic = (volatile uint32_t *)phys_to_virt(lapic_addr);
    return lapic[reg / 4];
}

void apic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t *lapic = (volatile uint32_t *)phys_to_virt(lapic_addr);
    lapic[reg / 4] = value;
}

uint32_t ioapic_read(uint32_t reg) {
    volatile uint32_t *ioregsel = (volatile uint32_t *)phys_to_virt(ioapic_addr);
    volatile uint32_t *iowin    = (volatile uint32_t *)((uint8_t *)phys_to_virt(ioapic_addr) + 0x10);

    *ioregsel = reg;
    return *iowin;
}

void ioapic_write(uint32_t reg, uint64_t value) {
    volatile uint32_t *ioregsel = (volatile uint32_t *)phys_to_virt(ioapic_addr);
    volatile uint32_t *iowin    = (volatile uint32_t *)((uint8_t *)phys_to_virt(ioapic_addr) + 0x10);

    *ioregsel = reg;
    *iowin    = (uint32_t)(value & 0xFFFFFFFF);

    *ioregsel = reg + 1;
    *iowin    = (uint32_t)(value >> 32);
}

void apic_eoi() {
    apic_write(0xB0, 0x0);
}

void ioapic_set_entry(uint8_t irq, uint8_t vector) {
    uint32_t lapic_id = apic_read(0x20) >> 24;

    uint64_t entry = vector;
    entry |= IOAPIC_TRIGGER_LEVEL;
    entry |= IOAPIC_POLARITY_LOW;
    entry |= ((uint64_t)lapic_id << 56);

    ioapic_write(IOAPICREDTBL(irq), entry);
}

void ioapic_mask(uint8_t irq) {
    uint32_t low = ioapic_read(IOAPICREDTBL(irq));
    low |= (uint32_t)IOAPIC_MASKED;
    ioapic_write(IOAPICREDTBL(irq), low);
}

void ioapic_unmask(uint8_t irq) {
    uint32_t low = ioapic_read(IOAPICREDTBL(irq));
    low &= ~(uint32_t)IOAPIC_MASKED;
    ioapic_write(IOAPICREDTBL(irq), low);
}

uint8_t apic_init() {
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    paging_map_page(kernel_pml4, phys_to_virt(lapic_addr), lapic_addr,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_DISABLE_CACHE);
    paging_map_page(kernel_pml4, phys_to_virt(ioapic_addr), ioapic_addr,
                     PAGE_PRESENT | PAGE_WRITABLE | PAGE_DISABLE_CACHE);

    uint64_t apic_base = read_msr(0x1B);
    apic_base |= APIC_ENABLE;
    write_msr(0x1B, apic_base);

    apic_write(APIC_SVR, apic_read(APIC_SVR) | 0x100 | 0xFF);

    apic_write(APIC_TIMER_DIV, 0x3);
    apic_write(APIC_LVT_TIMER, 0x20 | 0x20000);
    apic_write(APIC_TIMER_INIT, apic_count);

    uint32_t ioapic_ver = ioapic_read(0x01);
    uint8_t  max_redir  = (ioapic_ver >> 16) & 0xFF;
    uint8_t  irq_lines  = max_redir + 1;

    for (int i = 0; i < irq_lines; i++) {
        ioapic_write(IOAPICREDTBL(i), IOAPIC_MASKED);
    }
    
    ioapic_set_entry(0, 0x20);
    ioapic_unmask(0);
    print("APIC initialized\n");
    return 0;
}

void timer_handler() {
    system_ticks++;
    sched_check_timeouts();
    apic_eoi();
    schedule();
}