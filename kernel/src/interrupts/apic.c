#include <stdint.h>
#include <stdbool.h>
#include <portio.h>
#include <mm/paging.h>
#include <mm/hhdm.h>
#include <logging/printk.h>
#include <msr.h>
#include <drivers/acpi.h>
#include <interrupts/apic.h>

#define APIC_BASE_MSR    0x1B
#define APIC_BASE_BSP    (1 << 8)
#define APIC_BASE_EXTD   (1 << 10)
#define APIC_BASE_ENABLE (1 << 11)
#define APIC_BASE_MASK   0xFFFFFFFFFFFFF000ULL
#define APIC_VIRT_BASE   0xFFFFFFFF80200000ULL
#define IOAPIC_VIRT_BASE 0xFFFFFFFF80201000ULL
#define APIC_PHYS_BASE   0xFEE00000ULL
#define IOAPIC_PHYS_BASE 0xFEC00000ULL

#define IOAPIC_REG_SELECT 0x00
#define IOAPIC_REG_WINDOW 0x10

#define TSC_CALIBRATE_MS  10

#define PIT_CHANNEL0   0x40
#define PIT_COMMAND    0x43
#define PIT_BASE_HZ    1193182
#define PIT_MODE0_LOHI 0x30
#define PIT_READBACK   0xE2
#define PIT_WAIT_TIMEOUT_ITERS 5000000

#define APIC_REG_ICR_LOW  0x300
#define APIC_REG_ICR_HIGH 0x310

#define ICR_DELIVERY_FIXED   (0 << 8)
#define ICR_DEST_PHYSICAL    (0 << 11)
#define ICR_LEVEL_ASSERT     (1 << 14)
#define ICR_DEST_NO_SHORTHAND (0 << 18)
#define ICR_SEND_PENDING_TIMEOUT_ITERS 2000000

#define APIC_SPURIOUS_VECTOR 0xFF
#define APIC_SVR_VECTOR_MASK 0xFFu

#define APIC_DEFAULT_TICKS_PER_SEC 1000000000u

#define APIC_MMIO_FLAGS (ENTRY_FLAG_PRESENT | ENTRY_FLAG_RW | ENTRY_FLAG_NX | \
                          ENTRY_FLAG_PCD | ENTRY_FLAG_PWT)

volatile uint32_t *apic_virt = NULL;
volatile uint32_t *ioapic_virt = NULL;
static volatile int apic_mapped = 0;
static volatile int ioapic_initialized = 0;

static volatile uint32_t calibrated_ticks_per_sec = 0;
static volatile int timer_calibrated = 0;
static volatile int calibration_lock = 0;

static bool cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile (
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );
    return (edx & (1u << 9)) != 0;
}

static void pit_set_oneshot(uint16_t divisor) {
    outb(PIT_COMMAND, PIT_MODE0_LOHI);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));
}

static bool pit_wait_ms(uint32_t ms) {
    uint32_t ticks = (PIT_BASE_HZ * ms) / 1000;
    if (ticks > 0xFFFF) {
        ticks = 0xFFFF;
    }
    pit_set_oneshot((uint16_t)ticks);

    uint32_t spin = 0;
    while (1) {
        outb(PIT_COMMAND, PIT_READBACK);
        uint8_t status = inb(PIT_CHANNEL0);
        if (status == 0xFF) {
            return false;
        }
        if (status & (1 << 7)) {
            return true;
        }
        if (++spin > PIT_WAIT_TIMEOUT_ITERS) {
            return false;
        }
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

static void cpu_force_xapic_mode(uint64_t phys, bool is_bsp) {
    uint64_t msr = cpu_read_apic_msr();

    msr &= ~(uint64_t)(APIC_BASE_ENABLE | APIC_BASE_EXTD);
    cpu_write_apic_msr(msr);

    msr = (phys & APIC_BASE_MASK) | APIC_BASE_ENABLE;
    if (is_bsp)
        msr |= APIC_BASE_BSP;
    cpu_write_apic_msr(msr);
}

uint32_t apic_read(uint32_t reg) {
    if (apic_virt == NULL) {
        return 0;
    }
    return apic_virt[reg / 4];
}

void apic_write(uint32_t reg, uint32_t value) {
    if (apic_virt == NULL) {
        return;
    }
    apic_virt[reg / 4] = value;
}

void apic_eoi(void) {
    apic_write(APIC_REG_EOI, 0);
}

uint8_t apic_id(void) {
    return (uint8_t)(apic_read(APIC_REG_ID) >> 24);
}

static uint32_t apic_timer_calibrate(void) {
    apic_write(APIC_REG_TIMER_DCR, APIC_TIMER_DCR_1);
    apic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED);
    apic_write(APIC_REG_TIMER_ICR, 0xFFFFFFFF);

    uint32_t apic_start = apic_read(APIC_REG_TIMER_CCR);
    bool ok = pit_wait_ms(TSC_CALIBRATE_MS);
    uint32_t apic_end = apic_read(APIC_REG_TIMER_CCR);

    apic_write(APIC_REG_TIMER_ICR, 0);

    if (!ok) {
        log_warn("apic: PIT calibration timed out (no legacy PIT?), using fallback timer rate\n");
        return APIC_DEFAULT_TICKS_PER_SEC;
    }

    uint32_t ticks = apic_start - apic_end;
    if (ticks == 0) {
        log_warn("apic: timer calibration measured 0 ticks, using fallback timer rate\n");
        return APIC_DEFAULT_TICKS_PER_SEC;
    }

    return (ticks * 1000) / TSC_CALIBRATE_MS;
}

void apic_send_ipi(uint8_t apic_id, uint8_t vector) {
    uint32_t spin = 0;
    while (apic_read(APIC_REG_ICR_LOW) & (1 << 12)) {
        if (++spin > ICR_SEND_PENDING_TIMEOUT_ITERS) {
            log_warn("apic: IPI send timed out waiting for prior delivery to clear\n");
            return;
        }
        asm volatile ("pause");
    }
    apic_write(APIC_REG_ICR_HIGH, (uint32_t)apic_id << 24);
    apic_write(APIC_REG_ICR_LOW,
               vector | ICR_DELIVERY_FIXED | ICR_DEST_PHYSICAL |
               ICR_LEVEL_ASSERT | ICR_DEST_NO_SHORTHAND);
}

void apic_timer_init(uint32_t hz) {
    if (hz == 0) {
        hz = 1000;
    }

    if (__atomic_load_n(&timer_calibrated, __ATOMIC_ACQUIRE) == 0) {
        while (__atomic_exchange_n(&calibration_lock, 1, __ATOMIC_ACQUIRE)) {
            asm volatile ("pause");
        }
        if (__atomic_load_n(&timer_calibrated, __ATOMIC_ACQUIRE) == 0) {
            uint32_t result = apic_timer_calibrate();
            __atomic_store_n(&calibrated_ticks_per_sec, result, __ATOMIC_RELEASE);
            __atomic_store_n(&timer_calibrated, 1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&calibration_lock, 0, __ATOMIC_RELEASE);
    }

    uint32_t ticks_per_sec = __atomic_load_n(&calibrated_ticks_per_sec, __ATOMIC_ACQUIRE);
    uint32_t interval = ticks_per_sec / hz;
    if (interval == 0) {
        log_warn("apic: timer calibration returned %u ticks/sec for %u Hz; forcing interval 1\n",
                 ticks_per_sec, hz);
        interval = 1;
    }

    apic_write(APIC_REG_TIMER_DCR, APIC_TIMER_DCR_1);
    apic_write(APIC_REG_LVT_TIMER, APIC_TIMER_VECTOR | APIC_TIMER_PERIODIC);
    apic_write(APIC_REG_TIMER_ICR, interval);
}

static uint32_t ioapic_read(uint8_t reg) {
    if (ioapic_virt == NULL) {
        return 0;
    }
    ioapic_virt[IOAPIC_REG_SELECT / 4] = reg;
    return ioapic_virt[IOAPIC_REG_WINDOW / 4];
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    if (ioapic_virt == NULL) {
        return;
    }
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

void ioapic_init(void) {
    if (__atomic_exchange_n(&ioapic_initialized, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }

    uint64_t ioapic_phys = IOAPIC_PHYS_BASE;
    if (acpi_get_apic_info(NULL, &ioapic_phys)) {
        log_info("apic: using ACPI IOAPIC address %llX\n", (unsigned long long)ioapic_phys);
    }

    paging_map_page(kernel_pml4, IOAPIC_VIRT_BASE, ioapic_phys, 0x1000, APIC_MMIO_FLAGS);
    ioapic_virt = (volatile uint32_t *)IOAPIC_VIRT_BASE;

    uint32_t version = ioapic_read(IOAPIC_REG_VERSION);
    if (version == 0 || version == 0xFFFFFFFF) {
        log_warn("apic: IOAPIC not present or unreadable; skipping IOAPIC init\n");
        ioapic_virt = NULL;
        return;
    }

    uint8_t max_irqs = (version >> 16) & 0xFF;
    for (uint8_t i = 0; i <= max_irqs; i++)
        ioapic_mask(i);
}

void enable_apic(uint8_t id, bool is_bsp) {
    if (!cpu_has_apic()) {
        log_error("apic: CPU reports no on-chip APIC support\n");
        return;
    }

    uint64_t phys = cpu_get_apic_base();
    uint64_t msr  = cpu_read_apic_msr();
    uint64_t apic_phys = APIC_PHYS_BASE;

    if (acpi_get_apic_info(&apic_phys, NULL)) {
        log_info("apic: using ACPI LAPIC address %llX\n", (unsigned long long)apic_phys);
    }

    if (phys == 0) {
        phys = apic_phys;
    }

    if (phys == 0) {
        log_warn("apic: LAPIC base MSR returned 0; skipping LAPIC init\n");
        return;
    }

    if (is_bsp) {
        log_debug("Apic - AP: %d, phys = %X, msr = %X, virt = %X\n", id, phys, msr, apic_virt);
    }

    if (msr & APIC_BASE_EXTD) {
        log_warn("apic: firmware left LAPIC in x2APIC mode; forcing xAPIC mode\n");
        cpu_force_xapic_mode(phys, is_bsp);
    } else if ((msr & APIC_BASE_ENABLE) == 0) {
        cpu_set_apic_base(phys, is_bsp);
    }

    if (is_bsp) {
        paging_map_page(kernel_pml4, APIC_VIRT_BASE, phys, 0x1000, APIC_MMIO_FLAGS);
        apic_virt = (volatile uint32_t *)APIC_VIRT_BASE;
        __atomic_store_n(&apic_mapped, 1, __ATOMIC_RELEASE);
    } else {
        while (!__atomic_load_n(&apic_mapped, __ATOMIC_ACQUIRE)) {
            asm volatile ("pause");
        }
        apic_virt = (volatile uint32_t *)APIC_VIRT_BASE;
    }

    apic_write(APIC_REG_TPR,       0);
    apic_write(APIC_REG_LVT_LINT0, APIC_LVT_MASKED);
    apic_write(APIC_REG_LVT_LINT1, APIC_LVT_MASKED);
    apic_write(APIC_REG_LVT_ERROR, APIC_LVT_MASKED);
    apic_write(APIC_REG_SVR,
               (apic_read(APIC_REG_SVR) & ~APIC_SVR_VECTOR_MASK) |
               APIC_SPURIOUS_VECTOR | APIC_SVR_ENABLE);
}