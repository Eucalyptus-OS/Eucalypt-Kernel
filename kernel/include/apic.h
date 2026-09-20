#pragma once

#include <stdint.h>

// Tick counter incremented by each timer interrupt, used for scheduling
extern volatile uint64_t system_ticks;

// Local APIC register read/write and init helpers
uint32_t apic_read(uint32_t reg);
void apic_write(uint32_t reg, uint32_t value);
void apic_eoi();
uint8_t apic_init();

// I/O APIC redirection table helpers for routing interrupts to the local APIC
uint32_t ioapic_read(uint32_t reg);
void ioapic_write(uint32_t reg, uint64_t value);
void ioapic_set_entry(uint8_t irq, uint8_t vector);
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);