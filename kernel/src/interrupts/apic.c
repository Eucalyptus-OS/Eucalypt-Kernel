#include <stdint.h>
#include <stdbool.h>
#include <portio.h>
#include <mm/paging.h>
#include <assert.h>
#include <interrupts/apic.h>

#define APIC_BASE_MSR    0x1B
#define APIC_BASE_BSP    (1 << 8)
#define APIC_BASE_ENABLE (1 << 11)
#define APIC_BASE_MASK   0xFFFFFFFFFFFFF000ULL
#define APIC_HEAP_BASE   0xFFFFFFFFC0000000ULL
#define APIC_HEAP_SIZE   0x10000
#define APIC_VIRT_BASE 0xFFFFFFFF80200000ULL
#define IOAPIC_VIRT_BASE 0xFFFFFFFF80201000ULL

#define IOAPIC_REG_SELECT 0x00
#define IOAPIC_REG_WINDOW 0x10

#define TSC_CALIBRATE_MS  10

#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43
#define PIT_BASE_HZ    1193182

static volatile uint32_t *apic_virt   = NULL;
static volatile uint32_t *ioapic_virt = NULL;

static void pit_set_oneshot(uint16_t divisor) {
    outb(PIT_COMMAND, 0x34);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}

static void pit_wait_ms(uint32_t ms) {
    uint32_t ticks = (PIT_BASE_HZ * ms) / 1000;
    if (ticks > 0xFFFF) ticks = 0xFFFF;
    pit_set_oneshot((uint16_t)ticks);
    while (1) {
        outb(PIT_COMMAND, 0xE2);
        if (inb(PIT_CHANNEL0) & (1 << 7))
            break;
    }
}

static void cpu_write_apic_msr(uint64_t value) {
    __asm__ volatile (
        "wrmsr"
        : : "c"((uint32_t)APIC_BASE_MSR),
            "a"((uint32_t)(value & 0xFFFFFFFF)),
            "d"((uint32_t)(value >> 32))
    );
}

static uint64_t cpu_read_apic_msr(void) {
    uint32_t low, high;
    __asm__ volatile (
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"((uint32_t)APIC_BASE_MSR)
    );
    return ((uint64_t)high << 32) | low;
}

static uint64_t cpu_get_apic_base(void) {
    return cpu_read_apic_msr() & APIC_BASE_MASK;
}

static void cpu_set_apic_base(uint64_t base, bool is_bsp) {
    uint64_t msr = (base & APIC_BASE_MASK) | APIC_BASE_ENABLE;
    if (is_bsp)
        msr |= APIC_BASE_BSP;
    cpu_write_apic_msr(msr);
}

uint32_t apic_read(uint32_t reg) {
    ASSERT_NOT_NULL(apic_virt);
    return apic_virt[reg / 4];
}

void apic_write(uint32_t reg, uint32_t value) {
    ASSERT_NOT_NULL(apic_virt);
    apic_virt[reg / 4] = value;
}

void apic_eoi(void) {
    apic_write(APIC_REG_EOI, 0);
}

uint8_t apic_id(void) {
    return (uint8_t)(apic_read(APIC_REG_ID) >> 24);
}

static uint32_t apic_timer_calibrate(uint64_t hz) {
    apic_write(APIC_REG_TIMER_DCR, APIC_TIMER_DCR_1);
    apic_write(APIC_REG_TIMER_ICR, 0xFFFFFFFF);
    apic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED);

    uint32_t apic_start = apic_read(APIC_REG_TIMER_CCR);
    pit_wait_ms(TSC_CALIBRATE_MS);
    uint32_t apic_end = apic_read(APIC_REG_TIMER_CCR);

    uint32_t ticks = apic_start - apic_end;
    return (ticks * hz) / TSC_CALIBRATE_MS;
}

void apic_timer_init(uint32_t hz) {
    uint32_t ticks_per_sec = apic_timer_calibrate(hz);
    uint32_t interval      = ticks_per_sec / hz;

    apic_write(APIC_REG_TIMER_DCR, APIC_TIMER_DCR_1);
    apic_write(APIC_REG_LVT_TIMER, APIC_TIMER_VECTOR | APIC_TIMER_PERIODIC);
    apic_write(APIC_REG_TIMER_ICR, interval);
}

static uint32_t ioapic_read(uint8_t reg) {
    ASSERT_NOT_NULL(ioapic_virt);
    ioapic_virt[IOAPIC_REG_SELECT / 4] = reg;
    return ioapic_virt[IOAPIC_REG_WINDOW / 4];
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    ASSERT_NOT_NULL(ioapic_virt);
    ioapic_virt[IOAPIC_REG_SELECT / 4] = reg;
    ioapic_virt[IOAPIC_REG_WINDOW / 4] = value;
}

void ioapic_set_entry(uint8_t irq, uint8_t vector, uint8_t dest, bool masked) {
    uint8_t  reg   = IOAPIC_REG_REDTBL + irq * 2;
    uint64_t entry = vector;
    if (masked)
        entry |= APIC_LVT_MASKED;
    ioapic_write(reg,     (uint32_t)(entry & 0xFFFFFFFF));
    ioapic_write(reg + 1, (uint32_t)((uint64_t)dest << 24));
}

void ioapic_mask(uint8_t irq) {
    uint8_t  reg = IOAPIC_REG_REDTBL + irq * 2;
    uint32_t low = ioapic_read(reg);
    ioapic_write(reg, low | APIC_LVT_MASKED);
}

void ioapic_unmask(uint8_t irq) {
    uint8_t  reg = IOAPIC_REG_REDTBL + irq * 2;
    uint32_t low = ioapic_read(reg);
    ioapic_write(reg, low & ~(uint32_t)APIC_LVT_MASKED);
}

void ioapic_init(uint64_t phys_base) {
    ASSERT_NOT_NULL(apic_virt);
    paging_map_page(kernel_pml4, IOAPIC_VIRT_BASE, phys_base, 0x1000,
                    ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW | ENTRY_FLAG_NX);
    ioapic_virt = (volatile uint32_t *)IOAPIC_VIRT_BASE;

    uint8_t max_irqs = (ioapic_read(IOAPIC_REG_VERSION) >> 16) & 0xFF;
    for (uint8_t i = 0; i <= max_irqs; i++)
        ioapic_mask(i);
}

void enable_apic(bool is_bsp) {
    uint64_t phys = cpu_get_apic_base();
    uint64_t msr  = cpu_read_apic_msr();

    if ((msr & APIC_BASE_ENABLE) == 0)
        cpu_set_apic_base(phys, is_bsp);

    // map directly into kernel page tables, not a separate vm_space
    paging_map_page(kernel_pml4, APIC_VIRT_BASE, phys, 0x1000,
                    ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW | ENTRY_FLAG_NX);
    apic_virt = (volatile uint32_t *)APIC_VIRT_BASE;

    apic_write(APIC_REG_TPR,       0);
    apic_write(APIC_REG_LVT_LINT0, APIC_LVT_MASKED);
    apic_write(APIC_REG_LVT_LINT1, APIC_LVT_MASKED);
    apic_write(APIC_REG_LVT_ERROR, APIC_LVT_MASKED);
    apic_write(APIC_REG_SVR,       apic_read(APIC_REG_SVR) | APIC_SVR_ENABLE);
}