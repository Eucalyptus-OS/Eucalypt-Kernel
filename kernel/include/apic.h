#pragma once

#include <stdint.h>

// Incremented by the LAPIC timer handler on every tick.
extern volatile uint64_t system_ticks;

uint32_t apic_read(uint32_t reg);
void apic_write(uint32_t reg, uint32_t value);
void apic_eoi();
uint8_t apic_init();

uint32_t ioapic_read(uint32_t reg);
void ioapic_write(uint32_t reg, uint64_t value);
void ioapic_set_entry(uint8_t irq, uint8_t vector);
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);
