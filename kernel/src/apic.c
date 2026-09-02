#include <stdint.h>
#include <mm/hhdm.h>
#include <mm/paging.h>
#include <acpi.h>
#include <apic.h>
#include <idt.h>
#include <multitasking/sched.h>
#include <logging/print.h>

#define APIC_ENABLE         0x800
#define APIC_SVR            0xF0
#define APIC_EOI            0xB0
#define APIC_LVT_TIMER      0x320
#define APIC_TIMER_DIV      0x3E0
#define APIC_TIMER_INIT     0x380
#define APIC_ID             0x20

#define IOAPICREDTBL(n)     (0x10 + 2 * (n))
#define IOAPIC_MASKED       (1ULL << 16)
#define IOAPIC_TRIGGER_LEVEL (1ULL << 15)
#define IOAPIC_POLARITY_LOW  (1ULL << 13)

#define TIMER_VECTOR        0x20
#define TIMER_INIT_COUNT    0x100000

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

// LAPIC registers are 32-bit words at 16-byte-strided offsets.
uint32_t apic_read(uint32_t reg) {
    volatile uint32_t *lapic = (volatile uint32_t *)phys_to_virt(lapic_addr);
    return lapic[reg / 4];
}

void apic_write(uint32_t reg, uint32_t value) {
    volatile uint32_t *lapic = (volatile uint32_t *)phys_to_virt(lapic_addr);
    lapic[reg / 4] = value;
}

// IOAPIC uses an indirect register window: select via IOREGSEL, access IOWIN.
static inline volatile uint32_t *ioapic_ioregsel() {
    return (volatile uint32_t *)phys_to_virt(ioapic_addr);
}

static inline volatile uint32_t *ioapic_iowin() {
    return (volatile uint32_t *)((uint8_t *)phys_to_virt(ioapic_addr) + 0x10);
}

uint32_t ioapic_read(uint32_t reg) {
    *ioapic_ioregsel() = reg;
    return *ioapic_iowin();
}

void ioapic_write(uint32_t reg, uint64_t value) {
    // 64-bit redirection entries are written as two 32-bit halves.
    *ioapic_ioregsel() = reg;
    *ioapic_iowin()    = (uint32_t)(value & 0xFFFFFFFF);

    *ioapic_ioregsel() = reg + 1;
    *ioapic_iowin()    = (uint32_t)(value >> 32);
}

void apic_eoi() {
    apic_write(APIC_EOI, 0x0);
}

void ioapic_set_entry(uint8_t irq, uint8_t vector) {
    // Route the GSI to this CPU's LAPIC; level-triggered, active-low,
    // matching QEMU's ISA override conventions.
    uint32_t lapic_id = apic_read(APIC_ID) >> 24;

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

static void timer_handler(interrupt_frame_t *f) {
    (void)f;
    system_ticks++;
    // EOI before switching: the LAPIC must not think the interrupt is still
    // being serviced while we may be off this stack for a while.
    apic_eoi();
    schedule();
}

uint8_t apic_init() {
    // Enable the LAPIC globally and give it a spurious interrupt vector with
    // the "spurious dispatch enabled" bit set.
    uint64_t apic_base = read_msr(0x1B);
    apic_base |= APIC_ENABLE;
    write_msr(0x1B, apic_base);

    apic_write(APIC_SVR, apic_read(APIC_SVR) | 0x100 | 0xFF);

    // Periodic timer: divide by 16, vector 0x20, arbitrary initial count.
    apic_write(APIC_TIMER_DIV, 0x3);
    apic_write(APIC_LVT_TIMER, TIMER_VECTOR | 0x20000);
    apic_write(APIC_TIMER_INIT, TIMER_INIT_COUNT);

    // Start with every IOAPIC line masked so nothing fires uninvited.
    uint32_t ioapic_ver = ioapic_read(0x01);
    uint8_t  max_redir  = (ioapic_ver >> 16) & 0xFF;

    for (int i = 0; i <= max_redir; i++) {
        ioapic_mask(i);
    }

    idt_register_handler(TIMER_VECTOR, timer_handler);
    print("APIC initialized (IOAPIC has %u redir entries)", max_redir + 1);
    return 0;
}
