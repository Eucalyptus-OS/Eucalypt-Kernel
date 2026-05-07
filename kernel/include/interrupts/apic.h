#pragma once
#include <stdint.h>
#include <stdbool.h>

#define APIC_REG_ID           0x20
#define APIC_REG_VERSION      0x30
#define APIC_REG_TPR          0x80
#define APIC_REG_EOI          0xB0
#define APIC_REG_SVR          0xF0
#define APIC_REG_ICR_LOW      0x300
#define APIC_REG_ICR_HIGH     0x310
#define APIC_REG_LVT_TIMER    0x320
#define APIC_REG_LVT_LINT0    0x350
#define APIC_REG_LVT_LINT1    0x360
#define APIC_REG_LVT_ERROR    0x370
#define APIC_REG_TIMER_ICR    0x380
#define APIC_REG_TIMER_CCR    0x390
#define APIC_REG_TIMER_DCR    0x3E0

#define APIC_SVR_ENABLE       (1 << 8)
#define APIC_LVT_MASKED       (1 << 16)
#define APIC_TIMER_PERIODIC   (1 << 17)
#define APIC_TIMER_VECTOR     0x20
#define APIC_TIMER_DCR_1      0xB

#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VERSION    0x01
#define IOAPIC_REG_REDTBL     0x10

void enable_apic(bool is_bsp);
void apic_eoi(void);
uint32_t apic_read(uint32_t reg);
void apic_write(uint32_t reg, uint32_t value);
uint8_t apic_id(void);
void apic_timer_init(uint32_t hz);
void ioapic_init(uint64_t phys_base);
void ioapic_set_entry(uint8_t irq, uint8_t vector, uint8_t dest, bool masked);
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);
