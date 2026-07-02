#pragma once

#include <stdbool.h>
#include <stdint.h>

void acpi_log_tables();
bool acpi_get_apic_info(uint64_t *lapic_phys, uint64_t *ioapic_phys);
