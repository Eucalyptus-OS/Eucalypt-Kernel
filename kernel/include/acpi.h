#pragma once

#include <stdint.h>

// Physical MMIO bases for the local APIC and I/O APIC, filled in by acpi_parse_tables
extern uint64_t lapic_addr;
extern uint64_t ioapic_addr;

// Parse the ACPI tables found at boot and record the APIC MMIO addresses
void acpi_parse_tables();
