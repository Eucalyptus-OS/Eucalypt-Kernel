#pragma once

#include <stdint.h>

// Filled in by acpi_init() from MADT; physical addresses of the MMIO regions.
extern uint64_t lapic_addr;
extern uint64_t ioapic_addr;

uint8_t acpi_init(uint64_t rsdp_phys);
